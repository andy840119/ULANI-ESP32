#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "tesserae.h"
#include "ulani_store.h"

static const char *TAG = "tesserae";

#define NVS_NAMESPACE "tesserae"

#define HTTP_TIMEOUT_MS  15000
#define FRAME_TIMEOUT_MS 60000

/* Used when the server does not say, and as the floor on what it may ask for. */
#define DEFAULT_POLL_S 900
#define MIN_POLL_S      60
#define ERROR_RETRY_S   60

/* The server prefixes "v" when it displays this, so keep it a bare version. */
#define FW_VERSION "0.1.0"

/*
 * Spectra-6 nibble -> ULANI palette index.
 *
 * Tesserae renders for a six-colour Spectra panel (0 black, 1 white, 2 yellow,
 * 3 red, 5 blue, 6 green); the calendar is a seven-colour ACeP part with a
 * different order (0 black, 1 white, 2 green, 3 blue, 4 red, 5 yellow,
 * 6 orange). Nothing maps to orange, which Spectra-6 does not have, and 0x4 is
 * unused on that panel -- anything unexpected becomes white rather than a
 * stray block of colour.
 */
static const uint8_t PALETTE_MAP[16] = {
    [0x0] = 0, /* black  -> black  */
    [0x1] = 1, /* white  -> white  */
    [0x2] = 5, /* yellow -> yellow */
    [0x3] = 4, /* red    -> red    */
    [0x4] = 1, /* unused -> white  */
    [0x5] = 3, /* blue   -> blue   */
    [0x6] = 2, /* green  -> green  */
    [0x7] = 1, [0x8] = 1, [0x9] = 1, [0xa] = 1,
    [0xb] = 1, [0xc] = 1, [0xd] = 1, [0xe] = 1, [0xf] = 1,
};

static struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t wake; /* poll now instead of waiting out the interval */

    tesserae_frame_cb_t on_frame;
    void               *user;

    char server_url[TESSERAE_URL_MAX];
    char pairing_code[TESSERAE_CODE_MAX];
    char token[TESSERAE_TOKEN_MAX];
    char device_id[TESSERAE_ID_MAX];
    char etag[TESSERAE_ETAG_MAX];
    uint8_t slot;

    tesserae_state_t state;
    char             last_error[96];
    int32_t          next_poll_s;
    int64_t          due_us;
    uint32_t         last_frame_at;
} t;

static void lock(void)   { xSemaphoreTake(t.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(t.lock); }

const char *tesserae_state_str(tesserae_state_t s)
{
    switch (s) {
    case TESSERAE_DISABLED:     return "disabled";
    case TESSERAE_UNREGISTERED: return "unregistered";
    case TESSERAE_IDLE:         return "idle";
    case TESSERAE_WORKING:      return "working";
    case TESSERAE_ERROR:        return "error";
    }
    return "?";
}

static void set_error(const char *what)
{
    lock();
    strlcpy(t.last_error, what, sizeof(t.last_error));
    t.state = TESSERAE_ERROR;
    unlock();
    ESP_LOGW(TAG, "%s", what);
}

/* ---------------------------------------------------------------- config */

static void config_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(t.server_url);
    nvs_get_str(h, "url", t.server_url, &n);
    n = sizeof(t.pairing_code);
    nvs_get_str(h, "code", t.pairing_code, &n);
    n = sizeof(t.token);
    nvs_get_str(h, "token", t.token, &n);
    n = sizeof(t.device_id);
    nvs_get_str(h, "devid", t.device_id, &n);
    n = sizeof(t.etag);
    nvs_get_str(h, "etag", t.etag, &n);
    nvs_get_u8(h, "slot", &t.slot);
    nvs_close(h);
}

static void config_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "url", t.server_url);
    nvs_set_str(h, "code", t.pairing_code);
    nvs_set_str(h, "token", t.token);
    nvs_set_str(h, "devid", t.device_id);
    nvs_set_str(h, "etag", t.etag);
    nvs_set_u8(h, "slot", t.slot);
    nvs_commit(h);
    nvs_close(h);
}

