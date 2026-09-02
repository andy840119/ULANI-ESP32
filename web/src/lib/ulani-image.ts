/*
 * Turning a photograph into something the panel can display.
 *
 * The wire format is one palette index per pixel, so quantising is not a
 * cosmetic step that could be skipped -- there is no way to send an image
 * without it. Everything happens here in the browser: the ESP32 has neither
 * the memory nor the cycles for a 384000-pixel error-diffusion pass, and doing
 * it here means the preview shown to the user is the actual bytes that will be
 * sent, not an approximation of them.
 *
 * The palette and the framing behaviour are taken from src/dither.js in
 * Grassboy's ULANI.node.js.
 */

export const PANEL_W = 800;
export const PANEL_H = 480;

/* Palette order is the wire encoding; index 3 means "blue" to the panel. */
export const PALETTE: [number, number, number][] = [
  [0, 0, 0], //         0 black
  [209, 208, 202], //   1 white
  [69, 121, 81], //     2 green
  [82, 91, 151], //     3 blue
  [175, 76, 74], //     4 red
  [207, 194, 88], //    5 yellow
  [192, 99, 30], //     6 orange
];

export const PAYLOAD_BYTES = (PANEL_W * PANEL_H) / 2;

export type Rotation = 0 | 90 | 180 | 270;

/*
 * contain -- whole image, solid bars where it does not reach the edges.
 * cover   -- fills the panel, cropping whatever falls outside.
 * wash    -- contain, but over a dimmed copy of the image blown up to fill,
 *            so the bars pick up its colours instead of being flat. This is
 *            what dither.js does by default; it is an effect, not a fit, and
 *            it looks like a double exposure if you were not expecting it.
 */
export type Fit = 'contain' | 'cover' | 'wash';

export interface RenderOptions {
  rotation: Rotation;
  fit: Fit;
  /* Bar colour for `contain`; the dimming layer for `wash`. */
  background: string;
  washBackground: string;
}

export const DEFAULT_OPTIONS: RenderOptions = {
  rotation: 0,
  fit: 'contain',
  background: '#000000',
  washBackground: '#000000aa',
};

/*
 * Where to draw a width x height image inside a box, both fitted (letterboxed)
 * and filled (cropped). Ported from calcBoundingBox in dither.js.
 */
function placement(
  width: number,
  height: number,
  boxW: number,
  boxH: number,
): { contain: [number, number, number, number]; cover: [number, number, number, number] } {
  const scaleContain = Math.min(boxW / width, boxH / height);
  const cw = width * scaleContain;
  const ch = height * scaleContain;

  const scaleCover = Math.max(boxW / width, boxH / height);
  const vw = width * scaleCover;
  const vh = height * scaleCover;

  return {
    contain: [(boxW - cw) / 2, (boxH - ch) / 2, cw, ch],
    cover: [(boxW - vw) / 2, (boxH - vh) / 2, vw, vh],
  };
}

/* Composes the source image onto an 800x480 canvas honouring the options. */
export function compose(
  source: CanvasImageSource,
  sourceW: number,
  sourceH: number,
  opts: RenderOptions,
): HTMLCanvasElement {
  const canvas = document.createElement('canvas');
  canvas.width = PANEL_W;
  canvas.height = PANEL_H;

  const ctx = canvas.getContext('2d', { willReadFrequently: true })!;
  ctx.imageSmoothingQuality = 'high';

  // Rotating swaps the box the image is fitted into.
  const upright = opts.rotation === 90 || opts.rotation === 270;
  const boxW = upright ? PANEL_H : PANEL_W;
  const boxH = upright ? PANEL_W : PANEL_H;

  ctx.save();
  switch (opts.rotation) {
    case 90:
      ctx.translate(0, PANEL_H);
      ctx.rotate(-Math.PI / 2);
      break;
    case 180:
      ctx.translate(PANEL_W, PANEL_H);
      ctx.rotate(Math.PI);
      break;
    case 270:
      ctx.translate(PANEL_W, 0);
      ctx.rotate(Math.PI / 2);
      break;
  }

  const box = placement(sourceW, sourceH, boxW, boxH);

  if (opts.fit === 'cover') {
    ctx.drawImage(source, ...box.cover);
  } else {
    if (opts.fit === 'wash') {
      ctx.drawImage(source, ...box.cover);
      ctx.fillStyle = opts.washBackground;
    } else {
      ctx.fillStyle = opts.background;
    }
    ctx.fillRect(0, 0, boxW, boxH);
    ctx.drawImage(source, ...box.contain);
  }
  ctx.restore();

  return canvas;
}

