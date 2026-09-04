/*
 * HTTP front end. This file is deliberately thin: it converts requests into
 * ulani_app commands and renders ulani_app status as JSON. No protocol
 * knowledge lives here.
 */

#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "net_provision.h"
#include "tesserae.h"
#include "ulani_store.h"
#include "ulani_app.h"
#include "web_server.h"

static const char *TAG = "web";

/* Built by tools/build_web.py from web/dist. */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[]     asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[]       asm("_binary_app_js_gz_end");
extern const uint8_t app_css_gz_start[]    asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[]      asm("_binary_app_css_gz_end");

static httpd_handle_t s_server;

/* --------------------------------------------------------------- helpers */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, root);
}

/* Reads a small JSON body. Returns NULL and answers the request on failure. */
static cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        /* An empty body is legal for commands that take no arguments. */
        return req->content_len == 0 ? cJSON_CreateObject() : NULL;
    }
    char buf[513];
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        return NULL;
    }
    buf[received] = 0;
    return cJSON_Parse(buf);
}

static esp_err_t send_gz(httpd_req_t *req, const char *type,
                         const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static void add_device_array(cJSON *root, const char *key)
{
    ulani_device_t devs[ULANI_APP_MAX_DEVICES];
    size_t n = ulani_app_get_devices(devs, ULANI_APP_MAX_DEVICES);

    cJSON *arr = cJSON_AddArrayToObject(root, key);
    for (size_t i = 0; i < n; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "name", devs[i].name);
        cJSON_AddStringToObject(d, "address", devs[i].addr);
        cJSON_AddNumberToObject(d, "rssi", devs[i].rssi);
        cJSON_AddItemToArray(arr, d);
    }
}

/* ------------------------------------------------------------- endpoints */

static esp_err_t get_status(httpd_req_t *req)
{
    ulani_app_status_t st;
    ulani_app_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", ulani_state_str(st.state));
    cJSON_AddBoolToObject(root, "connected", st.connected);
    cJSON_AddNumberToObject(root, "activeSlot", st.active_slot);
    cJSON_AddNumberToObject(root, "batteryRaw", st.battery_rsp);
    if (st.battery_valid) {
        cJSON_AddNumberToObject(root, "batteryLevel", st.battery_level);
    }
    /* How stale each reading is. Absent means "never read". */
    if (st.battery_age_ms != UINT32_MAX) {
        cJSON_AddNumberToObject(root, "batteryAgeMs", st.battery_age_ms);
    }
    if (st.slot_age_ms != UINT32_MAX) {
        cJSON_AddNumberToObject(root, "slotAgeMs", st.slot_age_ms);
    }
    cJSON_AddStringToObject(root, "address", st.connected_addr);
    cJSON_AddStringToObject(root, "name", st.connected_name);
    cJSON_AddStringToObject(root, "error", st.last_error);

    if (st.saved_addr[0]) {
        cJSON *saved = cJSON_AddObjectToObject(root, "savedDevice");
        cJSON_AddStringToObject(saved, "address", st.saved_addr);
        cJSON_AddStringToObject(saved, "name", st.saved_name);
        cJSON_AddBoolToObject(saved, "autoConnect", st.auto_connect);
    }

    /* How long the link is held before the board hands the calendar back. */
    cJSON_AddNumberToObject(root, "idleTimeoutMs", st.idle_timeout_ms);

    cJSON *xfer = cJSON_AddObjectToObject(root, "transfer");
    cJSON_AddBoolToObject(xfer, "active", st.transfer_active);
    cJSON_AddNumberToObject(xfer, "slot", st.transfer_slot);
    cJSON_AddNumberToObject(xfer, "attempt", st.transfer_attempt);
    cJSON_AddNumberToObject(xfer, "sent", st.transfer_sent);
    cJSON_AddNumberToObject(xfer, "total", st.transfer_total);

    if (st.last_transfer_valid) {
        cJSON *last = cJSON_AddObjectToObject(root, "lastTransfer");
        cJSON_AddBoolToObject(last, "ok", st.last_transfer_ok);
        cJSON_AddNumberToObject(last, "rsp", st.last_transfer_rsp);
    }

    /*
     * The scan results ride along here so the UI can drive everything from a
     * single poll. Two polling loops on a phone exhausted the socket pool and
     * made accept() fail, which looked exactly like "cannot connect".
     */
    add_device_array(root, "devices");

    return send_json(req, root);
}