static void mac_string(char *out, size_t len)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ------------------------------------------------------------------ http */

/* Performs a request and returns the body. Caller frees. NULL on transport error. */
static char *http_json(esp_http_client_method_t method, const char *url,
                       const char *body, const char *auth, const char *code,
                       const char *if_none_match, int *out_status,
                       char *out_etag, size_t etag_len)
{
    esp_http_client_config_t cfg = {
        .url             = url,
        .method          = method,
        .timeout_ms      = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return NULL;
    }

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    if (auth && auth[0]) {
        char hdr[TESSERAE_TOKEN_MAX + 16];
        snprintf(hdr, sizeof(hdr), "Bearer %s", auth);
        esp_http_client_set_header(cli, "Authorization", hdr);
    }
    if (code && code[0]) {
        esp_http_client_set_header(cli, "X-Pairing-Code", code);
    }
    if (if_none_match && if_none_match[0]) {
        char hdr[TESSERAE_ETAG_MAX + 4];
        snprintf(hdr, sizeof(hdr), "\"%s\"", if_none_match);
        esp_http_client_set_header(cli, "If-None-Match", hdr);
    }

    int len = body ? (int)strlen(body) : 0;
    esp_err_t err = esp_http_client_open(cli, len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(cli);
        return NULL;
    }
    if (len > 0 && esp_http_client_write(cli, body, len) != len) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return NULL;
    }

    int64_t content = esp_http_client_fetch_headers(cli);
    *out_status = esp_http_client_get_status_code(cli);

    if (out_etag && etag_len) {
        char *value = NULL;
        if (esp_http_client_get_header(cli, "ETag", &value) == ESP_OK && value) {
            /* Servers quote ETags; the header we send back adds them again. */
            const char *p = value;
            size_t n = strlen(p);
            if (n >= 2 && p[0] == '"' && p[n - 1] == '"') {
                n -= 2;
                p++;
            }
            if (n >= etag_len) n = etag_len - 1;
            memcpy(out_etag, p, n);
            out_etag[n] = 0;
        }
    }

    size_t cap = (content > 0 && content < 4096) ? (size_t)content + 1 : 4096;
    char  *buf = calloc(1, cap);
    if (buf) {
        int got = esp_http_client_read_response(cli, buf, (int)cap - 1);
        if (got < 0) {
            free(buf);
            buf = NULL;
        }
    }

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return buf;
}

static char *identity_body(void)
{
    char mac[13];
    mac_string(mac, sizeof(mac));

    cJSON *o = cJSON_CreateObject();
    /* The admin may have named the device; the MAC is only the fallback. */
    cJSON_AddStringToObject(o, "device_id", t.device_id[0] ? t.device_id : mac);
    cJSON_AddStringToObject(o, "kind", TESSERAE_DEVICE_KIND);
    cJSON_AddNumberToObject(o, "panel_w", ULANI_IMG_W);
    cJSON_AddNumberToObject(o, "panel_h", ULANI_IMG_H);
    cJSON_AddStringToObject(o, "fw_version", FW_VERSION);
    cJSON_AddStringToObject(o, "mac", mac);

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static void json_str(cJSON *o, const char *key, char *out, size_t len)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strlcpy(out, v->valuestring, len);
    }
}

/* ---------------------------------------------------------- registration */

/*
 * Two ways in. With a pairing code the server can decide immediately; without
 * one the device announces itself and waits for the admin to approve it in the
 * Tesserae UI, which is matched by MAC. The second is friendlier -- there is no
 * code to find -- so an empty code is a supported configuration, not an error.
 */
