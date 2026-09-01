#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "net_provision.h"

static const char *TAG = "net_ap";

#define NVS_NAMESPACE "ulani_wifi"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

/*
 * Long enough that a router being slow to answer does not look like a wrong
 * password, short enough that recovery after a reboot is not annoying.
 */
#define RETRY_DELAY_US (10 * 1000 * 1000)

/*
 * Give up calling it "connecting" after this many consecutive failures so the
 * UI can say something useful. Retries continue regardless: the router may
 * simply be off, and the board should join on its own when it comes back.
 */
#define RETRY_BEFORE_FAILED 4

static struct {
    SemaphoreHandle_t lock;
    esp_netif_t      *sta_netif;

    bool configured;
    char ssid[NET_SSID_MAX];

    net_sta_state_t state;
    char            ip[16];
    int8_t          rssi;
    uint8_t         last_reason;
    int             retries;

    esp_timer_handle_t retry_timer;

    volatile bool     scanning;
    net_scan_result_t scan[NET_SCAN_MAX];
    size_t            scan_n;
} w;

static void lock(void)   { xSemaphoreTake(w.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(w.lock); }

const char *net_sta_state_str(net_sta_state_t s)
{
    switch (s) {
    case NET_STA_DISABLED:   return "disabled";
    case NET_STA_CONNECTING: return "connecting";
    case NET_STA_CONNECTED:  return "connected";
    case NET_STA_FAILED:     return "failed";
    }
    return "?";
}

/* ------------------------------------------------------------- credentials */

static esp_err_t creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) {
        size_t n = pass_len;
        if (nvs_get_str(h, NVS_KEY_PASS, pass, &n) != ESP_OK) {
            pass[0] = '\0'; /* open network */
        }
    }
    nvs_close(h);
    return err;
}

static esp_err_t creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void creds_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, NVS_KEY_SSID);
    nvs_erase_key(h, NVS_KEY_PASS);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ events */

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (w.configured) {
        ESP_LOGI(TAG, "retrying join to \"%s\"", w.ssid);
        esp_wifi_connect();
    }
}

static void schedule_retry(void)
{
    esp_timer_stop(w.retry_timer);
    esp_timer_start_once(w.retry_timer, RETRY_DELAY_US);
}

static void collect_scan_results(void)
{
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);

    uint16_t want = found < NET_SCAN_MAX ? found : NET_SCAN_MAX;
    wifi_ap_record_t *records = calloc(want ? want : 1, sizeof(wifi_ap_record_t));
    if (!records) {
        esp_wifi_clear_ap_list();
        w.scanning = false;
        return;
    }

    if (esp_wifi_scan_get_ap_records(&want, records) == ESP_OK) {
        lock();
        w.scan_n = 0;
        for (uint16_t i = 0; i < want && w.scan_n < NET_SCAN_MAX; i++) {
            if (records[i].ssid[0] == '\0') {
                continue; /* hidden network: nothing to show or tap */
            }
            /* The driver sorts by signal, so the first hit for an SSID wins. */
            bool dup = false;
            for (size_t j = 0; j < w.scan_n; j++) {
                if (strcmp(w.scan[j].ssid, (const char *)records[i].ssid) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            net_scan_result_t *r = &w.scan[w.scan_n++];
            strlcpy(r->ssid, (const char *)records[i].ssid, sizeof(r->ssid));
            r->rssi    = records[i].rssi;
            r->channel = records[i].primary;
            r->open    = (records[i].authmode == WIFI_AUTH_OPEN);
        }
        unlock();
        ESP_LOGI(TAG, "scan found %u network(s)", (unsigned)w.scan_n);
    }

    free(records);
    w.scanning = false;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "station " MACSTR " joined", MAC2STR(e->mac));
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "station " MACSTR " left", MAC2STR(e->mac));
        break;
    }

    case WIFI_EVENT_STA_START:
        if (w.configured) {
            esp_wifi_connect();
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *e = data;
        lock();
        w.last_reason = e->reason;
        w.ip[0]       = '\0';
        if (w.configured) {
            w.retries++;
            w.state = (w.retries >= RETRY_BEFORE_FAILED) ? NET_STA_FAILED
                                                         : NET_STA_CONNECTING;
        } else {
            w.state = NET_STA_DISABLED;
        }
        unlock();

        ESP_LOGW(TAG, "station disconnected, reason=%d", e->reason);
        if (w.configured) {
            schedule_retry();
        }
        break;
    }

    case WIFI_EVENT_SCAN_DONE:
        collect_scan_results();
        break;

    default:
        break;
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    ip_event_got_ip_t *e = data;

    lock();
    w.state   = NET_STA_CONNECTED;
    w.retries = 0;
    snprintf(w.ip, sizeof(w.ip), IPSTR, IP2STR(&e->ip_info.ip));
    unlock();

    ESP_LOGI(TAG, "joined \"%s\", reachable at http://%s/", w.ssid, w.ip);
}

