# 架構文檔（ARCHITECTURE）

> 本文檔對 ESP32 TEC 蟄眠實驗溫控系統做完整的架構層面梳理，涵蓋兩個獨立固件目標的模塊劃分、數據流、狀態機與併發模型。可獨立閱讀，無需先讀其他文檔。
>
> 源碼分析基礎（只讀，未做任何修改）：`src/main.cpp`（1240 行，主 ESP，含 2026-09-19 `abc8f9a` BME688 接入；**2026-10-04 解凍後約 1340 行**：換裝 BME680、1 Hz 讀取、防結露警示 `condRisk`、斷電自動恢復——相關小節已更新）、`camera/src/main.cpp`（275 行，相機）、`platformio.ini` / `camera/platformio.ini`。本文不寫行號，一律用函數名/變量名定位，見文末「源碼定位速查」。

---

## 目錄

1. [系統全景](#1-系統全景)
2. [兩固件邊界](#2-兩固件邊界)
3. [主 ESP 模塊劃分](#3-主-esp-模塊劃分)
4. [相機固件模塊劃分](#4-相機固件模塊劃分)
5. [數據流](#5-數據流)
6. [狀態機](#6-狀態機)
7. [併發與實時性模型](#7-併發與實時性模型)
8. [存儲與持久化](#8-存儲與持久化)
9. [故障域與降級路徑](#9-故障域與降級路徑)
10. [已知架構限制](#10-已知架構限制)

---

## 1. 系統全景

本系統由**兩台完全獨立的 ESP32 固件**組成，無直接電氣或總線連接，僅通過 IP 網絡松耦合（相機優先連手機熱點 `OPhone 12`，連不上 10 秒後回退連主 ESP 的 `ESP32-TEMP` AP；主 ESP 在 STA+AP 備援模式下與相機同處手機熱點，純 AP 模式下相機經回退也能連到主 ESP 的 AP）。

```text
┌─────────────── 硬件層 ───────────────┐   ┌──────────── 網絡層 ───────────┐   ┌─── 客戶端 ───┐
│ 3× DS18B20 (OneWire, GPIO4)        │   │ 主 ESP WiFi                   │   │ 瀏覽器 SPA   │
│ TEC H-bridge (TEC_EN/LPWM/RPWM)    │◀▶│  · 純 AP: ESP32-TEMP          │◀▶│ (儀表板)      │
│ FAN PWM (GPIO18, LEDC ch0)         │   │  · STA+AP 備援: 連指定 WiFi   │   └──────────────┘
│ SSD1306 OLED (I2C 21/22, 0x3C)     │   │ HTTP server port 80           │
│ BME680 (同 I2C, 0x76/0x77, 僅監測) │   └───────────────────────────────┘
│ EEPROM (flash 模擬, 256B)         │
└─────────────────────────────────────┘            ▲
                   ▲                                │ mDNS esp32-cam
                   │ 控制信號                        │ (僅 STA 模式可解析)
                   │                                ▼
┌─────────────── 主 ESP 固件 src/main.cpp ────────────────────────────┐
│ setup() → loop() 單線程 Arduino 調度                                  │
│  · 感測層 readSensor()/controlTemp()  · HTTP 控制層 /control /test  │
│  · 安全層 emergencyStop()/NAN 計數守衛 · 顯示層 updateOLED()         │
│  · 環境監測 bmeInit()/bmeTick()（BME680，不參與控制）                │
└──────────────────────────────────────────────────────────────────────┘

┌──────── 相機固件 camera/src/main.cpp（獨立第二個 ESP32-S3）─────────┐
│ WiFi STA（連手機熱點 OPhone 12，失敗回退 ESP32-TEMP AP）             │
│ 雙 httpd server: port 80 串流 / port 81 控制（IR/LED）               │
│ 每個 httpd 實例只有 1 個 server task（串流佔住 80 口時 80 口不接新請求）│
└──────────────────────────────────────────────────────────────────────┘
```

**關鍵點：** 兩台 ESP32 **不共享任何硬件資源**。相機 IR/LED 在瀏覽器中由前端**直接發請求到相機 81 端口**（`toggleIR()`/`toggleLED()`）。前端只在 `camIP` 為空時才退回主 ESP 的 `/light` 代理（`setup()` 裡 `server.on("/light", …)`），但 `/data` 恆返回 `camIP`（未解析到時為默認 `192.168.4.2`），所以**實際上前端永遠直連、代理路徑走不到**；且該代理連的是相機 **80 口**，而 `/light` 註冊在 81 口，本身也是壞的（鎖版未修，見 §3.4）。串流與控制分端口，是為了避免串流佔住 80 口導致燈光無反應。

---

## 2. 兩固件邊界

| 維度 | 主 ESP（TEC 溫控） | 相機（行為觀測） |
|------|---------------------|-----------------|
| 板型 | `board = esp32dev`（實物為 FireBeetle 2 ESP32-E；`platformio.ini` 未指定專用 variant） | DFRobot DFR1154 ESP32-S3 (`dfrobot_dfr1154`)，16MB Flash + 8MB OPI PSRAM |
| 主源文件 | `src/main.cpp`（約 1340 行） | `camera/src/main.cpp`（275 行） |
| 框架 | Arduino + Arduino-ESP32 core | Arduino + Arduino-ESP32 core + esp32-camera |
| WiFi 角色 | 默認 AP（`ESP32-TEMP` / 192.168.4.1）；可切 STA+AP 備援 | STA（連 `OPhone 12` 手機熱點，失敗回退 `ESP32-TEMP`） |
| HTTP | 單 `WebServer` (port 80)，`loop()` 內 `handleClient()` 同步分派 | 雙 `httpd` (port 80 串流 / port 81 控制)，每實例 1 個 server task |
| 持久狀態 | EEPROM 256B（save/loadState） | 無 |
| 任務併發 | 單 `loop()` 主任務 + 看門狗；WiFi/LWIP 協議棧由 core 內部 task 承載，HTTP handler 在 `loop()` 中執行 | 主 loop 僅重連 WiFi；串流、控制各由自己 httpd 實例的 server task 執行 |
| 構建命令 | `pio run` | `cd camera && pio run -e esp32s3cam`（獨立 PlatformIO 項目，根目錄執行會報 `UnknownEnvNamesError`） |

兩固件**沒有任何源碼共享**（無 `include/` 公共頭、無 `lib/` 共用庫，兩者目前均只有 PlatformIO 模板 README）。換感測器或調閾值需重刷主 ESP；相機配置（WiFi 帳密、引腳）則硬編碼在 `camera/src/main.cpp` 頂部。

---

## 3. 主 ESP 模塊劃分

`src/main.cpp` 是單文件 Arduino sketch，按職責可在閱讀上劃分為 7 個模塊（**代碼本身沒有物理拆分**，所有函數與全局都在同一編譯單元，靠註釋分節組織）。下表給出每個模塊的入口、關鍵函數、依賴與產出。

### 3.1 全局配置與常量（文件開頭的 `#define` 與全局變量）

| 項 | 值 | 備註 |
|----|----|------|
| `FAN_PIN=18`, `TEC_EN=19`, `TEC_LPWM=26`, `TEC_RPWM=25`, `DS18B20_PIN=4` | GPIO 編號 | FireBeetle 板標 D-pin ≠ GPIO，詳見 CONFIG_GUIDE |
| `PWM_FREQ=25000` Hz, `PWM_RES=10`, `TEC_PWM_RES=10` | LEDC 配置 | 風扇與 TEC 一致均 10-bit |
| `FAN_CH=0`, `TEC_L_CH=1`, `TEC_R_CH=2` | LEDC 通道分配 | 三路獨立 |
| `EMA_ALPHA=0.5f` | 溫度濾波係數 | 指數移動平均（EMA） |
| `FAN_AFTERRUN_MS=10000`, `FAN_AFTERRUN_SPEED=200` | 風扇延遲關閉 | ~78% |
| `STATE_SAVE_DEBOUNCE_MS=750` | 狀態保存去抖 | 避免抖動頻繁寫 EEPROM |
| DS18B20 ROM 常數 `nestAddr/roomAddr/ventAddr` | 8 字節 DeviceAddress | ROM 出廠位址認人 |
| 安全閾值默認 `safeMin=5.0`, `safeMax=35.0`, `ventMax=50.0` | °C | 可被 `/control` 覆蓋並持久化 |
| `BME_INTERVAL_MS=1000`, `BME_GAS_EVERY=10`, `BME_BURNIN_MS=600000`, `BME_GAS_ALPHA=0.02f` | BME680 溫溼壓讀取週期（1 Hz）/ 每 N 次量一次 Gas / Gas 燒機期 / Gas 基線 EMA | 見 3.7 |
| `COND_ON_MARGIN=1.0f`, `COND_OFF_MARGIN=2.0f`, `COND_STALE_MS=30000` | 防結露警示進入/退出餘量（°C，相對露點）/ BME 數據過期判定 | 見 3.7 |

### 3.2 感測層（`readSensor()`、`doScan()`）

- `doScan()` — OneWire/Dallas 總線掃描，按 ROM 比對給三顆感測器賦角色；處理 `cnt==0` 回退路徑並打印 raw ROM 便於排錯。
- `readSensor()` — **非阻塞轉換狀態機**：先 `requestTemperatures()` 立即返回置 `convPending=true`；下次 loop 迭代等到 `millis()-convStart >= 750` 才真正讀 3 個溫度。讀後做範圍合法化（`-55~85°C` 外判 NAN；注意判據是 `> 85.0`，**85.0 本身被接受**，而 85°C 正是 DS18B20 的上電覆位值，見 LIMITATIONS #37）、校準偏移疊加、EMA 濾波，最後寫入 `nestT/roomT/ventT`（濾波值，`/data` 輸出的就是它）和 `readTemps[]`（偏移後未濾波值，目前沒有任何接口對外輸出），並調用 `controlTemp()` 閉環。
- 斷線值：`getTempC(addr)` 讀不到時返回 `DEVICE_DISCONNECTED_C`（-127），代碼裡 `== DEVICE_DISCONNECTED_F` 的比較不會命中，實際靠 `< -55` 下限判 NAN。
- `sameAddr()` — 8 字節逐位比較，ROM 識別基礎。

**輸入：DS18B20 OneWire 總線；輸出：3 個濾波後溫度 + raw 數組。**

### 3.3 控制層（`setFan()` … `controlTemp()`）

控制層是**單點真相來源** `controlTemp()`，被 `readSensor()` 和 `handleControl()` 兩處調用以立即生效新閾值。

- `setFan(int s)` — 風扇佔空比 0-255 → `ledcWrite` 10-bit（×4）。
- `setTec(int cool, int heat)` — **手動 / `/test` 通道**：8-bit 0-255 放大 4 倍寫到 TEC_L/R 通道，同步置 `cooling/heating` 標誌。
- `setTecPwm(float power, bool isCool)` — **自動 bang-bang 通道**：歸一化 0-1.0 → 0-1023 PWM，單向驅動（另一方向寫 0）。
- `controlTemp()` — 安全 + bang-bang 五段式決策（詳見 [6.1](#61-controltemp-狀態機)）。
- `emergencyStop()` / `stopAll()` / `startAll()` — 系統級狀態切換，含風扇延遲邏輯。**`TEC_EN` 只在 `startAll()`（及 `/test?en=1`）中拉高**；`setup()` 先把它拉低，2026-10-04 起若 `loadState()` 恢復出 `systemOn=true`，`setup()` 末尾會自動調用 `startAll()`（斷電自動恢復，LIMITATIONS #31，未上機驗證）。`emergencyStop()` 已把 `systemOn=false` 存入 EEPROM，所以緊急停機不會被重啟復活。

### 3.4 網絡層（`handle*()` 各 handler、`setup()` 裡的 `server.on(...)`）

- `WebServer server(80)` — 5 個 handler 路由 + 3 個 inline lambda 路由（共 8 個）：
  - `GET /` → `handleRoot()` 返回 `PROGMEM` 內嵌的 SPA HTML（`INDEX[]`，約 410 行 inline HTML+CSS+JS）。
  - `GET /data` → `handleData()` JSON 狀態快照（溫度為濾波值；含 `bmeOk/bmeT/bmeH/bmeP/bmeGas/bmeGasRel/bmeDP`，BME 不在線時為 `null`）。
  - `POST /control` → `handleControl()` 參數寫入 + `scheduleStateSave()` / `saveState()`，再 `controlTemp()` 即時生效。
  - `GET /test` → `handleTest()` 手動 PWM 測試，置 `tecManual/fanManual` 標誌。
  - `GET /diag` → `handleDiag()` 診斷 JSON。
  - `GET /light`（inline） → 設計上是相機不可直連時的代理；樂觀更新本地 `lightIR/lightLED` 緩存，再盡最大努力同步到相機 `camIP:80`。**已知缺陷**：相機的 `/light` 在 81 口，連 80 口會 404；且前端總拿得到 `camIP`，實際不會走這條路徑。
  - `GET /strip`（inline，2026-09-26） → `?b=0-100` 設 LED 燈帶亮度（GPIO13，LEDC ch4，5 kHz），回 `{"strip":N}`；不存 EEPROM。
  - `GET /camenable`（inline） → 切換 `camEnabled` 標誌並回 JSON。

### 3.5 顯示層（`updateOLED()`、`loop()` 裡的 OLED 段）

- `updateOLED()` — U8g2 `firstPage()/nextPage()` 分頁繪製，6 段佈局（巢穴/活動/出風口溫度 + 狀態 + 風扇% + 目標 + 模式），每 2 秒刷新。
- 主循環每 5 秒 `Wire.beginTransmission(0x3C)` 探測，OLED 斷線自動 `u8g2.begin()` 重初始化（熱插拔恢復，commit `e9c6829`）。

### 3.6 持久化層（`scheduleStateSave()` / `saveState()` / `loadState()`）

- `scheduleStateSave()` — 去抖調度：設 `stateSavePending=true`+`stateSaveDue`。
- `saveState()` — 寫 EEPROM 256B（詳細地址見 CONFIG_GUIDE）。
- `loadState()` — 上電恢復，含 `0xAA` 有效標記、`ventMax` 越界 clamp、`wifiMode>1` reset、SSID 0xFF 兼容默認回退。
- `setWifiModeAndRestart(mode)` — `saveState()` + `delay(200)` + `ESP.restart()`，用於 WiFi 模式切換。

### 3.7 環境監測層 BME680（`bme*` 全局與 `COND_*` 常量、`bmeInit()` … `updateCondRisk()`，2026-09-19 `abc8f9a` 接入，2026-10-04 改 1 Hz + 防結露警示）

- `bmeInit(quiet)` — 在 `setup()` 中 `Wire.begin(21,22)` 之後、`u8g2.begin()` 之前調用；依次探測 0x76 / 0x77，設過採樣、IIR 與氣體加熱器 320°C/150ms。用 Adafruit BME680 驅動（實際裝的就是 BME680），**未用 BSEC**，因此沒有 IAQ / eCO₂，只有氣體電阻。
- `bmeTick()` — 每次 `loop()` 調用的非阻塞狀態機：`beginReading()` 記下完成時刻，到點才 `endReading()` 取值；每 1 s（`BME_INTERVAL_MS`）讀一次溫溼壓，加熱器平時關閉（`setGasHeater(0,0)`），每 `BME_GAS_EVERY`=10 次才開加熱器量一次 Gas（即 10 s 一次，降低自熱）；成功讀取時更新 `lastBmeOk`。連續 3 次 `beginReading` 失敗標記離線，離線後每 60 s 重試 `bmeInit(true)`（可熱插拔）；單次 `endReading` 失敗沿用上一筆。
- 派生量：露點（Magnus 公式）；`bmeGasRel` = 氣體電阻 ÷ 慢速 EMA 基線（`BME_GAS_ALPHA=0.02`）×100%。上電前 10 分鐘（`BME_BURNIN_MS`）為燒機期，Gas 值不應引用。
- 防結露警示：`updateCondRisk()` 每次 `loop()` 調用，用純函數 `condRiskNext(prev, ventT, bmeDP)` 做遲滯——出風口 ≤ 露點 + `COND_ON_MARGIN`(1°C) 進入、> 露點 + `COND_OFF_MARGIN`(2°C) 退出；BME 離線或數據超過 `COND_STALE_MS`(30 s) 未更新時強制為 false。**僅警示，不影響 `controlTemp()`**。出風口氣溫只是冷端表面的代理量（冷端表面更冷），警示可能偏晚。
- 輸出：`/data` 的 `bmeOk/bmeT/bmeH/bmeP/bmeGas/bmeGasRel/bmeDP/condRisk` 字段；網頁 BME 面板 + 結露風險橫幅；OLED Vent 行 `DEW` 標記；CSV 後 6 列。
- **與控制完全解耦**：不參與 `controlTemp()`、閾值、安全邏輯，也不寫 EEPROM。

---

## 4. 相機固件模塊劃分

`camera/src/main.cpp` 極簡，按職責可分 3 塊：

| 模塊 | 函數 | 說明 |
|------|------|------|
| 串流服務 | `stream_handler()` | MJPEG multipart 幀循環；每幀前用 `send(fd, &probe, 0, MSG_DONTWAIT)` 探測客戶端是否斷開；10 秒無幀發出超時退出；**始終 `esp_camera_fb_return(fb)`** 防幀緩存洩漏 |
| 快照服務 | `capture_handler()` | 單幀 JPEG 抓取，用於不發 multipart 的客戶端 |
| 控制 / 狀態 | `light_handler()` / `status_handler()` | IR/LED GPIO 控制與狀態 JSON；GET query 參數解析 |
| 啟動 | `startCameraServer()` | 起 2 個 `httpd` 實例，stream(80) 與 control(81)；`send_wait_timeout=1` 避免慢客戶端阻塞 5 秒；`ctrl_port=32769` 與默認 32768 錯開 |
| WiFi | `connectWiFi()` | 兩級回退：`OPhone 12` → `ESP32-TEMP`；連接成功後 `(re)startCameraServer()` + `MDNS.begin("esp32-cam")`（重連時只 `httpd_stop` 了串流 server，控制 server 未停，再次啟動會打印 "Control server failed!"，舊實例仍在工作） |
| setup / loop | `setup()` / `loop()` | camera_config 全字段硬編碼；PSRAM 缺失回退 QVGA DRAM；`set_vflip/hmirror/brightness/contrast` 等傳感器調參寫死；主 loop 僅監測 WiFi 掉線重連 |

### 4.1 傳感器配置（`setup()` 裡的 `camera_config_t`）

VGA 640×480 / JPEG quality 16 / `CAMERA_GRAB_LATEST` / `fb_count=2` / `fb_location=PSRAM` / `xclk=20MHz`。
**無 PSRAM 時** (`!psramFound()`) 回退 DRAM + 單幀緩衝，避免 DMA 崩潰（這是 README 反覆強調"必須用 `dfrobot_dfr1154` 板型"的根本原因）。注意：代碼裡的 QVGA 回退實際無效——`esp_camera_init()` 之後無條件 `s->set_framesize(s, FRAMESIZE_VGA)` 又改回了 VGA，只有 `fb_location`/`fb_count` 的回退生效（鎖版未改）。

---

## 5. 數據流

### 5.1 主 ESP 主迴路數據流

```text
            ┌──────────── OneWire GPIO4 ───────────┐
            │ 3× DS18B20 (ROM 識別)                │
            └──────────────┬─────────────────────────┘
                           │ requestTemperatures (750ms 非阻塞)
                           ▼
        ┌────────── readSensor() ──────────┐
        │ 範圍合法化 → 偏移 → EMA 濾波       │
        └──────────────┬─────────────────────┘
                       │ nestT/roomT/ventT (全局)
                       ▼
        ┌────────── controlTemp() ─────────┐  ◀── http 觸發（僅 /control；/test 不調用它）
        │ 5 段安全 + bang-bang 決策          │
        └──────┬───────────────┬────────────┘
   製冷/加熱    │               │ 風扇
               ▼               ▼
        ledcWrite(TEC_L/R)  ledcWrite(FAN)
        digitalWrite(TEC_EN)

        旁路輸出：
        ┌────────── bmeTick()（BME680，1 Hz）──┐ →  bmeT/H/P/Gas/DP，只進 /data 與 CSV，不進控制
        ┌────────── /data JSON ──────────┐  →  瀏覽器儀表板
        └────────── updateOLED() ────────┘  →  SSD1306
        └────────── EEPROM saveState() ──┘  →  flash 持久化
```

### 5.2 HTTP 控制流

- **寫參數**：瀏覽器 `fetch('/control?...',{method:'POST'})` → `handleControl()` 把參數寫入全局變量 → `scheduleStateSave()`（750ms 去抖）→ `controlTemp()` 即時生效。
- **系統開關**：`handleControl("system=1")` → `startAll()`（同步置 `systemOn=true`、復位 `nanCount=0`、關閉手動態）→ 立即 `saveState()`（不走去抖，確保斷電不丟）。
- **手動 PWM 測試**：`/test?cool=..&heat=..&en=..&fan=..` → 直接寫 PWM。帶 `cool`/`heat` 時置 `tecManual`，之後 `controlTemp()` 直接 return（被 `if(!systemOn||tecManual||manualMode) return` 攔截，**所有保護一併跳過**）；只帶 `fan` 時只置 `fanManual`，TEC 仍自動控制、風扇保持手動值直到停止/重開系統（52% 用法即此）。網頁"製冷/加熱"小按鈕 = `/control?manual=1` + `/test?cool|heat=200&en=1`（TEC 約 78%）。

### 5.3 瀏覽器 ↔ 相機 直連流

```text
瀏覽器                        主 ESP /camenable         相機 (port 80/81)
   │  GET /data camEnabled=true ────────▶                   │
   │  ◀── camIP (mDNS esp32-cam 解析) ──────────────────── │
   │                                                       │
   │  ──── <img src=http://camIP/stream> ─────────────────▶(port 80 MJPEG)
   │  ──── fetch http://camIP:81/light?ir=.. ─────────────▶(port 81 控制)
   │   （僅當 camIP 為空才退到主 ESP /light 代理；但 /data 恆返回 camIP
   │    ——未解析時為默認 192.168.4.2——所以實際永遠直連，代理不會被用到）
```

純 AP 模式下主 ESP 不運行 `MDNS.begin()`，`camIP` 保持默認 `192.168.4.2`。相機回退連入 `ESP32-TEMP` 時通常拿到 .2，但若手機/電腦先連上 AP 佔用了 .2，相機會拿到別的地址而網頁仍去連 .2（**推斷**，未實測）。

### 5.4 顯示流

主迴路中 OLED 每 2 秒讀全局溫度 + 狀態繪製；瀏覽器側每 1 秒（串流時 5 秒）輪詢 `/data` 推入 `H`（600 滑動窗口）和 `allData`（10000 上限）數組，Canvas 重繪趨勢圖。注意：溫度值本身約每 4 秒才更新一次，CSV 中相鄰行多為重複值。網頁"更新頻率"滑條可調 1–10 秒（相機開啟時至少 5 秒）；09-26 之前它會被輪詢覆蓋，已修。

---

## 6. 狀態機

### 6.1 `controlTemp()` 狀態機

這是全系統的**核心決策機**，被 `readSensor()`（每完成一次讀數，約 4 秒一次）和 `handleControl()`（每次 `/control` 請求）調用。返回前會被下列前置 guard 之一攔截：

```text
guard 0  systemOn==false || tecManual || manualMode  → return（不動作；手動模式下以下所有保護都不執行）
guard 1  isnan(nestT)||isnan(roomT)||isnan(ventT)    → nanCount++
            └ nanCount>=3 → emergencyStop()+saveState()+return（連續 3 次讀數 ≈ 12 s）
            └ 否則 return（容忍 1~2 次瞬態跳變）
guard 2  ventT >= ventMax (默認 50°C)                → emergencyStop()+saveState()+return
guard 3  nestT < safeMin || nestT > safeMax           → setTecPwm(0)+風扇 255(過熱) 或 60≈24%(過冷) +return
─────────────────────────────────────────────────────────
熱節流   if ventT > ventMax-10 → throttle = (ventMax-ventT)/10  ∈[0,1]
決策    nestT > targetTemp+hysteresis → setTecPwm(throttle,true) + setFan(255)
        nestT < targetTemp-hysteresis → setTecPwm(throttle,false)+ setFan(255)
        else（在死區內）→ setTecPwm(0) + 風扇 after-run 延遲邏輯
```

**重要：** 處於死區時如檢測到剛從製冷/加熱切出（`cooling||heating` 仍為 true），啟動 `fanAfterRunTimer`；之後 10 秒內每幀保持 `FAN_AFTERRUN_SPEED=200`(~78%)，超過 10 秒關扇。

> ⚠️ 注意 README 提到"製冷時風扇應以 52% 運行"的實測發現，但**當前 `controlTemp()` 中製冷分支仍寫 `setFan(255)` 全速**——這是已知的代碼與文檔偏差，鎖版凍結不改，演示時手動設 52%，詳 [LIMITATIONS.md](../LIMITATIONS.md) #34 與 [CONFIG_GUIDE.md](CONFIG_GUIDE.md) §2.3。

### 6.2 風扇 after-run 狀態機（`loop()` 內獨立）

```text
狀態：fanAfterRunTimer == 0      → 不在 after-run
狀態：fanAfterRunTimer > 0 且 !fanManual
   ├─ millis()-timer < 10s → setFan(200)
   └─ millis()-timer >= 10s → setFan(0) + 重置 timer=0
```

啟動點：`controlTemp()` 進入死區且檢測到剛切出冷/熱；`stopAll()` 在 `fanSpeed>0` 時也置 `fanAfterRunTimer = millis()`。`startAll()` 會清零 timer 取消延遲。

### 6.3 非阻塞感測狀態機（`readSensor()`）

```text
convPending=false → requestTemperatures(); convPending=true; convStart=millis(); return
convPending=true  → if millis()-convStart < 750 return   (還未到)
                  → else 讀 3 溫度、濾波、controlTemp(); convPending=false
```

由 `loop()` 每 2 秒觸發一次 `readSensor()`：第一次調用只發起轉換就返回，下一次（2 秒後）才讀取——所以**一次完整讀數約 4 s**（舊文檔寫的 2.75 s 有誤）。這是隱含的採樣率限制，也是 NAN 連續 3 次 ≈ 12 s 的原因。CSV 實測：P1/P2 溫度約每 4 s 變一次，P3 約每 5 s。

### 6.4 NAN 守衛：實際只有一道

`loop()` 頂部有一行看似"後備"的守衛：

```cpp
if (systemOn && sensorInit && nanCount >= 3) { emergencyStop(); saveState(); }
```

但 `nanCount` **只在 `controlTemp()` 內遞增**，而且 `controlTemp()` 在達到 3 時已經自己調用了 `emergencyStop()`。因此這行守衛**不構成第二道保險**：

- 手動模式（`tecManual/manualMode`）下 `controlTemp()` 第一行就 return，`nanCount` 根本不會遞增，loop 守衛也永遠不會觸發——**手動模式下沒有任何斷線保護**（LIMITATIONS #8）。
- `!dsOk` 時 `readSensor()` 第一行就 return，同樣不會遞增 `nanCount`。

**斷線缺口的實際範圍**（LIMITATIONS #36）：`dsOk` 只在 `doScan()` 裡改變，而 `doScan()` 只在已有探頭缺失時才每 10 秒重跑。
- 開機三顆都識別到、運行中總線整體斷開 → `dsOk` 仍為 true，`getTempC()` 返回 -127 → 被 `< -55` 判 NAN → 正常走 nanCount → 約 12 s 急停。✅
- 已有探頭缺失、重掃時發現 **0 個設備** → `dsOk=false` → `readSensor()` 直接 return → `nanCount` 不再增長（只有 `/control` 請求觸發的 `controlTemp()` 還會讓它 +1）→ **不會急停**，TEC 保持上次狀態。❌

### 6.5 EEPROM 保存狀態機

```text
觸發  → stateSavePending=true; stateSaveDue=millis()+750
loop 中 if pending && (long)(millis()-due)>=0 → saveState(); pending=false
例外：/control 帶 system 參數時、以及緊急停機的兩個調用點（controlTemp 內 / loop 守衛）
      會直接調 saveState() 走立即路徑（不經去抖）；emergencyStop() 本身不調 saveState()
```

---

## 7. 併發與實時性模型

### 7.1 主 ESP：單 loop 主任務 + Arduino-ESP32 後端 task

主 ESP 沒有顯式多任務。所有控制、感測、HTTP、顯示都在 `loop()` 串行執行。Arduino-ESP32 core 在背後運行 WiFi stack、LWIP、mDNS 等內部 task，但 **HTTP handler 不在這些 task 裡執行**：`loop()` 調 `server.handleClient()`，由 `WebServer` 在 loop 任務內同步分派到 `handleControl()` 等 user handler。因此 handler 與 `readSensor()`/`controlTemp()` 不會併發，不存在數據競爭。

**關鍵的時序要求：**
- `loop()` 頂部 `esp_task_wdt_reset()` 必須在 **7 秒**內執行（`setup()` 裡的 `esp_task_wdt_init(7, true)`；緊鄰的源碼註釋仍寫"3 秒"，已過時，以代碼 7 秒為準）。
- OneWire 時序由 `paulstoffregen/OneWire` 庫用 `micros()` 軟件循環保證，對 WiFi 中斷極敏感。基於此 README 強調"`doScan()` 必須 `WiFi.softAP()` 之前執行"。
- HTTP 處理是非阻塞的，單個請求不應 >100ms，否則後續 IO 會被卡。`handleLight` 中的相機代理同步連接最長等 2 秒，可能影響響應性（用 `esp_task_wdt_reset()` 中間餵狗避免重啟）。

### 7.2 相機：每個 httpd 實例一個 server task

ESP-IDF `esp_http_server` 的每個實例只有**一個 server task**，在它的循環裡輪詢監聽 socket 與控制 socket，並在該 task 內調用 URI handler（[官方文檔](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_http_server.html)）。`stream_handler()` 是 `while(true)` 阻塞式，直到客戶端斷開或 10 秒無幀超時才返回——**串流進行期間，80 口這個實例不能再服務第二路 `/stream` 或 `/capture`**。控制端點放在 81 口的獨立實例（獨立 server task、`ctrl_port=32769`）正是為了不被串流卡住。`max_open_sockets=4` 只是允許同時保持 4 個連接，**不等於能同時推 4 路流**（相機固件開頭那句"Each client connection gets its own task"的註釋是錯的）。

`capture_handler()` 與 `stream_handler()` 在同一實例，所以串流期間 `/capture` 會排隊等待，而不是併發搶幀。

### 7.3 跨固件松耦合

主 ESP 與相機之間**無 RPC 心跳、無同步鎖**。相機掉線僅靠：
- 主 ESP 主循環每 5s~60s `MDNS.queryHost("esp32-cam")` 重新解析（僅 STA+AP 模式下成立）。
- 瀏覽器側 `camErr()` 重試 2 秒後 `camPoll()` 重連。

---

## 8. 存儲與持久化

| 介質 | 內容 | 容量 | 接口 |
|------|------|------|------|
| 主 ESP EEPROM（flash 模擬） | 系統狀態 + 安全閾值 + 校準偏移 + WiFi 配置 | 256 字節映射，已用 0–132（共 133 字節） | `EEPROM.begin(256)` / `put` / `get` / `commit` |
| 主 ESP PROGMEM | SPA 儀表板 HTML/CSS/JS | ~12KB（INDEX[] PROGMEM R"HTML(...)"） | `server.send_P` |
| 瀏覽器內存 | `H[]` 600 + `allData[]` 10000 | 單條 3 溫度 + 風扇/狀態 + 6 個 BME 字段 + 時間戳，約 1–2 MB 上限 | JS 全局變量 |
| 相機 | 攝像頭幀緩衝 PSRAM | 2 個 VGA 幀 + JPEG 中間緩衝 | `esp_camera_fb_get/return` |
| 相機 | WiFi 帳密 | 硬編碼字符串常量 | 無持久化 |

EEPROM 詳細地址映射見 [CONFIG_GUIDE.md]。

---

## 9. 故障域與降級路徑

下表列每個潛在故障點、當前處置與殘餘風險，便於運維時定位問題域。

| # | 故障 | 檢測位置 | 當前處置 | 殘餘風險 |
|---|------|----------|----------|----------|
| F1 | 任一 DS18B20 斷線（NAN） | `controlTemp()` guard1（loop 那行守衛不構成第二道，見 §6.4） | 連續 3 次讀數 NAN（約 12 s）緊急斷電 | 單次瞬態跳變被忽略（設計），約 12 s 內仍按舊狀態運行；**手動模式下完全不生效**（LIMITATIONS #8） |
| F2 | OneWire 總線無設備（`!dsOk`） | `doScan()` `cnt==0` | 重新 doScan 每 10s | 缺口範圍較窄（§6.4）：只有在已有探頭缺失、重掃發現 0 設備後，`readSensor()` 頂端 `if(!dsOk) return` 才會讓 `nanCount` 停止增長 → **不急停**，舊 TEC 狀態殘留（LIMITATIONS #36，鎖版未修）；三顆都在線時整體掉線仍會走 NAN 路徑急停 |
| F3 | 出風口過熱 `ventT>=ventMax` | `controlTemp()` guard2 | 緊急斷電 + saveState | 出風口探頭 NAN 時 guard1 先行，guard2 不執行；在 nanCount 累到 3 之前（約 12 s）TEC 繼續按上次決策運行；手動模式下不檢查 |
| F4 | H-bridge MOSFET fail-short | 無軟件檢測 | 僅靠 ventT 反饋 | 軟件無法干預，需硬件溫度保險（LIMITATIONS #32） |
| F5 | 主 ESP `loop()` 卡死 / 斷電重啟 | TWDT (7s 實際) | 看門狗復位 → setup → loadState 恢復參數與 `systemOn` | `setup()` 早期 `digitalWrite(TEC_EN, LOW)` 關 TEC；2026-10-04 起 `systemOn=true` 時 `setup()` 末尾自動 `startAll()` 恢復運行（LIMITATIONS #31，未上機驗證）；緊急停機已存 `systemOn=false`，不會被複活 |
| F6 | WiFi AP 啟動失敗 | 無顯式檢查 | 無降級，OLED/Serial 仍工作 | 遠程監控失效，但本地控制台不受影響 |
| F7 | WiFi 模式切換（STA+AP）連不上 | `setup()` 內 20 次重試 | 失敗回退 AP | 切換後總要 `ESP.restart()`，期間短時離線 |
| F8 | OLED 離線 | `loop()` 5s I2C 探測 | 自動 `u8g2.begin()` 重連 | 良好 |
| F9 | 瀏覽器關頁 / ESP 復位 | 無 | `allData` 丟失 | 已知缺陷（LIMITATIONS #5） |
| F10 | 相機 WiFi 掉線 | `loop()` 5s 檢測 | `connectWiFi()` 重連 | 重連只 `httpd_stop` 了 80 口串流 server；81 口控制 server 未停，再次 `startCameraServer()` 時 81 口啟動失敗（打印 "Control server failed!"），舊實例仍在工作，功能不受影響 |
| F11 | 串流慢客戶端 | `send_wait_timeout=1` + send(fd,0) probe | 1 秒超時斷開 | 不再阻塞其他客戶端 |
| F12 | 幀緩衝洩漏（發送失敗） | `esp_camera_fb_return(fb)` 總是調用 | 防止 PSRAM 耗盡 | 已修復 |
| F13 | DS18B20 上電覆位值 85.0°C（探頭接觸不良、掉電重連時出現） | 範圍檢查 `raw > 85.0` 才判 NAN，**85.0 本身被接受** | 無 | 85°C 進入 EMA（斷線後首值不經濾波直接等於 85）；出風口會誤觸 `ventT>=ventMax` 急停，巢穴會誤入極端分支。0826 CSV 有實例：16:22:57 出風 56.16°C 觸發一次誤急停（LIMITATIONS #37） |
| F14 | BME680 離線 | `bmeTick()` 連續 3 次 `beginReading` 失敗 | 標記離線，每 60 s 重試 | 與控制無關，`/data` 字段變 `null`；`condRisk` 強制為 false（數據過期 30 s 同理） |

---

## 10. 已知架構限制

以下是從架構層面（非細節 bug）觀察到的限制，會限制後續可擴展性：

1. **單文件巨 sketch**：`src/main.cpp` 約 1340 行包含感測、控制、HTTP、HTML、OLED、EEPROM、BME680，模塊邊界靠註釋組織，不利於單元測試與並行開發。這是本系統**只能寫 mirror 測試、無法直接測源碼**（見 `test/README_TESTS.md`）的最大根因。
2. **無感測/控制抽象層**：溫度讀、決策、PWM 寫三件事在 `controlTemp()` 內耦合，無法在 PC 上跑邏輯測試（必須 mock OneWire/Dallas/WebServer/ledc 全套 Arduino-API）。
3. **狀態散落全局變量**：`systemOn/cooling/heating/nanCount` 等十餘個全局變量無封裝，任何函數都能改（目前 HTTP handler 與感測都在 `loop()` 任務內串行執行，沒有併發競爭；但若日後改成多任務就需要加鎖）。
4. **HTML/CSS/JS 在 PROGMEM 內 raw 字符串中**：約 410 行前端代碼無法 lint、無法類型檢查、無法複用組件，靠 `R"HTML(...)"` 維護極脆弱。
5. **相機固件無 OTA、無配置接口**：換 WiFi SSID/密碼、改 IR/LED 引腳都要重新燒錄，與主 ESP 的可配置 WiFi 模式形成體驗落差。
6. **兩固件無共享庫**：`include/`、`lib/` 均為空模板，養成"放了也不會被編譯"的盲區。
7. **TWO `httpd` 的 ctrl_port 錯開靠硬編碼 32769**：未做枚舉常量化，未來擴第三個 server 時易撞端口。
8. **mDNS 名稱硬編碼**：`esp32-tec` 與 `esp32-cam` 在兩固件分別硬編碼，改名必須兩邊同步，且同一網絡內不能與其他設備重名。

---

## 附：源碼定位速查

本文不寫行號（源碼三個月裡從約 815 行漲到約 1340 行，行號很快就過時），下表列的是函數名/變量名，在源碼裡直接搜即可。

| 關心點 | 在 `src/main.cpp` 裡搜 |
|--------|------|
| 引腳定義 | `FAN_PIN` / `TEC_EN` / `TEC_LPWM` / `TEC_RPWM` / `DS18B20_PIN` / `LED_PIN` |
| ROM 位址 | `nestAddr` / `roomAddr` / `ventAddr` |
| BME680 全局與常量 | `BME_INTERVAL_MS`、`BME_GAS_EVERY`、`COND_ON_MARGIN` 等 |
| BME680 讀取與防結露 | `bmeInit()` / `bmeTick()` / `condRiskNext()` / `updateCondRisk()` |
| 安全閾值默認值 | `safeMin` / `safeMax` / `ventMax` |
| PWM 輸出 | `setFan()` / `setTec()` / `setTecPwm()` / `setStrip()` |
| 系統開關 | `stopAll()` / `startAll()` / `emergencyStop()` |
| 核心決策 | `controlTemp()` |
| 非阻塞讀數 | `readSensor()` |
| HTTP handler | `handleData()` / `handleControl()` / `handleDiag()` / `handleTest()` |
| HTTP 路由註冊 | `setup()` 裡的 `server.on(` |
| 儀表板 HTML | `INDEX[] PROGMEM` |
| 持久化 | `scheduleStateSave()` / `saveState()` / `loadState()` / `setWifiModeAndRestart()` |
| 總線掃描 | `doScan()` / `sameAddr()` |
| 啟動流程（看門狗/EEPROM/I2C 掃描/BME/WiFi/斷電恢復） | `setup()` |
| 主循環（看門狗/HTTP/NAN 守衛/風扇 after-run/重掃/讀數/BME/OLED/狀態保存/mDNS 重查） | `loop()` |
| OLED | `updateOLED()` |

| 關心點 | 在 `camera/src/main.cpp` 裡搜 |
|--------|------|
| IR/LED 引腳 | `PIN_IR` / `PIN_LED` |
| WiFi 帳密 | `AP_SSID` / `AP_PASS` / `FALLBACK_SSID` / `FALLBACK_PASS` |
| 傳感器配置 | `setup()` 裡的 `camera_config_t` |
| 串流 | `stream_handler()` |
| 兩個 httpd | `startCameraServer()` |
| WiFi 連接與回退 | `connectWiFi()` |

---

> 本文檔最初基於 `src/main.cpp`（1240 行，`abc8f9a`）與 `camera/src/main.cpp` 的靜態分析撰寫，源碼本身**未被修改**；2026-10-04 的改動已同步到相關小節，行號已全部換成函數名/變量名。任何邏輯改動請同步更新本文的相應小節。