static esp_err_t get_devices(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_device_array(root, "devices");
    return send_json(req, root);
}

static esp_err_t post_scan(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_err(req, "400 Bad Request", "invalid json");
    }
    cJSON *dur = cJSON_GetObjectItem(body, "durationMs");
    uint32_t ms = cJSON_IsNumber(dur) ? (uint32_t)dur->valuedouble : 8000;
    cJSON_Delete(body);

    esp_err_t err = ulani_app_cmd_scan(ms);
    return err == ESP_OK ? send_ok(req)
                         : send_err(req, "503 Service Unavailable", "busy");
}

static esp_err_t post_connect(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_err(req, "400 Bad Request", "invalid json");
    }
    cJSON *addr = cJSON_GetObjectItem(body, "address");
    if (!cJSON_IsString(addr) || strlen(addr->valuestring) != 17) {
        cJSON_Delete(body);
        return send_err(req, "400 Bad Request", "address required");
    }
    esp_err_t err = ulani_app_cmd_connect(addr->valuestring);
    cJSON_Delete(body);

    return err == ESP_OK ? send_ok(req)
                         : send_err(req, "503 Service Unavailable", "busy");
}

static esp_err_t post_disconnect(httpd_req_t *req)
{
    return ulani_app_cmd_disconnect() == ESP_OK
               ? send_ok(req)
               : send_err(req, "503 Service Unavailable", "busy");
}

static esp_err_t post_forget_device(httpd_req_t *req)
{
    return ulani_app_cmd_forget_device() == ESP_OK
               ? send_ok(req)
               : send_err(req, "503 Service Unavailable", "busy");
}

static esp_err_t post_refresh(httpd_req_t *req)
{
    return ulani_app_cmd_refresh() == ESP_OK
               ? send_ok(req)
               : send_err(req, "503 Service Unavailable", "busy");
}

static esp_err_t post_settings(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_err(req, "400 Bad Request", "invalid json");
    }
    cJSON *idle = cJSON_GetObjectItem(body, "idleTimeoutMs");
    if (cJSON_IsNumber(idle) && idle->valuedouble >= 0) {
        ulani_app_set_idle_timeout_ms((uint32_t)idle->valuedouble);
    }
    cJSON_Delete(body);
    return send_ok(req);
}

static esp_err_t post_slot(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_err(req, "400 Bad Request", "invalid json");
    }
    cJSON *slot = cJSON_GetObjectItem(body, "slot");
    if (!cJSON_IsNumber(slot)) {
        cJSON_Delete(body);
        return send_err(req, "400 Bad Request", "slot required");
    }
    esp_err_t err = ulani_app_cmd_set_slot((uint8_t)slot->valuedouble);
    cJSON_Delete(body);

    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, "400 Bad Request", "slot must be 1..4");
    }
    return err == ESP_OK ? send_ok(req)
                         : send_err(req, "503 Service Unavailable", "busy");
}

static esp_err_t post_test(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_err(req, "400 Bad Request", "invalid json");
    }
    cJSON *slot = cJSON_GetObjectItem(body, "slot");
    cJSON *seed = cJSON_GetObjectItem(body, "seed");
    cJSON *act  = cJSON_GetObjectItem(body, "activate");
    uint8_t  s = cJSON_IsNumber(slot) ? (uint8_t)slot->valuedouble : 1;
    uint32_t d = cJSON_IsNumber(seed) ? (uint32_t)seed->valuedouble : 0;
    /* Uploading does not move the display unless asked. Writing over the page
     * currently on screen repaints anyway; see ulani_app_cmd_test_image. */
    bool a = cJSON_IsBool(act) ? cJSON_IsTrue(act) : false;
    cJSON_Delete(body);

    esp_err_t err = ulani_app_cmd_test_image(s, d, a);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, "400 Bad Request", "slot must be 1..4");
    }
    return err == ESP_OK ? send_ok(req)
                         : send_err(req, "503 Service Unavailable", "busy");
}