/* -------------------------------------------------------------- public API */

esp_err_t net_provision_start_ap(const net_ap_cfg_t *cfg)
{
    w.lock = xSemaphoreCreateMutex();
    if (!w.lock) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name     = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &w.retry_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    w.sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, cfg->ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len       = strlen(cfg->ssid);
    wc.ap.channel        = cfg->channel ? cfg->channel : 1;
    wc.ap.max_connection = cfg->max_connections ? cfg->max_connections : 4;

    if (cfg->password && strlen(cfg->password) >= 8) {
        strlcpy((char *)wc.ap.password, cfg->password, sizeof(wc.ap.password));
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    /*
     * APSTA from the start, even with no credentials stored: the station
     * interface has to exist before it can scan, and the access point must
     * never go away.
     */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));

    /* We manage credentials ourselves; do not let the driver restore stale ones. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * BLE and WiFi share one antenna on the C3. Keeping the radio awake costs
     * power but stops the scheduler from starving image transfers.
     */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "AP up: ssid=%s auth=%s", cfg->ssid,
             wc.ap.authmode == WIFI_AUTH_OPEN ? "open" : "wpa2");
    return ESP_OK;
}

static esp_err_t apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t sc = { 0 };
    strlcpy((char *)sc.sta.ssid, ssid, sizeof(sc.sta.ssid));
    if (pass && pass[0]) {
        strlcpy((char *)sc.sta.password, pass, sizeof(sc.sta.password));
    }
    /* Let the driver pick whatever the router offers, open networks included. */
    sc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    return esp_wifi_set_config(WIFI_IF_STA, &sc);
}

esp_err_t net_sta_start(void)
{
    char ssid[NET_SSID_MAX] = { 0 };
    char pass[NET_PASS_MAX] = { 0 };

    if (creds_load(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK || ssid[0] == '\0') {
        ESP_LOGI(TAG, "no stored network; access point only");
        lock();
        w.state = NET_STA_DISABLED;
        unlock();
        return ESP_OK;
    }

    esp_err_t err = apply_sta_config(ssid, pass);
    if (err != ESP_OK) {
        return err;
    }

    lock();
    w.configured = true;
    w.retries    = 0;
    w.state      = NET_STA_CONNECTING;
    strlcpy(w.ssid, ssid, sizeof(w.ssid));
    unlock();

    ESP_LOGI(TAG, "joining stored network \"%s\"", ssid);
    return esp_wifi_connect();
}

esp_err_t net_sta_connect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) >= NET_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) >= NET_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = creds_save(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    err = apply_sta_config(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    lock();
    w.configured  = true;
    w.retries     = 0;
    w.last_reason = 0;
    w.state       = NET_STA_CONNECTING;
    w.ip[0]       = '\0';
    strlcpy(w.ssid, ssid, sizeof(w.ssid));
    unlock();

    esp_timer_stop(w.retry_timer);
    esp_wifi_disconnect();

    ESP_LOGI(TAG, "joining \"%s\"", ssid);
    return esp_wifi_connect();
}

esp_err_t net_sta_forget(void)
{
    creds_erase();

    lock();
    w.configured = false;
    w.state      = NET_STA_DISABLED;
    w.ssid[0]    = '\0';
    w.ip[0]      = '\0';
    w.retries    = 0;
    unlock();

    esp_timer_stop(w.retry_timer);
    esp_wifi_disconnect();

    ESP_LOGI(TAG, "stored network erased");
    return ESP_OK;
}

void net_sta_get_status(net_sta_status_t *out)
{
    lock();
    out->state       = w.state;
    out->rssi        = w.rssi;
    out->last_reason = w.last_reason;
    strlcpy(out->ssid, w.ssid, sizeof(out->ssid));
    strlcpy(out->ip, w.ip, sizeof(out->ip));
    unlock();

    if (out->state == NET_STA_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

/* -------------------------------------------------------------------- scan */

esp_err_t net_wifi_scan_start(void)
{
    if (w.scanning) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    w.scanning = true;
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        w.scanning = false;
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool net_wifi_scan_busy(void) { return w.scanning; }

size_t net_wifi_scan_results(net_scan_result_t *out, size_t max)
{
    lock();
    size_t n = w.scan_n < max ? w.scan_n : max;
    memcpy(out, w.scan, n * sizeof(net_scan_result_t));
    unlock();
    return n;
}
