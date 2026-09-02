/*
 * The four image slots.
 *
 * A picked file is processed straight away so the user sees the seven-colour
 * result before committing to anything -- the framing and rotation controls
 * are only useful if their effect is visible. Uploading is a separate step for
 * the same reason: the preview is what gets sent, so it should be the thing
 * the user approved.
 */

import { api, type Status } from '../lib/api';
import {
  DEFAULT_OPTIONS,
  indicesToCanvas,
  processFile,
  unpack,
  type Fit,
  type RenderOptions,
  type Rotation,
} from '../lib/ulani-image';

const SLOTS = [1, 2, 3, 4];

const options: RenderOptions = { ...DEFAULT_OPTIONS };

/* Processed but not yet uploaded, per slot. */
const pending = new Map<number, Uint8Array<ArrayBuffer>>();
/* The file a slot was built from, so options changes can re-run it. */
const sources = new Map<number, File>();

let guard: (fn: () => Promise<unknown>) => void = () => {};

const $ = <T extends HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

export function slotsMarkup(): string {
  return `
    <section class="card">
      <h2>處理方式</h2>
      <p class="hint">
        圖片會在瀏覽器裡縮放並轉成日曆的七色，下方預覽就是實際會顯示的畫面。
        調整後要重新按「上傳」才會生效。
      </p>
      <div class="fields">
        <label>
          <span>旋轉</span>
          <select id="opt-rotation">
            <option value="0">不旋轉（橫式）</option>
            <option value="90">向左 90°（直式）</option>
            <option value="180">180°</option>
            <option value="270">向右 90°（直式）</option>
          </select>
        </label>
        <label>
          <span>比例</span>
          <select id="opt-fit">
            <option value="contain">完整顯示（黑色留邊）</option>
            <option value="cover">填滿畫面（裁掉超出的部分）</option>
            <option value="wash">完整顯示（留邊用暗化的原圖填滿）</option>
          </select>
        </label>
      </div>
    </section>

    ${SLOTS.map(
      (n) => `
      <section class="card slot" id="slot-card-${n}">
        <div class="slot-head">
          <h2>第 ${n} 張</h2>
          <span class="tag" id="slot-tag-${n}">空的</span>
        </div>

        <div class="preview" id="slot-preview-${n}">
          <span class="placeholder">尚未選擇圖片</span>
        </div>

        <div class="actions">
          <label class="filebtn">
            選擇圖片
            <input type="file" accept="image/*" id="slot-file-${n}" />
          </label>
          <button id="slot-upload-${n}" disabled>上傳</button>
          <button id="slot-send-${n}" class="ghost" disabled>送到日曆</button>
          <button id="slot-clear-${n}" class="ghost" disabled>清除</button>
        </div>
      </section>`,
    ).join('')}
  `;
}

function setPreview(slot: number, canvas: HTMLCanvasElement | null, note?: string) {
  const box = $<HTMLDivElement>(`#slot-preview-${slot}`);
  box.innerHTML = '';
  if (canvas) {
    box.appendChild(canvas);
  } else {
    const span = document.createElement('span');
    span.className = 'placeholder';
    span.textContent = note ?? '尚未選擇圖片';
    box.appendChild(span);
  }
}

async function rebuild(slot: number) {
  const file = sources.get(slot);
  if (!file) return;

  setPreview(slot, null, '處理中…');
  const { preview, payload } = await processFile(file, options);
  pending.set(slot, payload);
  setPreview(slot, preview);
  $<HTMLButtonElement>(`#slot-upload-${slot}`).disabled = false;
  $<HTMLSpanElement>(`#slot-tag-${slot}`).textContent = '尚未上傳';
}

export function mountSlots(guardFn: (fn: () => Promise<unknown>) => void) {
  guard = guardFn;

  $<HTMLSelectElement>('#opt-rotation').addEventListener('change', (ev) => {
    options.rotation = Number((ev.target as HTMLSelectElement).value) as Rotation;
    guard(async () => {
      for (const n of SLOTS) await rebuild(n);
    });
  });

  $<HTMLSelectElement>('#opt-fit').addEventListener('change', (ev) => {
    options.fit = (ev.target as HTMLSelectElement).value as Fit;
    guard(async () => {
      for (const n of SLOTS) await rebuild(n);
    });
  });

  for (const n of SLOTS) {
    $<HTMLInputElement>(`#slot-file-${n}`).addEventListener('change', (ev) => {
      const file = (ev.target as HTMLInputElement).files?.[0];
      if (!file) return;
      sources.set(n, file);
      guard(() => rebuild(n));
    });

    $(`#slot-upload-${n}`).addEventListener('click', () =>
      guard(async () => {
        const payload = pending.get(n);
        if (!payload) return;
        $<HTMLSpanElement>(`#slot-tag-${n}`).textContent = '上傳中…';
        await api.uploadSlot(n, payload);
        pending.delete(n);
        $<HTMLButtonElement>(`#slot-upload-${n}`).disabled = true;
      }),
    );

    $(`#slot-send-${n}`).addEventListener('click', () => guard(() => api.sendSlot(n)));

    $(`#slot-clear-${n}`).addEventListener('click', () =>
      guard(async () => {
        await api.clearSlot(n);
        sources.delete(n);
        pending.delete(n);
        setPreview(n, null);
      }),
    );

    $(`#slot-preview-${n}`).addEventListener('click', () => {
      // Only useful for a slot whose image lives on the board and nowhere else.
      if (sources.has(n)) return;
      guard(async () => {
        setPreview(n, null, '讀取中…');
        const payload = await api.downloadSlot(n);
        setPreview(n, indicesToCanvas(unpack(payload)));
      });
    });
  }
}

export function renderSlots(st: Status) {
  const busy = st.transfer.active;

  for (const n of SLOTS) {
    const info = st.slots?.find((s) => s.slot === n);
    const stored = info?.stored ?? false;
    const hasPending = pending.has(n);

    const tag = $<HTMLSpanElement>(`#slot-tag-${n}`);
    if (hasPending) {
      tag.textContent = '尚未上傳';
    } else if (stored) {
      tag.textContent = `已存 · ${info!.crc.toString(16).padStart(4, '0')}`;
    } else {
      tag.textContent = '空的';
    }

    $<HTMLButtonElement>(`#slot-send-${n}`).disabled = !stored || !st.connected || busy;
    $<HTMLButtonElement>(`#slot-clear-${n}`).disabled = !stored;

    // A stored slot with nothing on screen can be fetched back for a look.
    const box = $<HTMLDivElement>(`#slot-preview-${n}`);
    const empty = box.querySelector('canvas') === null;
    box.classList.toggle('loadable', stored && empty && !sources.has(n));
    if (stored && empty && !sources.has(n) && !box.querySelector('.placeholder[data-busy]')) {
      const ph = box.querySelector('.placeholder');
      if (ph && ph.textContent === '尚未選擇圖片') {
        ph.textContent = '已存在日曆上 · 點此預覽';
      }
    }
  }
}
