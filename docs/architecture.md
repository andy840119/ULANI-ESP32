# 專案結構

```
components/
  ulani_ble/        BLE central + 協定層。只認 opcode 和 byte stream，
                    不碰 HTTP、檔案系統或影像格式
  ulani_app/        商業邏輯：唯一能呼叫 ulani_ble 的 task、keepalive、狀態快照
  ulani_store/      四頁成品的 SPIFFS 儲存，上傳與傳輸都是串流，不進 RAM
  tesserae/         Tesserae server 的 REST client 與調色盤轉換
  net_provision/    SoftAP + STA + captive portal DNS
  web_server/       HTTP：REST 端點 + 內嵌的前端靜態檔
  status_led/       板上 LED 狀態燈（可用 Kconfig 選板子／腳位）
main/               只做啟動組裝
web/                前端（獨立 Vite 專案，不受 idf.py 管）
  src/lib/          api client、共用 UI plumbing
  src/pages/        一個分頁一個 module（markup / render / mount）
tools/
  build_web.py      前端 build → gzip → 韌體
  verify_payload.*  拿 C 實作跟照抄 JS 語意的 reference 對答案
docs/               這些文件
```

## 分層原則

- **協定層不知道有網頁，網頁層不知道有 opcode。** `ulani_ble` 只處理 BLE 與位元組；
  它不知道 HTTP、檔案或影像格式的存在。
- 兩層之間只透過 `ulani_app.h` 溝通。`ulani_app` 是唯一被允許呼叫 `ulani_ble` 的地方，
  也是唯一持有那條藍牙連線的 task。
- 影像 payload（192000 bytes）**從不整份進 RAM**：前端算完後串流寫入 SPIFFS，韌體再
  一邊讀一邊以 230 bytes 為單位送出去，傳輸期間 RAM 峰值不到 16 KB。

## 前端

`web/` 是獨立的 Vite + TypeScript 專案，build 後 gzip 內嵌進 `web_server`。每個分頁
（日曆／WiFi／Tesserae／設定）是 `src/pages/` 底下一個 module，各自 export
`xMarkup()` / `renderX()` / `mountX()`；`main.ts` 只負責組裝與那個唯一的狀態輪詢。
共用的 `$`、busy-lock guard、以及 API client 在 `src/lib/`。
