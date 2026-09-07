/*
 * On-board event log.
 *
 * The board runs unattended with nothing on UART0, so anything that only
 * reaches the serial console may as well not have happened. This keeps a
 * record on the board itself, to be exported from the web UI after the fact.
 *
 * Two tiers, because they answer different questions:
 *
 *   Tier A, the records below -- fixed 24 bytes, no strings, kept in a RAM
 *   ring and (optionally) drained to flash. Days of history: "what happened,
 *   in what order, on which day".
 *
 *   Tier B, the text trace -- whatever ESP_LOGx already prints, captured
 *   through esp_log_set_vprintf into a RAM ring and never written to flash.
 *   Half an hour at most: "the detail of that one moment".
 *
 * The wording of an event lives in the web UI, not here: a record carries a
 * number and up to two integers, which is what keeps it to 24 bytes. That
 * makes the numbers a wire format -- see diag_code_t.
 *
 * Every entry point is safe to call from any task and never allocates, never
 * blocks on flash and never formats a string. A log that changes the timing
 * of what it is watching is worse than no log at all.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bumped whenever the record layout or the meaning of an existing field
 * changes -- never for a newly added code. It rides along in every export and
 * in each flash segment header, so an old export stays readable.
 */
#define DIAG_SCHEMA_VERSION 1

typedef struct {
    uint32_t seq;       /* monotonic; a gap means those records are gone */
    uint32_t uptime_ms; /* always meaningful, even before the clock is set */
    uint32_t epoch;     /* 0 = the clock was still unknown */
    uint16_t code;      /* diag_code_t */
    uint8_t  slot;      /* page 1..4, or 0 when the event is not about a page */
    int8_t   result;    /* 0 = ok; otherwise event-specific */
    int32_t  a;
    int32_t  b;
} diag_rec_t;

_Static_assert(sizeof(diag_rec_t) == 24, "diag_rec_t is a stored format");

/*
 * Event codes.
 *
 * **Append only.** These numbers are written to flash and handed out in
 * exports, so a code keeps its meaning forever: never renumber one, never
 * reuse the number of one you removed, and never change what a's and b's mean
 * for an existing code (add a new code instead). The UI's table in
 * web/src/lib/logcodes.ts is the other half of this contract.
 *
 * Grouped in hundreds by source so a listing can be filtered on code / 100.
 */
typedef enum {
    /* ---- system (1xx) ---- */
    DIAG_SYS_BOOT          = 100, /* a = esp_reset_reason(), b = boot id      */
    DIAG_SYS_HEAP          = 101, /* a = free bytes, b = lowest ever seen     */
    DIAG_SYS_TIME_SYNC     = 102, /* a = step in seconds, b = source (0 SNTP, 1 HTTP Date) */
    DIAG_SYS_LOG_CLEARED   = 103,
    DIAG_SYS_LOG_PAUSED    = 104, /* flash full: a = free bytes in storage    */
    DIAG_SYS_LOG_DROPPED   = 105, /* a = records lost to recycling            */
    DIAG_SYS_REBOOT        = 106, /* a reboot was asked for, not observed     */

    /* ---- wifi (2xx) ---- */
    DIAG_NET_CONNECTED     = 200, /* a = rssi, b = IPv4 as a host-order u32   */
    DIAG_NET_DISCONNECTED  = 201, /* a = esp_wifi disconnect reason           */
    DIAG_NET_RSSI          = 202, /* a = rssi                                 */

    /* ---- tesserae (3xx), slot = which page's client ---- */
    DIAG_TESS_STATUS       = 300, /* heartbeat done: a = HTTP status, b = ms  */
    DIAG_TESS_FRAME        = 301, /* /frame done: a = HTTP status (200/304/204), b = ms */
    DIAG_TESS_FRAME_STORED = 302, /* a = bytes, b = ms                        */
    DIAG_TESS_FRAME_FAILED = 303, /* a = HTTP status, b = bytes written       */
    DIAG_TESS_REGISTERED   = 304, /* a = HTTP status                          */
    DIAG_TESS_REJECTED     = 305, /* token refused (401); re-registering      */
    DIAG_TESS_NEXT_POLL    = 306, /* a = seconds the server asked for         */

    /* ---- calendar over BLE (4xx) ---- */
    DIAG_BLE_CONNECTED     = 400, /* a = ATT MTU                              */
    DIAG_BLE_DISCONNECTED  = 401, /* a = BLE reason code                      */
    DIAG_BLE_SLOT_CHANGED  = 402, /* the panel moved on its own: a = from, b = to,
                                   * result = 1 if this is the first read after
                                   * reconnecting (so it moved while we were away) */
    DIAG_BLE_SLOT_SET      = 403, /* we sent 0b: slot, a = why (0 user, 1 repaint) */
    DIAG_BLE_SEND_BEGIN    = 404, /* slot = target, a = page on screen now,
                                   * b = 1 if the page number is stamped on it */
    DIAG_BLE_SEND_ATTEMPT  = 405, /* a = reply word, b = ms, result = attempt  */
    DIAG_BLE_SEND_DONE     = 406, /* slot, a = page on screen after, b = ms    */
    DIAG_BLE_OP_TIMEOUT    = 407, /* a = opcode that got no reply              */
    DIAG_BLE_UNMATCHED     = 408, /* a = reply word, b = opcode we waited for  */
    DIAG_BLE_BATTERY       = 409, /* a = raw reply word (only when it moves)   */
    DIAG_BLE_SEND_PAYLOAD  = 410, /* a = payload crc: the same image twice is
                                   * worth being able to see                   */

    /* ---- web API (5xx), only calls that change something ---- */
    DIAG_WEB_ACTION        = 500, /* a = caller IPv4, b = diag_web_action_t    */
} diag_code_t;

