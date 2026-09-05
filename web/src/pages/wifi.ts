/*
 * The WiFi tab: join the user's home network so the page is reachable without
 * the ESP32 hotspot. The access point stays up regardless.
 */

import { api, type WifiStatus } from '../lib/api';
import { $, guard } from '../lib/ui';

const WIFI_LABEL: Record<WifiStatus['state'], string> = {
  disabled: '未設定',
  connecting: '連線中…',
  connected: '已連線',
  failed: '連線失敗',
};

export function wifiMarkup(): string {
  return `
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
  </div>`;
}

let wifiKey = '';
let joining: string | null = null;

export function renderWifi(w: WifiStatus) {
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

export function mountWifi() {
  $('#btn-wifi-scan').addEventListener('click', () => guard(() => api.wifi.scan()));

  $('#btn-wifi-forget').addEventListener('click', () =>
    guard(async () => {
      await api.wifi.forget();
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
      await api.wifi.connect(ssid, password);
      hideJoinForm();
    });
  });
}
