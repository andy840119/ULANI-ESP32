/*
 * App shell. Assembles the tab modules into the page, wires the tab strip and
 * the settings gear, and runs the single status poll that feeds every tab. All
 * the per-tab markup, rendering and event wiring lives in ./pages/*; this file
 * only orchestrates them.
 */

import './style.css';
import { api, type Status } from './lib/api';
import { $ } from './lib/ui';
import { calendarMarkup, mountCalendar, renderCalendar } from './pages/calendar';
import { mountSettings, renderSettings, settingsMarkup } from './pages/settings';
import { mountStatus, renderStatus, showOffline, statusMarkup } from './pages/status';
import { mountTesserae, renderTesserae, tesseraeMarkup } from './pages/tesserae';
import { mountWifi, renderWifi, wifiMarkup } from './pages/wifi';

document.querySelector<HTMLDivElement>('#app')!.innerHTML = `
  ${statusMarkup()}

  <nav class="tabs" id="tabs">
    <button data-tab="calendar" class="active">日曆</button>
    <button data-tab="wifi">WiFi</button>
    <button data-tab="tesserae">Tesserae</button>
  </nav>

  ${calendarMarkup()}

  <div class="panel" data-panel="tesserae" hidden>
    ${tesseraeMarkup()}
  </div>

  ${wifiMarkup()}

  ${settingsMarkup()}

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

/* --------------------------------------------------------------- mount */

mountStatus();
mountCalendar();
mountWifi();
mountSettings();
mountTesserae();

/* ---------------------------------------------------------------- poll */

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
    api.calendar.connect(st.savedDevice.address).catch(() => {});
  }
}

/*
 * One request at a time, never overlapping: the ESP32 has a small socket pool
 * shared with the captive-portal DNS, and a phone already holds several
 * keep-alive connections open on its own.
 *
 * A single dropped poll is not worth shouting about -- a phone juggling the
 * hotspot drops one now and then. Only call it lost after several in a row.
 */
let pollFails = 0;
const POLL_FAIL_LIMIT = 3;

async function poll() {
  try {
    const st = await api.calendar.get();
    pollFails = 0;
    renderStatus(st);
    renderCalendar(st);
    renderSettings(st);
    maybeConnectOnLoad(st);
    renderWifi(await api.wifi.get());
    renderTesserae((await api.tesserae.get()).clients, st.connected);
  } catch {
    if (++pollFails >= POLL_FAIL_LIMIT) showOffline();
  }
  setTimeout(poll, 2000);
}

/*
 * The firmware version is baked in at build time and never changes while the
 * board runs, so fetch it once and drop it in the footer. "dev" (an untagged
 * local build) is shown as such; a real version marks a production build.
 */
api.system
  .version()
  .then((v) => {
    $('#fw-version').textContent = v.production
      ? `韌體版本 ${v.version}`
      : `開發版（${v.version}）`;
  })
  .catch(() => {
    /* Old firmware without /api/system/version, or offline: leave the line blank. */
  });

poll();
