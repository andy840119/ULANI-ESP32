#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "ulani_app.h"

static const char *TAG = "ulani_app";

#define NVS_NAMESPACE "ulani_dev"
#define NVS_KEY_ADDR  "addr"
#define NVS_KEY_NAME  "name"
#define NVS_KEY_TYPE  "type"

/*
 * How long to wait before trying the remembered device again. Long enough that
 * a calendar which is switched off does not keep the worker busy, short enough
 * that switching it on is noticed without touching the UI.
 */
#define AUTO_CONNECT_RETRY_US (30 * 1000 * 1000)

typedef enum {
    CMD_SCAN,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_SET_SLOT,
    CMD_REFRESH,
    CMD_TEST_IMAGE,
    CMD_FORGET_DEVICE,
} cmd_id_t;

typedef struct {
    cmd_id_t id;
    uint32_t arg;
    char     addr[18];
    uint8_t  slot;
    bool     activate;
} cmd_t;

static struct {
    QueueHandle_t     queue;
    SemaphoreHandle_t lock;

    ulani_app_status_t status;

    ulani_device_t devices[ULANI_APP_MAX_DEVICES];
    size_t         device_n;

    int64_t last_op_us; /* for the idle timeout the device enforces */

    ulani_device_t saved;        /* remembered device, addr empty if none */
    bool           auto_connect; /* armed: keep trying to reach `saved` */
    int64_t        next_auto_us;
} a;

static void status_lock(void)   { xSemaphoreTake(a.lock, portMAX_DELAY); }
static void status_unlock(void) { xSemaphoreGive(a.lock); }

static void set_error(const char *what, esp_err_t err)
{
    status_lock();
    snprintf(a.status.last_error, sizeof(a.status.last_error), "%s: %s",
             what, esp_err_to_name(err));
    status_unlock();
    ESP_LOGW(TAG, "%s failed: %s", what, esp_err_to_name(err));
}

static void clear_error(void)
{
    status_lock();
    a.status.last_error[0] = 0;
    status_unlock();
}

/* --------------------------------------------------------- saved device */

static void saved_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    size_t n = sizeof(a.saved.addr);
    if (nvs_get_str(h, NVS_KEY_ADDR, a.saved.addr, &n) == ESP_OK) {
        n = sizeof(a.saved.name);
        if (nvs_get_str(h, NVS_KEY_NAME, a.saved.name, &n) != ESP_OK) {
            a.saved.name[0] = 0;
        }
        uint8_t type = 0;
        if (nvs_get_u8(h, NVS_KEY_TYPE, &type) == ESP_OK) {
            a.saved.addr_type = type;
        }
        ESP_LOGI(TAG, "remembered device %s (%s)", a.saved.addr, a.saved.name);
    }
    nvs_close(h);
}

static void saved_store(const ulani_device_t *dev)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, NVS_KEY_ADDR, dev->addr);
    nvs_set_str(h, NVS_KEY_NAME, dev->name);
    nvs_set_u8(h, NVS_KEY_TYPE, dev->addr_type);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "remembering %s (%s)", dev->addr, dev->name);
}

static void saved_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_ADDR);
        nvs_erase_key(h, NVS_KEY_NAME);
        nvs_erase_key(h, NVS_KEY_TYPE);
        nvs_commit(h);
        nvs_close(h);
    }
    memset(&a.saved, 0, sizeof(a.saved));
    ESP_LOGI(TAG, "forgot the remembered device");
}

static void publish_saved(void)
{
    status_lock();
    strlcpy(a.status.saved_addr, a.saved.addr, sizeof(a.status.saved_addr));
    strlcpy(a.status.saved_name, a.saved.name, sizeof(a.status.saved_name));
    a.status.auto_connect = a.auto_connect;
    status_unlock();
}

/* ------------------------------------------------------------- BLE events */

