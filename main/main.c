/*
 * ULANI-ESP32 entry point.
 *
 * Bring-up order matters: NVS first (NimBLE stores bonds there), then the
 * SoftAP so the user has something to join, then BLE, then the web UI.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "net_provision.h"
#include "ulani_app.h"
#include "ulani_store.h"
#include "web_server.h"

static const char *TAG = "main";

/* Leave the AP open: this is a local setup portal, not a network. */
#define AP_PASSWORD NULL

static void build_ssid(char *out, size_t len)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, len, "ULANI-Setup-%02X%02X", mac[4], mac[5]);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    char ssid[32];
    build_ssid(ssid, sizeof(ssid));

    net_ap_cfg_t ap = {
        .ssid            = ssid,
        .password        = AP_PASSWORD,
        .channel         = 1,
        .max_connections = 4,
    };
    ESP_ERROR_CHECK(net_provision_start_ap(&ap));
    ESP_ERROR_CHECK(net_dns_hijack_start());

    /* Joins the user's own network if one has been saved. The AP stays up. */
    ESP_ERROR_CHECK(net_sta_start());

    /* Before the app layer, which reads the slot contents on startup. */
    ESP_ERROR_CHECK(ulani_store_init());

    ESP_ERROR_CHECK(ulani_app_start());
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "ready -- join \"%s\" and open http://192.168.4.1/", ssid);
}