static bool try_register(void)
{
    char url[TESSERAE_URL_MAX + 40];
    bool discover = (t.pairing_code[0] == 0);
    snprintf(url, sizeof(url), "%s/api/v1/device/%s", t.server_url,
             discover ? "discover" : "register");

    char *body = identity_body();
    if (!body) {
        return false;
    }

    int   status = 0;
    char *rsp = http_json(HTTP_METHOD_POST, url, body, NULL,
                          discover ? NULL : t.pairing_code, NULL, &status, NULL, 0);
    free(body);

    if (!rsp) {
        set_error("cannot reach the server");
        return false;
    }

    cJSON *r = cJSON_Parse(rsp);
    free(rsp);
    if (!r) {
        set_error("server sent something that is not JSON");
        return false;
    }

    bool ok = false;
    if (status == 200 || status == 201) {
        char token[TESSERAE_TOKEN_MAX] = { 0 };
        json_str(r, "device_token", token, sizeof(token));

        if (token[0]) {
            lock();
            strlcpy(t.token, token, sizeof(t.token));
            json_str(r, "device_id", t.device_id, sizeof(t.device_id));
            /* Spent. Keeping it only guarantees a 403 the next time round. */
            t.pairing_code[0] = 0;
            t.state = TESSERAE_IDLE;
            t.last_error[0] = 0;
            unlock();
            config_save();
            ESP_LOGI(TAG, "registered as %s", t.device_id);
            ok = true;
        } else {
            /* discover before approval: a normal, expected state */
            lock();
            t.state = TESSERAE_UNREGISTERED;
            strlcpy(t.last_error,
                    "在 tesserae 的 Settings > Devices > Discovered 按 Register",
                    sizeof(t.last_error));
            unlock();
        }
    } else {
        /* The server's own wording beats anything guessed at from a status. */
        cJSON *detail = cJSON_GetObjectItemCaseSensitive(r, "detail");
        if (!cJSON_IsString(detail)) {
            detail = cJSON_GetObjectItemCaseSensitive(r, "error");
        }
        char msg[96];
        if (cJSON_IsString(detail) && detail->valuestring[0]) {
            snprintf(msg, sizeof(msg), "server said (HTTP %d): %s",
                     status, detail->valuestring);
        } else {
            snprintf(msg, sizeof(msg), "registration failed (HTTP %d)", status);
        }
        set_error(msg);

        /*
         * A pairing code is single use. Retrying a rejected one cannot work,
         * and hammering it every minute would also burn a fresh code the
         * moment the user issued one. Drop it and fall back to announcing
         * ourselves, which needs no code at all -- the device then shows up
         * under Discovered and a click finishes the job.
         */
        if (!discover && status == 403) {
            lock();
            t.pairing_code[0] = 0;
            unlock();
            config_save();
            ESP_LOGW(TAG, "pairing code spent; announcing instead, "
                          "approve this device under Discovered");
        }
    }

    cJSON_Delete(r);
    return ok;
}

/* ------------------------------------------------------------- telemetry */

static int32_t post_status(void)
{
    char url[TESSERAE_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/api/v1/device/%s/status",
             t.server_url, t.device_id);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "fw_version", FW_VERSION);
    /* Mains powered through the ESP32, so a battery reading would be a lie. */
    cJSON_AddNumberToObject(o, "rssi", 0);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) {
        return -1;
    }

    int   status = 0;
    char *rsp = http_json(HTTP_METHOD_POST, url, body, t.token, NULL, NULL,
                          &status, NULL, 0);
    free(body);
    if (!rsp) {
        return -1;
    }

    int32_t next = -1;
    cJSON  *r = cJSON_Parse(rsp);
    free(rsp);
    if (r) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(r, "next_poll_s");
        if (cJSON_IsNumber(v)) {
            next = (int32_t)v->valuedouble;
        }
        cJSON_Delete(r);
    }
    if (status == 401) {
        lock();
        t.token[0] = 0;
        t.state = TESSERAE_UNREGISTERED;
        unlock();
        config_save();
        ESP_LOGW(TAG, "token rejected; will register again");
    }
    return next;
}

/* ----------------------------------------------------------- frame fetch */

/*
 * Streams the frame into the slot store, translating the palette as it goes.
 * Never holds more than a couple of kilobytes: the whole frame is 192000 bytes
 * and the heap on this part is nowhere near that once the radios are up.
 */
