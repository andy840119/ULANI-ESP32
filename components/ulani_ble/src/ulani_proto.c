#include "ulani_proto.h"

#include <stdio.h>
#include <string.h>

static uint16_t crc16_step(uint16_t acc, uint8_t byte)
{
    uint16_t k = (uint16_t)(acc ^ ((uint16_t)byte << 8));
    for (int i = 0; i < 8; i++) {
        k = (k & 0x8000u) ? (uint16_t)((k << 1) ^ 0x1021u) : (uint16_t)(k << 1);
    }
    return k;
}

uint16_t ulani_crc16(uint16_t seed, const uint8_t *data, size_t len)
{
    uint16_t acc = seed;
    for (size_t i = 0; i < len; i++) {
        acc = crc16_step(acc, data[i]);
    }
    return acc;
}

void ulani_crc16_init(ulani_crc16_ctx_t *ctx)
{
    ctx->acc = 0;
}

void ulani_crc16_update(ulani_crc16_ctx_t *ctx, const uint8_t *data, size_t len)
{
    ctx->acc = ulani_crc16(ctx->acc, data, len);
}

uint16_t ulani_crc16_final(const ulani_crc16_ctx_t *ctx)
{
    return ctx->acc;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ulani_hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    size_t len = strlen(hex);
    size_t n = (len + 1) / 2; /* odd length still yields one more byte */
    if (n > out_len) {
        return -1;
    }

    size_t w = 0;
    for (size_t i = 0; i < len; i += 2) {
        int hi = hex_nibble(hex[i]);
        if (hi < 0) {
            return -1;
        }
        if (i + 1 < len) {
            int lo = hex_nibble(hex[i + 1]);
            if (lo < 0) {
                return -1;
            }
            out[w++] = (uint8_t)((hi << 4) | lo);
        } else {
            /*
             * JS: parseInt("c", 16) === 12, so a lone trailing character becomes
             * the low nibble of its own byte rather than the high nibble.
             */
            out[w++] = (uint8_t)hi;
        }
    }
    return (int)w;
}

int ulani_build_send_header(uint8_t slot, uint64_t timestamp_ms, uint16_t crc,
                            uint8_t *out, size_t out_len)
{
    if (slot < ULANI_SLOT_MIN || slot > ULANI_SLOT_MAX) {
        return -1;
    }

    char hex[40];
    /*
     * "%x" on the CRC is deliberate: the JS uses toString(16) with no padding,
     * so a CRC below 0x1000 shortens the header. See ulani_hex_to_bytes().
     */
    int n = snprintf(hex, sizeof(hex), "010002ee000%u02%08x%x",
                     (unsigned)slot,
                     (unsigned)(timestamp_ms & 0xffffffffULL),
                     (unsigned)crc);
    if (n < 0 || (size_t)n >= sizeof(hex)) {
        return -1;
    }
    return ulani_hex_to_bytes(hex, out, out_len);
}
