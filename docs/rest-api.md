# REST API

路徑依子系統分層：`system`（板子本身）、`calendar`（ULANI 裝置與四頁）、`wifi`、
`tesserae`（畫面來源服務）。前端的 `api` 物件照同樣的分層
（`api.calendar.get()`、`api.wifi.scan()`⋯，見 `web/src/lib/api.ts`）。

| Method | Path | Body / 說明 |
|---|---|---|
| GET | `/api/system/version` | 韌體版本；有版本號代表 production build |
| POST | `/api/system/settings` | `{"idleTimeoutMs": 300000}`（閒置多久後斷線，0=不斷） |
| GET | `/api/system/settings/export` | 匯出設定（含配對）為 JSON |
| POST | `/api/system/settings/import` | 匯入設定後重開機 |
| GET | `/api/calendar/status` | 連線狀態、電量、目前相框、掃描結果 |
| GET | `/api/calendar/devices` | 掃描到的裝置 |
| POST | `/api/calendar/scan` | `{"durationMs": 8000}` |
| POST | `/api/calendar/connect` | `{"address": "aa:bb:cc:dd:ee:ff"}` |
| POST | `/api/calendar/disconnect` | |
| POST | `/api/calendar/forget` | 忘記記住的日曆並停止自動連線 |
| POST | `/api/calendar/refresh` | 重讀電量與目前相框 |
| POST | `/api/calendar/slot` | `{"slot": 1}`（切換顯示的頁） |
| POST | `/api/calendar/test-image` | `{"slot": 1, "seed": 0, "activate": false}` |
| POST | `/api/calendar/slot/send` | `{"slot": 1}`（把存的圖送到那一頁） |
| POST | `/api/calendar/slot/badge` | `{"slot": 1, "on": true}` |
| GET | `/api/calendar/slot/download?slot=1` | 該頁目前的影像 payload |
| GET | `/api/wifi/status` | 站台狀態 + 最近一次掃描結果 |
| POST | `/api/wifi/scan` | 非同步掃描，結果從 `/api/wifi/status` 讀 |
| POST | `/api/wifi/connect` | `{"ssid": "...", "password": "..."}` |
| POST | `/api/wifi/forget` | 清除已存的網路 |
| GET | `/api/tesserae/status` | Tesserae 連線狀態 |
| POST | `/api/tesserae/connect` | `{"serverUrl", "pairingCode", "deviceId", "token", "slot"}` |
| POST | `/api/tesserae/poll` | 不等排程，立刻抓一次 |
| POST | `/api/tesserae/forget` | 清除 server 設定與 token |

> 掃描結果隨 `/api/calendar/status` 一起回，UI 只需要一個輪詢就能驅動整個畫面。
