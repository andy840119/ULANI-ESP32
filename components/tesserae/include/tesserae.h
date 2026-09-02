/*
 * Tesserae device client.
 *
 * Tesserae (https://github.com/dmellok/tesserae) is a self-hosted service that
 * renders calendar pages and serves them to e-paper devices in whatever packed
 * format the panel wants. One of its device kinds -- seeed_ee04_73e6 -- is
 * 800x480 at 4bpp packed, which is byte-for-byte the size of a ULANI frame, so
 * the server can produce something the calendar can display directly and this
 * firmware never touches an image.
 *
 * Only the palette differs: Tesserae emits Spectra-6 nibbles and the calendar
 * expects its own order. That is a sixteen-entry lookup applied as the frame
 * streams past, which is why nothing here needs the whole frame in memory --
 * it could not have it anyway, since 192000 bytes is more heap than a C3 has
 * left once WiFi, BLE and the web server have taken theirs.
 *
 * The device pulls; the server decides when. /status answers with next_poll_s
 * and this client sleeps for exactly that long. Tesserae does have a push
 * channel (SSE) but its own contract calls it an optimisation and restricts it
 * to touch panels doing partial redraws, so polling is the whole story here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TESSERAE_URL_MAX   128
#define TESSERAE_CODE_MAX  64
#define TESSERAE_TOKEN_MAX 256
#define TESSERAE_ID_MAX    33
#define TESSERAE_ETAG_MAX  80

/*
 * The device kind claimed at registration. Selects the server-side renderer,
 * and this one is the 800x480 4bpp Spectra-6 layout -- the same 192000 bytes
 * the calendar takes. Do not swap it for waveshare_photopainter_73, which is
 * the same size but expects the device to rotate the result 180 degrees.
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
    tesserae_state_t state;
    char             server_url[TESSERAE_URL_MAX];
    char             device_id[TESSERAE_ID_MAX];
    bool             registered;
    uint8_t          slot;          /* which calendar frame this feeds */
    int32_t          next_poll_s;   /* cadence the server asked for */
    int32_t          seconds_until_poll;
    char             last_error[96];
    /* Unix time of the last frame actually handed to the calendar, 0 if none. */
    uint32_t         last_frame_at;
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
 * Stores the server details. There are three ways to be let in, matching the
 * three the Tesserae UI offers, and which one is in play is decided by what is
 * filled in here:
 *
 *   token given      -- "Add without pairing" already issued an access token
 *                       and a device id; use them and skip registration.
 *   pairing code     -- "Issue pairing code"; POST /register redeems it.
 *   neither          -- POST /discover and wait to be approved in the UI.
 *
 * device_id may be empty, in which case the MAC is used. It must be set for
 * the token path, because there the id is whatever the admin typed.
 */
esp_err_t tesserae_configure(const char *server_url, const char *pairing_code,
                             const char *device_id, const char *token,
                             uint8_t slot);

/* Forgets the server, the token and the stored ETag. */
esp_err_t tesserae_forget(void);

/* Asks for a frame now instead of waiting out the remaining interval. */
esp_err_t tesserae_poll_now(void);

void tesserae_get_status(tesserae_status_t *out);

#ifdef __cplusplus
}
#endif
