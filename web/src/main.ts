import './style.css';
import { api, type Status, type WifiStatus } from './lib/api';
import { mountTesserae, renderTesserae, tesseraeMarkup } from './pages/tesserae';

const STATE_LABEL: Record<Status['state'], string> = {
  off: '藍牙未啟動',
  idle: '待機',
  scanning: '掃描中…',
  connecting: '連線中…',
  discovering: '取得服務中…',
  ready: '已連線',
  transferring: '傳送圖片中…',
};

let busy = false;
let lastDeviceKey = '';
let savedAddress: string | null = null;
let calendarConnected = false;

const $ = <T extends HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

document.querySelector<HTMLDivElement>('#app')!.innerHTML = `
  <header class="appbar">
    <h1>ULANI</h1>
    <div class="appbar-status">
      <button class="ghost mini" id="bar-connect" hidden>連線</button>
      <span class="chip"><span class="dot" id="bar-dot"></span><span id="bar-device">—</span></span>
      <span class="ringwrap" id="bar-batt" hidden title="電量">
        <span class="ring" id="bar-batt-ring"></span><span id="bar-batt-pct"></span>
      </span>
      <span class="ringwrap" id="bar-xfer" hidden title="傳送進度">
        <span class="ring xfer" id="bar-xfer-ring"></span><span id="bar-xfer-pct"></span>
      </span>
      <button class="iconbtn" id="btn-settings" title="設定" aria-label="設定">⚙</button>
    </div>
  </header>
  <section class="card" id="status-card">
    <div class="row"><span>狀態</span><strong id="s-state">—</strong></div>
    <div class="row"><span>裝置</span><strong id="s-device">未連線</strong></div>
    <div class="row"><span>目前相框</span><strong id="s-slot">—</strong></div>
    <div class="row meter-row" id="s-battery-row">
      <div class="rowfill" id="s-battery-fill"></div>
      <span>電量</span><strong id="s-battery">—</strong>
    </div>
    <div class="row meter-row" id="s-progress-row">
      <div class="rowfill" id="s-fill"></div>
      <span>傳送進度</span><strong id="s-pct">—</strong>
    </div>
    <p class="error" id="s-error" hidden></p>
  </section>

  <nav class="tabs" id="tabs">
    <button data-tab="calendar" class="active">日曆</button>
    <button data-tab="wifi">WiFi</button>
    <button data-tab="tesserae">Tesserae</button>
  </nav>

  <div class="panel" data-panel="calendar">
    <section class="card">
      <h2>1. 連上日曆</h2>
      <ol class="steps">
        <li>把日曆放在 ESP32 旁邊，確認它已開機。</li>
        <li>如果官方 App 或電腦正連著它，請先關掉——一次只能有一個裝置連線。</li>
        <li>按下「搜尋日曆」，選擇你的裝置。</li>
      </ol>

      <details class="notice">
        <summary>連不上 / connect: ESP_FAIL？先回復出廠預設值</summary>
        <p>
          日曆只會記住一個配對過的裝置，並拒絕跟其他裝置建立配對——這時搜尋得到裝置、
          但一按連線就在配對階段被日曆中斷（<code>connect: ESP_FAIL</code>）。這會發生在
          兩種情況：日曆曾和官方 App 配對過；或這片板子更新／從 <code>0x0</code> 重刷過
          韌體，板子這邊的配對金鑰被清掉、和日曆對不上了。移除官方 App 不會清掉日曆的
          紀錄，必須在日曆上回復出廠預設值。
        </p>
        <p class="quote">
          按著【功能鍵】再戳【重置孔】一下，直到 LED 燈開始閃爍後，再放開【功能鍵】。
          螢幕會開始顯示回復出廠值的操作指示圖。
        </p>
        <p class="src">操作步驟引自 ULANI 官方說明：ulani.com.tw/ulani-app.html</p>
      </details>

      <div class="row" id="saved-row" hidden>
        <span>已記住</span><strong id="saved-name">—</strong>
      </div>
      <p class="hint" id="saved-hint" hidden>
        每次通電都會自動連上這台，不用再按搜尋。
      </p>

      <div class="actions">
        <button id="btn-scan">搜尋日曆</button>
        <button id="btn-disconnect" class="ghost">中斷連線</button>
        <button id="btn-forget" class="ghost" hidden>清除記住的裝置</button>
      </div>
      <ul class="devices" id="device-list"><li class="empty">尚未搜尋</li></ul>
    </section>
    <section class="card" id="test-card">
      <h2>2. 測試</h2>
      <p class="hint">
        送一張隨機色塊測試圖，確認整條連線與傳輸都正常。
        日曆一次只能更新一張，下面選的那一張會被覆蓋。傳輸約需 30–60 秒。
      </p>
      <h3>送測試圖到第幾張</h3>
      <div class="slots" id="send-buttons">
        ${[1, 2, 3, 4].map((n) => `<button data-send="${n}">${n}</button>`).join('')}
      </div>
      <label class="toggle">
        <input type="checkbox" id="send-activate">
        <span>傳完後切到那一張</span>
      </label>
      <p class="hint">
        上傳歸上傳，切換頁面歸切換頁面：預設只把圖寫進去，畫面留在原本那一張。
        寫進去的剛好就是正在顯示的那一張時，會自動重繪，否則畫面會停在舊圖。
        日曆重繪要花大半分鐘，這段期間收不了下一張，所以連續換好幾張時別勾。
      </p>
      <h3>切換目前顯示的那一張</h3>
      <div class="slots" id="slot-buttons">
        ${[1, 2, 3, 4].map((n) => `<button data-slot="${n}">${n}</button>`).join('')}
      </div>
    </section>
  </div>

  <div class="panel" data-panel="tesserae" hidden>
    ${tesseraeMarkup()}
  </div>

  <div class="panel" data-panel="wifi" hidden>
    <section class="card" id="wifi-card">
      <h2>連上家裡的 WiFi</h2>
      <p class="hint">
        連上之後就能從電腦瀏覽器直接開這個頁面，不必再切換到 ESP32 的熱點。
        熱點會一直開著，設定失敗也還是連得回來。
      </p>

      <div class="row"><span>狀態</span><strong id="w-state">—</strong></div>
      <div class="row" id="w-addr-row" hidden>
        <span>家用網路網址</span><strong id="w-addr">—</strong>
      </div>
      <div class="row">
        <span>熱點網址</span>
        <strong><a href="http://192.168.4.1/">http://192.168.4.1/</a></strong>
      </div>

      <details class="notice">
        <summary>連上之後手機會短暫斷線，這是正常的</summary>
        <p>
          ESP32 只有一組無線電，熱點和家用網路必須待在同一個頻道上（所有型號都一樣，
          換成 S3 也不會改變）。連上路由器時熱點會被迫換到路由器的頻道，正連著熱點的
          裝置會斷線幾秒再自動連回來。
        </p>
        <p>
          <strong>熱點本身不會關閉。</strong>SSID 照常廣播，上面那個
          <code>192.168.4.1</code> 一直都通，就算家用網路設錯或路由器關機也一樣。
        </p>
      </details>

      <div class="actions">
        <button id="btn-wifi-scan">搜尋 WiFi</button>
        <button id="btn-wifi-forget" class="ghost">清除已存的網路</button>
      </div>

      <ul class="devices" id="wifi-list"><li class="empty">尚未搜尋</li></ul>

      <form class="join" id="wifi-form" hidden>
        <label>
          <span id="w-join-ssid"></span>
          <input type="password" id="w-pass" placeholder="WiFi 密碼" autocomplete="off" />
        </label>
        <div class="actions">
          <button type="submit">連線</button>
          <button type="button" id="btn-wifi-cancel" class="ghost">取消</button>
        </div>
      </form>
    </section>
  </div>

  <div class="panel" data-panel="settings" hidden>
    <section class="card" id="settings-card">
      <h2>連線設定</h2>
      <label class="field">
        <span>閒置多久後斷線省電</span>
        <select id="idle-timeout">
          <option value="60000">1 分鐘</option>
          <option value="300000">5 分鐘</option>
          <option value="900000">15 分鐘</option>
          <option value="1800000">30 分鐘</option>
          <option value="0">一直保持連線</option>
        </select>
      </label>
      <p class="hint">
        傳完圖或手動連線後會先保持連線這麼久，閒置超過就自動斷線——連著卻閒著會讓
        板子持續耗電、發熱。之後要送圖時板子會自己重新連回來，不需要有人在旁邊按，
        所以斷線不影響自動更新。選「一直保持連線」就不會主動斷開。
      </p>
    </section>
    <section class="card" id="backup-card">
      <h2>設定備份</h2>
      <p class="hint">
        用網頁刷韌體是從 <code>0x0</code> 整個蓋掉，會連記住的日曆、WiFi、Tesserae
        設定、以及和日曆的<strong>配對</strong>一起清掉（<code>idf.py flash</code> 只寫
        程式那幾塊，所以不會）。刷機前先匯出、刷完再匯入就能救回來——連配對一起還原，
        所以<strong>不用再把日曆回復原廠</strong>。圖片不含在內，會從 Tesserae 或重新
        上傳補回。備份含配對金鑰，只適用於同一片板子。
      </p>
      <div class="actions">
        <button type="button" id="btn-export">匯出設定</button>
        <button type="button" id="btn-import">匯入設定…</button>
        <input type="file" id="import-file" accept="application/json,.json" hidden />
      </div>
      <p class="hint" id="backup-msg" hidden></p>
    </section>
  </div>

  <footer>
    <p class="version" id="fw-version"></p>
  </footer>
`;

