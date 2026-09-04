#include <string.h>
#include <time.h>

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

typedef struct {
    uint8_t slot; /* 1..4 */

    char server_url[TESSERAE_URL_MAX];
    char pairing_code[TESSERAE_CODE_MAX];
    char token[TESSERAE_TOKEN_MAX];
    char device_id[TESSERAE_ID_MAX];
    char etag[TESSERAE_ETAG_MAX];

    tesserae_state_t state;
    char             last_error[96];
    int32_t          next_poll_s;
    int64_t          due_us;
    uint32_t         last_frame_epoch;
} client_t;

static struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t wake; /* interrupt the task's wait after a config change */

    tesserae_frame_cb_t on_frame;
    void               *user;

    client_t client[TESSERAE_CLIENTS];

    /*
     * Wall clock, learned from the server's HTTP Date header rather than SNTP,
     * as the reference firmware does. base_epoch is the server time at the
     * instant base_us was sampled from the monotonic timer; 0 = unknown.
     */
    int64_t wall_base_epoch;
    int64_t wall_base_us;
} s;

static void lock(void)   { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s.lock); }

const char *tesserae_state_str(tesserae_state_t st)
{
    switch (st) {
    case TESSERAE_DISABLED:     return "disabled";
    case TESSERAE_UNREGISTERED: return "unregistered";
    case TESSERAE_IDLE:         return "idle";
    case TESSERAE_WORKING:      return "working";
    case TESSERAE_ERROR:        return "error";
    }
    return "?";
}

static void set_error(client_t *c, const char *what)
{
    lock();
    strlcpy(c->last_error, what, sizeof(c->last_error));
    c->state = TESSERAE_ERROR;
    unlock();
    ESP_LOGW(TAG, "slot %u: %s", c->slot, what);
}

/* ----------------------------------------------------------------- clock */

/*
 * "Sun, 06 Nov 1994 08:49:37 GMT" -> unix epoch, without pulling in strptime
 * or timegm (neither is reliably present here). Returns 0 if it does not
 * parse, which the caller treats as "clock still unknown".
 */