/*
 * Floyd-Steinberg error diffusion onto the panel palette, in place.
 *
 * Nearest-colour on its own leaves seven flat bands where a photograph had a
 * gradient; spreading the error is what makes a seven-colour panel look like
 * it has more colours than it does. Returns the palette index of every pixel.
 */
const clamp = (v: number) => (v < 0 ? 0 : v > 255 ? 255 : v);

export function dither(image: ImageData): Uint8Array {
  const { width, height, data } = image;
  const indices = new Uint8Array(width * height);

  // Signed working copy: diffused error routinely pushes values outside 0..255.
  const buf = new Float32Array(width * height * 3);
  for (let i = 0, p = 0; i < indices.length; i++, p += 3) {
    buf[p] = data[i * 4];
    buf[p + 1] = data[i * 4 + 1];
    buf[p + 2] = data[i * 4 + 2];
  }

  const spread = (x: number, y: number, er: number, eg: number, eb: number, f: number) => {
    if (x < 0 || x >= width || y >= height) return;
    const p = (y * width + x) * 3;
    buf[p] += er * f;
    buf[p + 1] += eg * f;
    buf[p + 2] += eb * f;
  };

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = y * width + x;
      const p = i * 3;
      /*
       * Clamp before matching. Diffused error can push a pixel far outside
       * 0..255, and matching on those values picks colours the neighbourhood
       * cannot justify, which shows up as isolated bright speckles.
       */
      const r = clamp(buf[p]);
      const g = clamp(buf[p + 1]);
      const b = clamp(buf[p + 2]);

      let best = 0;
      let bestDist = Infinity;
      for (let c = 0; c < PALETTE.length; c++) {
        const [pr, pg, pb] = PALETTE[c];
        const d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2;
        if (d < bestDist) {
          bestDist = d;
          best = c;
        }
      }
      indices[i] = best;

      const [nr, ng, nb] = PALETTE[best];
      const er = r - nr;
      const eg = g - ng;
      const eb = b - nb;

      spread(x + 1, y, er, eg, eb, 7 / 16);
      spread(x - 1, y + 1, er, eg, eb, 3 / 16);
      spread(x, y + 1, er, eg, eb, 5 / 16);
      spread(x + 1, y + 1, er, eg, eb, 1 / 16);

      data[i * 4] = nr;
      data[i * 4 + 1] = ng;
      data[i * 4 + 2] = nb;
      data[i * 4 + 3] = 255;
    }
  }

  return indices;
}

/* Two pixels per byte, high nibble first, exactly as the panel expects. */
export function pack(indices: Uint8Array): Uint8Array<ArrayBuffer> {
  const out = new Uint8Array(PAYLOAD_BYTES);
  for (let i = 0; i < out.length; i++) {
    out[i] = (indices[i * 2] << 4) | indices[i * 2 + 1];
  }
  return out;
}

export function unpack(payload: Uint8Array): Uint8Array {
  const indices = new Uint8Array(PANEL_W * PANEL_H);
  for (let i = 0; i < payload.length; i++) {
    indices[i * 2] = payload[i] >> 4;
    indices[i * 2 + 1] = payload[i] & 0x0f;
  }
  return indices;
}

/* Renders palette indices back to a canvas, for previewing a stored slot. */
export function indicesToCanvas(indices: Uint8Array): HTMLCanvasElement {
  const canvas = document.createElement('canvas');
  canvas.width = PANEL_W;
  canvas.height = PANEL_H;

  const ctx = canvas.getContext('2d')!;
  const image = ctx.createImageData(PANEL_W, PANEL_H);

  for (let i = 0; i < indices.length; i++) {
    const [r, g, b] = PALETTE[indices[i]] ?? PALETTE[0];
    image.data[i * 4] = r;
    image.data[i * 4 + 1] = g;
    image.data[i * 4 + 2] = b;
    image.data[i * 4 + 3] = 255;
  }
  ctx.putImageData(image, 0, 0);
  return canvas;
}

export interface Processed {
  /* What the panel will show. Same pixels the payload encodes. */
  preview: HTMLCanvasElement;
  payload: Uint8Array<ArrayBuffer>;
}

export async function processFile(file: File, opts: RenderOptions): Promise<Processed> {
  const bitmap = await createImageBitmap(file);
  try {
    const canvas = compose(bitmap, bitmap.width, bitmap.height, opts);
    const ctx = canvas.getContext('2d', { willReadFrequently: true })!;
    const image = ctx.getImageData(0, 0, PANEL_W, PANEL_H);

    const indices = dither(image);
    ctx.putImageData(image, 0, 0);

    return { preview: canvas, payload: pack(indices) };
  } finally {
    bitmap.close();
  }
}
