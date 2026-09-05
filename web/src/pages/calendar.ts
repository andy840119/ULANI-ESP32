/*
 * The 日曆 tab: connect to a calendar (scan / remembered device) and the test
 * card that streams a colour-block image to a page. Reads the shared status for
 * the device list, the remembered device, and which page is mid-transfer.
 */

import { api, type Status } from '../lib/api';
import { $, guard } from '../lib/ui';

export function calendarMarkup(): string {
  return `
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
  </div>`;
}

let lastDeviceKey = '';

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

export function renderCalendar(st: Status) {
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

export function mountCalendar() {
  // Scanning is asynchronous in the firmware; the status poll shows the results.
  $('#btn-scan').addEventListener('click', () => guard(() => api.calendar.scan()));

  $('#btn-disconnect').addEventListener('click', () => guard(() => api.calendar.disconnect()));

  $('#btn-forget').addEventListener('click', () => guard(() => api.calendar.forget()));

  // One image per transfer: the slot the user picks is the only one touched.
  $('#send-buttons').addEventListener('click', (ev) => {
    const btn = (ev.target as HTMLElement).closest<HTMLButtonElement>('button[data-send]');
    if (!btn) return;
    const slot = Number(btn.dataset.send);
    const activate = $<HTMLInputElement>('#send-activate').checked;
    guard(() => api.calendar.testImage(slot, Math.floor(Math.random() * 0xffffff) + 1, activate));
  });

  $('#device-list').addEventListener('click', (ev) => {
    const btn = (ev.target as HTMLElement).closest<HTMLButtonElement>('button[data-address]');
    if (!btn) return;
    guard(() => api.calendar.connect(btn.dataset.address!));
  });

  $('#slot-buttons').addEventListener('click', (ev) => {
    const btn = (ev.target as HTMLElement).closest<HTMLButtonElement>('button[data-slot]');
    if (!btn) return;
    guard(() => api.calendar.setPage(Number(btn.dataset.slot)));
  });
}
