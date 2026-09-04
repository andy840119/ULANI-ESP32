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
#include "ulani_store.h"

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

    /*
     * How long ago each reading actually came off the calendar. The keepalive
     * alternates between the two, so they age independently, and a field that
     * has stopped moving is the visible symptom of a link that is up but no
     * longer answering. UINT32_MAX until the first successful read.
     */
    uint32_t      battery_age_ms;
    uint32_t      slot_age_ms;
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

    /* What is stored for each slot, indexed 0..3 for slots 1..4. */
    ulani_slot_info_t slots[ULANI_SLOT_MAX];
} ulani_app_status_t;

esp_err_t ulani_app_start(void);

/*
 * Notified after a stored-slot send finishes, with whether it reached the
 * calendar. Lets a higher layer stamp "last sent" on a real BLE success
 * rather than on the moment the send was queued. Runs on the app task.
 */
void ulani_app_set_slot_sent_cb(void (*cb)(uint8_t slot, bool ok));

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
 *
 * activate switches the panel to that slot once the image lands. Uploading and
 * switching are separate operations on the wire, so passing false leaves the
 * display where it is -- which also skips the repaint the panel would
 * otherwise spend the next half minute on. Writing over the page currently on
 * screen repaints regardless, or the panel would keep showing an image that
 * slot no longer holds.
 */
esp_err_t ulani_app_cmd_test_image(uint8_t slot, uint32_t seed, bool activate);

/* Streams the image stored for `slot` to the calendar. */
esp_err_t ulani_app_cmd_send_slot(uint8_t slot);

/*
 * Re-reads what is on disk. Call after an upload or a delete; the status
 * snapshot is a cache so that polling does not stat the filesystem.
 */
void ulani_app_slots_changed(void);

/* Payload source producing the same generated pattern. */
void ulani_testpattern_src(ulani_payload_src_t *src, uint32_t *seed_storage, uint32_t seed);

/*
 * Page-number badge: a black right triangle in the bottom-right corner, about
 * 2 cm wide, with the page number in white inside it. Pages 1..4 only.
 */
typedef struct {
    ulani_payload_src_t inner;
    uint8_t             page;
} ulani_page_badge_t;

/*
 * Wraps inner so every byte it hands out has the badge stamped on it. storage
 * must outlive the transfer. Returns ESP_ERR_INVALID_ARG for a page outside
 * 1..4.
 */
esp_err_t ulani_page_badge_src(ulani_payload_src_t *src, ulani_page_badge_t *storage,
                               const ulani_payload_src_t *inner, uint8_t page);

/*
 * True when (x, y) falls inside the badge, with the palette index to draw.
 * Exposed for tools/tests that want to check the shape without a transfer.
 */
bool ulani_page_badge_pixel(uint8_t page, uint32_t x, uint32_t y, uint8_t *index);

#ifdef __cplusplus
}
#endif
