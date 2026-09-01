import './style.css';
import { api, type Device, type Status } from './lib/api';

const STATE_LABEL: Record<Status['state'], string> = {
  off: '藍牙未啟動',
  idle: '待機',
  scanning: '掃描中…',
  connecting: '連線中…',
  discovering: '取得服務中…',
  ready: '已連線',
  transferring: '傳送圖片中…',
};

let devices: Device[] = [];
let busy = false;

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
    <div class="row"><span>電量回應</span><strong id="s-battery">—</strong></div>
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
      <li>按下「搜尋日曆」，選擇你的裝置。</li>
    </ol>
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

function renderDevices() {
  const list = $<HTMLUListElement>('#device-list');
  if (devices.length === 0) {
    list.innerHTML = '<li class="empty">沒有找到日曆，確認它已開機且沒有被別的裝置佔用</li>';
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

function renderStatus(st: Status) {
  $('#s-state').textContent = STATE_LABEL[st.state] ?? st.state;
  $('#s-device').textContent = st.connected
    ? `${st.name || 'ULANI'} (${st.address})`
    : '未連線';
  $('#s-slot').textContent = st.activeSlot ? String(st.activeSlot) : '—';
  $('#s-battery').textContent = st.batteryRaw
    ? `0x${st.batteryRaw.toString(16).padStart(4, '0')}`
    : '—';

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
}

/* --------------------------------------------------------------- events */

$('#btn-scan').addEventListener('click', () =>
  guard(async () => {
    devices = [];
    renderDevices();
    await api.scan();
    // The firmware scans asynchronously; poll until it settles.
    for (let i = 0; i < 12; i++) {
      await new Promise((r) => setTimeout(r, 1000));
      devices = (await api.devices()).devices;
      renderDevices();
      const st = await api.status();
      if (st.state !== 'scanning') break;
    }
  }),
);

$('#btn-disconnect').addEventListener('click', () => guard(() => api.disconnect()));

$('#btn-test').addEventListener('click', () =>
  guard(async () => {
    const st = await api.status();
    const slot = st.activeSlot || 1;
    await api.testImage(slot, Math.floor(Math.random() * 0xffffff) + 1);
  }),
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

async function poll() {
  try {
    renderStatus(await api.status());
  } catch {
    $('#s-state').textContent = '無法連上 ESP32';
  }
  setTimeout(poll, 1000);
}

renderDevices();
poll();