static bool fetch_frame_body(const char *url, bool with_auth)
{
    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = FRAME_TIMEOUT_MS,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return false;
    }

    /*
     * A signed /renders/ URL carries its own authorisation and wants no
     * header; the /api/v1/device/... frame routes want the bearer. The
     * reference firmware splits the same way -- image_fetch() passes no token
     * for the plain frame, image_fetch_auth() passes one for deck and overlay.
     */
    if (with_auth && t.token[0]) {
        char hdr[TESSERAE_TOKEN_MAX + 16];
        snprintf(hdr, sizeof(hdr), "Bearer %s", t.token);
        esp_http_client_set_header(cli, "Authorization", hdr);
    }

    bool ok = false;
    ulani_store_writer_t writer;
    bool writing = false;

    if (esp_http_client_open(cli, 0) != ESP_OK) {
        set_error("cannot open the frame URL");
        goto done;
    }

    int64_t total = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        ESP_LOGW(TAG, "frame body HTTP %d from %s", status, url);
        /*
         * A 403 on a render artifact is a server-side policy, not a bad
         * request: Tesserae only serves /renders/ to clients it sees as being
         * on the LAN, unless "Allow REST clients on public networks" is
         * switched on. Reaching a LAN server through a public hostname puts
         * the device on the wrong side of that check, so say what to change
         * rather than leaving a status code on screen.
         */
        char msg[96];
        if (status == 403) {
            snprintf(msg, sizeof(msg),
                     "server 拒絕下載圖片(403)：請改用 NAS 的區網 IP，"
                     "或開啟 Allow REST clients on public networks");
        } else {
            snprintf(msg, sizeof(msg), "frame download failed (HTTP %d)", status);
        }
        set_error(msg);
        goto done;
    }
    if (total > 0 && total != (int64_t)ULANI_PAYLOAD_BYTES) {
        char msg[96];
        snprintf(msg, sizeof(msg), "frame is %lld bytes, expected %u",
                 total, (unsigned)ULANI_PAYLOAD_BYTES);
        set_error(msg);
        goto done;
    }

    if (ulani_store_write_begin(t.slot, &writer) != ESP_OK) {
        set_error("cannot open storage for the frame");
        goto done;
    }
    writing = true;

    static uint8_t buf[1024];
    size_t written = 0;

    while (written < ULANI_PAYLOAD_BYTES) {
        int want = (int)sizeof(buf);
        if (written + (size_t)want > ULANI_PAYLOAD_BYTES) {
            want = (int)(ULANI_PAYLOAD_BYTES - written);
        }
        int got = esp_http_client_read(cli, (char *)buf, want);
        if (got <= 0) {
            set_error("frame download ended early");
            goto done;
        }
        for (int i = 0; i < got; i++) {
            buf[i] = (uint8_t)((PALETTE_MAP[buf[i] >> 4] << 4) |
                                PALETTE_MAP[buf[i] & 0x0f]);
        }
        if (ulani_store_write(&writer, buf, (size_t)got) != ESP_OK) {
            set_error("could not store the frame");
            goto done;
        }
        written += (size_t)got;
    }

    if (ulani_store_write_commit(&writer) != ESP_OK) {
        set_error("frame did not store cleanly");
        goto done;
    }
    writing = false;
    ok = true;

done:
    if (writing) {
        ulani_store_write_abort(&writer);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return ok;
}

