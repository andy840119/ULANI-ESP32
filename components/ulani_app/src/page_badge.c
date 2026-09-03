/*
 * Page-number badge overlay.
 *
 * Stamps "which slot is this" onto the bottom-right corner of any payload
 * before it goes out: a black right triangle roughly 2 cm wide with the slot
 * number in white inside it. The panel is 800x480 over ~163 mm, so 2 cm is
 * about 98 px; BADGE_PX below is that, rounded.
 *
 * Like the test pattern this is a pure function of (x, y), so it composes with
 * ulani_payload_src_t without buffering the 192000-byte image anywhere.
 *
 * Digits are stitched out of rectangles (a seven-segment layout) and only
 * 1..4 exist, which is all the panel has slots for.
 */

#include <string.h>

#include "ulani_app.h"

#define BADGE_PX 98 /* leg length of the triangle, in pixels */

/* Digit cell, placed near the centroid of the triangle so it stays inside. */
#define DIGIT_W 22
#define DIGIT_H 36
#define STROKE  5

#define IDX_BLACK 0
#define IDX_WHITE 1

/* Seven-segment bits. */
#define SEG_A 0x01 /* top          */
#define SEG_B 0x02 /* upper right  */
#define SEG_C 0x04 /* lower right  */
#define SEG_D 0x08 /* bottom       */
#define SEG_E 0x10 /* lower left   */
#define SEG_F 0x20 /* upper left   */
#define SEG_G 0x40 /* middle       */

static const uint8_t k_segments[4] = {
    SEG_B | SEG_C,                                 /* 1 */
    SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,         /* 2 */
    SEG_A | SEG_B | SEG_G | SEG_C | SEG_D,         /* 3 */
    SEG_F | SEG_B | SEG_G | SEG_C,                 /* 4 */
};

/* Digit cell origin: 30% of the leg away from the corner, on both axes. */
#define DIGIT_X0 ((int)ULANI_IMG_W - 1 - (BADGE_PX * 3) / 10 - DIGIT_W / 2)
#define DIGIT_Y0 ((int)ULANI_IMG_H - 1 - (BADGE_PX * 3) / 10 - DIGIT_H / 2)

static bool digit_pixel(uint8_t page, int dx, int dy)
{
    const uint8_t segs = k_segments[page - 1];
    const int     mid0 = DIGIT_H / 2 - STROKE / 2;
    const int     mid1 = mid0 + STROKE;

    if ((segs & SEG_A) && dy < STROKE) {
        return true;
    }
    if ((segs & SEG_D) && dy >= DIGIT_H - STROKE) {
        return true;
    }
    if ((segs & SEG_G) && dy >= mid0 && dy < mid1) {
        return true;
    }
    if ((segs & SEG_F) && dx < STROKE && dy < mid1) {
        return true;
    }
    if ((segs & SEG_E) && dx < STROKE && dy >= mid0) {
        return true;
    }
    if ((segs & SEG_B) && dx >= DIGIT_W - STROKE && dy < mid1) {
        return true;
    }
    if ((segs & SEG_C) && dx >= DIGIT_W - STROKE && dy >= mid0) {
        return true;
    }
    return false;
}

bool ulani_page_badge_pixel(uint8_t page, uint32_t x, uint32_t y, uint8_t *index)
{
    if (page < 1 || page > 4) {
        return false;
    }

    /* Hypotenuse runs from (W-BADGE, H-1) to (W-1, H-BADGE). */
    const int sum = (int)x + (int)y;
    if (sum < (int)ULANI_IMG_W - 1 + (int)ULANI_IMG_H - 1 - BADGE_PX) {
        return false;
    }

    const int dx = (int)x - DIGIT_X0;
    const int dy = (int)y - DIGIT_Y0;
    if (dx >= 0 && dx < DIGIT_W && dy >= 0 && dy < DIGIT_H &&
        digit_pixel(page, dx, dy)) {
        *index = IDX_WHITE;
    } else {
        *index = IDX_BLACK;
    }
    return true;
}

static esp_err_t read_badged(void *ctx, size_t offset, uint8_t *out, size_t len)
{
    const ulani_page_badge_t *b = (const ulani_page_badge_t *)ctx;

    esp_err_t err = b->inner.read(b->inner.ctx, offset, out, len);
    if (err != ESP_OK) {
        return err;
    }

    /* Only the last BADGE_PX rows can be touched; the rest is a memcpy's worth
     * of work we can skip entirely. */
    const size_t first = ((size_t)ULANI_IMG_H - BADGE_PX) * ULANI_IMG_W / 2;
    size_t       i     = (offset < first) ? (first - offset) : 0;

    for (; i < len; i++) {
        size_t p = (offset + i) * 2; /* two pixels per byte, high nibble first */
        uint32_t y = (uint32_t)(p / ULANI_IMG_W);
        uint32_t x = (uint32_t)(p % ULANI_IMG_W);

        uint8_t hi, lo;
        if (ulani_page_badge_pixel(b->page, x, y, &hi)) {
            out[i] = (uint8_t)((out[i] & 0x0F) | (hi << 4));
        }
        if (ulani_page_badge_pixel(b->page, x + 1, y, &lo)) {
            out[i] = (uint8_t)((out[i] & 0xF0) | lo);
        }
    }
    return ESP_OK;
}

esp_err_t ulani_page_badge_src(ulani_payload_src_t *src, ulani_page_badge_t *storage,
                               const ulani_payload_src_t *inner, uint8_t page)
{
    if (src == NULL || storage == NULL || inner == NULL || inner->read == NULL ||
        page < 1 || page > 4) {
        return ESP_ERR_INVALID_ARG;
    }
    storage->inner = *inner;
    storage->page  = page;
    src->read      = read_badged;
    src->ctx       = storage;
    return ESP_OK;
}
