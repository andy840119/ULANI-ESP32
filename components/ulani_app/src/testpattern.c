/*
 * Generated test payload.
 *
 * The panel expects 800x480 palette indices (0..6), two per byte, high nibble
 * first. Producing them arithmetically means the "random image" self-test costs
 * no flash and no RAM: we can answer any byte range from the seed alone, which
 * is exactly what ulani_payload_src_t asks for.
 *
 * Palette order matches dither.js:
 *   0 black, 1 white, 2 green, 3 blue, 4 red, 5 yellow, 6 orange
 */

#include <string.h>

#include "ulani_app.h"

#define PALETTE_N 7
#define BLOCK_PX  40

static uint32_t mix(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

static uint8_t pixel_index(uint32_t seed, uint32_t x, uint32_t y)
{
    /* A one-block border makes it obvious when the image lands rotated. */
    if (x < 4 || y < 4 || x >= ULANI_IMG_W - 4 || y >= ULANI_IMG_H - 4) {
        return 0;
    }
    uint32_t bx = x / BLOCK_PX;
    uint32_t by = y / BLOCK_PX;
    uint32_t h  = mix(seed * 2654435761U + bx * 73856093U + by * 19349663U);
    return (uint8_t)(h % PALETTE_N);
}

static esp_err_t read_pattern(void *ctx, size_t offset, uint8_t *out, size_t len)
{
    const uint32_t seed = *(const uint32_t *)ctx;

    if (offset + len > ULANI_PAYLOAD_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < len; i++) {
        size_t   p  = (offset + i) * 2; /* two pixels per byte */
        uint32_t x0 = (uint32_t)(p % ULANI_IMG_W);
        uint32_t y0 = (uint32_t)(p / ULANI_IMG_W);
        uint32_t x1 = (uint32_t)((p + 1) % ULANI_IMG_W);
        uint32_t y1 = (uint32_t)((p + 1) / ULANI_IMG_W);

        out[i] = (uint8_t)((pixel_index(seed, x0, y0) << 4) |
                            pixel_index(seed, x1, y1));
    }
    return ESP_OK;
}

void ulani_testpattern_src(ulani_payload_src_t *src, uint32_t *seed_storage, uint32_t seed)
{
    *seed_storage = seed;
    src->read = read_pattern;
    src->ctx  = seed_storage;
}
