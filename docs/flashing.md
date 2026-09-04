# 用瀏覽器把韌體刷進 ESP32-C3

不需要裝 esp-idf、不需要命令列。用支援 Web Serial 的瀏覽器（**Chrome 或 Edge**，
桌機版；Firefox / Safari / 手機瀏覽器不支援）就能直接把韌體刷進板子。

適合的情況：想更新到新版本，或是拿到一塊全新的 ESP32-C3 要第一次燒錄。

## 1. 下載韌體

到本專案的 [Releases](https://github.com/andy840119/ULANI-ESP32/releases) 頁面，
在最新版本下面的 **Assets** 展開，抓這個檔案：

- **`ulani-esp32-merged.bin`** — 一個檔案包含 bootloader、分割表與韌體，直接燒在
  位置 `0x0`，最省事，**建議用這個**。

如果你的工具需要分開的檔案（進階），同一個 Release 也有：

| 檔案 | 燒錄位置 |
| --- | --- |
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `ulani_esp32.bin` | `0x10000` |

## 2. 開刷機工具

打開 **<https://esptool.spacehuhn.com/>**（純網頁版 esptool，不會上傳你的檔案）。

1. 用 USB 線把 ESP32-C3 接上電腦。
2. 按 **Connect**，在跳出的視窗選擇板子的序列埠（找不到就看下面的疑難排解）。
3. 連上後，在檔案列表：
   - 位置填 `0x0`，選剛剛下載的 **`ulani-esp32-merged.bin`**。
   - （若用分開檔案，就依上表逐一加上三個檔案與各自的位置。）
4. 按 **Program** 開始燒錄，跑完就完成了。板子會自動重開。

> 燒錄設定（flash mode `dio`、freq `80m`、size `4MB`）已經包在 merged 檔裡，
> 用預設值即可，不用另外設定。

## 3. 完成後

板子重開後會開一個熱點，用手機或電腦連進去、開 `http://192.168.4.1/` 就是操作
介面。設定家裡 WiFi、連日曆的步驟都在網頁上（見 [README](../README.md)）。

> 韌體只是換掉程式本身。已經記住的日曆、WiFi、Tesserae 設定存在另一塊
> 分割區（NVS / SPIFFS），刷新版不會被清掉。

## 疑難排解

- **Connect 後看不到序列埠 / 一直連不上**：多數 C3 開發板用內建 USB-JTAG，接上
  就會出現；若你的板子是走 USB-UART 晶片（CH340、CP2102 等），要先裝對應驅動。
- **選了埠卻連線失敗**：按住板子的 **BOOT** 鍵再插 USB（或再按一下 **RESET**）
  讓它進下載模式，然後再 Connect。
- **Program 中途失敗**：換一條「可傳資料」的 USB 線（有些線只有供電），或降低
  工具裡的 baud rate 再試。
- **瀏覽器沒有 Connect 按鈕 / 提示不支援**：改用桌機版 Chrome 或 Edge，Web Serial
  只在這兩個上面有。

## 給維護者：Release 怎麼產生

`.github/workflows/release.yml` 會在推上 `v*` 這種版本 tag 時自動 build 韌體，
並把上面那些 `.bin` 附到對應的 GitHub Release。手動想產生一份也可以到 Actions
頁面用 **Run workflow**（workflow_dispatch），產物會放在該次 run 的 artifacts。
