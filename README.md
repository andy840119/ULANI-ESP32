# ULANI-ESP32

讓 [ULANI 電子日曆](https://www.ulani.com.tw/) 變成一台
[Tesserae](https://github.com/dmellok/tesserae) 底下的裝置。

一片 ESP32 常駐在日曆旁邊，透過藍牙控制日曆；Tesserae（自架的儀表板服務）按你設定
的樣式和頻率算好圖，ESP32 定時去把圖抓回來、送進日曆。全程不需要官方 App，也不需要
一台開著藍牙的電腦。ESP32 會開一個熱點，用手機或電腦連進去、開網頁就能設定。

- **四頁獨立**：ULANI 有四頁，可各接一個 Tesserae dashboard。
- **記住日曆**：配對一次後每次通電自動連回，配對金鑰存在板子上。
- **省電**：閒置就放掉藍牙連線，要送圖時自己連回來，不用有人在場。
- **C3 / S3 皆可**：原始碼不綁晶片。

## 我是新手，只想讓日曆動起來

看 **[docs/getting-started.md](docs/getting-started.md)** —— 從架 Tesserae、燒韌體、
到第一張圖上牆的完整教學，會解釋中間每個角色是什麼。

## 我想改這個專案

- [docs/building.md](docs/building.md) — clone 之後怎麼編譯、燒錄、跑前端。
- [docs/architecture.md](docs/architecture.md) — 專案結構與分層。
- [docs/rest-api.md](docs/rest-api.md) — 韌體的 REST 端點。
- [docs/protocol.md](docs/protocol.md) — 逆推出來的 ULANI 藍牙協定。
- [docs/flashing.md](docs/flashing.md) — 用瀏覽器刷 release 韌體。
- [docs/releasing.md](docs/releasing.md) — 發 release 的版號規則。
- [docs/known-issues.md](docs/known-issues.md) — 已知限制。
- [docs/background.md](docs/background.md) — 為什麼有這個專案、為什麼這樣選型。
- [AGENTS.md](AGENTS.md) — 給 coding agent 的導覽。

## 致謝

- **[Grassboy/ULANI.node.js](https://github.com/Grassboy/ULANI.node.js)** —— 日曆的
  藍牙協定完全來自這份逆向工程成果，本專案的協定層照它實作，沒有它就沒有這個專案。
- **[Tesserae](https://github.com/dmellok/tesserae)** —— 產生日曆畫面的服務，本專案
  把 ULANI 接成它的一台裝置。
- **[Claude Code](https://claude.com/claude-code)** —— 大部分程式與文件由它協助完成。