/* b of DIAG_WEB_ACTION. Append only, same contract as the codes. */
typedef enum {
    DIAG_WEB_SET_SLOT      = 1,
    DIAG_WEB_SEND_SLOT     = 2,
    DIAG_WEB_TEST_IMAGE    = 3,
    DIAG_WEB_CONNECT       = 4,
    DIAG_WEB_DISCONNECT    = 5,
    DIAG_WEB_FORGET_DEVICE = 6,
    DIAG_WEB_TESS_CONNECT  = 7,
    DIAG_WEB_TESS_FORGET   = 8,
    DIAG_WEB_TESS_POLL     = 9,
    DIAG_WEB_UPLOAD        = 10,
    DIAG_WEB_SETTINGS      = 11,
    DIAG_WEB_REBOOT        = 12,
} diag_web_action_t;

/* --------------------------------------------------------------- lifecycle */

/*
 * Drains records to flash. Called from the log's own task, never from a
 * logging site, so it may take its time. len is always a multiple of
 * sizeof(diag_rec_t).
 */
typedef esp_err_t (*diag_write_fn)(const void *data, size_t len, void *user);

/*
 * "Not now": true while something is going on that flash writes must not
 * disturb -- an image transfer, where the radio is already starved.
 */
typedef bool (*diag_busy_fn)(void *user);

typedef struct {
    uint16_t      records;     /* ring capacity; 0 = DIAG_DEFAULT_RECORDS */
    uint16_t      trace_bytes; /* text ring; 0 = no text capture at all */
    diag_write_fn write;       /* NULL = RAM only, whatever the setting says */
    diag_busy_fn  busy;        /* NULL = always safe to write */
    void         *user;
} diag_log_cfg_t;

#define DIAG_DEFAULT_RECORDS 512  /* 12 KB */
#define DIAG_DEFAULT_TRACE   8192

esp_err_t diag_log_start(const diag_log_cfg_t *cfg);

/* ---------------------------------------------------------------- writing */

void diag_log(uint16_t code, uint8_t slot, int8_t result, int32_t a, int32_t b);

/* ---------------------------------------------------------------- reading */

typedef struct {
    uint32_t oldest_seq;  /* seq of the oldest record still held */
    uint32_t next_seq;    /* seq the next record will get */
    uint32_t dropped;     /* records recycled out of the ring since boot */
    uint16_t capacity;
    uint16_t count;
    uint32_t boot_id;
    bool     enabled;
    bool     persist;
    int      trace_level; /* esp_log_level_t threshold for the text ring */
    size_t   trace_len;   /* bytes currently held */
    uint32_t trace_total; /* bytes ever written; total - len = bytes lost */
} diag_log_stats_t;

void diag_log_get_stats(diag_log_stats_t *out);

/*
 * Copies out up to max records with seq >= since, oldest first. Returns how
 * many were written. Records recycled since the caller last looked are simply
 * missing -- compare the first seq against what you asked for.
 */
size_t diag_log_read(uint32_t since, diag_rec_t *out, size_t max);

/*
 * Reads the text trace by absolute byte offset (0 = the first byte ever
 * captured), so a reader that streams it in chunks can tell that the ring
 * moved underneath it: bytes below stats.trace_total - stats.trace_len are
 * gone, and *from is advanced to the oldest byte still held.
 */
size_t diag_trace_read(uint32_t *from, char *out, size_t max);

/* --------------------------------------------------------------- settings */

/*
 * Disabling stops recording; it deliberately does not clear what is already
 * held, because the usual reason to reach for the switch is that something
 * just happened. Both settings persist in NVS.
 */
void diag_log_set_enabled(bool on);
void diag_log_set_persist(bool on);
void diag_log_set_trace_level(int level);

/* Throws away the RAM ring and the text trace, and asks the sink to drop what
 * it holds. Records the fact that it happened. */
void diag_log_clear(void);

/* Called by the sink when it had to recycle or refuse; shows up in the stats
 * and, for the flash-full case, as an event. */
void diag_log_note_dropped(uint32_t records);
void diag_log_note_flash_full(uint32_t free_bytes);

#ifdef __cplusplus
}
#endif