static void on_ble_event(const ulani_event_t *ev, void *user)
{
    (void)user;
    status_lock();

    switch (ev->type) {
    case ULANI_EV_STATE_CHANGED:
        a.status.state           = ev->state_changed.state;
        a.status.transfer_active = (ev->state_changed.state == ULANI_STATE_TRANSFERRING);
        break;

    case ULANI_EV_DEVICE_FOUND:
        if (a.device_n < ULANI_APP_MAX_DEVICES) {
            a.devices[a.device_n++] = ev->device_found.dev;
        }
        break;

    case ULANI_EV_CONNECTED:
        a.status.connected = true;
        for (size_t i = 0; i < a.device_n; i++) {
            if (strcmp(a.devices[i].addr, a.status.connected_addr) == 0) {
                strlcpy(a.status.connected_name, a.devices[i].name,
                        sizeof(a.status.connected_name));
                break;
            }
        }
        break;

    case ULANI_EV_DISCONNECTED:
        a.status.connected       = false;
        a.status.active_slot     = 0;
        a.status.battery_rsp     = 0;
        a.status.battery_level   = 0;
        a.status.battery_valid   = false;
        a.status.transfer_active = false;
        a.status.connected_name[0] = 0;
        break;

    case ULANI_EV_TRANSFER_PROGRESS:
        a.status.transfer_active = true;
        a.status.transfer_slot   = ev->progress.slot;
        a.status.transfer_sent   = ev->progress.sent;
        a.status.transfer_total  = ev->progress.total;
        break;

    case ULANI_EV_TRANSFER_DONE:
        a.status.transfer_active     = false;
        a.status.last_transfer_valid = true;
        a.status.last_transfer_ok    = ev->transfer_done.ok;
        a.status.last_transfer_rsp   = ev->transfer_done.rsp;
        break;

    case ULANI_EV_SLOT_CHANGED:
        a.status.active_slot = ev->slot_changed.slot;
        break;

    case ULANI_EV_SCAN_DONE:
        break;
    }

    status_unlock();
}

/* ----------------------------------------------------------------- worker */

/*
 * Reads battery and active slot. Neither is essential, so a failure here does
 * not tear the connection down -- but it does get reported: silently leaving
 * the fields blank makes a dead notify path look like an empty reading.
 */
static void refresh_device_info(void)
{
    uint16_t  battery = 0;
    esp_err_t bat_err = ulani_ble_get_battery(&battery);
    if (bat_err == ESP_OK) {
        /* The reply is <opcode><level>; the level is the low byte. */
        uint8_t level = (uint8_t)(battery & 0xff);
        ESP_LOGI(TAG, "battery reply %04x -> level %u", battery, level);
        status_lock();
        a.status.battery_rsp   = battery;
        a.status.battery_level = level;
        a.status.battery_valid = true;
        status_unlock();
    } else {
        set_error("read battery", bat_err);
    }

    uint8_t   slot     = 0;
    esp_err_t slot_err = ulani_ble_get_active_slot(&slot);
    if (slot_err == ESP_OK) {
        ESP_LOGI(TAG, "active slot %u", slot);
        status_lock();
        a.status.active_slot = slot;
        status_unlock();
    } else {
        set_error("read active slot", slot_err);
    }

    a.last_op_us = esp_timer_get_time();
}

