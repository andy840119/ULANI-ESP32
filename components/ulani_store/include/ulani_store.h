/*
 * Persistent storage for the four image slots.
 *
 * A slot holds one finished 192000-byte payload -- palette indices packed two
 * per byte, exactly what goes out over BLE. The original photograph never
 * reaches the board: the browser scales, dithers and packs it, so the only
 * thing stored here is the panel-ready result. Four of those fit in the
 * storage partition with room to spare, where four originals would not.
 *
 * Nothing in this component holds a payload in RAM. Uploads stream in through
 * a writer, transfers stream out through a ulani_payload_src_t, and both work
 * a few kilobytes at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "ulani_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     present;
    size_t   size;    /* bytes on disk; a complete slot is ULANI_PAYLOAD_BYTES */
    uint16_t crc;     /* CRC-16 of the payload, 0 if not yet computed */
} ulani_slot_info_t;

/* Mounts the storage partition, formatting it if it has never been used. */
esp_err_t ulani_store_init(void);

void ulani_store_info(uint8_t slot, ulani_slot_info_t *out);

/* -------------------------------------------------------------- uploading */

typedef struct {
    FILE             *fp;
    uint8_t           slot;
    size_t            written;
    ulani_crc16_ctx_t crc; /* hashed as it streams in, so no second pass */
} ulani_store_writer_t;

/*
 * Writes go to a temporary file and only replace the slot on commit, so an
 * upload that dies halfway leaves whatever was there intact.
 */
esp_err_t ulani_store_write_begin(uint8_t slot, ulani_store_writer_t *w);
esp_err_t ulani_store_write(ulani_store_writer_t *w, const void *data, size_t len);
esp_err_t ulani_store_write_commit(ulani_store_writer_t *w);
void      ulani_store_write_abort(ulani_store_writer_t *w);

esp_err_t ulani_store_delete(uint8_t slot);

/* -------------------------------------------------------------- streaming */

typedef struct {
    FILE *fp;
} ulani_store_reader_t;

/*
 * Points a payload source at a stored slot. The reader must stay alive for as
 * long as the source is in use; close it with ulani_store_reader_close().
 */
esp_err_t ulani_store_payload_src(uint8_t slot, ulani_store_reader_t *r,
                                  ulani_payload_src_t *src);
void      ulani_store_reader_close(ulani_store_reader_t *r);

/* ------------------------------------------------------------- event log */

/*
 * Where the event log lands when it is set to survive a reboot. Kept here
 * rather than in diag_log because it is a storage policy: this partition also
 * holds four 192000-byte pages plus the temporary file an upload streams
 * through, and the log must never be the reason one of those cannot be
 * written. A diagnostic that breaks the thing it is diagnosing is worse than
 * no diagnostic.
 *
 * The log is a fixed number of equal segments, appended to in turn. When the
 * newest is full the *oldest* is truncated and reused, so the log has a hard
 * ceiling and recycling costs one file deletion rather than a rewrite --
 * SPIFFS is happy to drop a whole file and very unhappy to remove bytes from
 * the front of one.
 */
#define ULANI_LOG_SEGMENT_BYTES (64 * 1024)
#define ULANI_LOG_SEGMENTS_MIN  2
#define ULANI_LOG_SEGMENTS_MAX  8

/* Never fill the partition past this; the log stops instead. Room for one
 * page plus the .tmp an upload needs, with a little to spare. */
#define ULANI_LOG_RESERVE_BYTES (400 * 1024)

/*
 * Appends to the newest segment, recycling the oldest when it is full.
 * Returns ESP_ERR_NO_MEM (and stops writing) rather than crowding out a
 * page. Whatever it drops is reported through diag_log_note_*.
 */
esp_err_t ulani_store_log_append(const void *data, size_t len);

/* Bytes of log held, oldest first, headers excluded. */
size_t ulani_store_log_size(void);

/* Reads the log as one stream, oldest record first. */
size_t ulani_store_log_read(size_t offset, void *out, size_t len);

esp_err_t ulani_store_log_clear(void);

/* How many segments to keep: 2..8, i.e. a ceiling of 128 KB to 512 KB.
 * Persists, and shrinking drops the oldest segments immediately. */
void    ulani_store_log_set_segments(uint8_t segments);
uint8_t ulani_store_log_segments(void);

size_t ulani_store_free_bytes(void);

#ifdef __cplusplus
}
#endif
