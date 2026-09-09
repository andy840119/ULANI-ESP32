# 事件日誌

板子平常不接電腦，而 console 走 UART0、實際上沒東西接在上面，所以**序列埠的訊息等於
不存在**。這套日誌是為了回答「沒人在場的時候發生了什麼事」而做的（issue #52）。

## 兩層

| | Tier A：事件 | Tier B：文字 trace |
|---|---|---|
| 內容 | 固定 24 bytes 的結構化紀錄 | `ESP_LOGx` 原本就會印的東西 |
| 收集 | 各處呼叫 `diag_log()` | 掛 `esp_log_set_vprintf()`，既有的 log 一行都不用改 |
| 存哪 | RAM ring →（可選）寫進 SPIFFS | **只在 RAM**，斷電就沒了 |
| 撐多久 | 預設 256 KB，約 10 天 | 8 KB，大約幾十分鐘 |
| 用來回答 | 「哪一天幾點、前後順序」 | 「那一刻的細節」 |

兩層都可以在後台的**設定 → 事件日誌**開關、匯出、清除。「停用」只是停止記錄，
**不會清掉已經記下的東西**——會去按那個開關的時候，通常正是最不該毀證的時候。

## 紀錄長什麼樣

```c
typedef struct {
    uint32_t seq;       /* 單調遞增；斷號 = 那段被回收掉了 */
    uint32_t uptime_ms; /* 永遠有效，對時之前也一樣 */
    uint32_t epoch;     /* 0 = 當下還不知道時間 */
    uint16_t code;      /* 事件編號，見 diag_log.h 的 diag_code_t */
    uint8_t  slot;      /* 1..4，或 0 表示與頁碼無關 */
    int8_t   result;    /* 0 = ok，其餘依事件而定 */
    int32_t  a, b;      /* 依事件而定 */
} diag_rec_t;
```

**不存字串**，這是它能壓到 24 bytes 的原因，也表示措辭歸前端管：
`web/src/lib/logcodes.ts` 是這份格式的另一半。

### 兩條不能違反的規則

1. **事件編號只增不改。** 這些數字會寫進 flash、會出現在匯出檔裡，所以一個編號的意思
   永遠不變：不重編、不回收已刪除的編號、不改動既有編號 `a`／`b` 的意義（要改就開一個
   新編號）。改了的話，以後讀舊的匯出檔會讀成別的意思。
2. **改動 `diag_rec_t` 的欄位配置就要 bump `DIAG_SCHEMA_VERSION`**（新增編號不用）。
   版本號寫在每個 flash segment 的檔頭和每份匯出檔的第一行，版本對不上的 segment 會被
   忽略而不是誤讀。

## 容量與回收

日誌**不會寫到空間用完為止**。`storage` 分割區是 2.24 MB，四頁成品要 768 KB，傳輸中還
會多一個 192 KB 的 `.tmp`；日誌要是把空間吃光，下一張 frame 就存不進去、日曆從此不再
更新。**除錯工具不該弄壞它在除錯的東西。**

- **固定上限、分段輪替**：等大的 64 KB segment，保留 2～8 個（128 KB～512 KB）。永遠
  append 到最新的那一段，滿了就**把最舊的那一段截斷後重用**。SPIFFS 刪整個檔很便宜，
  從檔頭砍資料很貴。
- **segment 的新舊看檔頭裡的 `seg_seq`，不是檔名**——檔名會被重複使用。
- **空間下限**：要開一個新 segment 時如果分割區剩不到 400 KB，就不開新的（改成回收舊
  的，因為回收會**釋放**空間而不是佔用）；真的無處可去就暫停寫 flash，並記一筆
  `DIAG_SYS_LOG_PAUSED`。
- **傳圖期間絕不寫 flash**：無線電是共用的，那 30 秒本來就吃緊（見
  [known-issues.md](known-issues.md)）。落地只在 idle 時批次做，`main.c` 用
  `ulani_app_transfer_active()` 把關。
- **回收看得見**：被丟掉的筆數會累計並記成 `DIAG_SYS_LOG_DROPPED`，`seq` 的斷號也留在
  那裡。匯出檔會說少了幾筆，而不是假裝自己是完整的。

## 匯出格式

CSV（預設）第一行是註解，其餘是 `seq,uptime_ms,epoch,code,slot,result,a,b`：

```
# ulani event log schema=1 fw=dev boot=1a2b3c4d exported=1788610203 dropped=0
seq,uptime_ms,epoch,code,slot,result,a,b
```

`format=ndjson` 則是一行一個 JSON 物件，欄位名同上。匯出的內容是 **flash 裡的部分接上
還只在 RAM 裡的部分**，接點取自 `flushed_seq`。

> **`seq` 每次開機從 0 重數**，所以 flash 裡可能留著上一次開機、數字更大的紀錄。要判斷
> 「RAM 裡哪些還沒落地」只能看 `flushed_seq`，不能拿 flash 最後一筆的 `seq` 往下接——
> 那樣重開機之後會整段掉。檔案本身仍是照時間順序寫的；開機的分界看 `code=100` 那一筆
> （它帶著 boot id）。

## 時間

板子沒有 RTC。開機後連上網路才會用 **SNTP** 對時（優先用 DHCP 給的 NTP server，通常是
路由器；退而求其次才是 `pool.ntp.org`），連不到 NTP 時由 Tesserae 回應的 HTTP `Date`
header 補上。對時之前寫下的紀錄 `epoch` 是 0，只有 `uptime_ms` 有意義——所以兩個欄位都
留著，事後可以靠 `DIAG_SYS_TIME_SYNC` 那一筆把前面那段推算回真正的時間。

## 讀取的路徑不要用 cJSON 組樹

即時清單和匯出都是**一筆一筆格式化進行緩衝區、用 chunk 串流出去**的，不要為了方便改成
「先組成 cJSON 再一次列印」。一筆紀錄組成樹大約要九個節點加八個 key 字串（約 800 bytes），
兩百筆就遠超過這塊板子僅有的 ~60 KB 可用 heap，而且**失敗的時機正好是日誌終於累積到有東西
可看的時候**——一開始測都好好的，跑幾小時之後才開始整個讀不出來。

## 怎麼加一個事件

1. 在 `diag_log.h` 的 `diag_code_t` **最後面**加一個編號，註解寫清楚 `a`／`b`／`result`
   各代表什麼。
2. 在事發的地方呼叫 `diag_log(code, slot, result, a, b)`。它可以從任何 task 呼叫，
   不配置記憶體、不格式化字串、不碰 flash——**但不要放進傳圖那個逐包迴圈裡**，
   會改變被觀測對象的行為。
3. 在 `web/src/lib/logcodes.ts` 的 `describe()` 加上對應的中文敘述；如果那是「一眼要看到
   的事」，順便加進 `isNotable()`。
4. `python tools/build_web.py` 再 `idf.py build`。
