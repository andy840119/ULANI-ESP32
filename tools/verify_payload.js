#!/usr/bin/env node
/*
 * Cross-checks the C protocol layer against the original Node.js semantics.
 *
 * The functions below are copied verbatim (modulo formatting) from
 * Grassboy's ULANI.node.js -- src/dither.js CRC16 + hexToUint8Array, and the
 * header string from src/BLEComm.js startSendImage(). We generate the same test
 * pattern the firmware generates, run it through this reference code, and
 * compare against what tools/verify_payload.c prints.
 *
 * Usage:
 *     gcc -O2 -I components/ulani_ble/include -o build/verify \
 *         tools/verify_payload.c components/ulani_ble/src/ulani_proto.c
 *     node tools/verify_payload.js | diff - <(./build/verify)
 */

/* ---- reference implementations, from ULANI.node.js ---- */

const CRC16 = function (paramShort, paramArrayOfByte, paramLong1, paramLong2) {
  let i = paramLong1;
  let j;
  let k;
  for (j = paramShort; paramLong2 > 0; j = k) {
    k = (j ^ (paramArrayOfByte[i] << 8)) & 0xffff;
    for (paramShort = 0; paramShort < 8; paramShort++) {
      if (k & 0x8000) {
        k = ((k << 1) ^ 0x1021) & 0xffff;
      } else {
        k = (k << 1) & 0xffff;
      }
    }
    i++;
    paramLong2--;
  }
  return j.toString(16);
};

const hexToUint8Array = function (hex) {
  const array = [];
  for (let i = 0; i < hex.length; i += 2) {
    array.push(parseInt(hex.substr(i, 2), 16));
  }
  return new Uint8Array(array);
};

/* ---- the same pattern components/ulani_app/src/testpattern.c produces ---- */

const W = 800;
const H = 480;
const PALETTE_N = 7;
const BLOCK_PX = 40;

function mix(h) {
  h = h >>> 0;
  h ^= h >>> 16;
  h = Math.imul(h, 0x7feb352d) >>> 0;
  h ^= h >>> 15;
  h = Math.imul(h, 0x846ca68b) >>> 0;
  h ^= h >>> 16;
  return h >>> 0;
}

function pixelIndex(seed, x, y) {
  if (x < 4 || y < 4 || x >= W - 4 || y >= H - 4) return 0;
  const bx = Math.floor(x / BLOCK_PX);
  const by = Math.floor(y / BLOCK_PX);
  const h = mix(
    (Math.imul(seed, 2654435761) +
      Math.imul(bx, 73856093) +
      Math.imul(by, 19349663)) >>>
      0,
  );
  return h % PALETTE_N;
}

function buildPayload(seed) {
  const bytes = new Uint8Array((W * H) / 2);
  for (let i = 0; i < bytes.length; i++) {
    const p = i * 2;
    const hi = pixelIndex(seed, p % W, Math.floor(p / W));
    const lo = pixelIndex(seed, (p + 1) % W, Math.floor((p + 1) / W));
    bytes[i] = (hi << 4) | lo;
  }
  return bytes;
}

/* ---- report ---- */

const SEEDS = [1, 2, 42, 0xdeadbeef, 123456789];
const TIMESTAMP = 0x12345678; // fixed so both sides print the same header

for (const seed of SEEDS) {
  const payload = buildPayload(seed);
  const crc = CRC16(0, payload, 0, payload.length);
  const header =
    '010002ee000' +
    '1' +
    '02' +
    TIMESTAMP.toString(16).padStart(8, '0') +
    crc;
  const bytes = Array.from(hexToUint8Array(header))
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
  console.log(
    `seed=${seed >>> 0} len=${payload.length} crc=${crc} header=${bytes}`,
  );
}