static int64_t parse_http_date(const char *d)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = { 0 };
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;

    /* Skip the weekday and its comma. */
    const char *p = strchr(d, ',');
    p = p ? p + 1 : d;
    while (*p == ' ') {
        p++;
    }
    if (sscanf(p, "%d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) {
        return 0;
    }

    const char *m = strstr(months, mon);
    if (!m) {
        return 0;
    }
    int month = (int)(m - months) / 3; /* 0..11 */

    /* Days from the civil calendar (Howard Hinnant's algorithm). */
    int y = year - (month < 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (month + (month > 1 ? -2 : 10)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;

    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

static void note_wall_clock(const char *date_hdr)
{
    if (!date_hdr) {
        return;
    }
    int64_t epoch = parse_http_date(date_hdr);
    if (epoch <= 0) {
        return;
    }
    lock();
    s.wall_base_epoch = epoch;
    s.wall_base_us    = esp_timer_get_time();
    unlock();
}

static uint32_t now_epoch(void)
{
    lock();
    int64_t base = s.wall_base_epoch;
    int64_t at   = s.wall_base_us;
    unlock();
    if (base <= 0) {
        return 0;
    }
    return (uint32_t)(base + (esp_timer_get_time() - at) / 1000000);
}

/* ---------------------------------------------------------------- config */

static void mkkey(char *out, size_t len, const char *stem, uint8_t slot)
{
    snprintf(out, len, "%s%u", stem, (unsigned)slot);
}

static void config_load(client_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char k[16];
    size_t n;
    mkkey(k, sizeof(k), "url", c->slot);
    n = sizeof(c->server_url);
    nvs_get_str(h, k, c->server_url, &n);
    mkkey(k, sizeof(k), "code", c->slot);
    n = sizeof(c->pairing_code);
    nvs_get_str(h, k, c->pairing_code, &n);
    mkkey(k, sizeof(k), "token", c->slot);
    n = sizeof(c->token);
    nvs_get_str(h, k, c->token, &n);
    mkkey(k, sizeof(k), "devid", c->slot);
    n = sizeof(c->device_id);
    nvs_get_str(h, k, c->device_id, &n);
    mkkey(k, sizeof(k), "etag", c->slot);
    n = sizeof(c->etag);
    nvs_get_str(h, k, c->etag, &n);
    nvs_close(h);
}

static void config_save(client_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char k[16];
    mkkey(k, sizeof(k), "url", c->slot);   nvs_set_str(h, k, c->server_url);
    mkkey(k, sizeof(k), "code", c->slot);  nvs_set_str(h, k, c->pairing_code);
    mkkey(k, sizeof(k), "token", c->slot); nvs_set_str(h, k, c->token);
    mkkey(k, sizeof(k), "devid", c->slot); nvs_set_str(h, k, c->device_id);
    mkkey(k, sizeof(k), "etag", c->slot);  nvs_set_str(h, k, c->etag);
    nvs_commit(h);
    nvs_close(h);
}

static void config_erase(client_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char k[16];
    mkkey(k, sizeof(k), "url", c->slot);   nvs_erase_key(h, k);
    mkkey(k, sizeof(k), "code", c->slot);  nvs_erase_key(h, k);
    mkkey(k, sizeof(k), "token", c->slot); nvs_erase_key(h, k);
    mkkey(k, sizeof(k), "devid", c->slot); nvs_erase_key(h, k);
    mkkey(k, sizeof(k), "etag", c->slot);  nvs_erase_key(h, k);
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

    char *dv = NULL;
    if (esp_http_client_get_header(cli, "Date", &dv) == ESP_OK) {
        note_wall_clock(dv);
    }

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

static char *identity_body(client_t *c)
{
    char mac[13];
    mac_string(mac, sizeof(mac));

    cJSON *o = cJSON_CreateObject();
    /* The admin may have named the device; the MAC is only the fallback. */
    cJSON_AddStringToObject(o, "device_id", c->device_id[0] ? c->device_id : mac);
    cJSON_AddStringToObject(o, "kind", TESSERAE_DEVICE_KIND);
    cJSON_AddNumberToObject(o, "panel_w", ULANI_IMG_W);
    cJSON_AddNumberToObject(o, "panel_h", ULANI_IMG_H);
    cJSON_AddStringToObject(o, "fw_version", FW_VERSION);
    cJSON_AddStringToObject(o, "mac", mac);

    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return str;
}

static void json_str(cJSON *o, const char *json_key, char *out, size_t len)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, json_key);
    if (cJSON_IsString(v) && v->valuestring) {
        strlcpy(out, v->valuestring, len);
    }
}

/* ---------------------------------------------------------- registration */

/*
 * With a pairing code the server can decide immediately; without one the
 * device announces itself and waits for the admin to approve it in the
 * Tesserae UI, matched by MAC. An empty code is a supported configuration.
 */
static bool try_register(client_t *c)
{
    char url[TESSERAE_URL_MAX + 40];
    bool discover = (c->pairing_code[0] == 0);
    snprintf(url, sizeof(url), "%s/api/v1/device/%s", c->server_url,
             discover ? "discover" : "register");

    char *body = identity_body(c);
    if (!body) {
        return false;
    }

    int   status = 0;
    char *rsp = http_json(HTTP_METHOD_POST, url, body, NULL,
                          discover ? NULL : c->pairing_code, NULL, &status, NULL, 0);
    free(body);

    if (!rsp) {
        set_error(c, "cannot reach the server");
        return false;
    }

    cJSON *r = cJSON_Parse(rsp);
    free(rsp);
    if (!r) {
        set_error(c, "server sent something that is not JSON");
        return false;
    }

    bool ok = false;
    if (status == 200 || status == 201) {
        char token[TESSERAE_TOKEN_MAX] = { 0 };
        json_str(r, "device_token", token, sizeof(token));

        if (token[0]) {
            lock();
            strlcpy(c->token, token, sizeof(c->token));
            json_str(r, "device_id", c->device_id, sizeof(c->device_id));
            c->pairing_code[0] = 0; /* single use; keeping it guarantees a 403 */
            c->state = TESSERAE_IDLE;
            c->last_error[0] = 0;
            unlock();
            config_save(c);
            ESP_LOGI(TAG, "slot %u registered as %s", c->slot, c->device_id);
            ok = true;
        } else {
            /* discover before approval: a normal, expected state */
            lock();
            c->state = TESSERAE_UNREGISTERED;
            strlcpy(c->last_error,
                    "approve this device under Settings > Devices > Discovered",
                    sizeof(c->last_error));
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
        set_error(c, msg);

        /*
         * A pairing code is single use. Retrying a rejected one cannot work,
         * and hammering it would also burn a fresh one the user just issued.
         * Drop it and fall back to announcing, which needs no code.
         */
        if (!discover && status == 403) {
            lock();
            c->pairing_code[0] = 0;
            unlock();
            config_save(c);
            ESP_LOGW(TAG, "slot %u: pairing code spent; announcing instead",
                     c->slot);
        }
    }

    cJSON_Delete(r);
    return ok;
}

/* ------------------------------------------------------------- telemetry */

static int32_t post_status(client_t *c)
{
    char url[TESSERAE_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/api/v1/device/%s/status",
             c->server_url, c->device_id);

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
    char *rsp = http_json(HTTP_METHOD_POST, url, body, c->token, NULL, NULL,
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
        c->token[0] = 0;
        c->state    = TESSERAE_UNREGISTERED;
        unlock();
        config_save(c);
        ESP_LOGW(TAG, "slot %u: token rejected; will register again", c->slot);
    }
    return next;
}

/* ----------------------------------------------------------- frame fetch */

/*
 * Streams the frame into the slot store, translating the palette as it goes.
 * Never holds more than a kilobyte: the frame is 192000 bytes and the heap is
 * nowhere near that once the radios are up.
 */
static bool fetch_frame_body(client_t *c, const char *url, bool with_auth)
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
     * header; the /api/v1/device/... routes want the bearer. The reference
     * firmware splits the same way: image_fetch() passes no token for the
     * plain frame, image_fetch_auth() passes one for deck and overlay.
     */
    if (with_auth && c->token[0]) {
        char hdr[TESSERAE_TOKEN_MAX + 16];
        snprintf(hdr, sizeof(hdr), "Bearer %s", c->token);
        esp_http_client_set_header(cli, "Authorization", hdr);
    }

    bool ok = false;
    ulani_store_writer_t writer;
    bool writing = false;

    if (esp_http_client_open(cli, 0) != ESP_OK) {
        set_error(c, "cannot open the frame URL");
        goto done;
    }

    int64_t total = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);

    char *dv = NULL;
    if (esp_http_client_get_header(cli, "Date", &dv) == ESP_OK) {
        note_wall_clock(dv);
    }

    if (status != 200) {
        ESP_LOGW(TAG, "slot %u: frame body HTTP %d from %s", c->slot, status, url);
        /*
         * A 403 here is server policy, not a bad request: Tesserae serves
         * /renders/ only to clients it sees on the LAN unless "Allow REST
         * clients on public networks" is on. Reaching a LAN server through a
         * public hostname fails that check, so say what to change.
         */
        char msg[96];
        if (status == 403) {
            snprintf(msg, sizeof(msg),
                     "frame refused (403): use the server's LAN IP, or enable "
                     "\"Allow REST clients on public networks\"");
        } else {
            snprintf(msg, sizeof(msg), "frame download failed (HTTP %d)", status);
        }
        set_error(c, msg);
        goto done;
    }
    if (total > 0 && total != (int64_t)ULANI_PAYLOAD_BYTES) {
        char msg[96];
        snprintf(msg, sizeof(msg), "frame is %lld bytes, expected %u",
                 total, (unsigned)ULANI_PAYLOAD_BYTES);
        set_error(c, msg);
        goto done;
    }

    if (ulani_store_write_begin(c->slot, &writer) != ESP_OK) {
        set_error(c, "cannot open storage for the frame");
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
            set_error(c, "frame download ended early");
            goto done;
        }
        for (int i = 0; i < got; i++) {
            buf[i] = (uint8_t)((PALETTE_MAP[buf[i] >> 4] << 4) |
                                PALETTE_MAP[buf[i] & 0x0f]);
        }
        if (ulani_store_write(&writer, buf, (size_t)got) != ESP_OK) {
            set_error(c, "could not store the frame");
            goto done;
        }
        written += (size_t)got;
    }

    if (ulani_store_write_commit(&writer) != ESP_OK) {
        set_error(c, "frame did not store cleanly");
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
static bool poll_frame(client_t *c)
{
    char url[TESSERAE_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/api/v1/device/%s/frame",
             c->server_url, c->device_id);

    int  status = 0;
    char etag[TESSERAE_ETAG_MAX] = { 0 };
    char *rsp = http_json(HTTP_METHOD_GET, url, NULL, c->token, NULL, c->etag,
                          &status, etag, sizeof(etag));

    if (status == 304) {
        free(rsp);
        ESP_LOGI(TAG, "slot %u: frame unchanged", c->slot);
        return false;
    }
    if (status == 204) {
        free(rsp);
        ESP_LOGI(TAG, "slot %u: server has nothing rendered yet", c->slot);
        return false;
    }
    if (!rsp || status != 200) {
        free(rsp);
        char msg[80];
        snprintf(msg, sizeof(msg), "frame request failed (HTTP %d)", status);
        set_error(c, msg);
        return false;
    }

    cJSON *r = cJSON_Parse(rsp);
    free(rsp);
    if (!r) {
        set_error(c, "frame response was not JSON");
        return false;
    }

    char frame_url[256] = { 0 };
    json_str(r, "url", frame_url, sizeof(frame_url));
    cJSON_Delete(r);

    if (!frame_url[0]) {
        set_error(c, "frame response had no URL");
        return false;
    }

    /*
     * The server may answer with a path. Resolve it against the origin only,
     * dropping any path on the configured server URL, as resolve_url() does.
     */
    char absolute[TESSERAE_URL_MAX + 256];
    if (strncmp(frame_url, "http://", 7) == 0 ||
        strncmp(frame_url, "https://", 8) == 0) {
        strlcpy(absolute, frame_url, sizeof(absolute));
    } else {
        char origin[TESSERAE_URL_MAX];
        strlcpy(origin, c->server_url, sizeof(origin));
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

    ESP_LOGI(TAG, "slot %u: fetching %s", c->slot, absolute);
    if (!fetch_frame_body(c, absolute, !signed_url)) {
        return false;
    }

    lock();
    strlcpy(c->etag, etag, sizeof(c->etag));
    c->last_error[0] = 0;
    unlock();
    c->last_frame_epoch = now_epoch();
    config_save(c);

    ESP_LOGI(TAG, "slot %u: frame stored", c->slot);
    return true;
}

/* ------------------------------------------------------------------ task */

/* Advances one client if it is due, updating its next due time. */
static void service(client_t *c)
{
    if (c->server_url[0] == 0) {
        lock();
        c->state = TESSERAE_DISABLED;
        unlock();
        c->due_us = esp_timer_get_time() + (int64_t)ERROR_RETRY_S * 1000000;
        return;
    }

    lock();
    c->state = TESSERAE_WORKING;
    unlock();

    if (c->device_id[0] == 0 && c->token[0] != 0) {
        set_error(c, "a token needs the device id it was issued against");
        c->due_us = esp_timer_get_time() + (int64_t)ERROR_RETRY_S * 1000000;
        return;
    }

    if (c->token[0] == 0) {
        bool got = try_register(c);
        c->due_us = esp_timer_get_time() +
                    (int64_t)(got ? 1 : ERROR_RETRY_S) * 1000000;
        return;
    }

    int32_t next  = post_status(c);
    bool    fresh = c->token[0] ? poll_frame(c) : false;

    if (fresh && s.on_frame) {
        s.on_frame(c->slot, s.user);
    }

    if (next < MIN_POLL_S) {
        next = (next > 0) ? MIN_POLL_S : DEFAULT_POLL_S;
    }

    lock();
    c->next_poll_s = next;
    if (c->state == TESSERAE_WORKING) {
        c->state = TESSERAE_IDLE;
    }
    unlock();

    c->due_us = esp_timer_get_time() + (int64_t)next * 1000000;
    ESP_LOGI(TAG, "slot %u: next poll in %d s", c->slot, (int)next);
}

static void tesserae_task(void *param)
{
    (void)param;

    for (;;) {
        int64_t now     = esp_timer_get_time();
        int64_t soonest = now + (int64_t)DEFAULT_POLL_S * 1000000;

        for (int i = 0; i < TESSERAE_CLIENTS; i++) {
            client_t *c = &s.client[i];
            if (c->due_us <= now) {
                service(c);
                now = esp_timer_get_time();
            }
            if (c->due_us < soonest) {
                soonest = c->due_us;
            }
        }

        int64_t wait_us = soonest - esp_timer_get_time();
        if (wait_us > 0) {
            /* A configure() or poll-now gives the semaphore to cut this short. */
            xSemaphoreTake(s.wake, pdMS_TO_TICKS(wait_us / 1000));
        }
    }
}

/* ------------------------------------------------------------ public API */

static client_t *client_for(uint8_t slot)
{
    if (slot < ULANI_SLOT_MIN || slot > ULANI_SLOT_MAX) {
        return NULL;
    }
    return &s.client[slot - ULANI_SLOT_MIN];
}

esp_err_t tesserae_start(const tesserae_cfg_t *cfg)
{
    memset(&s, 0, sizeof(s));

    s.lock = xSemaphoreCreateMutex();
    s.wake = xSemaphoreCreateBinary();
    if (!s.lock || !s.wake) {
        return ESP_ERR_NO_MEM;
    }
    if (cfg) {
        s.on_frame = cfg->on_frame;
        s.user     = cfg->user;
    }

    int64_t start = esp_timer_get_time();
    for (int i = 0; i < TESSERAE_CLIENTS; i++) {
        client_t *c = &s.client[i];
        c->slot = (uint8_t)(ULANI_SLOT_MIN + i);
        config_load(c);
        c->state = c->server_url[0]
                       ? (c->token[0] ? TESSERAE_IDLE : TESSERAE_UNREGISTERED)
                       : TESSERAE_DISABLED;
        /* Stagger first contact, and give WiFi a moment before any of it. */
        c->due_us = start + (int64_t)(5 + i * 2) * 1000000;
    }

    if (xTaskCreate(tesserae_task, "tesserae", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t tesserae_configure(uint8_t slot, const char *server_url,
                             const char *pairing_code, const char *device_id,
                             const char *token)
{
    client_t *c = client_for(slot);
    if (!c) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!server_url || strlen(server_url) >= TESSERAE_URL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (token && strlen(token) >= TESSERAE_TOKEN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (device_id && strlen(device_id) >= TESSERAE_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* "Add without pairing" issues a token against a chosen id; one without
     * the other cannot address anything. */
    if (token && token[0] && !(device_id && device_id[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    strlcpy(c->server_url, server_url, sizeof(c->server_url));
    strlcpy(c->pairing_code, pairing_code ? pairing_code : "", sizeof(c->pairing_code));
    strlcpy(c->device_id, device_id ? device_id : "", sizeof(c->device_id));
    strlcpy(c->token, token ? token : "", sizeof(c->token));
    c->etag[0]       = 0; /* the old frame belonged to the old identity */
    c->state         = c->token[0] ? TESSERAE_IDLE : TESSERAE_UNREGISTERED;
    c->last_error[0] = 0;
    unlock();

    config_save(c);
    c->due_us = 0;
    xSemaphoreGive(s.wake);
    return ESP_OK;
}

esp_err_t tesserae_forget(uint8_t slot)
{
    client_t *c = client_for(slot);
    if (!c) {
        return ESP_ERR_INVALID_ARG;
    }

    config_erase(c);
    lock();
    c->server_url[0]    = 0;
    c->pairing_code[0]  = 0;
    c->token[0]         = 0;
    c->device_id[0]     = 0;
    c->etag[0]          = 0;
    c->state            = TESSERAE_DISABLED;
    c->last_error[0]    = 0;
    c->last_frame_epoch = 0;
    unlock();

    c->due_us = 0;
    xSemaphoreGive(s.wake);
    return ESP_OK;
}

esp_err_t tesserae_poll_now(uint8_t slot)
{
    client_t *c = client_for(slot);
    if (!c) {
        return ESP_ERR_INVALID_ARG;
    }
    if (c->server_url[0] == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Drop the ETag so the server resends even an unchanged frame. */
    lock();
    c->etag[0] = 0;
    unlock();

    c->due_us = 0;
    xSemaphoreGive(s.wake);
    return ESP_OK;
}

void tesserae_get_status(uint8_t slot, tesserae_status_t *out)
{
    memset(out, 0, sizeof(*out));
    client_t *c = client_for(slot);
    if (!c) {
        return;
    }

    ulani_slot_info_t info;
    ulani_store_info(slot, &info);

    lock();
    out->slot        = c->slot;
    out->state       = c->state;
    out->registered  = (c->token[0] != 0);
    out->next_poll_s = c->next_poll_s;
    out->last_frame_epoch = c->last_frame_epoch;
    strlcpy(out->server_url, c->server_url, sizeof(out->server_url));
    strlcpy(out->device_id, c->device_id, sizeof(out->device_id));
    strlcpy(out->last_error, c->last_error, sizeof(out->last_error));
    int64_t due = c->due_us;
    unlock();

    out->has_frame = info.present;

    int64_t remaining = (due - esp_timer_get_time()) / 1000000;
    out->seconds_until_poll = remaining > 0 ? (int32_t)remaining : 0;
}
