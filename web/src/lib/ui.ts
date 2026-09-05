/*
 * Shared UI plumbing: the element helper, the one-action-at-a-time guard, and
 * the status-card error line. Every tab module leans on these so none of them
 * has to re-implement the busy lock or know how errors are shown.
 */

export const $ = <T extends HTMLElement>(sel: string) =>
  document.querySelector<T>(sel)!;

let busy = false;

/* True while an action is in flight; renders skip live-updating what the user
 * is looking at (e.g. an error the action is about to replace). */
export const isBusy = () => busy;

function setBusy(value: boolean) {
  busy = value;
  /* Tabs stay live: switching views is not an action against the device. */
  document.querySelectorAll<HTMLButtonElement>('.panel button').forEach((b) => {
    b.disabled = value;
  });
}

/* Turn a few known backend errors into something a user can act on. */
function friendlyError(msg: string): string {
  if (msg.includes('pairing refused')) {
    return '日曆拒絕配對——它可能還記得舊的配對（曾配過官方 App，或韌體更新過）。'
      + '請將日曆回復原廠設定後再連一次。';
  }
  return msg;
}

export function showError(msg: string) {
  const el = $<HTMLParagraphElement>('#s-error');
  el.textContent = friendlyError(msg);
  el.hidden = !msg;
}

/*
 * Runs an action with the busy lock held so two taps cannot overlap on the
 * ESP32's small socket pool, and funnels any failure to the error line.
 */
export type Guard = (fn: () => Promise<unknown>) => void;

export const guard: Guard = (fn) => {
  if (busy) return;
  setBusy(true);
  void (async () => {
    try {
      await fn();
    } catch (err) {
      showError(err instanceof Error ? err.message : String(err));
    } finally {
      setBusy(false);
    }
  })();
};

/*
 * The calendar never tells us anything on its own, so every number on screen is
 * as old as the last time the firmware asked. Showing that age turns "the
 * battery says 5%" into "the battery said 5%, twelve seconds ago", and makes a
 * link that is up but no longer answering visible instead of silent.
 */
export function ago(ms: number): string {
  const s = Math.round(ms / 1000);
  if (s < 60) return `${s} 秒前`;
  const m = Math.round(s / 60);
  return m < 60 ? `${m} 分鐘前` : `${Math.round(m / 60)} 小時前`;
}

/*
 * An age badge next to a reading. When the link is down the number is frozen at
 * its last value; mark it (and a suspiciously old one while connected) so a
 * stale reading is never mistaken for a live one.
 */
const STALE_MS = 90_000;

export function ageBadge(ms: number | undefined, connected: boolean): string {
  if (ms === undefined) return '';
  const frozen = !connected;
  const stale = frozen || ms > STALE_MS;
  const note = frozen ? '，已離線' : '';
  return ` <small class="age${stale ? ' stale' : ''}">${ago(ms)}${note}</small>`;
}