/* ----------------------------------------------------------------- tabs */

const TAB_KEY = 'ulani.tab';

function selectTab(name: string) {
  document.querySelectorAll<HTMLButtonElement>('#tabs button').forEach((b) => {
    b.classList.toggle('active', b.dataset.tab === name);
  });
  document.querySelectorAll<HTMLElement>('.panel').forEach((p) => {
    p.hidden = p.dataset.panel !== name;
  });
  $('#btn-settings').classList.toggle('active', name === 'settings');
  localStorage.setItem(TAB_KEY, name);
}

$('#tabs').addEventListener('click', (ev) => {
  const btn = (ev.target as HTMLElement).closest<HTMLButtonElement>('button[data-tab]');
  if (btn) selectTab(btn.dataset.tab!);
});

// The gear opens a settings view that lives outside the tab strip.
$('#btn-settings').addEventListener('click', () => selectTab('settings'));

/*
 * Joining a network reloads nothing, but the browser may well be pointed at a
 * new address afterwards -- remembering the tab keeps the user where they were.
 */
selectTab(localStorage.getItem(TAB_KEY) ?? 'calendar');

function setBusy(value: boolean) {
  busy = value;
  /* Tabs stay live: switching views is not an action against the device. */
  document.querySelectorAll<HTMLButtonElement>('.panel button').forEach((b) => {
    b.disabled = value;
  });
}

