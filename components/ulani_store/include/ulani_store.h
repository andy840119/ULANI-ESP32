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

#ifdef __cplusplus
}
#endif
