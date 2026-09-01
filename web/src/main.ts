import './style.css';
import { api, type Status } from './lib/api';

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
let currentSlot = 0;

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
    <p class="error" id="s-error" hidden></p>
    <div class="progress" id="s-progress" hidden>
      <div class="bar"><div class="fill" id="s-fill"></div></div>
      <span id="s-pct">0%</span>
    </div>
  </section>

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

    <div class="actions">
      <button id="btn-scan">搜尋日曆</button>
      <button id="btn-disconnect" class="ghost">中斷連線</button>
    </div>
    <ul class="devices" id="device-list"><li class="empty">尚未搜尋</li></ul>
  </section>

  <section class="card" id="test-card">
    <h2>2. 測試</h2>
    <p class="hint">送一張隨機色塊測試圖，確認整條連線與傳輸都正常。一次傳輸約需 30–60 秒。</p>
    <div class="actions">
      <button id="btn-test">隨機更新畫面</button>
    </div>
    <h3>切換相框</h3>
    <div class="slots" id="slot-buttons">
      ${[1, 2, 3, 4].map((n) => `<button data-slot="${n}">${n}</button>`).join('')}
    </div>
  </section>

  <footer>
    <p>圖片上傳與 dither 設定將在下一階段加入。</p>
  </footer>
`;

function setBusy(value: boolean) {
  busy = value;
  document.querySelectorAll('button').forEach((b) => {
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
  currentSlot = st.activeSlot;
  $('#s-state').textContent = STATE_LABEL[st.state] ?? st.state;
  $('#s-device').textContent = st.connected
    ? `${st.name || 'ULANI'} (${st.address})`
    : '未連線';
  $('#s-slot').textContent = st.activeSlot ? String(st.activeSlot) : '—';
  renderBattery(st);

  if (!busy) showError(st.error);

  const progress = $<HTMLDivElement>('#s-progress');
  progress.hidden = !st.transfer.active;
  if (st.transfer.active && st.transfer.total > 0) {
    const pct = Math.round((st.transfer.sent / st.transfer.total) * 100);
    $<HTMLDivElement>('#s-fill').style.width = `${pct}%`;
    $('#s-pct').textContent = `${pct}%`;
  }

  document.querySelectorAll<HTMLButtonElement>('#slot-buttons button').forEach((b) => {
    b.classList.toggle('active', Number(b.dataset.slot) === st.activeSlot);
  });

  $<HTMLElement>('#test-card').classList.toggle('disabled', !st.connected);

  renderDevices(st.devices, st.state === 'scanning');
}

/* --------------------------------------------------------------- events */

// Scanning is asynchronous in the firmware; the status poll shows the results.
$('#btn-scan').addEventListener('click', () => guard(() => api.scan()));

$('#btn-disconnect').addEventListener('click', () => guard(() => api.disconnect()));

$('#btn-test').addEventListener('click', () =>
  guard(() =>
    api.testImage(currentSlot || 1, Math.floor(Math.random() * 0xffffff) + 1),
  ),
);

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

/* ---------------------------------------------------------------- poll */

/*
 * One request at a time, never overlapping: the ESP32 has a small socket pool
 * shared with the captive-portal DNS, and a phone already holds several
 * keep-alive connections open on its own.
 */
async function poll() {
  try {
    renderStatus(await api.status());
  } catch {
    $('#s-state').textContent = '無法連上 ESP32';
  }
  setTimeout(poll, 2000);
}

poll();