async function guard(fn: () => Promise<unknown>) {
  if (busy) return;
  setBusy(true);
  try {
    await fn();
  } catch (err) {
    showError(err instanceof Error ? err.message : String(err));
  } finally {
    setBusy(false);
  }
}

/* Turn a few known backend errors into something a user can act on. */
function friendlyError(msg: string): string {
  if (msg.includes('pairing refused')) {
    return '日曆拒絕配對——它可能還記得舊的配對（曾配過官方 App，或韌體更新過）。'
      + '請將日曆回復原廠設定後再連一次。';
  }
  return msg;
}

function showError(msg: string) {
  const el = $<HTMLParagraphElement>('#s-error');
  el.textContent = friendlyError(msg);
  el.hidden = !msg;
}

function renderDevices(devices: Status['devices'], scanning: boolean) {
  // Rebuilding this list on every poll would fight the user for taps.
  const key = scanning + devices.map((d) => d.address).join();
  if (key === lastDeviceKey) return;
  lastDeviceKey = key;

  const list = $<HTMLUListElement>('#device-list');
  if (devices.length === 0) {
    list.innerHTML = scanning
      ? '<li class="empty">搜尋中…</li>'
      : '<li class="empty">沒有找到日曆，確認它已開機且沒有被別的裝置佔用</li>';
    return;
  }
  list.innerHTML = devices
    .map(
      (d) => `
      <li>
        <div>
          <strong>${d.name}</strong>
          <small>${d.address} · ${d.rssi} dBm</small>
        </div>
        <button data-address="${d.address}">連線</button>
      </li>`,
    )
    .join('');
}

