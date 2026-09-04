/*
 * The Tesserae tab.
 *
 * The calendar has four pages, so this is four independent clients, one per
 * page. The prose at the top -- what Tesserae is, how to stand a server up --
 * is shared; below it, one card per page carries that page's own server,
 * status, a preview of what it currently shows, and a settings form.
 *
 * Pointing a page at a server is a couple of fields, but knowing what to put
 * in them means having stood a container up and found a button three menus
 * deep, so the setup prose stays even though it repeats what a Tesserae user
 * may already know.
 */

import { api, type TesseraeClient, type TesseraeState } from '../lib/api';
import { framePreview } from '../lib/frame-preview';

const SLOTS = [1, 2, 3, 4];

const $ = <T extends HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

const STATE_LABEL: Record<TesseraeState, string> = {
  disabled: '未設定',
  unregistered: '等待核准',
  idle: '已連線',
  working: '更新中…',
  error: '發生問題',
};

function clientCard(slot: number): string {
  return `
    <section class="card tclient" id="tc-${slot}">
      <div class="slot-head">
        <h3>第 ${slot} 頁</h3>
        <span class="tag" id="tc-state-${slot}">未設定</span>
      </div>

      <div class="preview loadable" id="tc-preview-${slot}">
        <span class="placeholder">尚未收到圖片</span>
      </div>

      <div class="row"><span>上次更新</span><strong id="tc-last-${slot}">—</strong></div>
      <div class="row" id="tc-next-row-${slot}" hidden>
        <span>下次更新</span><strong id="tc-next-${slot}">—</strong>
      </div>
      <p class="error" id="tc-error-${slot}" hidden></p>

      <details class="tsettings" id="tc-settings-${slot}">
        <summary>設定這一頁的 server</summary>
        <form class="join" data-slot="${slot}">
          <label>
            <span>server 網址（區網 IP）</span>
            <input type="url" class="tc-url" placeholder="http://192.168.1.10:8765"
                   autocomplete="off" />
          </label>
          <label>
            <span>配對碼</span>
            <input type="text" class="tc-code" placeholder="Issue pairing code 給的碼"
                   autocomplete="off" />
          </label>
          <details class="notice">
            <summary>沒有配對碼？（不建議）</summary>
            <p>
              留空 server 網址以外的欄位，板子會報到並等你在
              Settings → Devices → Discovered 按 Register。或用 Add without
              pairing 拿到的 Device id 與 access token 填在下面（兩者都要）。
            </p>
            <label><span>Device id</span>
              <input type="text" class="tc-devid" placeholder="例如 ulani-page1"
                     autocomplete="off" /></label>
            <label><span>access token</span>
              <input type="text" class="tc-token" placeholder="Add without pairing 給的 token"
                     autocomplete="off" /></label>
          </details>
          <div class="actions">
            <button type="submit">儲存並連線</button>
            <button type="button" class="ghost tc-poll">立即更新</button>
            <button type="button" class="ghost tc-forget">清除</button>
          </div>
        </form>
      </details>
    </section>`;
}

export function tesseraeMarkup(): string {
  return `
    <section class="card">
      <h2>什麼是 tesserae</h2>
      <p class="hint">
        <a href="https://github.com/dmellok/tesserae">tesserae</a>
        是一套可以自己架設的服務，負責產生日曆畫面。要顯示什麼、長什麼樣子、
        多久更新一次，全部在它的網頁上設定。這台 ESP32 只按它指定的時間去把畫好的
        圖拿回來、透過藍牙送進 ULANI，<strong>不做任何影像處理</strong>。
      </p>
      <p class="hint">
        ULANI 有四頁，所以下面有四個獨立的客戶端，一頁一個。你可以讓四個不同的
        dashboard 分別餵四頁，也可以只設一兩頁、其餘留空。
      </p>
    </section>

    <section class="card">
      <h2>把 server 架起來</h2>
      <p class="hint">在 NAS 或任何能跑 Docker 的機器上：</p>
      <pre><code>mkdir tesserae &amp;&amp; cd tesserae
curl -fsSLO https://raw.githubusercontent.com/dmellok/tesserae/main/docker-compose.yml
docker compose up -d</code></pre>
      <p class="hint">
        開 <code>http://&lt;那台機器的IP&gt;:8765</code> 設定密碼並走完精靈。要拿配對碼：
        <strong>Settings → Devices → Add device → Transport 選 REST API
        → 按「+ Issue pairing code」</strong>。用配對碼時板子型號會被自動辨識成
        Seeed XIAO ePaper EE04，不用手動選。
      </p>
      <p class="hint warn">
        server 網址請填<strong>區網 IP</strong>，不要填外網網域——tesserae 預設只把
        圖片給區網內的用戶端，用外網網域會註冊成功但下載圖片時被回 403。
        配對碼是一次性的，失敗就回去重按一次拿新的。
      </p>
    </section>

    ${SLOTS.map(clientCard).join('')}
  `;
}