/* Returns true when a new frame was stored. */
static bool poll_frame(void)
{
    char url[TESSERAE_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/api/v1/device/%s/frame",
             t.server_url, t.device_id);

    int  status = 0;
    char etag[TESSERAE_ETAG_MAX] = { 0 };
    char *rsp = http_json(HTTP_METHOD_GET, url, NULL, t.token, NULL, t.etag,
                          &status, etag, sizeof(etag));

    if (status == 304) {
        free(rsp);
        ESP_LOGI(TAG, "frame unchanged");
        return false;
    }
    if (status == 204) {
        free(rsp);
        ESP_LOGI(TAG, "server has nothing rendered yet");
        return false;
    }
    if (!rsp || status != 200) {
        free(rsp);
        char msg[80];
        snprintf(msg, sizeof(msg), "frame request failed (HTTP %d)", status);
        set_error(msg);
        return false;
    }

    ESP_LOGI(TAG, "frame response: %.400s", rsp);

    cJSON *r = cJSON_Parse(rsp);
    free(rsp);
    if (!r) {
        set_error("frame response was not JSON");
        return false;
    }

    char frame_url[256] = { 0 };
    json_str(r, "url", frame_url, sizeof(frame_url));
    cJSON_Delete(r);

    if (!frame_url[0]) {
        set_error("frame response had no URL");
        return false;
    }

    /*
     * The server may answer with a path rather than an absolute URL. Resolve it
     * against the origin only, dropping any path on the configured server URL,
     * the way the reference firmware's resolve_url() does.
     */
    char absolute[TESSERAE_URL_MAX + 256];
    if (strncmp(frame_url, "http://", 7) == 0 ||
        strncmp(frame_url, "https://", 8) == 0) {
        strlcpy(absolute, frame_url, sizeof(absolute));
    } else {
        char origin[TESSERAE_URL_MAX];
        strlcpy(origin, t.server_url, sizeof(origin));
        char *p = strstr(origin, "://");
        p = p ? p + 3 : origin;
        char *slash = strchr(p, '/');
        if (slash) {
            *slash = 0;
        }
        snprintf(absolute, sizeof(absolute), "%s%s%s", origin,
                 frame_url[0] == '/' ? "" : "/", frame_url);
    }

    /* Signed URLs authenticate themselves; anything else needs the bearer. */
    bool signed_url = (strstr(absolute, "sig=") != NULL);

    ESP_LOGI(TAG, "fetching %s", absolute);
    if (!fetch_frame_body(absolute, !signed_url)) {
        return false;
    }

    lock();
    strlcpy(t.etag, etag, sizeof(t.etag));
    t.last_frame_at = (uint32_t)(esp_timer_get_time() / 1000000);
    t.last_error[0] = 0;
    unlock();
    config_save();

    ESP_LOGI(TAG, "frame stored in slot %u", t.slot);
    return true;
}

/* ------------------------------------------------------------------ task */

static void tesserae_task(void *param)
{
    (void)param;

    for (;;) {
        int64_t now = esp_timer_get_time();
        int64_t wait_us = t.due_us - now;

        if (wait_us > 0) {
            /* A configure() or poll-now cuts the wait short. */
            xSemaphoreTake(t.wake, pdMS_TO_TICKS(wait_us / 1000));
            continue;
        }

        if (t.server_url[0] == 0) {
            lock();
            t.state = TESSERAE_DISABLED;
            unlock();
            t.due_us = esp_timer_get_time() + (int64_t)ERROR_RETRY_S * 1000000;
            continue;
        }

        lock();
        t.state = TESSERAE_WORKING;
        unlock();

        if (t.device_id[0] == 0 && t.token[0] != 0) {
            set_error("a token needs the device id it was issued against");
            t.due_us = esp_timer_get_time() + (int64_t)ERROR_RETRY_S * 1000000;
            continue;
        }

        if (t.token[0] == 0) {
            bool got = try_register();
            t.due_us = esp_timer_get_time() +
                       (int64_t)(got ? 1 : ERROR_RETRY_S) * 1000000;
            continue;
        }

        int32_t next = post_status();
        bool    fresh = false;

        if (t.token[0]) {
            fresh = poll_frame();
        }

        if (fresh && t.on_frame) {
            t.on_frame(t.slot, t.user);
        }

        if (next < MIN_POLL_S) {
            next = (next > 0) ? MIN_POLL_S : DEFAULT_POLL_S;
        }

        lock();
        t.next_poll_s = next;
        if (t.state == TESSERAE_WORKING) {
            t.state = TESSERAE_IDLE;
        }
        unlock();

        t.due_us = esp_timer_get_time() + (int64_t)next * 1000000;
        ESP_LOGI(TAG, "next poll in %d s", (int)next);
    }
}