/*
 * The panel answers `06 00` with `06 <level>`. The scale is not documented
 * anywhere and was read off a live device, so the raw reply stays on screen
 * next to the percentage -- if the assumption is wrong it is visible at a
 * glance rather than quietly misleading.
 */
/*
 * The calendar never tells us anything on its own, so every number on screen is
 * as old as the last time the firmware asked. Showing that age turns "the
 * battery says 5%" into "the battery said 5%, twelve seconds ago", and makes a
 * link that is up but no longer answering visible instead of silent.
 */
function ago(ms: number): string {
  const s = Math.round(ms / 1000);
  if (s < 60) return `${s} 秒前`;
  const m = Math.round(s / 60);
  return m < 60 ? `${m} 分鐘前` : `${Math.round(m / 60)} 小時前`;
}

/*
 * A reading is only ever as fresh as the last time the firmware asked, so each
 * value carries its own age -- battery and slot age independently now that the
 * keepalive reads the slot every tick and the battery once a minute. When the
 * link is down (the board hands the calendar back when idle) the numbers are
 * frozen at their last value; mark them so a stale reading is never mistaken
 * for a live one.
 */
const STALE_MS = 90_000;

function ageBadge(ms: number | undefined, connected: boolean): string {
  if (ms === undefined) return '';
  const frozen = !connected;
  const stale = frozen || ms > STALE_MS;
  const note = frozen ? '，已離線' : '';
  return ` <small class="age${stale ? ' stale' : ''}">${ago(ms)}${note}</small>`;
}

function renderBattery(st: Status) {
  const row = $<HTMLDivElement>('#s-battery-row');
  const fill = $<HTMLDivElement>('#s-battery-fill');

  if (st.batteryLevel === undefined) {
    $('#s-battery').textContent = '—';
    fill.style.width = '0';
    row.classList.remove('low');
    return;
  }

  const pct = Math.max(0, Math.min(100, st.batteryLevel));
  const raw = `0x${st.batteryRaw.toString(16).padStart(4, '0')}`;
  $('#s-battery').innerHTML =
    `${pct}% <small>${raw}</small>${ageBadge(st.batteryAgeMs, st.connected)}`;

  fill.style.width = `${pct}%`;
  row.classList.toggle('low', pct <= 20);
}

/*
 * The always-visible top bar: a connection dot, the device name, and small
 * pie rings for battery and (only mid-transfer) send progress. It repeats a
 * little of the status card on purpose -- the card scrolls away, this does not.
 */
