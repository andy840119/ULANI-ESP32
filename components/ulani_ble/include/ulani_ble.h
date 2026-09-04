/*
 * ULANI BLE central -- protocol layer.
 *
 * This component knows about opcodes, GATT handles and packet pacing. It knows
 * nothing about HTTP, filesystems or image formats: callers hand it a payload
 * source and it streams the bytes out. Keep it that way.
 *
 * All ulani_ble_* calls below are blocking and must be issued from a single
 * caller task (ulani_app owns one). They are not safe to call from an HTTP
 * handler directly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ulani_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ULANI_STATE_OFF = 0,
    ULANI_STATE_IDLE,
    ULANI_STATE_SCANNING,
    ULANI_STATE_CONNECTING,
    ULANI_STATE_DISCOVERING,
    ULANI_STATE_READY,
    ULANI_STATE_TRANSFERRING,
} ulani_state_t;

const char *ulani_state_str(ulani_state_t s);

typedef struct {
    char    name[32];
    char    addr[18]; /* "aa:bb:cc:dd:ee:ff" */
    uint8_t addr_type;
    int8_t  rssi;
} ulani_device_t;

typedef enum {
    ULANI_EV_STATE_CHANGED,
    ULANI_EV_DEVICE_FOUND,
    ULANI_EV_SCAN_DONE,
    ULANI_EV_CONNECTED,
    ULANI_EV_DISCONNECTED,
    ULANI_EV_TRANSFER_PROGRESS,
    ULANI_EV_TRANSFER_DONE,
    ULANI_EV_SLOT_CHANGED,
} ulani_event_type_t;

typedef struct {
    ulani_event_type_t type;
    union {
        struct { ulani_state_t state; }                    state_changed;
        struct { ulani_device_t dev; }                     device_found;
        struct { int reason; }                             disconnected;
        struct { uint32_t sent; uint32_t total; uint8_t slot; uint8_t attempt; } progress;
        struct { bool ok; uint16_t rsp; uint8_t slot; }    transfer_done;
        struct { uint8_t slot; }                           slot_changed;
    };
} ulani_event_t;

typedef void (*ulani_event_cb_t)(const ulani_event_t *ev, void *user);

/*
 * Reads exactly len bytes of packed payload starting at offset.
 * The transfer makes two passes over the source (one to CRC it, one to send
 * it), so reads must be repeatable and side-effect free.
 * Returns ESP_OK, or an error to abort the transfer.
 */
typedef esp_err_t (*ulani_payload_read_fn)(void *ctx, size_t offset, uint8_t *out, size_t len);

typedef struct {
    ulani_payload_read_fn read;
    void                 *ctx;
} ulani_payload_src_t;

typedef struct {
    ulani_event_cb_t event_cb;
    void            *event_user;
} ulani_ble_cfg_t;

esp_err_t     ulani_ble_init(const ulani_ble_cfg_t *cfg);
ulani_state_t ulani_ble_state(void);
bool          ulani_ble_is_connected(void);

/* Scan for advertisers whose name starts with ULANI_NAME_PREFIX. */
esp_err_t ulani_ble_scan_start(uint32_t duration_ms);
esp_err_t ulani_ble_scan_stop(void);

/*
 * Adds a device to the table the connect path consults for address types, so a
 * known device can be reached without scanning for it first. Connecting to an
 * address that was never seen falls back to assuming a public address.
 */
esp_err_t ulani_ble_seed_device(const ulani_device_t *dev);

/* addr is "aa:bb:cc:dd:ee:ff". Blocks until the GATT handles are resolved. */
esp_err_t ulani_ble_connect(const char *addr, uint32_t timeout_ms);
esp_err_t ulani_ble_disconnect(void);

/* Op-channel commands. rsp receives the first two bytes of the reply frame. */
esp_err_t ulani_ble_check_customer_id(uint16_t *rsp);
esp_err_t ulani_ble_get_battery(uint16_t *rsp);
esp_err_t ulani_ble_ack(void);              /* keepalive; opcode 0x06, no wait */
esp_err_t ulani_ble_ask_disconnect(void);
esp_err_t ulani_ble_get_active_slot(uint8_t *slot);
esp_err_t ulani_ble_set_active_slot(uint8_t slot);

/*
 * Streams one 192000-byte payload into the given slot (1..4).
 * Blocks for the whole transfer -- roughly 30-60 s.
 */
esp_err_t ulani_ble_send_image(uint8_t slot, const ulani_payload_src_t *src);

/* Aborts an in-flight send_image from another task. */
void ulani_ble_abort_transfer(void);

#ifdef __cplusplus
}
#endif
