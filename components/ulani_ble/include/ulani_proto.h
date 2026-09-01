/*
 * ULANI wire protocol constants and helpers.
 *
 * Everything in here is a direct transcription of the behaviour observed in
 * Grassboy's ULANI.node.js (src/BLEComm.js + src/dither.js). Where that code
 * has quirks, we reproduce the quirk rather than "fixing" it -- see
 * ulani_hex_to_bytes() and docs/protocol.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GATT identifiers (128-bit, little-endian order is handled in ulani_ble.c). */
#define ULANI_UUID_SERVICE "1234a200-7cbc-11e9-8f9e-2a86e4085a59"
#define ULANI_UUID_CHR_OP  "1234a201-7cbc-11e9-8f9e-2a86e4085a59" /* write + notify */
#define ULANI_UUID_CHR_DAT "1234a202-7cbc-11e9-8f9e-2a86e4085a59" /* write + notify */

/* Advertised name prefix used when the user has not pinned an exact name. */
#define ULANI_NAME_PREFIX "ULANI Calendar"

/* Panel geometry. Fixed by the device, not configurable. */
#define ULANI_IMG_W 800
#define ULANI_IMG_H 480

/* One palette index per pixel, packed two-per-byte. */
#define ULANI_PAYLOAD_BYTES ((size_t)(ULANI_IMG_W * ULANI_IMG_H) / 2) /* 192000 */

/*
 * The JS splits the nibble string into 460-character pieces, then hex-decodes
 * each piece, so every BLE write carries 230 bytes. 192000 = 834*230 + 180,
 * i.e. 835 writes per image with a short final one.
 */
#define ULANI_CHUNK_BYTES  230
#define ULANI_CHUNK_COUNT  (((ULANI_PAYLOAD_BYTES) + (ULANI_CHUNK_BYTES) - 1) / (ULANI_CHUNK_BYTES))

/* Inter-packet delay the JS uses; too fast and the device drops the transfer. */
#define ULANI_CHUNK_GAP_MS 20

/* An op write is answered by a notify whose first byte repeats the opcode. */
#define ULANI_OP_TIMEOUT_MS 10000

/* Keepalive cadence, and the idle window after which the device is released. */
#define ULANI_ACK_INTERVAL_MS   10000
#define ULANI_IDLE_TIMEOUT_MS  300000

/* Opcodes (first byte of an op-channel frame). */
#define ULANI_OP_SEND_IMAGE   0x01
#define ULANI_OP_IMAGE_RESULT 0x02 /* arrives on the data channel */
#define ULANI_OP_CUSTOMER_ID  0x04
#define ULANI_OP_BATTERY      0x06
#define ULANI_OP_DISCONNECT   0x09
#define ULANI_OP_SET_SLOT     0x0b
#define ULANI_OP_GET_SLOT     0x0c

#define ULANI_SLOT_MIN 1
#define ULANI_SLOT_MAX 4

/*
 * CRC-16/XMODEM (poly 0x1021, init 0x0000, no reflection, no final xor) over
 * the whole 192000-byte payload.
 */
uint16_t ulani_crc16(uint16_t seed, const uint8_t *data, size_t len);

/* Streaming form of the same CRC, for hashing a payload we never hold in RAM. */
typedef struct {
    uint16_t acc;
} ulani_crc16_ctx_t;

void     ulani_crc16_init(ulani_crc16_ctx_t *ctx);
void     ulani_crc16_update(ulani_crc16_ctx_t *ctx, const uint8_t *data, size_t len);
uint16_t ulani_crc16_final(const ulani_crc16_ctx_t *ctx);

/*
 * Hex-string to bytes, matching the JS hexToUint8Array() *including* its
 * handling of odd-length input: the trailing single character is parsed as a
 * whole byte, so "abc" -> {0xab, 0x0c}. This matters because the JS renders the
 * CRC with Number.prototype.toString(16), which does not zero-pad; a CRC whose
 * top nibble is zero therefore produces a 3-character string and a *different*
 * header than a naive implementation would emit.
 *
 * Returns the number of bytes written, or -1 if out_len is too small.
 */
int ulani_hex_to_bytes(const char *hex, uint8_t *out, size_t out_len);

/*
 * Build the "start send image" header exactly as startSendImage() does:
 *   "010002ee000" + slot + "02" + <low 8 hex digits of ms timestamp> + <crc hex>
 * Writes into out and returns the byte length, or -1 on error.
 */
int ulani_build_send_header(uint8_t slot, uint64_t timestamp_ms, uint16_t crc,
                            uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif
