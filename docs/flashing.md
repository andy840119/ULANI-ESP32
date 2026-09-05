# 用瀏覽器把韌體刷進 ESP32-C3

不需要裝 esp-idf、不需要命令列。用支援 Web Serial 的瀏覽器（**Chrome 或 Edge**，
桌機版；Firefox / Safari / 手機瀏覽器不支援）就能直接把韌體刷進板子。

適合的情況：想更新到新版本，或是拿到一塊全新的 ESP32-C3 要第一次燒錄。



## 1. 認清你的板子是 C3 還是 S3

---

我們提供兩種韌體，對應兩種最常見的晶片：**ESP32-C3** 和 **ESP32-S3**。燒錯會開不起來(板子不會壞掉，也能重新燒錄)。

板子上的模組通常會印型號（如 `ESP32-C3`、`ESP32-S3`），XIAO 系列則看背面
標示。不確定就先確認再燒。



## 2. 下載韌體

---

到本專案的 [Releases](https://github.com/andy840119/ULANI-ESP32/releases) 頁面，
在最新版本下面的 **Assets** 展開，依你的晶片抓對應的 merged 檔案：

- C3：`**ulani-esp32c3-merged.bin**`
- S3：`**ulani-esp32s3-merged.bin**`

merged 檔一個就包含 bootloader、分割表與韌體，直接燒在位置 `0x0`，這是你唯一需要
的檔案。

> 進階：如果你的工具需要分開的 `bootloader` / `partition-table` / app 三個檔，
> 它們沒有放進 Release，但可以到 [Actions](https://github.com/andy840119/ULANI-ESP32/actions)
> 對應那次建置的 **Artifacts** 下載（各自的燒錄位置是 `0x0` / `0x8000` /
> `0x10000`）。



## 3. 開刷機工具

---

打開 [**https://esptool.spacehuhn.com/**](https://esptool.spacehuhn.com/)（純網頁版 esptool，不會上傳你的檔案）。

1. 用 USB 線把 ESP32-C3 接上電腦。
2. 按 **Connect**，在跳出的視窗選擇板子的序列埠（找不到就看下面的疑難排解）。如果有遇到問題可以看下面的疑難排解，多半是因為需要手動切換到刷機模式(下載模式)，照說明按下板子上的按鍵就能解決。
3. 連上後，在檔案列表：
  - 可能會有多個列表，只留下一個其他清掉，留下哪一個都 OK。
  - 位置填 `0x0`，選剛剛下載、**對應你晶片**的那個 merged 檔。
4. 按 **Program** 開始燒錄，跑完就完成了。板子會自動重開。

> 燒錄設定（flash mode `dio`、freq `80m`、size `4MB`）已經包在 merged 檔裡，
> 用預設值即可，不用另外設定。



## 4. 完成後

---

板子重開後會開一個熱點，用手機或電腦連進去、開 `http://192.168.4.1/` 就是操作
介面。設定家裡 WiFi、連日曆、接 Tesserae 的完整步驟見 [getting-started.md](getting-started.md)。

> 韌體只是換掉程式本身。已經記住的日曆、WiFi、Tesserae 設定存在另一塊
> 分割區（NVS / SPIFFS），刷新版不會被清掉。



## 疑難排解

---

一些比較常見的問題:

- **Connect 後看不到序列埠 / 一直連不上**：多數 C3 開發板用內建 USB-JTAG，接上
就會出現；若你的板子是走 USB-UART 晶片（CH340、CP2102 等），要先裝對應驅動。
- **選了裝置後出現 `Couldn't sync to ESP. Try resetting.`（或連上卻同步失敗）**：
最常見的情況。網頁工具連到序列埠了，但板子還在跑你的韌體、沒進「下載模式」，
所以摸不到 ROM bootloader。C3 super-mini 走晶片內建的 USB（USB-Serial-JTAG），
`idf.py` 會自動重置進下載模式，但瀏覽器（esptool-js）常常沒辦法，所以要手動進：
  1. 按住 **BOOT** 鍵（GPIO9）不放。
  2. 維持按著 BOOT，**按一下 RESET**（或分不清哪顆、只有一顆鍵時：按著 BOOT
   不放，直接**拔掉再插上 USB**）。
  3. 放開 BOOT。
  4. 回到網頁按 **Connect**。板子可能會重新列舉，跳出選單就**重新選一次那個埠**。
  5. 這時它停在 ROM bootloader（不會跑 app），加上 merged 檔燒 `0x0`、按 Program
   就會過了。
  6. 燒完按一下 **RESET**（或重插 USB）離開下載模式，新韌體才會開始跑。
- **一直連不上、Connect 沒反應**：確認沒有別的程式占著同一個埠——Arduino IDE /
PlatformIO 的序列埠監控、另一個開著同一工具的分頁、或 `idf.py monitor` 都會搶
埠，一次只能有一個程式用。關掉後拔插一次 USB 再試。
- **Program 中途失敗**：換一條「可傳資料」的 USB 線（有些線只有供電），或降低
工具裡的 baud rate 再試。
- **瀏覽器沒有 Connect 按鈕 / 提示不支援**：改用桌機版 Chrome 或 Edge，Web Serial
只在這兩個上面有。



## 給維護者：Release 怎麼產生

---

`.github/workflows/release.yml` 會在推上版本 tag 時自動 build C3 與 S3 兩種韌體，
並把上面那些 `.bin` 附到對應的 GitHub Release。tag 的命名規則見
[docs/releasing.md](releasing.md)。手動想產生一份也可以到 Actions 頁面用
**Run workflow**（workflow_dispatch），產物會放在該次 run 的 artifacts。