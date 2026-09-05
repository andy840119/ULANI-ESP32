# 編譯與開發

從 `git clone` 到燒出韌體、以及後續開發的流程。

## 需要的東西

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.5**（含 `idf.py` 與工具鏈）。
- [Node.js](https://nodejs.org/)（建前端用）。

## 第一次編譯

前端必須先建，因為韌體會把它 gzip 後**內嵌**進去：

```bash
python tools/build_web.py    # 在 web/ 跑 npm install + build，gzip 進 components/web_server/www
idf.py set-target esp32c3    # 或 esp32s3；原始碼不綁晶片，兩種都能編
idf.py build
idf.py -p COM<n> flash monitor
```

改完前端後，重跑 `tools/build_web.py` 再 `idf.py build` 即可。

> `components/web_server/www/*.gz` 是產物、不進版控。沒先跑 `build_web.py` 就 `idf.py
> build` 會直接報錯提醒你。

## 只跑前端（邊改邊看）

前端是 `web/` 底下獨立的 Vite 專案，不受 `idf.py` 管。改版面、調樣式時不用每次都重燒：

```bash
cd web
npm install     # 第一次
npm run dev     # http://localhost:5173
```

`npm run dev` 會把 `/api` 代理到 `http://192.168.4.1`（ESP32 熱點的位址）。如果板子
是連在你家 WiFi 上，把 `web/vite.config.ts` 裡的代理目標改成板子的區網 IP，就能一邊改
前端一邊接真的板子的資料（別把這個改動 commit 進去）。

前端結構見 [architecture.md](architecture.md)；REST 端點見 [rest-api.md](rest-api.md)。

## 發布 release 韌體

推一個版號 tag 會觸發 GitHub Action，自動編出 C3 與 S3 兩種韌體並附到 release。
版號規則與步驟見 [releasing.md](releasing.md)。使用者怎麼刷 release 韌體見
[flashing.md](flashing.md)。