/* ------------------------------------------------------------ public API */

esp_err_t tesserae_start(const tesserae_cfg_t *cfg)
{
    memset(&t, 0, sizeof(t));
    t.slot = 1;

    t.lock = xSemaphoreCreateMutex();
    t.wake = xSemaphoreCreateBinary();
    if (!t.lock || !t.wake) {
        return ESP_ERR_NO_MEM;
    }
    if (cfg) {
        t.on_frame = cfg->on_frame;
        t.user     = cfg->user;
    }

    config_load();
    t.state = t.server_url[0] ? (t.token[0] ? TESSERAE_IDLE : TESSERAE_UNREGISTERED)
                              : TESSERAE_DISABLED;
    if (t.slot < ULANI_SLOT_MIN || t.slot > ULANI_SLOT_MAX) {
        t.slot = 1;
    }

    /* Give WiFi a moment before the first attempt. */
    t.due_us = esp_timer_get_time() + 5 * 1000000;

    if (xTaskCreate(tesserae_task, "tesserae", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t tesserae_configure(const char *server_url, const char *pairing_code,
                             const char *device_id, const char *token,
                             uint8_t slot)
{
    if (!server_url || strlen(server_url) >= TESSERAE_URL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot < ULANI_SLOT_MIN || slot > ULANI_SLOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (token && strlen(token) >= TESSERAE_TOKEN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (device_id && strlen(device_id) >= TESSERAE_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * "Add without pairing" hands out a token against an id the admin chose,
     * so one without the other cannot address anything.
     */
    if (token && token[0] && !(device_id && device_id[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    strlcpy(t.server_url, server_url, sizeof(t.server_url));
    strlcpy(t.pairing_code, pairing_code ? pairing_code : "", sizeof(t.pairing_code));
    strlcpy(t.device_id, device_id ? device_id : "", sizeof(t.device_id));
    strlcpy(t.token, token ? token : "", sizeof(t.token));
    /* Whatever the server last said about a frame refers to the old identity. */
    t.etag[0]       = 0;
    t.slot          = slot;
    t.state         = t.token[0] ? TESSERAE_IDLE : TESSERAE_UNREGISTERED;
    t.last_error[0] = 0;
    unlock();

    config_save();
    t.due_us = 0;
    xSemaphoreGive(t.wake);
    return ESP_OK;
}

esp_err_t tesserae_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    lock();
    t.server_url[0]   = 0;
    t.pairing_code[0] = 0;
    t.token[0]        = 0;
    t.device_id[0]    = 0;
    t.etag[0]         = 0;
    t.state           = TESSERAE_DISABLED;
    t.last_error[0]   = 0;
    unlock();

    t.due_us = 0;
    xSemaphoreGive(t.wake);
    return ESP_OK;
}

esp_err_t tesserae_poll_now(void)
{
    if (t.server_url[0] == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Drop the ETag so the server sends the frame even if it has not changed. */
    lock();
    t.etag[0] = 0;
    unlock();

    t.due_us = 0;
    xSemaphoreGive(t.wake);
    return ESP_OK;
}

void tesserae_get_status(tesserae_status_t *out)
{
    lock();
    out->state      = t.state;
    out->registered = (t.token[0] != 0);
    out->slot       = t.slot;
    out->next_poll_s = t.next_poll_s;
    out->last_frame_at = t.last_frame_at;
    strlcpy(out->server_url, t.server_url, sizeof(out->server_url));
    strlcpy(out->device_id, t.device_id, sizeof(out->device_id));
    strlcpy(out->last_error, t.last_error, sizeof(out->last_error));
    unlock();

    int64_t remaining = (t.due_us - esp_timer_get_time()) / 1000000;
    out->seconds_until_poll = remaining > 0 ? (int32_t)remaining : 0;
}
