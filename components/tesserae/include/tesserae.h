/*
 * Tesserae device client -- one per ULANI page.
 *
 * Tesserae (https://github.com/dmellok/tesserae) is a self-hosted service that
 * renders calendar pages and serves them in whatever packed format a panel
 * wants. One of its device kinds, seeed_ee04_73e6, is 800x480 at 4bpp packed,
 * byte for byte a ULANI frame, so the server produces something the calendar
 * can display directly and this firmware never touches an image.
 *
 * The calendar holds four pages, so there are four independent clients here,
 * client i feeding slot i+1. Each registers separately (its own server, token
 * and schedule), so four dashboards can drive the four pages of one calendar.
 * Tesserae identifies a device by the MAC it announces, so each client also
 * announces its own: the real station MAC for page 1 and a locally-
 * administered variant of it for the rest, or the four would claim one
 * another's token and collapse into a single device there (issue #48).
 * A single task services all four in turn -- the radio is shared and transfers
 * serialise through ulani_app anyway, so four tasks would only contend.
 *
 * Only the palette differs from what the calendar wants: Tesserae emits
 * Spectra-6 nibbles and the calendar expects its own order, a sixteen-entry
 * lookup applied as the frame streams into flash. Nothing is held in RAM;
 * 192000 bytes is more heap than a C3 has once WiFi, BLE and the web server
 * have taken theirs.
 *
 * The device pulls; the server decides when, via next_poll_s on /status.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ulani_ble.h" /* ULANI_SLOT_MIN / _MAX */

#ifdef __cplusplus
extern "C" {
#endif

#define TESSERAE_CLIENTS   ULANI_SLOT_MAX /* one per page */
#define TESSERAE_URL_MAX   128
#define TESSERAE_CODE_MAX  64
#define TESSERAE_TOKEN_MAX 256
#define TESSERAE_ID_MAX    33
#define TESSERAE_ETAG_MAX  80

/*
 * The device kind claimed at registration -- the 800x480 4bpp Spectra-6
 * layout, the same 192000 bytes the calendar takes. Not
 * waveshare_photopainter_73, which is the same size but expects the device to
 * rotate the result 180 degrees.
 */
#define TESSERAE_DEVICE_KIND "seeed_ee04_73e6"

typedef enum {
    TESSERAE_DISABLED = 0, /* no server configured */
    TESSERAE_UNREGISTERED, /* server set, no token yet */
    TESSERAE_IDLE,         /* registered, waiting for the next poll */
    TESSERAE_WORKING,      /* talking to the server or fetching a frame */
    TESSERAE_ERROR,        /* last attempt failed; will retry */
} tesserae_state_t;

const char *tesserae_state_str(tesserae_state_t s);

typedef struct {
    uint8_t          slot;          /* 1..4, which page this client feeds */
    tesserae_state_t state;
    char             server_url[TESSERAE_URL_MAX];
    char             device_id[TESSERAE_ID_MAX];
    bool             registered;
    int32_t          next_poll_s;   /* cadence the server asked for */
    int32_t          seconds_until_poll;
    char             last_error[96];
    /* Wall-clock unix times, from the server's HTTP Date header (0 = unknown):
     *   last_check_epoch  when the server was last asked about this page
     *   last_frame_epoch  when a *new* frame was last stored (a 304 does not count)
     *   last_sent_epoch   when this page was last sent to the calendar over BLE
     */
    uint32_t         last_check_epoch;
    uint32_t         last_frame_epoch;
    uint32_t         last_sent_epoch;
    /* Whether a frame is stored for this slot and can be previewed. */
    bool             has_frame;
} tesserae_status_t;

/*
 * Called once a frame has been fetched, palette-converted and stored, so the
 * application can push it to the calendar. Runs on the tesserae task.
 */
typedef void (*tesserae_frame_cb_t)(uint8_t slot, void *user);

typedef struct {
    tesserae_frame_cb_t on_frame;
    void               *user;
} tesserae_cfg_t;

esp_err_t tesserae_start(const tesserae_cfg_t *cfg);

/*
 * Points the client for `slot` at a server. Three ways in, decided by what is
 * filled in: a token+id skip registration ("Add without pairing"), a pairing
 * code is redeemed at /register, and neither announces via /discover to be
 * approved in the UI. Any of pairing_code, device_id, token may be empty.
 */
esp_err_t tesserae_configure(uint8_t slot, const char *server_url,
                             const char *pairing_code, const char *device_id,
                             const char *token);

/* Forgets the server, token and stored ETag for one slot's client. */
esp_err_t tesserae_forget(uint8_t slot);

/* Asks one client for a frame now instead of waiting out its interval. */
esp_err_t tesserae_poll_now(uint8_t slot);

/*
 * Records that this slot's image was just sent to the calendar. Called from
 * the send-completion path, so "last sent" reflects a real BLE success rather
 * than the moment a frame was queued.
 */
void tesserae_note_sent(uint8_t slot);

void tesserae_get_status(uint8_t slot, tesserae_status_t *out);

#ifdef __cplusplus
}
#endif
