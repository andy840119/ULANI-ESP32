/*
 * Render a stored ULANI frame back to a canvas.
 *
 * A slot on the board holds packed palette indices -- two 4-bit ULANI colours
 * per byte, the wire format -- and there is no other copy of a Tesserae frame
 * anywhere. To show what a page currently displays, fetch those bytes and
 * paint them with the same seven-colour palette the panel uses.
 *
 * The palette is the ACeP order from the reference dither.js, and must match
 * the far side of the Spectra-6 lookup in components/tesserae/src/tesserae.c.
 */

export const PANEL_W = 800;
export const PANEL_H = 480;

const PALETTE: [number, number, number][] = [
  [0, 0, 0], //         0 black
  [209, 208, 202], //   1 white
  [69, 121, 81], //     2 green
  [82, 91, 151], //     3 blue
  [175, 76, 74], //     4 red
  [207, 194, 88], //    5 yellow
  [192, 99, 30], //     6 orange
];

/* Packed nibbles (192000 bytes) -> an 800x480 canvas. */
export function framePreview(payload: Uint8Array): HTMLCanvasElement {
  const canvas = document.createElement('canvas');
  canvas.width = PANEL_W;
  canvas.height = PANEL_H;

  const ctx = canvas.getContext('2d')!;
  const image = ctx.createImageData(PANEL_W, PANEL_H);
  const d = image.data;

  for (let i = 0; i < payload.length; i++) {
    const hi = payload[i] >> 4;
    const lo = payload[i] & 0x0f;
    for (let half = 0; half < 2; half++) {
      const px = i * 2 + half;
      const [r, g, b] = PALETTE[half === 0 ? hi : lo] ?? PALETTE[1];
      d[px * 4] = r;
      d[px * 4 + 1] = g;
      d[px * 4 + 2] = b;
      d[px * 4 + 3] = 255;
    }
  }
  ctx.putImageData(image, 0, 0);
  return canvas;
}
