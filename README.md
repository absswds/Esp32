# TEC 蟄眠實驗溫控系統

ESP32 開關式（bang-bang）溫控系統，用於脂尾袋鼩（fat-tailed dunnart）蟄眠實驗。3 顆 DS18B20 分角色監控，Web 儀表板 + OLED 即時顯示 + CSV 資料匯出。

## 目錄

- [硬體清單](#硬體清單)
- [接線圖與腳位](#接線圖與腳位)
- [系統架構](#系統架構)
- [初始設定](#初始設定)
- [建置與上傳](#建置與上傳)
- [WiFi 與網頁儀表板](#wifi-與網頁儀表板)
- [感測器角色與 ROM 位址](#感測器角色與-rom-位址)
- [溫控邏輯詳解](#溫控邏輯詳解)
- [OLED 螢幕](#oled-螢幕)
- [API 完整文件](#api-完整文件)
- [相機系統](#相機系統esp32-s3-ai-camera)
- [CSV 資料匯出](#csv-資料匯出)
- [參考文獻](#參考文獻)

---

## 硬體清單

| 零件 | 型號 / 規格 | 數量 | 備註 |
|------|------------|------|------|
| 開發板 | DFRobot **FireBeetle 2 ESP32-E** (DFR0654) | 1 | 注意 D-pin ≠ GPIO 編號 |
| 溫度感測器 | DS18B20 TO-92 防水探頭 | 3 | 巢穴 / 活動區 / 出風口 |
| 製冷片 | TEC1-**12703**（12V / 3A） | 1 | 經對比測試選定，優於 12706 |
| H 橋驅動板 | 12V 雙路 PWM（如 BTS7960） | 1 | 最大 43A，綽綽有餘 |
| 銅片 | 0.3mm 厚 40×40mm 紫銅片 | 1 | **TEC 熱端散熱關鍵**，夾在 TEC 熱面與散熱器之間，ΔT 從 1.4°C 提升至 18.5°C |
| 冷端換熱器 | X 形內鰭片鋁型材（長方體，箱內） | 1 | 空氣穿過鰭片縫隙換熱，**不可包覆**，必須裸露在箱內氣流路徑 |
| 熱端散熱器 | 鋁鰭片散熱器（箱外） | 1 | 銅片 + 散熱膏貼合 TEC 熱面 |
| 熱端風扇 | 12V 0.66A 7cm 5500RPM | 1 | 風量足，TEC 滿載散熱無壓力 |
| 冷端循環風扇 | 12V 軸流風扇 | 1 | **PWM 降速使用（30–52%）**，全速反而把冷端吹熱（氣流熱負載） |
| 箱體 | 3D 列印（20×24×16cm） | 1 | PLA 壁薄漏熱，**需內貼 XPS 10mm 保温板** |
| 保温材料 | XPS 擠塑板 B1 級 10mm（60×60cm） | 1 | 貼滿箱內壁 6 面，隔絕環境漏熱 |
| 相機 | DFRobot DFR1154 ESP32-S3 AI Camera（OV2640） | 1 | 行為觀測，VGA MJPEG 串流 + IR/LED |
| OLED 螢幕 | SSD1306 128×64 I2C（0.96 吋） | 1 | 可選 |
| 電阻 | 4.7kΩ 1/4W | 1 | OneWire 匯流排上拉 |
| 電源 | 12V DC 適配器（建議 ≥5A） | 1 | 同時供電 ESP32 + TEC |

### TEC 型號選擇說明

12703 vs 12706 對比測試結果（詳見 `TEC分析報告.html`）：

| 指標 | 12706 | 12703 |
|------|-------|-------|
| 標稱電流 | 6A | 3A |
| 製冷速率 | 0.414 °C/min | **0.448 °C/min** |
| 加熱速率 | 1.297 °C/min | **1.419 °C/min** |
| 實際總功耗 | ~72W | ~36W |

12703 在同樣散熱條件下表現更好，原因：
- 總功率低 → 熱端散熱壓力小 → 冷端能維持更低溫
- 電流剛好匹配系統電源輸出範圍
- 程式不須任何修改，直接可用

### 實測性能（2026-08-01，室溫 24–25°C）

封閉箱體（20×24×16cm）實測，12V 供電（原始數據：`TEC_2026-08-01_12-29-07.csv`）：

| 條件 | 出風口最低溫 | 說明 |
|------|------------|------|
| 冷端風扇 100% | 18.6°C | 氣流熱負載大，冷端被吹熱 |
| 冷端風扇 50% | 17.3°C | 降速立即見效 |
| **冷端風扇 52%（持續）** | **14.9°C** | **達論文 15°C 目標 ✅** |

**關鍵發現：冷端風扇風量 = 熱負載**

風扇全速時，每秒鐘把大量室溫空氣送到冷端面前，TEC 被迫把泵熱能力花在"冷卻新進空氣"上，冷端溫度被拉高（出風口最低只到 18.6°C）。降速後（30–52%），空氣在鰭片內停留更久、換熱更充分，冷端能維持更低溫（14.9°C）。**風扇不是越快越好。**

**已知瓶頸：箱內溫度分布不均**

出風口 14.9°C 但巢穴（對角最遠端）仍 22°C，相差 7°C。原因：3D 列印箱壁薄（2-3mm PLA）漏熱嚴重，冷氣循環到最遠端時已被環境熱量加熱。解決方案：
1. **箱內壁貼 XPS 10mm 保温板**（進行中）— 隔絕漏熱，預期巢穴降至 17–18°C
2. **巢穴局部冷區** — 用 XPS 剩料把巢穴隔成小空間（約 1/6 體積）直接導冷風，小空間降溫容易 6 倍

**⚠️ 冷凝水風險（達標後的新問題）：** 出風口 14.9°C 已低於室溫 24-25°C + 高濕度下的露點（約 18-20°C），冷端鰭片**必然凝露**，水滴可能被氣流吹向動物。正式實驗前需加集水盤/排水設計（見 LIMITATIONS.md #25）。

**熱端配置（驗證有效）：** 0.3mm 銅片夾在 TEC 熱面與散熱器之間（兩面塗散熱膏）+ 7cm 5500RPM 風扇。無銅片時 ΔT 僅 1.4°C，加銅片後 18.5°C——銅片把 TEC 熱量橫向攤平，讓整個散熱器底面參與散熱。

**電源：** 12V 額定（TEC1-12703 設計電壓），無 15.8V 升壓需求。

---

## 接線圖與腳位

### 主接線表

| 訊號 | GPIO | FireBeetle 板標 | 接線對象 | 說明 |
|------|------|---------------|---------|------|
| DS18B20 DATA | **4** | **D12** | 3 顆 DS18B20 並聯（黃線） | 必須 4.7kΩ 上拉到 3.3V |
| DS18B20 VCC | — | 3.3V | 3 顆 DS18B20 並聯（紅線） | 不要用 5V |
| DS18B20 GND | — | GND | 3 顆 DS18B20 並聯（黑線） | |
| FAN PWM | **18** | SCK | 風扇 PWM 線 | 25kHz, 8-bit |
| TEC EN | **19** | MISO | H-bridge EN 腳 | HIGH = 啟用 TEC |
| TEC LPWM | **26** | **D3** | H-bridge 左半橋輸入 | 製冷方向 |
| TEC RPWM | **25** | **D2** | H-bridge 右半橋輸入 | 加熱方向 |
| OLED SDA | **21** | SDA | OLED SDA | I2C 專用腳 |
| OLED SCL | **22** | SCL | OLED SCL | I2C 專用腳 |
| OLED VCC | — | 3.3V | OLED VCC | SSD1306 吃 3.3V |
| OLED GND | — | GND | OLED GND | |

> ⚠️ **TEC 電源線已對調（重要）：** H 橋輸出到 TEC 的兩根線已交換，使代碼的「製冷/加熱」方向與實際冷熱面一致。**不要**再對調回來——除非更換 TEC 模組後重新驗證方向。驗證方法：設 `/test?cool=255&heat=0&en=1`，摸 TEC 冷面（貼冷端換熱器那面）應變冷。

### FireBeetle 2 ESP32-E D-pin 陷阱

**板標 D 編號 ≠ GPIO 編號！**

| 板標 | 實際 GPIO | 可用？ | 說明 |
|------|----------|--------|------|
| D2 | 25 | ✅ | TEC RPWM（加熱）|
| D3 | 26 | ✅ | TEC LPWM（製冷）|
| D4 | 27 | ✅ | 空，備用 |
| D5 | 0 | ❌ Strapping | 勿用 |
| D8 | 5 | ❌ Strapping | 勿用 |
| D9 | 2 | ❌ Strapping | 勿用（內建 LED）|
| **D12** | **4** | ✅ | **DS18B20 接這裡** |
| **D13** | **12** | ❌ Strapping | **接了 4.7kΩ 上拉 = ESP32 變磚** |

---

## 系統架構

**兩個獨立韌體目標（各自 `pio run`）：**

```text
┌─ 主 ESP（FireBeetle 2 ESP32-E）─ src/main.cpp ─┐
│  TEC 溫控 + 3×DS18B20 + OLED + Web 儀表板     │
│  WiFi AP：ESP32-TEMP（192.168.4.1）           │
└───────────────────────────────────────────────┘

┌─ 相機（DFR1154 ESP32-S3 AI Camera）─ camera/src/main.cpp ─┐
│  MJPEG 串流（VGA 640×480）+ IR/LED 控制                   │
│  WiFi STA：連手機熱點，mDNS：esp32-cam                    │
└──────────────────────────────────────────────────────────┘
```

### 主 ESP 程式結構

```
src/main.cpp  (單一 Arduino sketch，約 815 行)

├── 硬體層
│   ├── OneWire + DallasTemperature → 3×DS18B20
│   ├── LEDC PWM ×3 → FAN / TEC_L / TEC_R
│   ├── GPIO OUT → TEC_EN
│   └── I2C HW → SSD1306 OLED
│
├── 控制層
│   ├── readSensor() — 非阻塞轉換狀態機（750ms）+ EMA 濾波
│   ├── controlTemp() — 雙層安全 + 開關控制（bang-bang）
│   ├── setTecPwm() — 0~1.0 歸一化功率 → 0~1023 PWM
│   └── emergencyStop() — 不可逆斷電
│
├── 網路層
│   ├── WiFi AP mode (ESP32-TEMP / 192.168.4.1)
│   └── WebServer port 80 (5 個路由)
│
└── 顯示層
    ├── INDEX[] PROGMEM — SPA 儀表板（HTML+CSS+JS inline）
    └── updateOLED() — SSD1306 狀態顯示
```

### Loop 狀態機

`loop()` 每個迭代執行 5 件事：

1. **`esp_task_wdt_reset()`** — 餵 3 秒硬體看門狗
2. **`server.handleClient()`** — 非阻塞 HTTP
3. **風扇延遲** — TEC 關閉後保持 78% 風扇 10 秒（吹散餘熱），然後關閉
4. **DS18B20 重掃描** — 每 10 秒嘗試（**任一探頭缺失**時：`!dsOk || !nestOK || !roomOK || !ventOK`）
5. **`readSensor()`** — 每 2 秒觸發一次非阻塞讀取

`readSensor()` 內部狀態機：
```
convPending=false → requestTemperatures() → 設 convPending=true
convPending=true  → 等 750ms → 讀取 → convPending=false
```

---

## 初始設定

### 1. 裝 PlatformIO

```bash
# 如果還沒裝 PlatformIO CLI
pip install platformio
```

或在 VS Code 裝 PlatformIO IDE 擴充套件。

### 2. 首次編譯前確認

- [ ] `platformio.ini` 中的 board 是 `esp32dev`
- [ ] DS18B20 的 ROM 位址與你手上的感測器一致（見下方）
- [ ] 4.7kΩ 上拉電阻已焊好

### 3. 燒錄

```bash
pio run --target upload
```

首次燒錄完成後，打開 Serial Monitor（115200 baud）確認輸出：
```
[DS18B20] 就緒 巢穴=1 活動=1 出風=1
[WiFi] 192.168.4.1
[Server] OK
```

### 4. 更換感測器時的 ROM 位址更新

如果換了其中一顆 DS18B20，需要更新 `src/main.cpp` 中的對應常數：

```cpp
DeviceAddress nestAddr = {0x28, 0xXX, 0xXX, ...};  // 改成新的 ROM
```

如何取得新位址：看 Serial Monitor 在 `doScan()` 時輸出的 16 位十六進位碼。

---

## 建置與上傳

```bash
# 只編譯（檢查語法錯誤）
pio run

# 編譯 + 上傳 + 自動打開 Serial Monitor
pio run --target upload --target monitor

# 只開 Serial Monitor（不上傳）
pio run --target monitor

# 清理重編
pio run --target clean
pio run
```

**專案設定：**
- Board：`esp32dev`
- 變體：`dfrobot_firebeetle2_esp32e`
- 框架：Arduino
- Baud：115200

**依賴（`platformio.ini`）：**

| Library | 版本 | 用途 |
|---------|------|------|
| `paulstoffregen/OneWire` | ^2.3.7 | OneWire 匯流排通訊 |
| `milesburton/DallasTemperature` | ^3.11.0 | DS18B20 高階 API |
| `olikraus/U8g2` | ^2.34.22 | SSD1306 OLED 顯示 |

---

## WiFi 與網頁儀表板

### 連線

| 項目 | 值 |
|------|-----|
| SSID | **ESP32-TEMP** |
| 密碼 | **12345678** |
| IP | **192.168.4.1** |

ESP32 開的是獨立 AP，手機/電腦直接連上去，不經任何路由器。連上後瀏覽器打開 `http://192.168.4.1`。

### 儀表板功能詳解

#### 頂部狀態列
- 綠點 = 系統正常 → 時間戳
- 紅點 = 感測器異常 → 錯誤訊息

#### 三感測器卡片
- 紅框 = 巢穴（溫控基準）
- 藍框 = 活動區（環境監測）
- 橘框 = 出風口（安全保護）
- 每張卡片即時顯示溫度

#### 溫度趨勢圖
- Canvas 繪製，自動縮放 Y 軸
- 圖表下方三個切換按鈕：**巢穴** / **活動區** / **出風口**
- 紅/藍/橘線對應三個感測器
- 右上角顯示最新一筆讀值
- 右下「導出 CSV」和「清除記錄」按鈕

#### 系統控制區
- **開啟/停止系統** — 總開關
- 狀態 pill：待機 / 製冷中 / 加熱中 / 維持中
- **製冷/加熱 pill** — 點擊直接手動控制（會發送到 `/test`）
- **切換手動/自動模式** — 手動模式下 `controlTemp()` 不執行

#### 目標溫度設定
- 目標溫度（10–40°C）：拉條 ⊕ 數字輸入框 ⊕ 滾輪
- 維持範圍（0.01–3°C）：同上
- 提示行：「巢穴 ＞X°C 製冷 | ＜Y°C 加熱 | 中間不動作」
- 數字框可直接打任意小數值，按 Enter 生效
- **打字時不會被 poll 覆蓋**（focus 鎖定，blur 解鎖）

#### 安全保護設定
- 巢穴最低溫（0–20°C）
- 巢穴最高溫（20–50°C）
- 出風口上限（30–80°C）
- 出風口超過上限 → 系統立即斷電（必須手動重啟）

#### 風速控制
- 手動風速 0–100%（拉條 + 百分比顯示）
- 手動調整後 3 秒自動釋放控制權回自動模式

#### 更新頻率
- 1–10 秒可調（預設 2 秒）

---

## 感測器角色與 ROM 位址

系統**不依賴匯流排掃描順序**辨識感測器，而是比對 ROM 出廠唯一位址：

| 角色 | ROM 位址 | 用途 |
|------|---------|------|
| 🏠 **巢穴** (Nest) | `28559122000000D6` | **溫控基準** — 製冷與加熱都看這顆 |
| 🌡 **活動區** (Room) | `284F59220000005E` | 純監測，不參與溫控邏輯 |
| 🌬 **出風口** (Vent) | `2820D720000000D7` | 硬體安全 — 過熱緊急斷電 |

### 辨識新感測器

1. 接上新 DS18B20 → 上傳程式 → 看 Serial Monitor
2. `doScan()` 會印出每個裝置的 ROM 位址和對應的角色
3. 認不到的會顯示 `[?]` → 把該 ROM 碼更新到 `src/main.cpp`

### 斷線行為

- 任一感測器斷線 → 對應溫度設為 `NAN`
- 網頁卡片顯示 `--`
- 巢穴斷線 → 溫控暫停（`isnan(nestT)` 直接 return）
- 出風口斷線 → 過熱保護失效（**已知缺陷**，見 LIMITATIONS.md）

---

## 溫控邏輯詳解

### 控制流程（`controlTemp()`）

```
系統開啟 且 非手動 mode 且 巢穴有讀值？
  ├── NO → return（不動作）
  └── YES ↓

出風口 ≥ ventMax（50°C）？
  ├── YES → emergencyStop() → 系統永久關閉
  └── NO ↓

巢穴 < safeMin（5°C）或 > safeMax（35°C）？
  ├── YES → 關 TEC，風扇全速（過熱）/ 0（過冷）
  └── NO ↓

巢穴 > 目標溫度 + 維持範圍？
  ├── YES → 製冷 100%，風扇 **52%（實測最優，見下方警告）**
  └── NO ↓

巢穴 < 目標溫度 - 維持範圍？
  ├── YES → 加熱 100%，風扇全速
  └── NO → 維持：關 TEC，風扇關閉
```

### 開關控制（bang-bang）

TEC 在超出維持範圍時以 **100% 功率**運行，進入範圍內後完全關閉。風扇在 TEC 啟動時，製冷維持 52%、加熱維持全速；TEC 關閉後延遲 10 秒（78%）吹散餘熱，然後關閉。

> ⚠️ **實測修正（2026-08-01）：** 冷端循環風扇全速（100%）會把冷端吹熱（出風口最低 18.6°C），**製冷時風扇應以 PWM 30–52% 運行**（出風口可達 14.9°C）。加熱時風扇維持全速。熱端散熱風扇不受此限，維持全速。


---

## OLED 螢幕

### 接線（4 線）

| OLED-2864 腳 | 接線 | FireBeetle |
|-------------|------|-----------|
| VCC | 紅 | 3.3V |
| GND | 黑 | GND |
| SCL | 黃 | GPIO22（SCL）|
| SDA | 藍 | GPIO21（SDA）|

GPIO21/GPIO22 是 FireBeetle 預設 I2C 腳，與其他功能不衝突。

### 顯示內容

```
Nest:  29.1 C
Room:  29.3 C
Vent:  29.5 C
────────────────────
Cooling    Fan:100%
Tgt:28.0C  Auto
```

- 上半部：三通道即時溫度，斷線顯示 `---`
- 分隔線
- 左下：當前狀態（Cooling / Heating / Idle / OFF）+ 目標溫度
- 右下：風扇百分比 + 自動/手動模式

每 2 秒刷新。

### 熱插拔自動恢復

OLED 支援熱插拔恢復（`e9c6829`）：
- 啟動時掃描 I2C 匯流排，確認 OLED 地址 0x3C
- `loop()` 每 5 秒檢查一次 `oledOnline` 狀態
- 若 OLED 斷線 → 自動呼叫 `u8g2.begin()` 重新初始化，無需重啟設備

---

## API 完整文件

WiFi AP 模式，Port 80，IP `192.168.4.1`。

### `GET /` — 儀表板

回傳內嵌 HTML SPA（PROGMEM）。Content-Type: `text/html; charset=utf-8`。

### `GET /data` — 即時數據

回傳 JSON：

```json
{
  "ok": true,
  "nest": 29.12,
  "room": 29.34,
  "vent": 29.56,
  "temps": [29.12, 29.34, 29.56],
  "sensorCount": 3,
  "fanSpeed": 100,
  "cooling": false,
  "heating": false,
  "systemOn": true,
  "manualMode": false,
  "targetTemp": 28.0,
  "hysteresis": 0.50,
  "safeMin": 5.0,
  "safeMax": 35.0,
  "ventMax": 50.0
}
```

| 欄位 | 類型 | 說明 |
|------|------|------|
| `nest` | float | 巢穴溫度 °C |
| `room` | float | 活動區溫度 °C |
| `vent` | float | 出風口溫度 °C |
| `temps` | [float×3] | 巢穴/活動/出風口陣列 |
| `sensorCount` | int | 實際 ROM 識別到的感測器數量 |
| `fanSpeed` | int | 風扇 PWM（0-255），網頁轉為 % |
| `cooling` | bool | 製冷中？ |
| `heating` | bool | 加熱中？ |
| `systemOn` | bool | 系統總開關 |
| `manualMode` | bool | 手動模式？ |
| `targetTemp` | float | 目標溫度 °C |
| `hysteresis` | float | 維持範圍 °C |
| `safeMin` / `safeMax` | float | 巢穴安全範圍 °C |
| `ventMax` | float | 出風口斷電閾值 °C |

錯誤時回傳 `{"ok":false,"message":"..."}`。

### `POST /control` — 控制指令

| 參數 | 範圍 | 說明 |
|------|------|------|
| `system` | 0/1 | 系統總開關 |
| `manual` | 0/1 | 切換手動/自動模式 |
| `targetTemp` | 10–40 | 目標溫度 °C |
| `hysteresis` | 0.01–3 | 維持範圍 °C |
| `safeMin` | 0–20 | 巢穴最低安全溫 |
| `safeMax` | 20–50 | 巢穴最高安全溫 |
| `ventMax` | 30–80 | 出風口斷電閾值 |
| `nestOff` | -5~5 | 巢穴感測器校準偏移 °C |
| `ventOff` | -5~5 | 出風口感測器校準偏移 °C |
| `wifi` | 0/1 | **WiFi 模式**：0=純 AP（默認，最穩定），1=STA+AP备援（先連指定 WiFi，失敗切 AP）。切換後系統自動重啟。 |
| `ssid` | 字串 | 搭配 `wifi=1`：要連接的 WiFi 名稱，存 EEPROM[35] |
| `pass` | 字串 | 搭配 `wifi=1`：WiFi 密碼，存 EEPROM[67] |

參數可組合。範例：

```
POST /control?system=1&targetTemp=28.0&hysteresis=0.5
POST /control?wifi=1&ssid=MyWiFi&pass=12345678   // 切到 STA 模式連 MyWiFi，自動重啟
POST /control?wifi=0                             // 回到純 AP 模式，自動重啟
```

### `GET /test` — 手動測試

| 參數 | 範圍 | 說明 |
|------|------|------|
| `en` | 0/1 | TEC EN 腳 |
| `fan` | 0–255 | 風扇 PWM |
| `cool` | 0–255 | 製冷 PWM |
| `heat` | 0–255 | 加熱 PWM |

使用後會設 `tecManual`/`fanManual` 旗標，自動模式會跳過已被手動控制的輸出。

### `GET /diag` — 診斷資訊

```json
{
  "pin": 4,
  "pinState": 1,
  "dsOk": true,
  "deviceCount": 3,
  "nest": 29.1,
  "room": 29.3,
  "vent": 29.5,
  "conversionPending": false
}
```

用於硬體偵錯：確認腳位狀態、匯流排裝置數量、感測器讀值。

---

## 相機系統（ESP32-S3 AI Camera）

### 硬體

- **DFRobot DFR1154**（ESP32-S3，16MB Flash + 8MB OPI PSRAM，OV2640）
- ⚠️ **必須用 `dfrobot_dfr1154` 板型**——通用 `esp32-s3-devkitc-1` 無 PSRAM，代碼強制 `CAMERA_FB_IN_PSRAM` 會導致 DMA 崩潰、串流卡死
- IR 燈：GPIO47；LED：GPIO3

### 網路

- WiFi STA 模式，連手機熱點（`OPhone 12`）
- mDNS：`esp32-cam`（主 ESP 為 `esp32-tec`）

### 雙端口架構（串流與控制分離）

| 路由 | 連接埠 | 用途 |
|------|--------|------|
| `/stream` | 80 | MJPEG 串流（VGA 640×480，jpeg_quality 16） |
| `/light` | 81 | `?ir=1/0` / `?led=1/0` 紅外燈 / LED |
| `/status` | 81 | JSON 狀態 `{"ir":0,"led":0}` |

**IR/LED 由前端直連相機 81 端口控制**（不經主 ESP 代理），避免串流佔住控制端點導致燈光無反應。

### 已修復的坑（全部解決）

1. 板型無 PSRAM → DMA 崩潰（改 `dfrobot_dfr1154`）
2. 幀緩衝洩漏 → 發送失敗未釋放（加 `esp_camera_fb_return(fb)`）
3. 慢客戶端阻塞串流 5 秒 → `send_wait_timeout = 1s`
4. 客戶端斷開後 `while(true)` 卡死 → 探針偵測斷開退出

### 建置

```bash
pio run -e esp32s3cam
pio run -e esp32s3cam --target upload
```

---

## CSV 資料匯出

### 格式

```csv
時間,巢穴,活動區,出風口,風扇(%),狀態
2026/7/22 16:07:30,29.1,29.3,29.4,0,維持
2026/7/22 16:07:31,29.1,29.3,29.4,100,製冷
```

- BOM（U+FEFF）開頭 → Excel 直接開不會亂碼
- 時戳含完整日期時間（`toLocaleString()`）
- 檔案名含時間：`TEC_2026-07-22_16-07-30.csv`

### 數據範圍

兩個記憶體緩衝區：

| 緩衝區 | 上限 | 用途 |
|--------|------|------|
| `H` | 600 筆 | 圖表顯示（滑動窗口，只保留最近 20 分鐘） |
| `allData` | 10000 筆 | CSV 匯出（超過上限時最舊資料被丟棄） |

**注意：** 關掉瀏覽器分頁或 ESP32 重啟 → `allData` 全部遺失。這是已知缺陷（LIMITATIONS.md #5）。

---

## 檔案結構

```
├── src/
│   └── main.cpp          ← 主 ESP 完整程式（~815 行）
├── camera/
│   ├── src/main.cpp      ← 相機韌體（雙 httpd：串流 80 / 控制 81）
│   ├── platformio.ini    ← 相機環境（board = dfrobot_dfr1154）
│   └── boards/           ← 自訂板型（16MB Flash + 8MB PSRAM）
├── platformio.ini        ← 主 ESP PlatformIO 專案設定
├── README.md             ← 本文件
├── CLAUDE.md             ← AI 開發輔助檔
├── LIMITATIONS.md        ← 與論文出入與不足（33 項）
├── TEC分析報告.html       ← 12706 vs 12703 對比分析圖表
├── TEC-12703_製冷.csv    ← 12703 製冷測試原始數據
├── TEC-12703_加熱.csv    ← 12703 加熱測試原始數據
├── TEC12703-8.5V_製冷.csv ← 12703 降壓 8.5V 製冷測試
├── TEC12703-8.5V_制熱.csv ← 12703 降壓 8.5V 加熱測試
├── TEC-12706_製冷.csv    ← 12706 製冷測試原始數據
├── TEC-12706_加熱.csv    ← 12706 加熱測試原始數據
├── TEC_2026-08-01_12-29-07.csv ← 密封箱實測（三檔風扇對比，47 分鐘）
└── 論文報告.pdf           ← 實驗論文（溫度需求基準）
```

## 參考文獻

- **Lopatko, O.V. et al. (1999)** — "Arousal patterns in the fat-tailed dunnart (Sminthopsis crassicaudata)" — 甦醒速率 0.7–1.0°C/min，為本系統溫控設計基準
- **DFRobot FireBeetle 2 ESP32-E 技術文件** — 腳位對應與 strapping pin 警告
- **Dallas Semiconductor DS18B20 Datasheet** — OneWire 時序需求與 12-bit 精度規格
- **TEC1-12703 / TEC1-12706 規格書** — 標稱電流 3A/6A，12V，ΔTmax 68°C
