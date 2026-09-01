# ULANI-ESP32

把 [Grassboy/ULANI.node.js](https://github.com/Grassboy/ULANI.node.js) 移植到
ESP32-C3：ESP32 開一個熱點，手機或電腦連進去後用網頁操作 ULANI 電子日曆，
不需要官方 App，也不需要一台開著藍牙的 Windows。

> **目前狀態：第一階段。** BLE 協定、熱點、captive portal 與「隨機更新畫面」
> 自我測試已完成且可編譯。圖片上傳與前端 dither 尚未實作。
> **所有 BLE 行為都還沒在真機上驗證過**，請見 [docs/protocol.md](docs/protocol.md)
> 最後一節。

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
5. 按「隨機更新畫面」送一張測試圖，約 30–60 秒

## 專案結構

```
components/
  ulani_ble/        BLE central + 協定層。只認 opcode 和 byte stream，
                    不碰 HTTP、檔案系統或影像格式
  ulani_app/        商業邏輯：唯一能呼叫 ulani_ble 的 task、keepalive、狀態快照
  net_provision/    SoftAP + captive portal DNS
  web_server/       HTTP：REST 端點 + 內嵌的前端靜態檔
main/               只做啟動組裝
web/                前端（獨立 npm 專案，不受 idf.py 管）
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
| POST | `/api/refresh` | 重讀電量與目前相框 |
| POST | `/api/slot` | `{"slot": 1}` |
| POST | `/api/test-image` | `{"slot": 1, "seed": 0}` |

`npm run dev` 會把 `/api` proxy 到 `http://192.168.4.1`，可以邊改前端邊接真的板子。

## 已知風險

- **MTU**：230 bytes 的 write 需要 ATT MTU ≥ 233。Node 版靠作業系統談，
  這裡在 `sdkconfig.defaults` 要到 247，但日曆給不給還沒實測。
- **配對**：原專案要求先在 Windows 配對，所以韌體預設主動發起 pairing
  （`CONFIG_ULANI_BLE_INITIATE_SECURITY`），失敗只警告不中斷。
- **WiFi/BLE 共存**：C3 單天線分時，傳圖那 30–60 秒網頁會變頓，這是正常的。
  `CONFIG_ULANI_BLE_CHUNK_GAP_MS` 可以調慢傳輸來換穩定度。

## 授權與致謝

協定的部分完全來自 Grassboy 的逆向工程成果，沒有那份函式庫就沒有這個專案。
