/*
 * The Tesserae tab.
 *
 * Most of this panel is prose, and it is arranged as the four questions a
 * first-time user actually asks in order: what is this, how do I run it, how
 * do I connect, and where does the picture go. Pointing the board at a server
 * is two fields; knowing what to put in them means having stood up a
 * container, found a button several menus deep, and understood that two
 * similar-looking strings are not interchangeable.
 */

import { api, type TesseraeStatus } from '../lib/api';

const $ = <T extends HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

const STATE_LABEL: Record<TesseraeStatus['state'], string> = {
  disabled: '未設定',
  unregistered: '等待核准',
  idle: '已連線',
  working: '更新中…',
  error: '發生問題',
};

export function tesseraeMarkup(): string {
  return `
    <section class="card">
      <h2>1. 什麼是 tesserae</h2>
      <p class="hint">
        <a href="https://github.com/dmellok/tesserae">tesserae</a>
        是一套可以自己架設的服務，負責產生日曆畫面。要顯示什麼、長什麼樣子、
        多久更新一次，全部在它的網頁上設定。
      </p>
      <p class="hint">
        這台 ESP32 只做兩件事：按 server 指定的時間去把畫好的圖拿回來，
        再透過藍牙送進 ULANI。<strong>板子本身不做任何影像處理</strong>——
        server 送過來的已經是日曆看得懂的格式了。
      </p>
      <p class="hint warn">
        server 關機時日曆就不會更新。沒有 NAS 的話跑在電腦上也可以，
        只是要接受「電腦沒開就不更新」。
      </p>
    </section>

    <section class="card">
      <h2>2. 把 server 架起來</h2>
      <p class="hint">在 NAS 或任何能跑 Docker 的機器上執行：</p>
      <pre><code>mkdir tesserae &amp;&amp; cd tesserae
curl -fsSLO https://raw.githubusercontent.com/dmellok/tesserae/main/docker-compose.yml
docker compose up -d</code></pre>
      <p class="hint">
        然後用瀏覽器開 <code>http://&lt;那台機器的IP&gt;:8765</code>，
        第一次進去會要你設定密碼並跟著精靈走完。
      </p>
      <p class="hint warn">
        下面填 server 網址時，<strong>請用區網 IP（例如
        <code>http://192.168.0.50:8765</code>），不要用外網網域名稱。</strong>
        tesserae 預設只把畫好的圖給它認定在區網內的用戶端；透過外網網域連進去
        （即使兩台其實在同一個區網），server 看到的來源會是公網位址，
        下載圖片時就會被回 403。
      </p>
      <p class="hint">
        真的需要用外網網域的話，要到 tesserae 的
        <strong>Settings → Server</strong> 把
        <strong>「Allow REST clients on public networks」</strong>打開。
        那會讓簽章網址在有效期內可以從網際網路下載，請自行評估。
      </p>
    </section>

    <section class="card">
      <h2>3. 取得配對碼並連線</h2>

      <div class="row"><span>狀態</span><strong id="t-state">—</strong></div>
      <div class="row" id="t-id-row" hidden>
        <span>裝置編號</span><strong id="t-id">—</strong>
      </div>
      <div class="row" id="t-next-row" hidden>
        <span>下次更新</span><strong id="t-next">—</strong>
      </div>
      <p class="error" id="t-error" hidden></p>

      <p class="hint">在 tesserae 的網頁上，依序：</p>
      <ol class="steps">
        <li>右上角進入 <strong>Settings</strong></li>
        <li>切到 <strong>Devices</strong> 分頁</li>
        <li>在 Add device 區塊，Transport 選 <strong>REST API</strong></li>
        <li>按右邊的 <strong>+ Issue pairing code</strong>，把它給你的碼複製下來</li>
      </ol>
      <p class="hint">
        用配對碼的話，<strong>板子型號會由 server 自動辨識</strong>，
        會顯示成「Seeed XIAO ePaper EE04 (7.3&quot; Spectra 6)」——那是正確的，
        它的畫面規格（800×480、4bpp）剛好和 ULANI 一樣。你不需要手動選任何東西。
      </p>
      <p class="hint warn">
        配對碼是<strong>一次性</strong>的，而且有時效。拿到之後請盡快貼進來按儲存；
        如果失敗了，回 tesserae 重新按一次「Issue pairing code」拿新的，
        <strong>不要重複使用舊的</strong>。
      </p>

      <form class="join" id="t-form">
        <label>
          <span>server 網址</span>
          <input type="url" id="t-url" placeholder="http://192.168.1.10:8765"
                 autocomplete="off" />
        </label>
        <label>
          <span>配對碼</span>
          <input type="text" id="t-code" placeholder="貼上 Issue pairing code 給的碼"
                 autocomplete="off" />
        </label>

        <details class="notice">
          <summary>沒有配對碼？其他兩種方式（不建議）</summary>
          <p>
            <strong>什麼都不填。</strong>板子會先向 server 報到，然後在
            Settings → Devices 最上面的 <strong>Discovered</strong> 區塊出現，
            你按下 Register 核准它就會完成。核准之前這裡顯示「等待核准」是正常的。
          </p>
          <p>
            配對碼被拒絕時也會自動退回這條路，所以碼過期了不會卡死——
            直接去 Discovered 按 Register 就好。
          </p>
          <p>
            <strong>Add without pairing。</strong>Add device 區塊裡那個可以展開的
            連結。這條路要你<strong>自己挑板子型號、自己填畫面尺寸</strong>，
            填錯了 server 就會算出日曆看不懂的格式，所以除非有特殊理由，
            建議直接用配對碼。
          </p>
          <p>
            走這條路的話它會給你一組 <strong>access token</strong>（不是配對碼），
            要連同你自己取的 Device id 一起填在下面：
          </p>
          <div class="fields">
            <label>
              <span>Device id</span>
              <input type="text" id="t-devid" placeholder="例如 ulani-esp32"
                     autocomplete="off" />
            </label>
            <label>
              <span>access token</span>
              <input type="text" id="t-token" placeholder="Add without pairing 給的 token"
                     autocomplete="off" />
            </label>
          </div>
          <p class="warn">
            這兩個欄位和上面的配對碼是<strong>互斥的</strong>，只填其中一邊。
            把 access token 貼進配對碼欄位會被 server 回 403。
          </p>
        </details>

        <div class="actions">
          <button type="submit">儲存並連線</button>
          <button type="button" id="t-poll" class="ghost">立即更新</button>
          <button type="button" id="t-forget" class="ghost">清除設定</button>
        </div>
      </form>
    </section>

    <section class="card">
      <h2>4. 畫面要放在第幾張</h2>
      <p class="hint">
        ULANI 可以存四張畫面。tesserae 產生的圖會固定覆蓋你選的那一張，
        其他三張不受影響。
      </p>
      <form class="join" id="t-slot-form">
        <label>
          <span>使用第幾張</span>
          <select id="t-slot">
            <option value="1">第 1 張</option>
            <option value="2">第 2 張</option>
            <option value="3">第 3 張</option>
            <option value="4">第 4 張</option>
          </select>
        </label>
        <div class="actions">
          <button type="submit">儲存</button>
        </div>
      </form>
    </section>
  `;
}

