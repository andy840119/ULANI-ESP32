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
#include "status_led.h"
#include "tesserae.h"
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

static void on_tesserae_frame(uint8_t slot, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "tesserae stored a new frame for slot %u; sending it", slot);
    ulani_app_slots_changed();
    ulani_app_cmd_send_slot(slot);
}

/*
 * How old the calendar's battery reading may be and still be worth reporting.
 * The board hands the link back when it is idle, so a reading is normally some
 * minutes old by the time a heartbeat goes out; past this it says more about
 * when the calendar was last reachable than about the battery, and leaving the
 * field out lets Tesserae keep the last value it had.
 */
#define BATTERY_MAX_AGE_MS (30 * 60 * 1000)

/*
 * The battery Tesserae shows is the calendar's -- the ESP32 itself runs off
 * mains. ulani_app owns the BLE link, so it is the one that knows whether the
 * reading is current. What the calendar's byte actually means is still open;
 * see the battery section of docs/protocol.md.
 */
static bool calendar_battery(uint8_t *pct, void *user)
{
    (void)user;
    ulani_app_status_t st;
    ulani_app_get_status(&st);
    if (!st.battery_valid || st.battery_age_ms > BATTERY_MAX_AGE_MS) {
        return false;
    }
    *pct = st.battery_level;
    return true;
}

/* A stored-slot send finished; if it reached the panel, record when. */
static void on_slot_sent(uint8_t slot, bool ok)
{
    if (ok) {
        tesserae_note_sent(slot);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Cosmetic, so a failure here should not stop the board coming up. */
    if (status_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "status LED init failed; carrying on without it");
    }

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

    /*
     * A frame that has just landed is worth showing straight away; the queue
     * takes it from here so the tesserae task is not held up by a BLE
     * transfer that runs for half a minute.
     */
    tesserae_cfg_t tess = {
        .on_frame = on_tesserae_frame,
        .battery  = calendar_battery,
    };
    ESP_ERROR_CHECK(tesserae_start(&tess));

    /* Route send completions to the matching Tesserae client's "last sent". */
    ulani_app_set_slot_sent_cb(on_slot_sent);

    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "ready -- join \"%s\" and open http://192.168.4.1/", ssid);
}