function renderStatusbar(st: Status) {
  calendarConnected = st.connected;
  const dot = $('#bar-dot');
  dot.classList.toggle('on', st.connected);

  const saved = st.savedDevice;
  savedAddress = saved?.address ?? null;
  $('#bar-device').textContent = st.connected
    ? st.name || 'ULANI'
    : saved
      ? saved.name || saved.address
      : '未連線';

  /* Offer a one-tap connect when a calendar is remembered but not linked. */
  $<HTMLButtonElement>('#bar-connect').hidden = !(saved && !st.connected);

  const batt = $<HTMLElement>('#bar-batt');
  if (st.connected && st.batteryLevel !== undefined) {
    const pct = Math.max(0, Math.min(100, st.batteryLevel));
    batt.hidden = false;
    const ring = $('#bar-batt-ring');
    ring.style.setProperty('--p', String(pct));
    ring.style.setProperty('--rc', pct <= 20 ? 'var(--danger)' : 'var(--accent)');
    $('#bar-batt-pct').textContent = `${pct}%`;
  } else {
    batt.hidden = true;
  }

  const xfer = $<HTMLElement>('#bar-xfer');
  if (st.transfer.active && st.transfer.total > 0) {
    const pct = Math.round((st.transfer.sent / st.transfer.total) * 100);
    xfer.hidden = false;
    $('#bar-xfer-ring').style.setProperty('--p', String(pct));
    $('#bar-xfer-pct').textContent = `${pct}%`;
  } else {
    xfer.hidden = true;
  }
}

function renderStatus(st: Status) {
  renderStatusbar(st);
  $('#s-state').textContent = STATE_LABEL[st.state] ?? st.state;
  $('#s-device').textContent = st.connected
    ? `${st.name || 'ULANI'} (${st.address})`
    : '未連線';
  $('#s-slot').innerHTML =
    (st.activeSlot ? String(st.activeSlot) : '—') + ageBadge(st.slotAgeMs, st.connected);
  renderBattery(st);
  renderIdleTimeout(st);

  if (!busy) showError(st.error);

  /*
   * The transfer row stays put; only its fill comes and goes. Popping the
   * whole row in on a transfer shifted everything below it, which read as the
   * UI jumping.
   */
  const transferring = st.transfer.active && st.transfer.total > 0;
  const fill = $<HTMLDivElement>('#s-fill');
  if (transferring) {
    const pct = Math.round((st.transfer.sent / st.transfer.total) * 100);
    fill.style.width = `${pct}%`;
    /*
     * A transfer restarts from 0 on a retry; say so, or the bar dropping back
     * and climbing again looks like it stalled.
     */
    const retry = st.transfer.attempt > 1 ? `　重試 ${st.transfer.attempt}` : '';
    $('#s-pct').textContent = `第 ${st.transfer.slot} 張 · ${pct}%${retry}`;
  } else {
    fill.style.width = '0';
    $('#s-pct').textContent = '—';
  }

  document.querySelectorAll<HTMLButtonElement>('#send-buttons button').forEach((b) => {
    b.classList.toggle('active',
      st.transfer.active && Number(b.dataset.send) === st.transfer.slot);
  });

  document.querySelectorAll<HTMLButtonElement>('#slot-buttons button').forEach((b) => {
    b.classList.toggle('active', Number(b.dataset.slot) === st.activeSlot);
  });

  $<HTMLElement>('#test-card').classList.toggle('disabled', !st.connected);

  renderDevices(st.devices, st.state === 'scanning');
  renderSaved(st);
}

/* Reflect the stored keep-alive choice, but never while the user is changing it. */
function renderIdleTimeout(st: Status) {
  const sel = $<HTMLSelectElement>('#idle-timeout');
  if (document.activeElement !== sel) {
    sel.value = String(st.idleTimeoutMs);
  }
}

function renderSaved(st: Status) {
  const saved = st.savedDevice;
  $<HTMLDivElement>('#saved-row').hidden = !saved;
  $<HTMLParagraphElement>('#saved-hint').hidden = !saved?.autoConnect;
  $<HTMLButtonElement>('#btn-forget').hidden = !saved;

  if (saved) {
    const label = saved.name || saved.address;
    $('#saved-name').innerHTML = saved.autoConnect
      ? `${label} <small>${saved.address}</small>`
      : `${label} <small>${saved.address} · 自動連線已停用</small>`;
  }
}

/* --------------------------------------------------------------- events */

// Scanning is asynchronous in the firmware; the status poll shows the results.
$('#btn-scan').addEventListener('click', () => guard(() => api.scan()));

