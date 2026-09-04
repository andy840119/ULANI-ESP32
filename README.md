# ULANI-ESP32

把 [Grassboy/ULANI.node.js](https://github.com/Grassboy/ULANI.node.js) 移植到
ESP32-C3：ESP32 開一個熱點，手機或電腦連進去後用網頁操作 ULANI 電子日曆，
不需要官方 App，也不需要一台開著藍牙的 Windows。

> **目前狀態：第一階段，連線已在真機打通。**
> 掃描、連線、配對、GATT 訂閱都已對著實機驗證（見
> [docs/protocol.md](docs/protocol.md)）。
> 圖片傳輸尚未成功，圖片上傳與前端 dither 也還沒實作。

## 硬體

- ESP32-C3，4 MB flash（無 PSRAM 需求）
- 一台 ULANI 電子日曆

192000 bytes 的影像 payload 從不進 RAM：前端算完後串流寫入，韌體再一邊讀一邊
以 230 bytes 為單位送出去，傳輸期間 RAM 峰值不到 16 KB。

## 編譯

前端必須先建，因為韌體會把它內嵌進去：

```bash
python tools/build_web.py          # npm install + build + gzip 進 components/web_server/www
idf.py set-target esp32c3
idf.py build
idf.py -p COM<n> flash monitor
```

改前端後重跑 `tools/build_web.py` 再 `idf.py build` 即可。

## 安裝（不編譯）

只是想燒錄、不想裝 esp-idf，可以用桌機版 Chrome / Edge 從瀏覽器刷機：到
[Releases](https://github.com/andy840119/ULANI-ESP32/releases) 抓
`ulani-esp32-merged.bin`，照 [docs/flashing.md](docs/flashing.md) 的步驟走即可。

## 使用

1. 開機後 ESP32 會開一個名為 `ULANI-Setup-XXXX` 的開放熱點（XXXX 是 MAC 末四碼）
2. 手機連上去後應該會自動跳出設定頁；沒跳的話開瀏覽器輸入 `http://192.168.4.1/`
3. 確認日曆已開機、且沒有被官方 App 或其他電腦佔用（一次只能一台）
4. 按「搜尋日曆」→ 選擇裝置 → 連線
5. 在「送測試圖到第幾張」選一個數字，約 30–60 秒

上傳歸上傳，切換頁面歸切換頁面：預設只把圖寫進去，畫面留在原本那一張，要換頁
請按下面的「切換目前顯示的那一張」。勾了「傳完後切到那一張」才會兩件事一起做。

例外是覆蓋掉正在顯示的那一張——那種情況一定會重繪，否則畫面會停在一張已經不存在
的舊圖。日曆重繪要花大半分鐘，那段期間收不了下一張，所以連續換好幾張時別勾。

送出去的圖右下角會自帶頁碼：一個約 2 公分寬的黑色直角三角形，裡面是白色的
頁數（1~4），所以拍一張日曆的照片就能知道現在看的是第幾頁。

連上過的日曆會被記住，**之後每次通電都會自動連回去**，大約在開機 10 秒後完成，
配對金鑰也存在 NVS 所以不需要重新配對。介面上會顯示「已記住」那台裝置，按
「清除記住的裝置」就會忘掉並停止自動連線。

你在日曆上按實體按鍵換頁時，日曆不會主動通知任何人，所以韌體會在 keepalive 裡
輪流問電量和目前相框，介面最慢 20 秒內就會跟上。

自動連線在兩種情況下會停止：你自己按「中斷連線」，或是閒置五分鐘後韌體主動把
日曆讓出來（這個行為沿用原專案，讓官方 App 有機會連上）。兩種情況都只要重新
連線一次就會再度啟用。

### 讓 tesserae 自動產生並推送日曆

[tesserae](https://github.com/dmellok/tesserae) 是一套可以自己架設的服務，負責產生
日曆畫面——樣式、顯示內容、更新頻率全部在它的網頁上設定。在 NAS 或任何能跑 Docker
的機器上：

```sh
mkdir tesserae && cd tesserae
curl -fsSLO https://raw.githubusercontent.com/dmellok/tesserae/main/docker-compose.yml
docker compose up -d
```

開 `http://<那台機器的IP>:8765` 設定密碼並走完精靈。

> **server 網址請填區網 IP，不要填外網網域名稱。** tesserae 預設只把 `/renders/`
> 的圖片給它認定在區網內的用戶端。透過外網網域連進去時（即使兩台其實在同一個區網，
> 因為 hairpin NAT 的關係）server 看到的來源是公網位址，註冊和心跳都會成功，
> 但下載圖片會被回 403。真要用外網網域的話，得到 Settings → Server 打開
> 「Allow REST clients on public networks」。

然後取得配對碼：

**Settings → Devices → Add device → Transport 選 REST API → 按「+ Issue pairing code」**

把碼複製下來，連同 server 網址填進本專案的「tesserae」分頁。用配對碼的話
**板子型號由 server 自動辨識**（會顯示成 Seeed XIAO ePaper EE04 (7.3" Spectra 6)，
內部代號 `seeed_ee04_73e6`），不需要手動選任何東西。

另外兩種加入方式不建議：

- **什麼都不填**：板子會自己報到，出現在 Settings → Devices 最上面的
  **Discovered** 區塊，按 Register 核准即可。
- **Add without pairing**：要自己挑板子型號和畫面尺寸，填錯 server 就會算出
  日曆看不懂的格式。它給的是 **access token 不是配對碼**，而且必須連同你自己取的
  Device id 一起填。

**ESP32 不做任何影像處理。** `seeed_ee04_73e6` 的規格（800×480、4bpp packed）
和 ULANI 完全相同，所以 server 算好的位元組可以直接送進日曆。唯一的轉換是調色盤
順序——tesserae 用 Spectra-6 的排列，ULANI 用七色 ACeP 的排列，在下載串流時用一張
16 格對照表換掉，見 [docs/protocol.md](docs/protocol.md)。

更新時機由 server 決定：板子 `POST /status`，server 在回應裡給 `next_poll_s`，
板子就照著等那麼久再回來問。tesserae 另有 MQTT/SSE 的推送通道，但它自己的文件把
SSE 定位成「最佳化，而非正確性需求」，而且只給有觸控的面板做局部更新用，所以這裡
和官方的 ESP32 韌體一樣走 REST 輪詢。

> server 關機時日曆就不會更新。

### 從電腦瀏覽器操作（選用）

介面的「WiFi」分頁可以讓 ESP32 連上你家裡的 WiFi。連上之後畫面會顯示一個
`http://192.168.x.x/` 的網址，用電腦瀏覽器開那個網址就能直接操作，不必再把
電腦或手機切到 ESP32 的熱點。

**熱點永遠會開著**，就算家用網路設定錯了或路由器關機也連得回來，密碼存在 NVS，
重開機會自動重連。

> ESP32 只有一組無線電，熱點和家用網路**必須待在同一個頻道**——這是所有型號的共同
> 限制，C3、S3、原版 ESP32 都一樣。所以連上路由器的瞬間熱點會被迫換到路由器的頻道，
> 正連著熱點的裝置會斷線幾秒再自動連回來。**熱點本身不會關閉**，`192.168.4.1`
> 一直有效。

### 日曆需要先回復出廠預設值

**如果日曆曾經和官方 App 配對過，必須先在日曆上回復出廠預設值，否則連不上。**
日曆會記住上一個配對過的裝置並拒絕新的配對，症狀是搜尋得到、一連線就失敗
（log 會顯示日曆在收到我們的 Pairing Request 之後直接切斷連線）。
移除手機上的 App 並不會清掉這筆記錄。

> 按著【功能鍵】再戳【重置孔】一下，直到 LED 燈開始閃爍後，再放開【功能鍵】。
> 螢幕會開始顯示回復出廠值的操作指示圖。

操作步驟引自 ULANI 官方說明：<https://www.ulani.com.tw/ulani-app.html>

## 專案結構

```
components/
  ulani_ble/        BLE central + 協定層。只認 opcode 和 byte stream，
                    不碰 HTTP、檔案系統或影像格式
  ulani_app/        商業邏輯：唯一能呼叫 ulani_ble 的 task、keepalive、狀態快照
  ulani_store/      四張成品的 SPIFFS 儲存，上傳與傳輸都是串流，不進 RAM
  tesserae/         tesserae server 的 REST client 與調色盤轉換
  net_provision/    SoftAP + captive portal DNS
  web_server/       HTTP：REST 端點 + 內嵌的前端靜態檔
main/               只做啟動組裝
web/                前端（獨立 npm 專案，不受 idf.py 管）
tools/
  build_web.py      前端 build → gzip → 韌體
  verify_payload.*  拿 C 實作跟照抄 JS 語意的 reference 對答案
docs/
  protocol.md       逆推出來的協定筆記，含刻意保留的怪癖
  background.md     為什麼有這個專案、連線策略還沒定案的部分
```

分層原則：協定層不知道有網頁，網頁層不知道有 opcode。
兩者之間只透過 `ulani_app.h` 溝通。

## REST API

| Method | Path | Body |
|---|---|---|
| GET | `/api/status` | |
| GET | `/api/devices` | |
| POST | `/api/scan` | `{"durationMs": 8000}` |
| POST | `/api/connect` | `{"address": "aa:bb:cc:dd:ee:ff"}` |
| POST | `/api/disconnect` | |
| POST | `/api/forget-device` | 忘記記住的日曆並停止自動連線 |
| GET | `/api/tesserae` | tesserae 連線狀態 |
| POST | `/api/tesserae/connect` | `{"serverUrl", "pairingCode", "deviceId", "token", "slot"}` |
| POST | `/api/tesserae/poll` | 不等排程，立刻抓一次 |
| POST | `/api/tesserae/forget` | 清除 server 設定與 token |
| POST | `/api/refresh` | 重讀電量與目前相框 |
| POST | `/api/slot` | `{"slot": 1}` |
| POST | `/api/test-image` | `{"slot": 1, "seed": 0, "activate": false}` |
| GET | `/api/wifi` | 站台狀態 + 最近一次掃描結果 |
| POST | `/api/wifi/scan` | 非同步掃描，結果從 `/api/wifi` 讀 |
| POST | `/api/wifi/connect` | `{"ssid": "...", "password": "..."}` |
| POST | `/api/wifi/forget` | 清除已存的網路 |

`npm run dev` 會把 `/api` proxy 到 `http://192.168.4.1`，可以邊改前端邊接真的板子。

## 已知風險

- **MTU**：230 bytes 的 write 需要 ATT MTU ≥ 233。實測日曆談到 **247**，
  所以切包方式不用改。
- **配對**：日曆拒絕在未加密的連線上讓我們訂閱 notify（回 ATT 0x05），所以韌體
  會在被拒時才發起配對並等待加密完成。目前提供的是 LE Legacy 配對
  （`CONFIG_ULANI_BLE_SECURE_CONNECTIONS` 預設關閉）。
- **WiFi/BLE 共存**：C3 單天線分時，傳圖那 30–60 秒網頁會變頓，這是正常的。
  `CONFIG_ULANI_BLE_CHUNK_GAP_MS` 可以調慢傳輸來換穩定度。
- **AP/STA 同頻道**：見上方說明。掃描 WiFi 時也會短暫佔用無線電，所以介面不會
  自動輪詢掃描，要按按鈕才掃。

專案的由來、官方 App 與 PC driver 的歷史、以及連線策略的取捨記在
[docs/background.md](docs/background.md)。

## 授權與致謝

協定的部分完全來自 Grassboy 的逆向工程成果，沒有那份函式庫就沒有這個專案。
