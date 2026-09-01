#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "ulani_app.h"

static const char *TAG = "ulani_app";

typedef enum {
    CMD_SCAN,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_SET_SLOT,
    CMD_REFRESH,
    CMD_TEST_IMAGE,
} cmd_id_t;

typedef struct {
    cmd_id_t id;
    uint32_t arg;
    char     addr[18];
    uint8_t  slot;
} cmd_t;

static struct {
    QueueHandle_t     queue;
    SemaphoreHandle_t lock;

    ulani_app_status_t status;

    ulani_device_t devices[ULANI_APP_MAX_DEVICES];
    size_t         device_n;

    int64_t last_op_us; /* for the idle timeout the device enforces */
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

static void refresh_device_info(void)
{
    uint16_t battery = 0;
    if (ulani_ble_get_battery(&battery) == ESP_OK) {
        status_lock();
        a.status.battery_rsp = battery;
        status_unlock();
    }

    uint8_t slot = 0;
    if (ulani_ble_get_active_slot(&slot) == ESP_OK) {
        status_lock();
        a.status.active_slot = slot;
        status_unlock();
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
        /* The JS asks for these right after ulaniready; keep the same order. */
        refresh_device_info();
        break;

    case CMD_DISCONNECT:
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
        ulani_payload_src_t src;
        ulani_testpattern_src(&src, &seed, cmd->arg);

        ESP_LOGI(TAG, "sending test pattern to slot %u (seed %u)",
                 cmd->slot, (unsigned)cmd->arg);
        err = ulani_ble_send_image(cmd->slot, &src);
        if (err != ESP_OK) {
            set_error("send image", err);
        } else {
            /* Make the result visible immediately, like helloworld.js does. */
            ulani_ble_set_active_slot(cmd->slot);
        }
        a.last_op_us = esp_timer_get_time();
        break;
    }
    }
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

        /* Idle tick: keep the link alive the way binaryAck() does. */
        if (!ulani_ble_is_connected()) {
            continue;
        }
        int64_t idle_us = esp_timer_get_time() - a.last_op_us;
        if (idle_us > (int64_t)ULANI_IDLE_TIMEOUT_MS * 1000) {
            ESP_LOGI(TAG, "idle for %lld s, releasing the device", idle_us / 1000000);
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

    ulani_ble_cfg_t cfg = { .event_cb = on_ble_event, .event_user = NULL };
    esp_err_t err = ulani_ble_init(&cfg);
    if (err != ESP_OK) {
        return err;
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

esp_err_t ulani_app_cmd_test_image(uint8_t slot, uint32_t seed)
{
    if (slot < ULANI_SLOT_MIN || slot > ULANI_SLOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    cmd_t cmd = { .id = CMD_TEST_IMAGE, .slot = slot,
                  .arg = seed ? seed : esp_random() };
    return post(&cmd);
}