$('#btn-disconnect').addEventListener('click', () => guard(() => api.disconnect()));

// Header quick-connect: reach for the remembered calendar without hunting for it.
$('#bar-connect').addEventListener('click', () => {
  if (savedAddress) guard(() => api.connect(savedAddress!));
});

$('#btn-forget').addEventListener('click', () => guard(() => api.forgetDevice()));

// One image per transfer: the slot the user picks is the only one touched.
$('#send-buttons').addEventListener('click', (ev) => {
  const btn = (ev.target as HTMLElement).closest<HTMLButtonElement>('button[data-send]');
  if (!btn) return;
  const slot = Number(btn.dataset.send);
  const activate = $<HTMLInputElement>('#send-activate').checked;
  guard(() => api.testImage(slot, Math.floor(Math.random() * 0xffffff) + 1, activate));
});

$('#device-list').addEventListener('click', (ev) => {
  const btn = (ev.target as HTMLElement).closest<HTMLButtonElement>('button[data-address]');
  if (!btn) return;
  guard(() => api.connect(btn.dataset.address!));
});

$('#slot-buttons').addEventListener('click', (ev) => {
  const btn = (ev.target as HTMLElement).closest<HTMLButtonElement>('button[data-slot]');
  if (!btn) return;
  guard(() => api.setSlot(Number(btn.dataset.slot)));
});

$('#idle-timeout').addEventListener('change', (ev) => {
  guard(() => api.setIdleTimeout(Number((ev.target as HTMLSelectElement).value)));
});

/* ------------------------------------------------------ settings backup */

function backupMsg(text: string, isError = false) {
  const el = $<HTMLParagraphElement>('#backup-msg');
  el.textContent = text;
  el.hidden = !text;
  el.classList.toggle('error', isError);
}

$('#btn-export').addEventListener('click', () =>
  guard(async () => {
    backupMsg('');
    const res = await fetch(api.settingsExportUrl);
    if (!res.ok) throw new Error(`匯出失敗（${res.status}）`);
    const blob = await res.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'ulani-settings.json';
    a.click();
    URL.revokeObjectURL(url);
    backupMsg('已匯出設定檔，收好它，刷機後就靠它還原。');
  }),
);

$('#btn-import').addEventListener('click', () => $('#import-file').click());

$('#import-file').addEventListener('change', (ev) => {
  const input = ev.target as HTMLInputElement;
  const file = input.files?.[0];
  input.value = ''; // let the same file be picked again after an error
  if (!file) return;
  guard(async () => {
    backupMsg('');
    const text = await file.text();
    try {
      JSON.parse(text);
    } catch {
      backupMsg('這不是有效的設定檔。', true);
      return;
    }
    const r = await api.importSettings(text);
    backupMsg(`已還原 ${r.restored} 筆設定，裝置正在重新啟動…重整頁面前請稍候幾秒。`);
  });
});

/* ---------------------------------------------------------------- wifi */

const WIFI_LABEL: Record<WifiStatus['state'], string> = {
  disabled: '未設定',
  connecting: '連線中…',
  connected: '已連線',
  failed: '連線失敗',
};

let wifiKey = '';
let joining: string | null = null;

function renderWifi(w: WifiStatus) {
  const state = WIFI_LABEL[w.state] ?? w.state;
  $('#w-state').textContent = w.ssid ? `${state}（${w.ssid}）` : state;

  const addrRow = $<HTMLDivElement>('#w-addr-row');
  addrRow.hidden = w.state !== 'connected' || !w.ip;
  if (!addrRow.hidden) {
    $('#w-addr').innerHTML = `<a href="http://${w.ip}/">http://${w.ip}/</a>`;
  }

  // Same guard as the BLE list: do not rebuild under the user's finger.
  const key = `${w.scanning}|${w.networks.map((n) => n.ssid).join()}`;
  if (key === wifiKey) return;
  wifiKey = key;

  const list = $<HTMLUListElement>('#wifi-list');
  if (w.scanning) {
    list.innerHTML = '<li class="empty">搜尋中…</li>';
    return;
  }
  if (w.networks.length === 0) {
    list.innerHTML = '<li class="empty">尚未搜尋</li>';
    return;
  }
  list.innerHTML = w.networks
    .map(
      (n) => `
      <li>
        <div>
          <strong>${n.ssid}</strong>
          <small>${n.rssi} dBm${n.open ? ' · 開放' : ''}</small>
        </div>
        <button data-ssid="${n.ssid}" data-open="${n.open}">選擇</button>
      </li>`,
    )
    .join('');
}

