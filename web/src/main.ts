import './style.css';
import { api, type Status, type WifiStatus } from './lib/api';

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

const $ = <T extends HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

document.querySelector<HTMLDivElement>('#app')!.innerHTML = `
  <header>
    <h1>ULANI 電子日曆</h1>
    <p class="sub">透過 ESP32 更新畫面</p>
  </header>
  <section class="card" id="status-card">
    <div class="row"><span>狀態</span><strong id="s-state">—</strong></div>
    <div class="row"><span>裝置</span><strong id="s-device">未連線</strong></div>
    <div class="row"><span>目前相框</span><strong id="s-slot">—</strong></div>
    <div class="row"><span>電量</span><strong id="s-battery">—</strong></div>
    <div class="meter" id="s-battery-meter" hidden>
      <div class="bar"><div class="fill" id="s-battery-fill"></div></div>
    </div>
    <div class="row" id="s-progress-row" hidden>
      <span>傳送進度</span><strong id="s-pct">—</strong>
    </div>
    <div class="meter" id="s-progress" hidden>
      <div class="bar"><div class="fill" id="s-fill"></div></div>
    </div>
    <p class="error" id="s-error" hidden></p>
  </section>

  <nav class="tabs" id="tabs">
    <button data-tab="calendar" class="active">日曆</button>
    <button data-tab="wifi">WiFi</button>
  </nav>

  <div class="panel" data-panel="calendar">
    <section class="card">
      <h2>1. 連上日曆</h2>
      <ol class="steps">
        <li>把日曆放在 ESP32 旁邊，確認它已開機。</li>
        <li>如果官方 App 或電腦正連著它，請先關掉——一次只能有一個裝置連線。</li>
        <li><strong>如果日曆曾經和官方 App 配對過，需要先回復出廠預設值</strong>（見下方說明）。</li>
        <li>按下「搜尋日曆」，選擇你的裝置。</li>
      </ol>

      <details class="notice">
        <summary>連不上？先回復出廠預設值</summary>
        <p>
          日曆會記住上一個配對過的裝置，並拒絕與新的裝置建立配對——這種情況下搜尋得到
          裝置、但一按連線就失敗。把官方 App 移除並不會清掉這筆記錄，必須在日曆上回復
          出廠預設值。
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
      <h3>切換目前顯示的那一張</h3>
      <div class="slots" id="slot-buttons">
        ${[1, 2, 3, 4].map((n) => `<button data-slot="${n}">${n}</button>`).join('')}
      </div>
    </section>
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

  <footer>
    <p>圖片上傳與 dither 設定將在下一階段加入。</p>
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
  localStorage.setItem(TAB_KEY, name);
}

$('#tabs').addEventListener('click', (ev) => {
  const btn = (ev.target as HTMLElement).closest<HTMLButtonElement>('button[data-tab]');
  if (btn) selectTab(btn.dataset.tab!);
});

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

function showError(msg: string) {
  const el = $<HTMLParagraphElement>('#s-error');
  el.textContent = msg;
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
function renderBattery(st: Status) {
  const meter = $<HTMLDivElement>('#s-battery-meter');

  if (st.batteryLevel === undefined) {
    $('#s-battery').textContent = '—';
    meter.hidden = true;
    return;
  }

  const pct = Math.max(0, Math.min(100, st.batteryLevel));
  const raw = `0x${st.batteryRaw.toString(16).padStart(4, '0')}`;
  $('#s-battery').innerHTML = `${pct}% <small>${raw}</small>`;

  meter.hidden = false;
  $<HTMLDivElement>('#s-battery-fill').style.width = `${pct}%`;
  meter.classList.toggle('low', pct <= 20);
}

function renderStatus(st: Status) {
  $('#s-state').textContent = STATE_LABEL[st.state] ?? st.state;
  $('#s-device').textContent = st.connected
    ? `${st.name || 'ULANI'} (${st.address})`
    : '未連線';
  $('#s-slot').textContent = st.activeSlot ? String(st.activeSlot) : '—';
  renderBattery(st);

  if (!busy) showError(st.error);

  const transferring = st.transfer.active && st.transfer.total > 0;
  $<HTMLDivElement>('#s-progress-row').hidden = !transferring;
  $<HTMLDivElement>('#s-progress').hidden = !transferring;
  if (transferring) {
    const pct = Math.round((st.transfer.sent / st.transfer.total) * 100);
    $<HTMLDivElement>('#s-fill').style.width = `${pct}%`;
    $('#s-pct').textContent = `第 ${st.transfer.slot} 張 · ${pct}%`;
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

$('#btn-forget').addEventListener('click', () => guard(() => api.forgetDevice()));

// One image per transfer: the slot the user picks is the only one touched.
$('#send-buttons').addEventListener('click', (ev) => {
  const btn = (ev.target as HTMLElement).closest<HTMLButtonElement>('button[data-send]');
  if (!btn) return;
  const slot = Number(btn.dataset.send);
  guard(() => api.testImage(slot, Math.floor(Math.random() * 0xffffff) + 1));
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

/* ---------------------------------------------------------------- poll */

/*
 * One request at a time, never overlapping: the ESP32 has a small socket pool
 * shared with the captive-portal DNS, and a phone already holds several
 * keep-alive connections open on its own.
 */
async function poll() {
  try {
    renderStatus(await api.status());
    renderWifi(await api.wifi());
  } catch {
    $('#s-state').textContent = '無法連上 ESP32';
  }
  setTimeout(poll, 2000);
}

poll();
