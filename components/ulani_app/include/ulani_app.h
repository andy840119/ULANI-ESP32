/*
 * Application layer: owns the single task that is allowed to talk to
 * ulani_ble, keeps a snapshot of state for the UI, and runs the keepalive.
 *
 * Everything here is safe to call from HTTP handlers: commands are queued and
 * return immediately, status reads are mutex-protected copies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ulani_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ULANI_APP_MAX_DEVICES 8

typedef struct {
    ulani_state_t state;
    bool          connected;
    uint8_t       active_slot;   /* 0 = unknown */
    uint16_t      battery_rsp;   /* raw op reply, 0 = unknown */
    bool          battery_valid;
    uint8_t       battery_level; /* low byte of the reply; see protocol.md */
    uint16_t      mtu;

    bool     transfer_active;
    uint8_t  transfer_slot;
    uint32_t transfer_sent;
    uint32_t transfer_total;

    bool     last_transfer_valid;
    bool     last_transfer_ok;
    uint16_t last_transfer_rsp;

    char last_error[64];
    char connected_addr[18];
    char connected_name[32];

    /* The device the board reconnects to on its own. Empty if none. */
    char saved_addr[18];
    char saved_name[32];
    bool auto_connect;
} ulani_app_status_t;

esp_err_t ulani_app_start(void);

void ulani_app_get_status(ulani_app_status_t *out);
size_t ulani_app_get_devices(ulani_device_t *out, size_t max);

/* All commands are queued; they return ESP_ERR_NO_MEM if the queue is full. */
esp_err_t ulani_app_cmd_scan(uint32_t duration_ms);
esp_err_t ulani_app_cmd_connect(const char *addr);
esp_err_t ulani_app_cmd_disconnect(void);
esp_err_t ulani_app_cmd_set_slot(uint8_t slot);
esp_err_t ulani_app_cmd_refresh(void);   /* battery + active slot */

/* Forgets the remembered device and stops reconnecting to it. */
esp_err_t ulani_app_cmd_forget_device(void);

/*
 * Phase 1 self-test: generates a 192000-byte pattern on the fly from seed and
 * streams it to the given slot. No filesystem involved.
 */
esp_err_t ulani_app_cmd_test_image(uint8_t slot, uint32_t seed);

/* Payload source producing the same generated pattern. */
void ulani_testpattern_src(ulani_payload_src_t *src, uint32_t *seed_storage, uint32_t seed);

#ifdef __cplusplus
}
#endif
