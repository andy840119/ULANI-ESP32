# 新手教學：讓日曆自動更新

這份教學帶你從零把整條路接起來：Tesserae 算圖 → ESP32 抓圖 → 送進 ULANI 日曆。
不需要寫程式，但需要照著設定幾個東西。

## 先認識這幾個角色

| 角色 | 是什麼 | 在這裡的工作 |
| --- | --- | --- |
| **ULANI 電子日曆** | 你手上那台 7.3 吋電子紙日曆 | 顯示畫面。它只會被藍牙更新，一次只能被一台裝置連著。 |
| **ESP32** | 一片很小、很便宜（約台幣一兩百元）的開發板，內建藍牙和 WiFi | 常駐在日曆旁邊。一邊用藍牙連日曆，一邊用 WiFi 連你家網路去跟 Tesserae 拿圖，再把圖送進日曆。**本專案的韌體就是燒在它上面。** |
| **Tesserae** | 一套你自己架在電腦／NAS 上的服務 | 產生日曆畫面：要顯示什麼、長怎樣、多久更新，全部在它的網頁上設定。它把每台裝置畫成一張「dashboard」。 |
| **你家 WiFi** | | 讓 ESP32 連得到 Tesserae。 |

流程是：**你在 Tesserae 上設計畫面 → Tesserae 算成圖 → ESP32 定時抓回來 → 藍牙送進日曆。**

你需要準備：一台 ULANI 日曆、一片 ESP32（ESP32-C3 或 S3，帶 USB）、一台能一直開著跑
Docker 的電腦或 NAS。

---

## 第一步：把 Tesserae 架起來

在能跑 Docker 的機器上：

```sh
mkdir tesserae && cd tesserae
curl -fsSLO https://raw.githubusercontent.com/dmellok/tesserae/main/docker-compose.yml
docker compose up -d
```

開瀏覽器進 `http://<那台機器的區網IP>:8765`，設定密碼、走完初始精靈。先擺著，
第三步會回來拿「配對碼」。

> Tesserae 的細節看它自己的[說明](https://github.com/dmellok/tesserae)。

## 第二步：弄一片 ESP32 並燒進韌體

買一片 **ESP32-C3** 或 **ESP32-S3** 開發板（例如常見的 C3 SuperMini），用 USB 線接電腦。

不想裝開發環境的話，用桌機版 Chrome / Edge 從瀏覽器就能燒：到本專案的
[Releases](https://github.com/andy840119/ULANI-ESP32/releases) 抓對應晶片的
`ulani-esp32c3-merged.bin` 或 `ulani-esp32s3-merged.bin`，照
**[flashing.md](flashing.md)** 一步步做（含進入下載模式、選埠、燒錄位置）。

燒完板子會重開，開始廣播一個熱點。

## 第三步：進 ESP32 後台，把三條線接上

ESP32 開機後會開一個名為 `ULANI-Setup-XXXX` 的**開放熱點**（XXXX 是板子 MAC 末四碼）。

1. 手機或電腦連上這個熱點，通常會自動跳出設定頁；沒跳就開瀏覽器輸入
   `http://192.168.4.1/`。這就是 ESP32 的後台。

2. **接日曆（藍牙）**——在「日曆」分頁：
   - 確認日曆已開機、沒有被官方 App 或其他電腦連著（一次只能一台）。
   - 按「搜尋日曆」→ 選你的裝置 → 連線。
   - 連上後可以按「送測試圖」丟一張色塊圖確認整條藍牙通了。
   - **連不上、出現 `connect: ESP_FAIL`？** 多半是日曆還記著舊配對（曾配過官方
     App，或這片板子重刷過韌體）。照「日曆」分頁裡的說明，把日曆**回復出廠預設值**
     後再連。連上後配對會記在板子上，之後通電自動連回。

3. **接家裡 WiFi**——在「WiFi」分頁：搜尋、選你家網路、輸入密碼、連線。
   - 連上後畫面會顯示一個 `http://192.168.x.x/` 的網址，之後用電腦瀏覽器開那個網址
     就能直接進後台，不用再切到 ESP32 的熱點。
   - 熱點不會關，設錯也回得來。連上路由器的瞬間手機可能斷幾秒再自動連回，是正常的。

4. **接 Tesserae**——在「Tesserae」分頁：
   - 回到 Tesserae 網頁拿配對碼：**Settings → Devices → Add device → Transport 選
     REST API → 按「+ Issue pairing code」**。
   - 把 **server 網址**（填**區網 IP**，例如 `http://192.168.1.10:8765`，不要填外網
     網域）和**配對碼**填進某一頁（第 1～4 頁各自獨立）。用配對碼時板子型號會被自動
     辨識，不用手動選。

> **為什麼 server 網址要填區網 IP？** Tesserae 預設只把圖片給區網內的用戶端；填外網
> 網域會註冊成功、但下載圖片時被回 403。配對碼是一次性的，失敗就回去再按一次拿新的。

## 第四步：做一張 dashboard，看它上牆

1. 在 Tesserae 網頁上，替剛剛那台裝置編輯它的 dashboard——放時鐘、行事曆、天氣、
   照片……都在 Tesserae 上設定，**ESP32 不做任何影像處理**。
2. 設定它多久更新一次。時間到，Tesserae 算好新圖，ESP32 下次向它詢問時就會抓回來、
   自動送進日曆。想馬上看效果，可以在 ESP32 後台的「Tesserae」分頁按「從 Tesserae
   更新資料」再按「把圖片送到 ULANI」。
3. 送出去的圖右下角會自帶頁碼（可在該頁設定裡關掉），拍張照就知道現在看的是第幾頁。

接好之後就不用管了：Tesserae 按排程算圖，ESP32 按排程抓圖送圖，日曆自己更新。

> Tesserae 那台機器關機時，日曆就不會再更新。
