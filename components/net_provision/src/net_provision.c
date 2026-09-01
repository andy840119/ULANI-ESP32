#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "net_provision.h"

static const char *TAG = "net_ap";

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "station " MACSTR " joined", MAC2STR(e->mac));
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "station " MACSTR " left", MAC2STR(e->mac));
    }
}

esp_err_t net_provision_start_ap(const net_ap_cfg_t *cfg)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));

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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * BLE and WiFi share one antenna on the C3. Dropping the AP to the minimum
     * usable power keeps the radio scheduler from starving image transfers.
     */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "AP up: ssid=%s auth=%s", cfg->ssid,
             wc.ap.authmode == WIFI_AUTH_OPEN ? "open" : "wpa2");
    return ESP_OK;
}
