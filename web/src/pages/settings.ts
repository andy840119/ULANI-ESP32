/*
 * The settings view (opened by the header gear): how long to hold the BLE link
 * before dropping it to save power, export/import of everything a web flash
 * would wipe, and the event log -- switches, a live tail, export and clear.
 */

import { api, type LogStatus, type Status } from '../lib/api';
import { describe, isNotable, sourceOf, timeText, SOURCE_LABEL, type LogRecord }
  from '../lib/logcodes';
import { $, guard } from '../lib/ui';

export function settingsMarkup(): string {
  return `
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
    <section class="card" id="log-card">
      <h2>事件日誌</h2>
      <p class="hint">
        板子平常不接電腦，序列埠的訊息沒有人看得到，所以它會把自己做過的事記在身上：
        每一次跟 Tesserae 的往返、跟日曆的連線與傳圖、以及後台上每一個會改變狀態的操作
        （含來源 IP）。出事之後把它匯出來看，就不用靠猜的。
      </p>
      <label class="field">
        <span>記錄事件</span>
        <select id="log-enabled">
          <option value="1">開啟</option>
          <option value="0">停用</option>
        </select>
      </label>
      <label class="field">
        <span>斷電後保留（寫入 flash）</span>
        <select id="log-persist">
          <option value="1">保留</option>
          <option value="0">只留在記憶體</option>
        </select>
      </label>
      <label class="field">
        <span>保留上限</span>
        <select id="log-segments">
          <option value="2">128 KB（約 5 天）</option>
          <option value="4">256 KB（約 10 天）</option>
          <option value="8">512 KB（約 20 天）</option>
        </select>
      </label>
      <label class="field">
        <span>文字 log 收錄層級</span>
        <select id="log-trace">
          <option value="0">不收</option>
          <option value="1">只收錯誤</option>
          <option value="2">錯誤與警告</option>
          <option value="3">全部（抓問題時再開）</option>
        </select>
      </label>
      <p class="hint">
        「停用」只是停止記錄，<strong>不會清掉已經記下來的東西</strong>。文字 log 只留在
        記憶體裡（約 30 分鐘），開成「全部」會轉得更快，抓完問題記得轉回來。
      </p>
      <p class="hint" id="log-stats"></p>
      <div class="actions">
        <button type="button" id="btn-log-csv">匯出 CSV</button>
        <button type="button" id="btn-log-ndjson">匯出 NDJSON</button>
        <button type="button" id="btn-log-trace">下載文字 log</button>
        <button type="button" id="btn-log-clear">清除日誌</button>
      </div>
      <ul class="log-list" id="log-list"></ul>
    </section>
  </div>`;
}

/* Reflect the stored keep-alive choice, but never while the user is changing it. */
export function renderSettings(st: Status) {
  const sel = $<HTMLSelectElement>('#idle-timeout');
  if (document.activeElement !== sel) {
    sel.value = String(st.idleTimeoutMs);
  }
}

function backupMsg(text: string, isError = false) {
  const el = $<HTMLParagraphElement>('#backup-msg');
  el.textContent = text;
  el.hidden = !text;
  el.classList.toggle('error', isError);
}

/* ------------------------------------------------------------- event log */

/*
 * The log has its own poll rather than riding the shared status one: it is
 * only interesting while this panel is open, and it is the one view where a
 * request that costs the board something should not be made behind the user's
 * back.
 */
let logTimer: number | undefined;

function logVisible(): boolean {
  const panel = document.querySelector<HTMLElement>('.panel[data-panel="settings"]');
  return !!panel && !panel.hidden;
}

function renderLogStats(st: LogStatus) {
  const held = st.count;
  const flash = (st.flashBytes / 1024).toFixed(0);
  const cap = ((st.segments * st.segmentBytes) / 1024).toFixed(0);
  const lost = st.dropped ? `，已回收 ${st.dropped} 筆` : '';
  $('#log-stats').textContent =
    `記憶體裡 ${held}/${st.capacity} 筆${lost}；flash 用了 ${flash} KB / ${cap} KB，` +
    `分割區還剩 ${(st.storageFree / 1024).toFixed(0)} KB。`;
}

function renderLogList(records: LogRecord[]) {
  const list = $<HTMLUListElement>('#log-list');
  /* Newest first: the reason anyone opens this is "what just happened". */
  list.innerHTML = records
    .slice(-120)
    .reverse()
    .map((rec) => {
      const src = sourceOf(rec);
      return `<li class="${isNotable(rec) ? 'notable' : ''}">
        <span class="log-time">${timeText(rec)}</span>
        <span class="log-src log-src-${src}">${SOURCE_LABEL[src]}</span>
        <span class="log-text"></span>
      </li>`;
    })
    .join('');
  /* Text goes in as text, never as markup: some of it is device-reported. */
  const texts = list.querySelectorAll<HTMLSpanElement>('.log-text');
  records
    .slice(-120)
    .reverse()
    .forEach((rec, i) => {
      if (texts[i]) texts[i].textContent = describe(rec);
    });
}

async function refreshLog() {
  const [st, tail] = await Promise.all([api.system.log.status(), api.system.log.tail()]);
  renderLogStats(st);
  renderLogList(tail.records);

  const set = (id: string, value: string | number) => {
    const el = $<HTMLSelectElement>(id);
    if (document.activeElement !== el) el.value = String(value);
  };
  set('#log-enabled', st.enabled ? 1 : 0);
  set('#log-persist', st.persist ? 1 : 0);
  set('#log-segments', st.segments);
  set('#log-trace', st.traceLevel);
}

function download(url: string, name: string) {
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.click();
}

export function startLogPolling() {
  if (logTimer !== undefined) return;
  logTimer = window.setInterval(() => {
    if (logVisible()) void refreshLog().catch(() => {});
  }, 5000);
  if (logVisible()) void refreshLog().catch(() => {});
}

export function mountSettings() {
  $('#idle-timeout').addEventListener('change', (ev) => {
    guard(() => api.system.setCalendarKeepAlive(Number((ev.target as HTMLSelectElement).value)));
  });

  $('#btn-export').addEventListener('click', () =>
    guard(async () => {
      backupMsg('');
      const res = await fetch(api.system.exportUrl);
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

  for (const [id, key] of [
    ['#log-enabled', 'enabled'],
    ['#log-persist', 'persist'],
    ['#log-segments', 'segments'],
    ['#log-trace', 'traceLevel'],
  ] as const) {
    $(id).addEventListener('change', (ev) => {
      const raw = Number((ev.target as HTMLSelectElement).value);
      const value = key === 'enabled' || key === 'persist' ? raw === 1 : raw;
      guard(async () => {
        await api.system.log.settings({ [key]: value });
        await refreshLog();
      });
    });
  }

  $('#btn-log-csv').addEventListener('click', () =>
    download(api.system.log.exportUrl('csv'), 'ulani-log.csv'));
  $('#btn-log-ndjson').addEventListener('click', () =>
    download(api.system.log.exportUrl('ndjson'), 'ulani-log.ndjson'));
  $('#btn-log-trace').addEventListener('click', () =>
    download(api.system.log.traceUrl, 'ulani-trace.txt'));

  $('#btn-log-clear').addEventListener('click', () => {
    if (!confirm('清除日誌？記憶體和 flash 裡的紀錄都會消失，無法復原。')) return;
    guard(async () => {
      await api.system.log.clear();
      await refreshLog();
    });
  });

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
      const r = await api.system.import(text);
      backupMsg(`已還原 ${r.restored} 筆設定，裝置正在重新啟動…重整頁面前請稍候幾秒。`);
    });
  });
}