/* --------------------------------------------------------------- tesserae */

static void add_tesserae_client(cJSON *arr, uint8_t slot)
{
    tesserae_status_t st;
    tesserae_get_status(slot, &st);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "slot", slot);
    cJSON_AddStringToObject(o, "state", tesserae_state_str(st.state));
    cJSON_AddStringToObject(o, "serverUrl", st.server_url);
    cJSON_AddStringToObject(o, "deviceId", st.device_id);
    cJSON_AddBoolToObject(o, "registered", st.registered);
    cJSON_AddNumberToObject(o, "nextPollS", st.next_poll_s);
    cJSON_AddNumberToObject(o, "secondsUntilPoll", st.seconds_until_poll);
    cJSON_AddNumberToObject(o, "lastCheckEpoch", st.last_check_epoch);
    cJSON_AddNumberToObject(o, "lastFrameEpoch", st.last_frame_epoch);
    cJSON_AddNumberToObject(o, "lastSentEpoch", st.last_sent_epoch);
    cJSON_AddBoolToObject(o, "hasFrame", st.has_frame);
    cJSON_AddBoolToObject(o, "badge", ulani_app_get_slot_badge(slot));
    cJSON_AddStringToObject(o, "error", st.last_error);
    cJSON_AddItemToArray(arr, o);
}

static esp_err_t get_tesserae(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "clients");
    for (uint8_t slot = ULANI_SLOT_MIN; slot <= ULANI_SLOT_MAX; slot++) {
        add_tesserae_client(arr, slot);
    }
    return send_json(req, root);
}

static esp_err_t post_tesserae_connect(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_err(req, "400 Bad Request", "invalid json");
    }

    cJSON *slot  = cJSON_GetObjectItem(body, "slot");
    cJSON *url   = cJSON_GetObjectItem(body, "serverUrl");
    cJSON *code  = cJSON_GetObjectItem(body, "pairingCode");
    cJSON *devid = cJSON_GetObjectItem(body, "deviceId");
    cJSON *token = cJSON_GetObjectItem(body, "token");

    if (!cJSON_IsNumber(slot)) {
        cJSON_Delete(body);
        return send_err(req, "400 Bad Request", "slot required");
    }
    if (!cJSON_IsString(url) || url->valuestring[0] == 0) {
        cJSON_Delete(body);
        return send_err(req, "400 Bad Request", "serverUrl required");
    }

    esp_err_t err = tesserae_configure(
        (uint8_t)slot->valuedouble,
        url->valuestring,
        cJSON_IsString(code)  ? code->valuestring  : "",
        cJSON_IsString(devid) ? devid->valuestring : "",
        cJSON_IsString(token) ? token->valuestring : "");
    cJSON_Delete(body);

    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, "400 Bad Request",
                        "slot must be 1..4; a token also needs its device id, "
                        "and the fields have length limits");
    }
    return err == ESP_OK ? send_ok(req)
                         : send_err(req, "500 Internal Server Error", "could not save");
}

/* slot from a {"slot":N} body; 0 if missing or out of range. */
static uint8_t body_slot(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return 0;
    }
    cJSON *slot = cJSON_GetObjectItem(body, "slot");
    uint8_t n = cJSON_IsNumber(slot) ? (uint8_t)slot->valuedouble : 0;
    cJSON_Delete(body);
    return (n >= ULANI_SLOT_MIN && n <= ULANI_SLOT_MAX) ? n : 0;
}

static esp_err_t post_tesserae_forget(httpd_req_t *req)
{
    uint8_t slot = body_slot(req);
    if (slot == 0) {
        return send_err(req, "400 Bad Request", "slot must be 1..4");
    }
    return tesserae_forget(slot) == ESP_OK
               ? send_ok(req)
               : send_err(req, "500 Internal Server Error", "could not erase");
}

static esp_err_t post_tesserae_poll(httpd_req_t *req)
{
    uint8_t slot = body_slot(req);
    if (slot == 0) {
        return send_err(req, "400 Bad Request", "slot must be 1..4");
    }
    if (tesserae_poll_now(slot) == ESP_ERR_INVALID_STATE) {
        return send_err(req, "409 Conflict", "no server configured for that slot");
    }
    return send_ok(req);
}