function describeNext(c: TesseraeClient): string {
  if (!c.registered) return '—';
  const s = c.secondsUntilPoll;
  if (s <= 0) return '即將更新';
  if (s < 60) return `${s} 秒後`;
  if (s < 3600) return `${Math.round(s / 60)} 分鐘後`;
  return `${(s / 3600).toFixed(1)} 小時後`;
}

function describeLast(epoch: number): string {
  if (!epoch) return '尚未收到';
  const d = new Date(epoch * 1000);
  return d.toLocaleString([], {
    month: 'numeric',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  });
}

/* Slots whose form has been pre-filled once, so polling never clobbers typing. */
const filled = new Set<number>();

function input(slot: number, cls: string): HTMLInputElement {
  return $(`#tc-${slot} .${cls}`) as HTMLInputElement;
}

function renderClient(c: TesseraeClient) {
  const n = c.slot;
  $(`#tc-state-${n}`).textContent = STATE_LABEL[c.state] ?? c.state;
  $(`#tc-last-${n}`).textContent = describeLast(c.lastFrameEpoch);

  $<HTMLDivElement>(`#tc-next-row-${n}`).hidden = !c.registered;
  $(`#tc-next-${n}`).textContent = describeNext(c);

  const err = $<HTMLParagraphElement>(`#tc-error-${n}`);
  err.hidden = !c.error;
  err.textContent = c.error;

  const box = $<HTMLDivElement>(`#tc-preview-${n}`);
  const hasCanvas = box.querySelector('canvas') !== null;
  box.classList.toggle('loadable', c.hasFrame && !hasCanvas);
  if (!hasCanvas) {
    const ph = box.querySelector('.placeholder');
    if (ph) ph.textContent = c.hasFrame ? '已有圖片 · 點此預覽' : '尚未收到圖片';
  }

  if (!filled.has(n) && c.serverUrl) {
    filled.add(n);
    input(n, 'tc-url').value = c.serverUrl;
    input(n, 'tc-devid').value = c.deviceId;
  }
}

export function renderTesserae(clients: TesseraeClient[]) {
  for (const c of clients) renderClient(c);
}

function placeholder(text: string): HTMLElement {
  const span = document.createElement('span');
  span.className = 'placeholder';
  span.textContent = text;
  return span;
}

function setPreview(slot: number, node: HTMLElement) {
  const box = $<HTMLDivElement>(`#tc-preview-${slot}`);
  box.innerHTML = '';
  box.appendChild(node);
}

export function mountTesserae(guard: (fn: () => Promise<unknown>) => void) {
  for (const n of SLOTS) {
    const card = $(`#tc-${n}`);

    card.querySelector('form')!.addEventListener('submit', (ev) => {
      ev.preventDefault();
      const url = input(n, 'tc-url').value.trim();
      if (!url) return;
      guard(() =>
        api.tesseraeConnect({
          slot: n,
          serverUrl: url,
          pairingCode: input(n, 'tc-code').value.trim(),
          deviceId: input(n, 'tc-devid').value.trim(),
          token: input(n, 'tc-token').value.trim(),
        }),
      );
    });

    card.querySelector('.tc-poll')!.addEventListener('click', () =>
      guard(() => api.tesseraePoll(n)),
    );

    card.querySelector('.tc-forget')!.addEventListener('click', () =>
      guard(async () => {
        await api.tesseraeForget(n);
        filled.delete(n);
        for (const cls of ['tc-url', 'tc-code', 'tc-devid', 'tc-token']) {
          input(n, cls).value = '';
        }
        setPreview(n, placeholder('尚未收到圖片'));
      }),
    );

    $(`#tc-preview-${n}`).addEventListener('click', () => {
      const box = $<HTMLDivElement>(`#tc-preview-${n}`);
      if (!box.classList.contains('loadable')) return;
      guard(async () => {
        setPreview(n, placeholder('讀取中…'));
        const bytes = await api.slotImage(n);
        setPreview(n, framePreview(bytes));
        box.classList.remove('loadable');
      });
    });
  }
}
