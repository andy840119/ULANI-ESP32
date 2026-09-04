# 發布 Release

韌體的發布靠打一個版本 tag 觸發：推上 tag 後，
[`.github/workflows/release.yml`](../.github/workflows/release.yml) 會自動 build
ESP32-C3 與 ESP32-S3 兩種韌體，並把可燒錄的 `.bin` 附到對應的 GitHub Release。
使用者怎麼拿這些檔案燒錄，見 [flashing.md](flashing.md)。

## 版號規則

```
yyyy.mmdd.count
```

- **`yyyy`** — 西元年，四位數。
- **`mmdd`** — 月與日，各補到兩位數（**保留前導 0**）。
- **`count`** — 當天第幾次發布，從 **0** 開始。同一天第一次是 `0`，要再發就 `1`、
  `2`⋯⋯。換一天就歸零。

也就是「哪一天發的、當天第幾次」，不帶語意化的大小版本號。

### 範例

| tag | 意思 |
| --- | --- |
| `2026.0904.0` | 2026-09-04 當天第一個 release |
| `2026.0904.1` | 同一天再發一次（改了東西、補個修正） |
| `2026.0910.0` | 2026-09-10 當天第一個 release |

> 月/日一定補零：9 月 4 日是 `0904`，不是 `94`。工作流程用
> `20[0-9][0-9].*` 比對 tag，所以年份開頭、這個格式的 tag 才會觸發建置。

## 怎麼發

1. 確認要發布的 commit 已經在 `main` 上（或你要發的 branch）。
2. 依上面規則決定 tag，例如今天是 2026-09-04 的第一次：

   ```bash
   git tag 2026.0904.0
   git push origin 2026.0904.0
   ```

3. 推上去後到 **Actions** 看 `release` 這條工作流程跑完（會 build 兩種晶片）。
4. 完成後到 **Releases** 頁面，該 tag 的 Release 底下 **Assets** 就會有：
   - `ulani-esp32c3-merged.bin` / `ulani-esp32s3-merged.bin`（各自燒 `0x0`）
   - 以及分開的 `bootloader-<chip>.bin`、`partition-table-<chip>.bin`、
     `ulani-<chip>.bin` 與 `flasher_args-<chip>.json`
5. 需要的話在 Release 頁面補上這次改了什麼的說明。

> 想在不打 tag 的情況下先驗證建置，可以到 Actions 頁面對這條工作流程按
> **Run workflow**（`workflow_dispatch`）。這種手動執行不會建立 Release，產物會
> 放在該次 run 的 **Artifacts**。

## 同一天發第幾次怎麼查

看 [Releases](https://github.com/andy840119/ULANI-ESP32/releases) 或
`git tag --list "<yyyy.mmdd>.*"`，看今天已經有幾個，下一個 `count` 接著往上加。
