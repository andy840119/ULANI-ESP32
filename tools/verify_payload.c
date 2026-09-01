/*
 * Host-side twin of tools/verify_payload.js: runs the firmware's own CRC,
 * header builder and test-pattern generator on a desktop compiler so the two
 * can be diffed. See verify_payload.js for the build command.
 *
 * This deliberately re-implements the pattern rather than including
 * testpattern.c, which pulls in ESP-IDF headers; keep the two in sync.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ulani_proto.h"

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
    if (x < 4 || y < 4 || x >= ULANI_IMG_W - 4 || y >= ULANI_IMG_H - 4) {
        return 0;
    }
    uint32_t bx = x / BLOCK_PX;
    uint32_t by = y / BLOCK_PX;
    uint32_t h  = mix(seed * 2654435761U + bx * 73856093U + by * 19349663U);
    return (uint8_t)(h % PALETTE_N);
}

int main(void)
{
    static const uint32_t seeds[] = { 1, 2, 42, 0xdeadbeefU, 123456789U };
    const uint64_t timestamp = 0x12345678ULL;

    uint8_t *payload = malloc(ULANI_PAYLOAD_BYTES);
    if (!payload) {
        return 1;
    }

    for (size_t si = 0; si < sizeof(seeds) / sizeof(seeds[0]); si++) {
        uint32_t seed = seeds[si];

        for (size_t i = 0; i < ULANI_PAYLOAD_BYTES; i++) {
            size_t p = i * 2;
            payload[i] = (uint8_t)(
                (pixel_index(seed, p % ULANI_IMG_W, p / ULANI_IMG_W) << 4) |
                 pixel_index(seed, (p + 1) % ULANI_IMG_W, (p + 1) / ULANI_IMG_W));
        }

        uint16_t crc = ulani_crc16(0, payload, ULANI_PAYLOAD_BYTES);

        uint8_t header[20];
        int hlen = ulani_build_send_header(1, timestamp, crc, header, sizeof(header));

        printf("seed=%u len=%u crc=%x header=", (unsigned)seed,
               (unsigned)ULANI_PAYLOAD_BYTES, (unsigned)crc);
        for (int i = 0; i < hlen; i++) {
            printf("%02x", header[i]);
        }
        printf("\n");
    }

    free(payload);
    return 0;
}
