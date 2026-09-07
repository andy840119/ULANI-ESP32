/*
 * The other half of the event log's wire format.
 *
 * A record on the board is a number and two integers -- that is what keeps it
 * to 24 bytes -- so the wording lives here, where it can be reworded without
 * invalidating logs already written. The numbers must match diag_code_t in
 * components/diag_log/include/diag_log.h, and like that enum this table is
 * append-only: a code keeps its meaning forever.
 */

export interface LogRecord {
  seq: number;
  uptimeMs: number;
  epoch: number;
  code: number;
  slot: number;
  result: number;
  a: number;
  b: number;
}

export type LogSource = 'system' | 'wifi' | 'tesserae' | 'calendar' | 'web';

const SOURCES: Record<number, LogSource> = {
  1: 'system',
  2: 'wifi',
  3: 'tesserae',
  4: 'calendar',
  5: 'web',
};

export const SOURCE_LABEL: Record<LogSource, string> = {
  system: '系統',
  wifi: 'WiFi',
  tesserae: 'Tesserae',
  calendar: '日曆',
  web: '後台',
};

export function sourceOf(rec: LogRecord): LogSource {
  return SOURCES[Math.floor(rec.code / 100)] ?? 'system';
}

const RESET_REASON: Record<number, string> = {
  1: '上電',
  2: '外部重置',
  3: '軟體重啟',
  4: 'panic',
  5: '中斷看門狗',
  6: '工作看門狗',
  7: '看門狗',
  8: '深度睡眠喚醒',
  9: '掉電保護',
  10: 'USB 重置',
  11: 'JTAG 重置',
};

const WEB_ACTION: Record<number, string> = {
  1: '切換顯示頁',
  2: '送出頁面圖片',
  3: '送測試圖',
  4: '連線日曆',
  5: '中斷日曆',
  6: '忘記日曆',
  7: '設定 Tesserae',
  8: 'forget Tesserae',
  9: '手動要求更新',
  10: '上傳圖片',
  11: '變更設定',
  12: '重新啟動',
};

/* "1.2.3.4" from the four bytes lwIP holds, lowest octet first. */
function ipText(v: number): string {
  const u = v >>> 0;
  return `${u & 0xff}.${(u >> 8) & 0xff}.${(u >> 16) & 0xff}.${(u >> 24) & 0xff}`;
}

function page(rec: LogRecord): string {
  return rec.slot ? `第 ${rec.slot} 頁` : '';
}

/*
 * One line of plain language per record. The awkward ones are worth reading
 * twice: a page that moved on its own, and whether it moved while the board
 * was not even connected, is the whole reason this log exists (issue #51).
 */
export function describe(rec: LogRecord): string {
  const { code, a, b, result } = rec;
  switch (code) {
    case 100:
      return `開機（${RESET_REASON[a] ?? `原因 ${a}`}）`;
    case 101:
      return `可用記憶體 ${(a / 1024).toFixed(1)} KB，最低曾到 ${(b / 1024).toFixed(1)} KB`;
    case 102:
      return `對時完成，來源：${b === 1 ? '伺服器 Date header' : 'NTP'}`;
    case 103:
      return '日誌已清除';
    case 104:
      return `空間不足，暫停寫入 flash（剩餘 ${(a / 1024).toFixed(0)} KB）`;
    case 105:
      return `舊日誌被回收，少了 ${a} 筆`;
    case 106:
      return '收到重新啟動的要求';

    case 200:
      return `WiFi 已連線，${ipText(b)}，訊號 ${a} dBm`;
    case 201:
      return `WiFi 斷線（reason ${a}）`;
    case 202:
      return `WiFi 訊號 ${a} dBm`;

    case 300:
      return `${page(rec)} 心跳 HTTP ${a}，耗時 ${b} ms`;
    case 301:
      return `${page(rec)} 詢問畫面 HTTP ${a}${
        a === 304 ? '（沒有新圖）' : a === 204 ? '（伺服器還沒算好）' : ''
      }，耗時 ${b} ms`;
    case 302:
      return `${page(rec)} 新畫面已存下，${a} bytes，耗時 ${b} ms`;
    case 303:
      return `${page(rec)} 取得畫面失敗（HTTP ${a}，已寫入 ${b} bytes）`;
    case 304:
      return `${page(rec)} 完成註冊（HTTP ${a}）`;
    case 305:
      return `${page(rec)} token 被拒，將重新註冊`;
    case 306:
      return `${page(rec)} 伺服器要求 ${a} 秒後再來`;

    case 400:
      return `日曆已連線（MTU ${a}）`;
    case 401:
      return `日曆斷線（reason ${a}）`;
    case 402:
      return `日曆自己換頁：第 ${a} 頁 → 第 ${b} 頁${
        result === 1 ? '（重新連線後才發現，可能是斷線期間換的）' : ''
      }`;
    case 403:
      return `我方切換到 ${page(rec)}（${a === 1 ? '傳完圖後重繪' : '使用者操作'}）`;
    case 404:
      return `開始傳圖到 ${page(rec)}，此刻螢幕在第 ${a || '?'} 頁，crc ${(b >>> 0)
        .toString(16)
        .padStart(4, '0')}`;
    case 405:
      return `第 ${result} 次嘗試回覆 ${(a >>> 0).toString(16).padStart(4, '0')}，耗時 ${b} ms`;
    case 406:
      return `傳圖${result === 0 ? '成功' : '失敗'}，之後螢幕在第 ${a || '?'} 頁，共 ${b} ms`;
    case 407:
      return `opcode ${(a >>> 0).toString(16).padStart(2, '0')} 等不到回覆`;
    case 408:
      return `收到對不上的回覆 ${(a >>> 0).toString(16).padStart(4, '0')}（當時在等 ${(b >>> 0)
        .toString(16)
        .padStart(2, '0')}）`;
    case 409:
      return `電量回覆 0x${(a >>> 0).toString(16).padStart(4, '0')}`;

    case 500:
      return `${ipText(a)} ${WEB_ACTION[b] ?? `動作 ${b}`}${rec.slot ? `（${page(rec)}）` : ''}`;

    default:
      return `事件 ${code}（a=${a} b=${b}）`;
  }
}

/* Records that deserve to stand out in the list. */
export function isNotable(rec: LogRecord): boolean {
  return (
    rec.code === 402 || // the panel moved on its own
    rec.code === 100 || // a restart
    rec.code === 104 ||
    rec.code === 105 ||
    rec.code === 303 ||
    rec.code === 305 ||
    rec.code === 407 ||
    rec.code === 408 ||
    (rec.code === 406 && rec.result !== 0)
  );
}

export function timeText(rec: LogRecord): string {
  if (rec.epoch > 0) {
    return new Date(rec.epoch * 1000).toLocaleString();
  }
  /* Before the clock was set, uptime is all there is. */
  const s = Math.floor(rec.uptimeMs / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return `開機後 ${h ? `${h} 時 ` : ''}${m} 分 ${s % 60} 秒`;
}
