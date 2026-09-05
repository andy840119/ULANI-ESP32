# 給 coding agent 的導覽

ESP32 韌體，讓 ULANI 電子日曆變成 [Tesserae](https://github.com/dmellok/tesserae) 底下的
一台裝置（藍牙連日曆、WiFi 連 Tesserae 拿圖）。先讀對的文件，別重新推導已經寫下來的東西。

## 先讀哪些

- [docs/architecture.md](docs/architecture.md) — 專案結構與分層。**動手前先看。**
- [docs/protocol.md](docs/protocol.md) — 逆推出來的 ULANI 藍牙協定。
- [docs/background.md](docs/background.md) — 為什麼這樣選型、連線策略。
- [docs/rest-api.md](docs/rest-api.md) — 韌體 REST 端點（前端 `web/src/lib/api.ts` 對應）。
- [docs/building.md](docs/building.md) — 怎麼編譯、燒錄、跑前端。
- [docs/known-issues.md](docs/known-issues.md) — 已知限制，別當成 bug 去「修」。

## 一定要知道的規矩

- **改前端後，先 `python tools/build_web.py` 再 `idf.py build`。** 韌體內嵌前端；跳過會
  build 失敗或燒到舊版。
- **`ulani_app` 是唯一能呼叫 `ulani_ble` 的地方**，也是唯一持有那條藍牙連線的 task。
  協定層不碰 HTTP／檔案／影像，網頁層不碰 opcode。
- **`protocol.md` 裡標「刻意保留」的怪癖不要「順手修掉」**（例如 CRC 未 padding）。它們
  是對著實機驗證過的，改了會壞。
- 影像 payload 從不整份進 RAM，都是串流。別引入需要一次載入整張圖的寫法。
- 前端每個分頁是 `web/src/pages/` 一個 module（`xMarkup`/`renderX`/`mountX`），共用東西在
  `web/src/lib/`。

## 編譯 / 燒錄

ESP-IDF v5.5。`idf.py set-target esp32c3`（或 `esp32s3`）→ `idf.py build` →
`idf.py -p <port> flash monitor`。細節見 [docs/building.md](docs/building.md)。

## 發 PR

看 [docs/pull-requests.md](docs/pull-requests.md)——**講重點，別長篇解釋不重要的細節。**
