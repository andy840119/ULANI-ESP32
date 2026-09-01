/*
 * Image transfer, mirroring BLEComm.prototype.startSendImage().
 *
 * The payload is 192000 bytes and never lives in RAM: we pull it from the
 * caller's source in 230-byte pieces, which is exactly one BLE write. Because
 * the header carries a CRC over the whole image, the source is read twice --
 * once to hash it, once to send it.
 */

#include <string.h>
#include <sys/time.h>

#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "ulani_ble_priv.h"

static const char *TAG = "ulani_xfer";

/* How often to report progress, in packets. Matches the JS (every 40). */
#define PROGRESS_EVERY 40

/* The device answers a finished image on the data channel; give it a while. */
#define RESULT_TIMEOUT_MS 30000

/* Comfortably inside the ten seconds of silence the panel tolerates. */
#define ACK_EVERY_US (8 * 1000 * 1000)

#define ULANI_RSP_SEND_ACCEPTED 0x0100
#define ULANI_RSP_IMAGE_OK      0x0200

static uint64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

static esp_err_t compute_crc(const ulani_payload_src_t *src, uint16_t *out)
{
    static uint8_t buf[512];
    ulani_crc16_ctx_t ctx;
    ulani_crc16_init(&ctx);

    for (size_t off = 0; off < ULANI_PAYLOAD_BYTES; ) {
        size_t n = ULANI_PAYLOAD_BYTES - off;
        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        esp_err_t err = src->read(src->ctx, off, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "payload read failed at %u: %s", (unsigned)off, esp_err_to_name(err));
            return err;
        }
        ulani_crc16_update(&ctx, buf, n);
        off += n;
    }

    *out = ulani_crc16_final(&ctx);
    return ESP_OK;
}

esp_err_t ulani_ble_send_image(uint8_t slot, const ulani_payload_src_t *src)
{
    if (slot < ULANI_SLOT_MIN || slot > ULANI_SLOT_MAX || !src || !src->read) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ulani_ble_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    ulani_transfer_abort_clear();

    uint16_t crc = 0;
    esp_err_t err = compute_crc(src, &crc);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "slot %u: payload crc=%04x", slot, crc);

    /* The JS always does this immediately before a transfer. */
    err = ulani_ble_check_customer_id(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "checkCustomerID failed: %s", esp_err_to_name(err));
    }

    uint8_t header[20];
    int hlen = ulani_build_send_header(slot, now_ms(), crc, header, sizeof(header));
    if (hlen < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ulani_data_result_arm();

    uint16_t rsp = 0;
    err = ulani_op_exec(header, (uint16_t)hlen, true, &rsp);
    if (err != ESP_OK) {
        return err;
    }
    if (rsp != ULANI_RSP_SEND_ACCEPTED) {
        ESP_LOGW(TAG, "device refused the transfer, rsp=%04x", rsp);
        ulani_event_t ev = { .type = ULANI_EV_TRANSFER_DONE,
                             .transfer_done = { .ok = false, .rsp = rsp, .slot = slot } };
        ulani_emit(&ev);
        return ESP_FAIL;
    }

    ulani_set_state(ULANI_STATE_TRANSFERRING);

    static uint8_t chunk[ULANI_CHUNK_BYTES];
    size_t   off   = 0;
    uint32_t index = 0;
    int64_t  last_ack_us = esp_timer_get_time();

    while (off < ULANI_PAYLOAD_BYTES) {
        /* An early result means the device gave up (or finished) on its own. */
        if (ulani_data_result_ready() || ulani_transfer_abort_requested()) {
            break;
        }

        size_t n = ULANI_PAYLOAD_BYTES - off;
        if (n > ULANI_CHUNK_BYTES) {
            n = ULANI_CHUNK_BYTES;
        }

        err = src->read(src->ctx, off, chunk, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "payload read failed at %u", (unsigned)off);
            goto aborted;
        }

        err = ulani_gatt_write_data(chunk, (uint16_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "data write failed at packet %u", (unsigned)index);
            goto aborted;
        }

        off += n;
        index++;

        vTaskDelay(pdMS_TO_TICKS(CONFIG_ULANI_BLE_CHUNK_GAP_MS));

        /*
         * The panel drops a link that goes quiet on the op channel for more
         * than ten seconds, and an image takes considerably longer than that.
         * The reference implementation runs its keepalive on a timer that keeps
         * ticking through a transfer; this task cannot, so interleave it here.
         */
        if (esp_timer_get_time() - last_ack_us > ACK_EVERY_US) {
            last_ack_us = esp_timer_get_time();
            if (ulani_ble_ack() != ESP_OK) {
                ESP_LOGW(TAG, "keepalive failed mid-transfer");
            }
        }

        if (index % PROGRESS_EVERY == 0 || off >= ULANI_PAYLOAD_BYTES) {
            ulani_event_t ev = { .type = ULANI_EV_TRANSFER_PROGRESS,
                                 .progress = { .sent  = (uint32_t)off,
                                               .total = (uint32_t)ULANI_PAYLOAD_BYTES,
                                               .slot  = slot } };
            ulani_emit(&ev);
        }
    }

    if (ulani_transfer_abort_requested()) {
        goto aborted;
    }

    rsp = 0;
    err = ulani_data_result_wait(RESULT_TIMEOUT_MS, &rsp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no image result after %u packets", (unsigned)index);
        goto aborted;
    }

    bool ok = (rsp == ULANI_RSP_IMAGE_OK);
    ESP_LOGI(TAG, "slot %u: transfer %s (rsp=%04x, %u packets)",
             slot, ok ? "ok" : "failed", rsp, (unsigned)index);

    if (ulani_ble_is_connected()) {
        ulani_set_state(ULANI_STATE_READY);
    }
    {
        ulani_event_t ev = { .type = ULANI_EV_TRANSFER_DONE,
                             .transfer_done = { .ok = ok, .rsp = rsp, .slot = slot } };
        ulani_emit(&ev);
    }
    return ok ? ESP_OK : ESP_FAIL;

aborted:
    ulani_data_result_wait(0, NULL); /* disarm */
    if (ulani_ble_is_connected()) {
        ulani_set_state(ULANI_STATE_READY);
    }
    {
        ulani_event_t ev = { .type = ULANI_EV_TRANSFER_DONE,
                             .transfer_done = { .ok = false, .rsp = 0, .slot = slot } };
        ulani_emit(&ev);
    }
    return err != ESP_OK ? err : ESP_FAIL;
}
