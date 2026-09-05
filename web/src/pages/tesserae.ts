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
import { $, guard } from '../lib/ui';

const SLOTS = [1, 2, 3, 4];

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
      <button type="button" class="slot-head tc-toggle" id="tc-toggle-${slot}"
              aria-expanded="false">
        <h3>第 ${slot} 頁</h3>
        <span class="tag" id="tc-state-${slot}">未設定</span>
        <span class="chevron" aria-hidden="true">▾</span>
      </button>

      <div class="tclient-body" id="tc-body-${slot}" hidden>
      <div class="preview loadable" id="tc-preview-${slot}">
        <span class="placeholder">尚未收到圖片</span>
      </div>

      <div class="row"><span>拿到新圖片時間</span><strong id="tc-last-${slot}">—</strong></div>
      <div class="row"><span>上次送出時間</span><strong id="tc-sent-${slot}">—</strong></div>
      <div class="row" id="tc-check-row-${slot}" hidden>
        <span>上次檢查時間</span><strong id="tc-check-${slot}">—</strong>
      </div>
      <div class="row" id="tc-next-row-${slot}" hidden>
        <span>下次向 Tesserae 詢問</span><strong id="tc-next-${slot}">—</strong>
      </div>
      <p class="error" id="tc-error-${slot}" hidden></p>

      <div class="actions">
        <button type="button" class="tc-poll">從 Tesserae 更新資料</button>
        <button type="button" class="tc-send">把圖片送到 ULANI 電子日曆</button>
      </div>

      <label class="toggle">
        <input type="checkbox" class="tc-badge">
        <span>在右下角標上頁碼（${slot}）</span>
      </label>
      <p class="hint">改這個之後，下一次送圖才會套用；想馬上看效果就按上面的送出。</p>

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
            <button type="button" class="ghost tc-forget">清除</button>
          </div>
        </form>
      </details>
      </div>
    </section>`;
}

export function tesseraeMarkup(): string {
  return `
    <section class="card">
      <h2>什麼是 Tesserae</h2>
      <p class="hint">
        <a href="https://github.com/dmellok/tesserae">Tesserae</a>
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
        server 網址請填<strong>區網 IP</strong>，不要填外網網域——Tesserae 預設只把
        圖片給區網內的用戶端，用外網網域會註冊成功但下載圖片時被回 403。
        配對碼是一次性的，失敗就回去重按一次拿新的。
      </p>
    </section>

    <p class="hint warn" id="tc-nocal" hidden>
      目前沒有連上日曆。設定與「從 Tesserae 更新資料」都能照常使用；按「把圖片送到
      ULANI 電子日曆」時，板子會先自動連上日曆再送出。
    </p>

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

function fmtTime(epoch: number): string {
  return new Date(epoch * 1000).toLocaleString([], {
    month: 'numeric',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  });
}

function describeLast(epoch: number, hasFrame: boolean): string {
  if (!epoch) return hasFrame ? '時間未知' : '尚未收到';
  return fmtTime(epoch);
}

/* Slots whose form has been pre-filled once, so polling never clobbers typing. */
const filled = new Set<number>();

/* Slots whose expand state has been decided, so the default only applies once. */
const expandDecided = new Set<number>();

function expand(slot: number, on: boolean, remember = true) {
  $<HTMLDivElement>(`#tc-body-${slot}`).hidden = !on;
  $(`#tc-toggle-${slot}`).setAttribute('aria-expanded', String(on));
  $(`#tc-${slot}`).classList.toggle('open', on);
  if (remember) {
    localStorage.setItem(`ulani.tc.${slot}`, on ? '1' : '0');
  }
}

function input(slot: number, cls: string): HTMLInputElement {
  return $(`#tc-${slot} .${cls}`) as HTMLInputElement;
}

function renderClient(c: TesseraeClient) {
  const n = c.slot;

  /*
   * First time we hear about a slot, open it if the user has an explicit
   * preference or if it is already configured -- an unused page stays a
   * one-line header the user can expand when they want it.
   */
  if (!expandDecided.has(n)) {
    expandDecided.add(n);
    const saved = localStorage.getItem(`ulani.tc.${n}`);
    const on = saved !== null ? saved === '1' : c.state !== 'disabled';
    expand(n, on, false);
  }

  $(`#tc-state-${n}`).textContent = STATE_LABEL[c.state] ?? c.state;
  $(`#tc-last-${n}`).textContent = describeLast(c.lastFrameEpoch, c.hasFrame);
  $(`#tc-sent-${n}`).textContent = c.lastSentEpoch ? fmtTime(c.lastSentEpoch) : '尚未送出';

  $<HTMLDivElement>(`#tc-check-row-${n}`).hidden = !c.registered;
  $(`#tc-check-${n}`).textContent = c.lastCheckEpoch ? fmtTime(c.lastCheckEpoch) : '—';

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

  /* Nothing to send until a frame has been stored for this page. */
  ($(`#tc-${n} .tc-send`) as HTMLButtonElement).disabled = !c.hasFrame;

  /* Reflect the stored badge setting, but never while the user is toggling it. */
  const badge = $(`#tc-${n} .tc-badge`) as HTMLInputElement;
  if (document.activeElement !== badge) {
    badge.checked = c.badge;
  }

  if (!filled.has(n) && c.serverUrl) {
    filled.add(n);
    input(n, 'tc-url').value = c.serverUrl;
    input(n, 'tc-devid').value = c.deviceId;
  }
}

export function renderTesserae(clients: TesseraeClient[], calendarConnected = false) {
  /*
   * When the calendar is not linked, the cards read as muted and the tab shows
   * a note -- but sending stays enabled, because a send auto-connects first.
   */
  $<HTMLParagraphElement>('#tc-nocal').hidden = calendarConnected;
  for (const c of clients) {
    renderClient(c);
    $(`#tc-${c.slot}`).classList.toggle('no-calendar', !calendarConnected);
  }
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

export function mountTesserae() {
  for (const n of SLOTS) {
    const card = $(`#tc-${n}`);

    $(`#tc-toggle-${n}`).addEventListener('click', () => {
      const open = $(`#tc-toggle-${n}`).getAttribute('aria-expanded') === 'true';
      expand(n, !open);
    });

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

    card.querySelector('.tc-send')!.addEventListener('click', () =>
      guard(() => api.sendSlot(n)),
    );

    card.querySelector('.tc-badge')!.addEventListener('change', (ev) =>
      guard(() => api.setSlotBadge(n, (ev.target as HTMLInputElement).checked)),
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