static void handle_cmd(const cmd_t *cmd)
{
    esp_err_t err;

    switch (cmd->id) {
    case CMD_SCAN:
        status_lock();
        a.device_n = 0;
        status_unlock();
        clear_error();
        err = ulani_ble_scan_start(cmd->arg);
        if (err != ESP_OK) {
            set_error("scan", err);
        }
        break;

    case CMD_CONNECT:
        clear_error();
        status_lock();
        strlcpy(a.status.connected_addr, cmd->addr, sizeof(a.status.connected_addr));
        status_unlock();
        err = ulani_ble_connect(cmd->addr, 15000);
        if (err != ESP_OK) {
            set_error("connect", err);
            break;
        }

        /*
         * Only remember a device we have actually reached, and arm the
         * reconnect at the same time: a link that drops later should come back
         * without the user going through the UI again.
         */
        {
            ulani_device_t dev = { 0 };
            strlcpy(dev.addr, cmd->addr, sizeof(dev.addr));
            for (size_t i = 0; i < a.device_n; i++) {
                if (strcmp(a.devices[i].addr, cmd->addr) == 0) {
                    dev = a.devices[i];
                    break;
                }
            }
            a.saved        = dev;
            a.auto_connect = true;
            saved_store(&dev);
            publish_saved();
        }

        /* The JS asks for these right after ulaniready; keep the same order. */
        refresh_device_info();
        break;

    case CMD_DISCONNECT:
        /* Deliberate: stop reconnecting until the user asks for it again. */
        a.auto_connect = false;
        publish_saved();
        ulani_ble_abort_transfer();
        ulani_ble_disconnect();
        break;

    case CMD_FORGET_DEVICE:
        a.auto_connect = false;
        saved_erase();
        publish_saved();
        ulani_ble_abort_transfer();
        ulani_ble_disconnect();
        break;

    case CMD_SET_SLOT:
        err = ulani_ble_set_active_slot(cmd->slot);
        if (err != ESP_OK) {
            set_error("set slot", err);
        }
        a.last_op_us = esp_timer_get_time();
        break;

    case CMD_REFRESH:
        if (ulani_ble_is_connected()) {
            refresh_device_info();
        }
        break;

    case CMD_TEST_IMAGE: {
        clear_error();
        static uint32_t seed;
        ulani_payload_src_t pattern;
        ulani_testpattern_src(&pattern, &seed, cmd->arg);

        /* Stamp the slot number on it so a photo of the panel is enough to
         * tell which page you are looking at. */
        static ulani_page_badge_t badge;
        ulani_payload_src_t       src;
        if (ulani_page_badge_src(&src, &badge, &pattern, cmd->slot) != ESP_OK) {
            src = pattern;
        }

        ESP_LOGI(TAG, "sending test pattern to slot %u (seed %u, %s)",
                 cmd->slot, (unsigned)cmd->arg,
                 cmd->activate ? "activating" : "leaving the display alone");
        err = ulani_ble_send_image(cmd->slot, &src);
        if (err != ESP_OK) {
            set_error("send image", err);
            a.last_op_us = esp_timer_get_time();
            break;
        }

        /*
         * Uploading and switching are separate operations on the wire, so an
         * upload leaves the display where it is. The one case that has to
         * ignore that is writing over the page currently on screen: the panel
         * would go on showing an image that slot no longer holds, so re-select
         * it to force a repaint. Ask the panel which page it is on rather than
         * trusting our snapshot, which goes stale the moment someone presses
         * the button on the device itself.
         */
        bool refresh = cmd->activate;
        if (!refresh) {
            uint8_t active = 0;
            if (ulani_ble_get_active_slot(&active) == ESP_OK) {
                refresh = (active == cmd->slot);
                if (refresh) {
                    ESP_LOGI(TAG, "slot %u is on screen; repainting it",
                             cmd->slot);
                }
            }
        }
        if (refresh) {
            ulani_ble_set_active_slot(cmd->slot);
        }
        a.last_op_us = esp_timer_get_time();
        break;
    }
    }
}

/*
 * Reaches for the remembered device while the radio is otherwise idle. The
 * connect call blocks for up to fifteen seconds when the calendar is not
 * around, so back off well past the ten-second idle tick rather than tying the
 * worker up and making the UI feel unresponsive.
 */
static void try_auto_connect(void)
{
    if (!a.auto_connect || a.saved.addr[0] == 0) {
        return;
    }
    if (ulani_ble_state() != ULANI_STATE_IDLE) {
        return; /* still scanning, connecting, or the host is not up yet */
    }
    if (esp_timer_get_time() < a.next_auto_us) {
        return;
    }
    a.next_auto_us = esp_timer_get_time() + AUTO_CONNECT_RETRY_US;

    ESP_LOGI(TAG, "reconnecting to %s", a.saved.addr);
    status_lock();
    strlcpy(a.status.connected_addr, a.saved.addr, sizeof(a.status.connected_addr));
    status_unlock();

    esp_err_t err = ulani_ble_connect(a.saved.addr, 15000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reconnect failed: %s (retrying)", esp_err_to_name(err));
        return;
    }
    refresh_device_info();
}

