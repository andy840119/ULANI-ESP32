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

**data channel 只接受 write without response。** 實測 properties 是 `0x16`
（READ | WRITE_NO_RSP | NOTIFY），沒有 `0x08`（WRITE），所以用有回應的寫入會在
**第一個封包**就被回 ATT `0x06`（Request Not Supported），接著日曆送 `0201` 宣告
傳輸失敗。op channel 的 properties 是 `0x1a`（含 `0x08`），用的是有回應的寫入——
兩個 channel 不一樣，必須各自依 properties 決定。

因為沒有回應也就沒有流量控制，唯一的背壓來自控制器緩衝耗盡
（`BLE_HS_ENOMEM`），此時要退避重試，不能把封包丟掉。

**傳輸期間仍要送 keepalive。** 一張圖要 20–30 秒，遠超過日曆容忍的 10 秒靜默。
Node 版的 `setInterval` 與傳輸並行，韌體這邊是單一 task，所以在送包迴圈裡每 8 秒
插入一次 `06 00`。

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

## 與 tesserae 的調色盤對照

[tesserae](https://github.com/dmellok/tesserae) 的 `seeed_ee04_73e6` 裝置格式
（在它的網頁上顯示為 **Seeed XIAO ePaper EE04 (7.3" Spectra 6)**）和 ULANI 的
payload 完全同尺寸（800×480、4bpp packed、192000 bytes），但**調色盤索引順序不同**：
它是六色 Spectra-6，ULANI 是七色 ACeP。

| nibble | tesserae (Spectra-6) | ULANI (ACeP) | 對照 |
|---|---|---|---|
| 0 | 黑 | 黑 | 0 → 0 |
| 1 | 白 | 白 | 1 → 1 |
| 2 | 黃 | 綠 | 2 → 5 |
| 3 | 紅 | 藍 | 3 → 4 |
| 4 | （未使用） | 紅 | 4 → 1 |
| 5 | 藍 | 黃 | 5 → 3 |
| 6 | 綠 | 橘 | 6 → 2 |

ULANI 的橘色沒有對應來源，因為 Spectra-6 沒有橘。未使用與超出範圍的值一律轉成白色，
免得畫面上出現一整塊莫名其妙的顏色。

轉換在下載串流的當下逐位元組完成（`components/tesserae/src/tesserae.c` 的
`PALETTE_MAP`），不需要額外的緩衝區。

## tesserae 的 /renders/ 授權

`/frame` 回傳的圖片網址長這樣：

```
http://<server>:8765/renders/<render_id>.bin?sig=<簽章>
```

`sig` 是 itsdangerous 的限時簽章，payload 就是那個路徑本身。**這個網址不需要
（也不接受）Authorization header**——參考韌體的 v1 路徑用 `image_fetch()`，
它傳的 token 是 NULL。只有 `/api/v1/device/<id>/frame/data` 那種 proto2/deck
路徑才要帶 bearer。

但是簽章驗證預設是關掉的（`app/auth.py`）：

```python
def _has_valid_render_signature(path):
    if not path.startswith("/renders/"): return False
    if not _public_rest_clients_enabled(): return False   # 預設 False
    return render_signature_valid(...)
```

關閉時 server 忽略簽章，往下掉到「來源必須是私有網段」的判定。因此透過外網網域
連線（含同區網的 hairpin NAT）會拿到 403，即使註冊與心跳都正常。

解法是**用區網 IP 當 server 網址**，或在 Settings → Server 打開
「Allow REST clients on public networks」。

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

### 電量的編碼尚未確認

`06 00` 的回覆是 `06 <level>`，實測讀到 `06 05`。**level 的刻度沒有任何文件佐證**——
它可能是 0–100 的百分比，也可能是 0–5 的格數。目前 UI 直接當百分比顯示，並且
把原始回覆 (`0x0605`) 並排印在旁邊，這樣假設一旦錯了會立刻看得出來，而不是
安靜地誤導人。請拿日曆自己顯示的電量比對後再回頭修正這一段。

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

- CRC 錯位那件事，裝置端到底是怎麼解讀的
- WiFi AP 開著時，共存的射頻排程會不會讓傳輸掉包
- 提供 LE Secure Connections（而非目前的 legacy）是否也能配對成功。
  切換到 legacy 和回復出廠預設值是同一輪做的，所以哪一項才是關鍵尚未釐清
