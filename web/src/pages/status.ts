/*
 * The chrome that is always on screen: the sticky app bar (connection dot,
 * device name, battery/progress rings, a quick-connect button) and the status
 * card below the tabs (state, device, current page, battery, transfer). The
 * gear and tab strip that also live up here are wired by the shell, not this.
 */

import { api, type Status } from '../lib/api';
import { $, ageBadge, guard, isBusy, showError } from '../lib/ui';

const STATE_LABEL: Record<Status['state'], string> = {
  off: '藍牙未啟動',
  idle: '待機',
  scanning: '掃描中…',
  connecting: '連線中…',
  discovering: '取得服務中…',
  ready: '已連線',
  transferring: '傳送圖片中…',
};

/* The remembered device's address, so the app-bar connect button has a target
 * without re-reading the status. Updated every render. */
let savedAddress: string | null = null;

export function statusMarkup(): string {
  return `
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
  </section>`;
}

/*
 * The always-visible top bar: a connection dot, the device name, and small pie
 * rings for battery and (only mid-transfer) send progress. It repeats a little
 * of the status card on purpose -- the card scrolls away, this does not.
 */
function renderStatusbar(st: Status) {
  $('#bar-dot').classList.toggle('on', st.connected);

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

/*
 * The panel answers `06 00` with `06 <level>`. The scale is not documented and
 * was read off a live device, so the raw reply stays on screen next to the
 * percentage -- if the assumption is wrong it is visible rather than misleading.
 */
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

export function renderStatus(st: Status) {
  renderStatusbar(st);
  $('#s-state').textContent = STATE_LABEL[st.state] ?? st.state;
  $('#s-device').textContent = st.connected
    ? `${st.name || 'ULANI'} (${st.address})`
    : '未連線';
  $('#s-slot').innerHTML =
    (st.activeSlot ? String(st.activeSlot) : '—') + ageBadge(st.slotAgeMs, st.connected);
  renderBattery(st);

  if (!isBusy()) showError(st.error);

  /*
   * The transfer row stays put; only its fill comes and goes. Popping the whole
   * row in on a transfer shifted everything below it, which read as jumping.
   */
  const transferring = st.transfer.active && st.transfer.total > 0;
  const fill = $<HTMLDivElement>('#s-fill');
  if (transferring) {
    const pct = Math.round((st.transfer.sent / st.transfer.total) * 100);
    fill.style.width = `${pct}%`;
    /* A transfer restarts from 0 on a retry; say so, or the bar dropping back
     * and climbing again looks like it stalled. */
    const retry = st.transfer.attempt > 1 ? `　重試 ${st.transfer.attempt}` : '';
    $('#s-pct').textContent = `第 ${st.transfer.slot} 張 · ${pct}%${retry}`;
  } else {
    fill.style.width = '0';
    $('#s-pct').textContent = '—';
  }
}

export function mountStatus() {
  // Header quick-connect: reach for the remembered calendar without hunting.
  $('#bar-connect').addEventListener('click', () => {
    if (savedAddress) guard(() => api.connect(savedAddress!));
  });
}

/* Shown when the poll gives up (see the shell's poll loop). */
export function showOffline() {
  $('#s-state').textContent = '無法連上 ESP32';
}