static void worker_task(void *param)
{
    (void)param;

    for (;;) {
        cmd_t cmd;
        if (xQueueReceive(a.queue, &cmd, pdMS_TO_TICKS(ULANI_ACK_INTERVAL_MS)) == pdTRUE) {
            handle_cmd(&cmd);
            continue;
        }

        if (!ulani_ble_is_connected()) {
            try_auto_connect();
            continue;
        }

        /* Idle tick: keep the link alive the way binaryAck() does. */
        int64_t idle_us = esp_timer_get_time() - a.last_op_us;
        if (idle_us > (int64_t)ULANI_IDLE_TIMEOUT_MS * 1000) {
            ESP_LOGI(TAG, "idle for %lld s, releasing the device", idle_us / 1000000);
            /*
             * Letting go on purpose so the official app can have a turn. Do not
             * immediately grab it back -- that would make the release useless.
             */
            a.auto_connect = false;
            publish_saved();
            ulani_ble_ask_disconnect();
            ulani_ble_disconnect();
        } else {
            ulani_ble_ack();
        }
    }
}

/* ------------------------------------------------------------ public API */

esp_err_t ulani_app_start(void)
{
    memset(&a, 0, sizeof(a));

    a.queue = xQueueCreate(8, sizeof(cmd_t));
    a.lock  = xSemaphoreCreateMutex();
    if (!a.queue || !a.lock) {
        return ESP_ERR_NO_MEM;
    }
    a.last_op_us = esp_timer_get_time();

    saved_load();
    a.auto_connect = (a.saved.addr[0] != 0);
    publish_saved();

    ulani_ble_cfg_t cfg = { .event_cb = on_ble_event, .event_user = NULL };
    esp_err_t err = ulani_ble_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * After init, not before: ulani_ble_init() clears its own state, which
     * would take the seeded entry with it. This is what lets the reconnect use
     * the right address type without scanning first.
     */
    if (a.auto_connect) {
        ulani_ble_seed_device(&a.saved);
    }

    if (xTaskCreate(worker_task, "ulani_app", 5120, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ulani_app_get_status(ulani_app_status_t *out)
{
    status_lock();
    *out = a.status;
    status_unlock();
}

size_t ulani_app_get_devices(ulani_device_t *out, size_t max)
{
    status_lock();
    size_t n = a.device_n < max ? a.device_n : max;
    memcpy(out, a.devices, n * sizeof(ulani_device_t));
    status_unlock();
    return n;
}

static esp_err_t post(const cmd_t *cmd)
{
    return xQueueSend(a.queue, cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ulani_app_cmd_scan(uint32_t duration_ms)
{
    cmd_t cmd = { .id = CMD_SCAN, .arg = duration_ms };
    return post(&cmd);
}

esp_err_t ulani_app_cmd_connect(const char *addr)
{
    cmd_t cmd = { .id = CMD_CONNECT };
    strlcpy(cmd.addr, addr, sizeof(cmd.addr));
    return post(&cmd);
}

esp_err_t ulani_app_cmd_disconnect(void)
{
    cmd_t cmd = { .id = CMD_DISCONNECT };
    return post(&cmd);
}

esp_err_t ulani_app_cmd_set_slot(uint8_t slot)
{
    if (slot < ULANI_SLOT_MIN || slot > ULANI_SLOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    cmd_t cmd = { .id = CMD_SET_SLOT, .slot = slot };
    return post(&cmd);
}

esp_err_t ulani_app_cmd_refresh(void)
{
    cmd_t cmd = { .id = CMD_REFRESH };
    return post(&cmd);
}

esp_err_t ulani_app_cmd_forget_device(void)
{
    cmd_t cmd = { .id = CMD_FORGET_DEVICE };
    return post(&cmd);
}

esp_err_t ulani_app_cmd_test_image(uint8_t slot, uint32_t seed, bool activate)
{
    if (slot < ULANI_SLOT_MIN || slot > ULANI_SLOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    cmd_t cmd = { .id = CMD_TEST_IMAGE, .slot = slot,
                  .arg = seed ? seed : esp_random(), .activate = activate };
    return post(&cmd);
}
