# ULANI BLE 協定筆記

全部內容是從 [Grassboy/ULANI.node.js](https://github.com/Grassboy/ULANI.node.js)
的 `src/BLEComm.js` 與 `src/dither.js` 逆推而來，沒有官方文件。
遇到原始碼的怪異行為時，這份移植選擇**照抄而非修正**——理由見下方「已知怪癖」。

## GATT

| 項目 | UUID | 用途 |
|---|---|---|
| Service | `1234a200-7cbc-11e9-8f9e-2a86e4085a59` | |
| Char 201 | `1234a201-…` | op channel，write-with-response + notify |
| Char 202 | `1234a202-…` | data channel，write-with-response + notify |

廣播名稱以 `ULANI Calendar` 開頭，後面接裝置專屬碼（例：`ULANI CalendarC0FFEE`）。

Node 版要求先在 Windows 藍牙設定完成配對，這點已由實機確認：日曆在未加密的連線上
會拒絕 CCCD 寫入。韌體因此採取「被拒才配對」的順序——先直接嘗試訂閱，收到
ATT 0x05 之後才發起配對並等待加密完成，這樣不在乎加密的機型就不會被打擾。
詳見下方「實機驗證結果」。

## Opcode

寫入 op channel，裝置以 notify 回覆，回覆的第一個 byte 會重複同一個 opcode。
下表的「回覆」是前兩個 byte。

| Frame | 意義 | 成功回覆 |
|---|---|---|
| `04 4e 42` | checkCustomerID，每次傳圖前必送 | `04xx` |
| `06 00` | 讀電量，同時當 keepalive | `06<level>` |
| `09 03` | 請求斷線 | `09xx` |
| `0b 0<slot>` | 切換顯示的相框（slot 1–4） | `0b00` |
| `0c 00` | 查詢目前相框 | `0c0<slot>` |
| `01 …` | 開始傳圖，見下 | `0100` = 接受 |

Node 版對 op 回覆設 10 秒 timeout。超時的話它 resolve 成 `<op>9999` 繼續跑，
韌體則回傳 `ESP_ERR_TIMEOUT` 讓上層決定。

### 時序

- 每 10 秒送一次 `06 00` 當 ack，否則連線會被裝置切掉。
- 超過 5 分鐘沒有任何實質操作，Node 版主動送 `09 03` 放掉裝置。
- 一台主機同時只能控制一台 ULANI；官方 App 若連著，ESP32 會連不上。

## 影像格式

面板固定 800×480。dither 成 7 色，palette 順序（取自 `dither.js`）：

| index | RGB | |
|---|---|---|
| 0 | `0,0,0` | 黑 |
| 1 | `209,208,202` | 白 |
| 2 | `69,121,81` | 綠 |
| 3 | `82,91,151` | 藍 |
| 4 | `175,76,74` | 紅 |
| 5 | `207,194,88` | 黃 |
| 6 | `192,99,30` | 橘 |

每個 pixel 一個 palette index，以 row-major 順序排成一長串數字字元，
再兩兩一組 hex-decode，**等於一個 byte 裝兩個 pixel，先高 nibble**。

```
384000 pixels → 384000 nibbles → 192000 bytes
```

Node 版把數字字串切成 460 字元一段，所以每次 BLE write 是 **230 bytes**：

```
192000 = 834 × 230 + 180      → 835 個封包，最後一包 180 bytes
```

每包之間 sleep 20 ms。230 bytes 的 write 需要 ATT MTU ≥ 233。

### 傳圖流程

1. 對整包 192000 bytes 算 CRC-16/XMODEM（poly `0x1021`, init `0x0000`）
2. 送 `04 4e 42`
3. 組 header 字串並送出：

   ```
   "010002ee000" + slot + "02" + <ms timestamp 的低 8 位 hex> + <crc hex>
   ```

4. 收到 `0100` 才開始，否則中止
5. 依序寫 835 個封包到 data channel
6. data channel 會 notify 一個 `02xx` frame：`0200` = 成功

傳輸中途若提早收到 `02xx`，代表裝置放棄了，要停止送資料。

## 已知怪癖（刻意保留）

**CRC 沒有補零。** `dither.js` 用 `crc.toString(16)` 產生 header 裡的 CRC，
沒有 `padStart(4, '0')`。當 CRC 小於 `0x1000` 時字串只有 3 碼，
接著 `hexToUint8Array` 用 `substr(i, 2)` 兩兩取字元，最後落單的那個字元
會被 `parseInt` 當成**完整一個 byte**：

```
crc = 0x0381 → "381" → [0x38, 0x01]     ← 而不是 [0x03, 0x81]
```

也就是說大約 1/16 的圖片，header 裡的 CRC 是錯位的——但既然 Node 版實測可以
換圖成功，代表裝置要嘛不驗 CRC，要嘛用同樣的錯位方式解讀。在有實機確認之前，
`ulani_hex_to_bytes()` 與 `ulani_build_send_header()` 完整複製這個行為。

`tools/verify_payload.js` 與 `tools/verify_payload.c` 會在數個 seed 上比對
C 實作與照抄 JS 語意的 reference 實作，其中 `seed=0xdeadbeef` 就是專門
用來釘住這個 case 的。

## 實機驗證結果

對著一台 `ULANI CalendarD536FD`（public address）實測：

| 項目 | 結果 |
|---|---|
| 廣播位址型別 | public，不會輪替 |
| ATT MTU | **247**，230 bytes 的寫入不需分段 |
| GATT handle | service 15–21；op 值 17 / CCCD 18；data 值 20 / CCCD 21 |
| 加密 | **必要**。未加密時寫 CCCD 會被回 ATT 0x05（insufficient authentication） |
| 配對 | 日曆一連上就主動送 Security Request（authreq `0x09`，bonding + SC） |

### 為什麼 central 也需要 peripheral 角色

`CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` 必須開著。NimBLE 把**接收** notification 的
處理放在 ATT server（`ble_att_svr_rx_notify`）裡，關掉 peripheral 角色會讓整個
ATT server 被編譯掉，連帶失去接收 notification 的能力。症狀非常難查：封包在 ATT
層被無聲丟棄，沒有錯誤也沒有事件，每一道 op 都只是逾時。用
`nm build/ulani_esp32.elf | grep ble_att_svr_rx_notify` 可以確認它有被連結進去。

### 日曆必須先回復出廠預設值

曾經和官方 App 配對過的日曆**會拒絕建立新的 bond**。症狀是掃描得到、連線也成功，
但在收到我們的 Pairing Request 之後約 130 ms 就主動切斷連線（HCI reason 0x13,
remote user terminated），連 Pairing Response 都不回。移除手機上的 App 沒有用，
必須在日曆上回復出廠預設值（按著功能鍵戳重置孔，見 README）。

回復出廠預設值之後，同一份韌體就能完成配對並訂閱 notify。

### 還沒驗證的部分

- 影像傳輸本身——目前還沒有成功送出過一張圖
- CRC 錯位那件事，裝置端到底是怎麼解讀的
- WiFi AP 開著時，共存的射頻排程會不會讓傳輸掉包
- 提供 LE Secure Connections（而非目前的 legacy）是否也能配對成功。
  切換到 legacy 和回復出廠預設值是同一輪做的，所以哪一項才是關鍵尚未釐清