/* Sends the image already stored for a slot to the calendar over BLE. */
static esp_err_t post_slot_badge(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_err(req, "400 Bad Request", "invalid json");
    }
    cJSON *slot = cJSON_GetObjectItem(body, "slot");
    cJSON *on   = cJSON_GetObjectItem(body, "on");
    uint8_t n = cJSON_IsNumber(slot) ? (uint8_t)slot->valuedouble : 0;
    if (n < ULANI_SLOT_MIN || n > ULANI_SLOT_MAX) {
        cJSON_Delete(body);
        return send_err(req, "400 Bad Request", "slot must be 1..4");
    }
    ulani_app_set_slot_badge(n, cJSON_IsTrue(on));
    cJSON_Delete(body);
    return send_ok(req);
}

static esp_err_t post_slot_send(httpd_req_t *req)
{
    uint8_t slot = body_slot(req);
    if (slot == 0) {
        return send_err(req, "400 Bad Request", "slot must be 1..4");
    }
    return ulani_app_cmd_send_slot(slot) == ESP_OK
               ? send_ok(req)
               : send_err(req, "503 Service Unavailable", "busy");
}

/*
 * Streams a stored slot back as raw packed nibbles, so the tab can render a
 * preview of what a page currently holds -- the only copy of a Tesserae frame
 * lives here, there is no original to fall back on.
 */
static esp_err_t get_slot_download(httpd_req_t *req)
{
    char query[32];
    uint8_t slot = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[8];
        if (httpd_query_key_value(query, "slot", value, sizeof(value)) == ESP_OK) {
            int n = atoi(value);
            if (n >= ULANI_SLOT_MIN && n <= ULANI_SLOT_MAX) {
                slot = (uint8_t)n;
            }
        }
    }
    if (slot == 0) {
        return send_err(req, "400 Bad Request", "slot must be 1..4");
    }

    ulani_store_reader_t reader;
    ulani_payload_src_t  src;
    if (ulani_store_payload_src(slot, &reader, &src) != ESP_OK) {
        return send_err(req, "404 Not Found", "nothing stored for that slot");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    static uint8_t buf[2048];
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < ULANI_PAYLOAD_BYTES; ) {
        size_t n = ULANI_PAYLOAD_BYTES - off;
        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        if (src.read(src.ctx, off, buf, n) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        off += n;
    }
    ulani_store_reader_close(&reader);
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* -------------------------------------------------------------------- wifi */

static esp_err_t get_wifi(httpd_req_t *req)
{
    net_sta_status_t sta;
    net_sta_get_status(&sta);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", net_sta_state_str(sta.state));
    cJSON_AddStringToObject(root, "ssid", sta.ssid);
    cJSON_AddStringToObject(root, "ip", sta.ip);
    cJSON_AddNumberToObject(root, "rssi", sta.rssi);
    cJSON_AddNumberToObject(root, "lastReason", sta.last_reason);
    cJSON_AddBoolToObject(root, "scanning", net_wifi_scan_busy());

    net_scan_result_t found[NET_SCAN_MAX];
    size_t n = net_wifi_scan_results(found, NET_SCAN_MAX);

    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", found[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", found[i].rssi);
        cJSON_AddNumberToObject(e, "channel", found[i].channel);
        cJSON_AddBoolToObject(e, "open", found[i].open);
        cJSON_AddItemToArray(arr, e);
    }

    return send_json(req, root);
}

static esp_err_t post_wifi_scan(httpd_req_t *req)
{
    esp_err_t err = net_wifi_scan_start();
    if (err == ESP_ERR_INVALID_STATE) {
        return send_err(req, "409 Conflict", "a scan is already running");
    }
    return err == ESP_OK ? send_ok(req)
                         : send_err(req, "500 Internal Server Error", "scan failed");
}

