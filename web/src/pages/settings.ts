/*
 * The settings view (opened by the header gear): how long to hold the BLE link
 * before dropping it to save power, and export/import of everything a web flash
 * would wipe.
 */

import { api, type Status } from '../lib/api';
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
