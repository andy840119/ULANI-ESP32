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

/*
 * The panel repaints its e-paper after an image lands and ignores a new send
 * request for the best part of a minute while it does -- op 0x01 simply never
 * gets an answer. Pressing the button a second time is what makes it work, so
 * do that here instead of handing the user an error. Four tries at a ten-second
 * op timeout plus these gaps covers roughly 50 s of a busy panel.
 */
#define SEND_HEADER_ATTEMPTS 4
#define SEND_RETRY_DELAY_MS  3000

#define ULANI_RSP_SEND_ACCEPTED 0x0100
#define ULANI_RSP_IMAGE_OK      0x0200

static uint64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

/*
 * Read ahead in blocks rather than a packet at a time. A slot lives in SPIFFS,
 * which reads at roughly 17 KB/s here -- about 13 ms for one 230-byte packet,
 * the same order as the gap between packets. Doing that inside the send loop
 * puts a flash operation between every write, which is both slow and jittery
 * at exactly the point where the panel is timing us.
 */
#define READ_AHEAD_PACKETS 20
#define READ_AHEAD_BYTES   (READ_AHEAD_PACKETS * ULANI_CHUNK_BYTES)

static esp_err_t compute_crc(const ulani_payload_src_t *src, uint16_t *out)
{
    static uint8_t buf[READ_AHEAD_BYTES];
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
    for (int attempt = 1;; attempt++) {
        err = ulani_op_exec(header, (uint16_t)hlen, true, &rsp);

        /* Anything but silence or a refusal is a real error; don't paper over it. */
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            return err;
        }
        if (err == ESP_OK && rsp == ULANI_RSP_SEND_ACCEPTED) {
            break;
        }

        if (attempt >= SEND_HEADER_ATTEMPTS || ulani_transfer_abort_requested() ||
            !ulani_ble_is_connected()) {
            if (err == ESP_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "device never answered the send request (%d attempts)",
                         attempt);
            } else {
                ESP_LOGW(TAG, "device refused the transfer, rsp=%04x", rsp);
            }
            ulani_data_result_wait(0, NULL); /* disarm */
            ulani_event_t ev = { .type = ULANI_EV_TRANSFER_DONE,
                                 .transfer_done = { .ok = false, .rsp = rsp, .slot = slot } };
            ulani_emit(&ev);
            return err != ESP_OK ? err : ESP_FAIL;
        }

        ESP_LOGW(TAG, "send request attempt %d got %s (rsp=%04x); the panel is "
                      "probably still repainting, retrying",
                 attempt, esp_err_to_name(err), rsp);
        vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_DELAY_MS));
    }

    ulani_set_state(ULANI_STATE_TRANSFERRING);

    static uint8_t block[READ_AHEAD_BYTES];
    size_t   block_base = 0;   /* payload offset the block starts at */
    size_t   block_len  = 0;
    size_t   off        = 0;
    uint32_t index      = 0;
    uint32_t stalls     = 0;   /* times the controller had no buffer */
    int64_t  last_ack_us = esp_timer_get_time();

    while (off < ULANI_PAYLOAD_BYTES) {
        /* An early result means the device gave up (or finished) on its own. */
        if (ulani_data_result_ready() || ulani_transfer_abort_requested()) {
            break;
        }

        if (off >= block_base + block_len) {
            block_base = off;
            block_len  = ULANI_PAYLOAD_BYTES - off;
            if (block_len > sizeof(block)) {
                block_len = sizeof(block);
            }
            err = src->read(src->ctx, block_base, block, block_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "payload read failed at %u", (unsigned)block_base);
                goto aborted;
            }
        }

        size_t n = ULANI_PAYLOAD_BYTES - off;
        if (n > ULANI_CHUNK_BYTES) {
            n = ULANI_CHUNK_BYTES;
        }

        uint32_t before = ulani_gatt_write_stalls();
        err = ulani_gatt_write_data(block + (off - block_base), (uint16_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "data write failed at packet %u", (unsigned)index);
            goto aborted;
        }
        stalls += ulani_gatt_write_stalls() - before;

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
    ESP_LOGI(TAG, "slot %u: transfer %s (rsp=%04x, %u packets, %u bytes, "
                  "%u buffer stalls)",
             slot, ok ? "ok" : "failed", rsp, (unsigned)index, (unsigned)off,
             (unsigned)stalls);

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