function describeNext(st: TesseraeStatus): string {
  if (!st.registered) return '—';
  const s = st.secondsUntilPoll;
  if (s <= 0) return '即將更新';
  if (s < 60) return `${s} 秒後`;
  if (s < 3600) return `${Math.round(s / 60)} 分鐘後`;
  return `${(s / 3600).toFixed(1)} 小時後`;
}

let filled = false;

export function renderTesserae(st: TesseraeStatus) {
  $('#t-state').textContent = STATE_LABEL[st.state] ?? st.state;

  $<HTMLDivElement>('#t-id-row').hidden = !st.deviceId;
  if (st.deviceId) $('#t-id').textContent = st.deviceId;

  $<HTMLDivElement>('#t-next-row').hidden = !st.registered;
  $('#t-next').textContent = describeNext(st);

  const err = $<HTMLParagraphElement>('#t-error');
  err.hidden = !st.error;
  err.textContent = st.error;

  // Populate once, then leave the fields alone so typing is not overwritten.
  if (!filled && st.serverUrl) {
    filled = true;
    $<HTMLInputElement>('#t-url').value = st.serverUrl;
    $<HTMLInputElement>('#t-devid').value = st.deviceId;
    $<HTMLSelectElement>('#t-slot').value = String(st.slot || 1);
  }
}

/* Both forms post the whole configuration; the split is only about where the
 * buttons sit, so each one reads the current value of every field. */
function submitAll(guard: (fn: () => Promise<unknown>) => void) {
  const serverUrl = $<HTMLInputElement>('#t-url').value.trim();
  if (!serverUrl) return;
  guard(() =>
    api.tesseraeConnect({
      serverUrl,
      pairingCode: $<HTMLInputElement>('#t-code').value.trim(),
      deviceId: $<HTMLInputElement>('#t-devid').value.trim(),
      token: $<HTMLInputElement>('#t-token').value.trim(),
      slot: Number($<HTMLSelectElement>('#t-slot').value),
    }),
  );
}

export function mountTesserae(guard: (fn: () => Promise<unknown>) => void) {
  for (const id of ['#t-form', '#t-slot-form']) {
    $(id).addEventListener('submit', (ev) => {
      ev.preventDefault();
      submitAll(guard);
    });
  }

  $('#t-poll').addEventListener('click', () => guard(() => api.tesseraePoll()));

  $('#t-forget').addEventListener('click', () =>
    guard(async () => {
      await api.tesseraeForget();
      filled = false;
      for (const id of ['#t-url', '#t-code', '#t-devid', '#t-token']) {
        $<HTMLInputElement>(id).value = '';
      }
    }),
  );
}
