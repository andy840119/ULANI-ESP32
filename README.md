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

## 使用

1. 開機後 ESP32 會開一個名為 `ULANI-Setup-XXXX` 的開放熱點（XXXX 是 MAC 末四碼）
2. 手機連上去後應該會自動跳出設定頁；沒跳的話開瀏覽器輸入 `http://192.168.4.1/`
3. 確認日曆已開機、且沒有被官方 App 或其他電腦佔用（一次只能一台）
4. 按「搜尋日曆」→ 選擇裝置 → 連線
5. 在「送測試圖到第幾張」選一個數字，約 30–60 秒

連上過的日曆會被記住，**之後每次通電都會自動連回去**，大約在開機 10 秒後完成，
配對金鑰也存在 NVS 所以不需要重新配對。介面上會顯示「已記住」那台裝置，按
「清除記住的裝置」就會忘掉並停止自動連線。

自動連線在兩種情況下會停止：你自己按「中斷連線」，或是閒置五分鐘後韌體主動把
日曆讓出來（這個行為沿用原專案，讓官方 App 有機會連上）。兩種情況都只要重新
連線一次就會再度啟用。

### 上傳自己的圖片

「傳圖」分頁可以為四張相框各上傳一張圖片。選好檔案後**瀏覽器會立刻把它處理成
日曆的七色並顯示預覽**，那張預覽就是實際會被送出去的畫面。確認後按「上傳」存進
ESP32，再按「送到日曆」透過藍牙傳過去。

「處理方式」可以調整旋轉與比例：

| 選項 | 行為 |
|---|---|
| 完整顯示（黑色留邊） | 等比縮放，留邊填純黑 |
| 填滿畫面 | 裁掉超出的部分 |
| 完整顯示（暗化原圖填滿留邊） | `dither.js` 原本的效果 |

**影像處理全部在瀏覽器裡完成**（`web/src/lib/ulani-image.ts`），ESP32 收到的已經是
成品——palette index 的位元組流。原圖從不上傳，所以四張圖在 flash 裡只佔
4 × 192000 = 768 KB。這也是唯一可行的做法：800×480 的誤差擴散需要超過 1 MB 的
工作記憶體，而 C3 只有 400 KB。

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
  net_provision/    SoftAP + captive portal DNS
  web_server/       HTTP：REST 端點 + 內嵌的前端靜態檔
main/               只做啟動組裝
web/                前端（獨立 npm 專案，不受 idf.py 管）
  src/lib/ulani-image.ts   縮放/裁切/旋轉、dither、打包——所有影像處理都在這
tools/
  build_web.py      前端 build → gzip → 韌體
  verify_payload.*  拿 C 實作跟照抄 JS 語意的 reference 對答案
docs/protocol.md    逆推出來的協定筆記，含刻意保留的怪癖
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
| POST | `/api/upload?slot=N` | 上傳 192000 bytes 的成品（`application/octet-stream`） |
| GET | `/api/slot/download?slot=N` | 取回已存的成品，用於重新整理後的預覽 |
| POST | `/api/slot/send` | `{"slot": 1}`，把已存的圖傳到日曆 |
| POST | `/api/slot/clear` | `{"slot": 1}` |
| POST | `/api/refresh` | 重讀電量與目前相框 |
| POST | `/api/slot` | `{"slot": 1}` |
| POST | `/api/test-image` | `{"slot": 1, "seed": 0}` |
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

## 授權與致謝

協定的部分完全來自 Grassboy 的逆向工程成果，沒有那份函式庫就沒有這個專案。