static esp_err_t post_wifi_connect(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_err(req, "400 Bad Request", "invalid json");
    }

    cJSON *ssid = cJSON_GetObjectItem(body, "ssid");
    cJSON *pass = cJSON_GetObjectItem(body, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == 0) {
        cJSON_Delete(body);
        return send_err(req, "400 Bad Request", "ssid required");
    }

    esp_err_t err = net_sta_connect(ssid->valuestring,
                                    cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(body);

    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, "400 Bad Request", "ssid or password too long");
    }
    return err == ESP_OK ? send_ok(req)
                         : send_err(req, "500 Internal Server Error", "could not join");
}

static esp_err_t post_wifi_forget(httpd_req_t *req)
{
    return net_sta_forget() == ESP_OK
               ? send_ok(req)
               : send_err(req, "500 Internal Server Error", "could not erase");
}

/* ---------------------------------------------------------- static files */

static esp_err_t get_index(httpd_req_t *req)
{
    return send_gz(req, "text/html", index_html_gz_start, index_html_gz_end);
}

static esp_err_t get_app_js(httpd_req_t *req)
{
    return send_gz(req, "application/javascript", app_js_gz_start, app_js_gz_end);
}

static esp_err_t get_app_css(httpd_req_t *req)
{
    return send_gz(req, "text/css", app_css_gz_start, app_css_gz_end);
}

/*
 * Captive-portal glue: phones probe a handful of vendor URLs and treat any
 * redirect as "there is a portal here". Sending them to / is enough.
 */
static esp_err_t redirect_to_root(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ init */

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 32;
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 6144;
    /*
     * Must stay below CONFIG_LWIP_MAX_SOCKETS with room for the listener and
     * the captive-portal DNS socket, or accept() fails with ENFILE and requests
     * are refused before any handler runs.
     */
    cfg.max_open_sockets = 10;
    cfg.backlog_conn     = 8;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                  .method = HTTP_GET,  .handler = get_index },
        { .uri = "/app.js",            .method = HTTP_GET,  .handler = get_app_js },
        { .uri = "/app.css",           .method = HTTP_GET,  .handler = get_app_css },
        { .uri = "/api/status",        .method = HTTP_GET,  .handler = get_status },
        { .uri = "/api/devices",       .method = HTTP_GET,  .handler = get_devices },
        { .uri = "/api/scan",          .method = HTTP_POST, .handler = post_scan },
        { .uri = "/api/connect",       .method = HTTP_POST, .handler = post_connect },
        { .uri = "/api/disconnect",    .method = HTTP_POST, .handler = post_disconnect },
        { .uri = "/api/refresh",       .method = HTTP_POST, .handler = post_refresh },
        { .uri = "/api/settings",      .method = HTTP_POST, .handler = post_settings },
        { .uri = "/api/forget-device", .method = HTTP_POST, .handler = post_forget_device },
        { .uri = "/api/slot",          .method = HTTP_POST, .handler = post_slot },
        { .uri = "/api/test-image",    .method = HTTP_POST, .handler = post_test },
        { .uri = "/api/tesserae",         .method = HTTP_GET,  .handler = get_tesserae },
        { .uri = "/api/tesserae/connect", .method = HTTP_POST, .handler = post_tesserae_connect },
        { .uri = "/api/tesserae/forget",  .method = HTTP_POST, .handler = post_tesserae_forget },
        { .uri = "/api/tesserae/poll",    .method = HTTP_POST, .handler = post_tesserae_poll },
        { .uri = "/api/slot/download", .method = HTTP_GET,  .handler = get_slot_download },
        { .uri = "/api/slot/send",     .method = HTTP_POST, .handler = post_slot_send },
        { .uri = "/api/slot/badge",    .method = HTTP_POST, .handler = post_slot_badge },
        { .uri = "/api/wifi",          .method = HTTP_GET,  .handler = get_wifi },
        { .uri = "/api/wifi/scan",     .method = HTTP_POST, .handler = post_wifi_scan },
        { .uri = "/api/wifi/connect",  .method = HTTP_POST, .handler = post_wifi_connect },
        { .uri = "/api/wifi/forget",   .method = HTTP_POST, .handler = post_wifi_forget },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_to_root);

    ESP_LOGI(TAG, "listening on port %d", cfg.server_port);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