function showJoinForm(ssid: string, open: boolean) {
  joining = ssid;
  $('#w-join-ssid').textContent = ssid;
  const pass = $<HTMLInputElement>('#w-pass');
  pass.value = '';
  pass.hidden = open;
  $<HTMLFormElement>('#wifi-form').hidden = false;
  if (!open) pass.focus();
}

function hideJoinForm() {
  joining = null;
  $<HTMLFormElement>('#wifi-form').hidden = true;
}

$('#btn-wifi-scan').addEventListener('click', () => guard(() => api.wifiScan()));

$('#btn-wifi-forget').addEventListener('click', () =>
  guard(async () => {
    await api.wifiForget();
    hideJoinForm();
  }),
);

$('#wifi-list').addEventListener('click', (ev) => {
  const btn = (ev.target as HTMLElement).closest<HTMLButtonElement>('button[data-ssid]');
  if (!btn) return;
  showJoinForm(btn.dataset.ssid!, btn.dataset.open === 'true');
});

$('#btn-wifi-cancel').addEventListener('click', hideJoinForm);

$('#wifi-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  if (!joining) return;
  const ssid = joining;
  const password = $<HTMLInputElement>('#w-pass').value;
  guard(async () => {
    await api.wifiConnect(ssid, password);
    hideJoinForm();
  });
});

mountTesserae(guard);

/* ---------------------------------------------------------------- poll */

/*
 * One request at a time, never overlapping: the ESP32 has a small socket pool
 * shared with the captive-portal DNS, and a phone already holds several
 * keep-alive connections open on its own.
 */
/*
 * The board hands the calendar back when it sits idle, so opening this page to
 * a remembered-but-disconnected device means the numbers on screen are frozen.
 * Reach for it once on load -- explicitly, not as a side effect of every poll
 * -- so the page comes up live. The firmware re-arms auto-reconnect when it
 * connects, so this is the only nudge it needs.
 */
let connectOnLoadDone = false;
function maybeConnectOnLoad(st: Status) {
  if (connectOnLoadDone) return;
  connectOnLoadDone = true;
  if (st.savedDevice && !st.connected) {
    api.connect(st.savedDevice.address).catch(() => {});
  }
}

/*
 * A single dropped poll is not worth shouting about -- a phone juggling the
 * hotspot and its own keep-alives drops one now and then. Only call it lost
 * after several in a row, and clear the moment one succeeds.
 */
let pollFails = 0;
const POLL_FAIL_LIMIT = 3;

async function poll() {
  try {
    const st = await api.status();
    pollFails = 0;
    renderStatus(st);
    maybeConnectOnLoad(st);
    renderWifi(await api.wifi());
    renderTesserae((await api.tesserae()).clients, calendarConnected);
  } catch {
    if (++pollFails >= POLL_FAIL_LIMIT) {
      $('#s-state').textContent = '無法連上 ESP32';
    }
  }
  setTimeout(poll, 2000);
}

/*
 * The firmware version is baked in at build time and never changes while the
 * board runs, so fetch it once and drop it in the footer. "dev" (an untagged
 * local build) is shown as such; a real version marks a production build.
 */
api
  .version()
  .then((v) => {
    $('#fw-version').textContent = v.production
      ? `韌體版本 ${v.version}`
      : `開發版（${v.version}）`;
  })
  .catch(() => {
    /* Old firmware without /api/version, or offline: leave the line blank. */
  });

poll();
