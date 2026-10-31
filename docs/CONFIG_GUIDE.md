# 配置指南（CONFIG_GUIDE）

> 本文檔彙總本系統**所有可配置項**：引腳、PWM 參數、溫度閾值、EEPROM 地址映射、調參說明、單位與硬約束。可獨立閱讀，涵蓋主 ESP 與相機兩固件。源碼分析基礎為只讀模式，**未做任何修改**。
>
> 如果只想知道"我現在改這個值會發生什麼、改之前要注意什麼"，請直接跳到 §6 "調參導引"。
>
> **狀態（2026-09-10 收尾鎖版）：** 引腳/閾值/EEPROM 佈局已核對（2026-09-26 複核時 `src/main.cpp` 為 1240 行，含 BME688）；代碼已知問題集中在 [../LIMITATIONS.md](../LIMITATIONS.md) #34~#37，鎖版凍結不再改代碼。
>
> **2026-10-04 解凍增補：** 傳感器換裝 BME680（1 Hz 溫溼壓 + 每 10 次量一次 Gas）、新增防結露警示 `condRisk`（僅警示）、斷電自動恢復（LIMITATIONS #31）；`src/main.cpp` 現約 1340 行。新常量見 §3.4。

---

## 目錄

1. [引腳映射](#1-引腳映射)
2. [PWM / LEDC 參數](#2-pwm--ledc-參數)
3. [溫度閾值與安全保護](#3-溫度閾值與安全保護)
4. [EEPROM 地址映射](#4-eeprom-地址映射)
5. [WiFi 配置](#5-wifi-配置)
6. [調參導引](#6-調參導引)
7. [單位與硬約束速查](#7-單位與硬約束速查)
8. [前端 UI 約束 vs 後端 constrain 對齊表](#8-前端-ui-約束-vs-後端-constrain-對齊表)
9. [相機固件配置](#9-相機固件配置)
10. [常見誤配置排查](#10-常見誤配置排查)

---

## 1. 引腳映射

### 1.1 主 ESP（FireBeetle 2 ESP32-E，board `esp32dev`）

定義見 `src/main.cpp:14-24`。

| 信號 | 宏 | GPIO | FireBeetle 板標 | 類型 | 用途 |
|------|-----|------|------------------|------|------|
| FAN | `FAN_PIN` | 18 | SCK | LEDC ch0 PWM 輸出 | 風扇 PWM |
| TEC EN | `TEC_EN` | 19 | MISO | 數字 OUT | H-bridge 使能，HIGH=ON |
| TEC LPWM | `TEC_LPWM` | 26 | D3 | LEDC ch1 PWM 輸出 | 製冷方向（**線已對調**，詳見 README 注意事項） |
| TEC RPWM | `TEC_RPWM` | 25 | D2 | LEDC ch2 PWM 輸出 | 加熱方向 |
| DS18B20 | `DS18B20_PIN` | 4 | **D12** | OneWire 雙向 | 3 顆並聯 + 4.7kΩ 上拉到 3.3V |
| LED 燈帶 | `LED_PIN`（ledc ch4，5kHz 10-bit） | 13 | D7 | 數字/PWM OUT | **已裝機**（2026-10-04 用戶確認）：經 IRLZ44N 驅動 12V COB 燈帶 |
| OLED SDA | — | 21 | SDA | I2C 雙向 | SSD1306 數據 |
| OLED SCL | — | 22 | SCL | I2C 雙向 | SSD1306 時鐘 |
| BME680 | — | 21/22 | SDA/SCL | I2C 雙向 | **已裝機**（2026-10-04 用戶確認）：與 OLED 併線（同址 0x76/0x77 衝突時需改址） |

> ⚠️ **板標 D13=GPIO12 是 strapping pin**：接 4.7kΩ 上拉會把 ESP32 誤認為 1.8V flash 電壓，可能致磚。**OneWire 必須接 D12（GPIO4），切勿接 D13**。
>
> ⚠️ **D5/D8/D9 是 strapping pin**（GPIO0/5/2），上電瞬間電平被採樣，勿用於 OneWire 或帶上拉的關鍵 IO。

### 1.2 FireBeetle 2 ESP32-E D-pin ↔ GPIO 對照

主要 D-pin 與 GPIO **不一致**，源碼中所有引腳號都是 GPIO 而非板標。常見映射：

| 板標 | GPIO | 可安全用？ |
|------|------|-----------|
| D2 | 25 | ✅ |
| D3 | 26 | ✅ |
| D7 | 13 | ✅（**LED 燈帶**經 IRLZ44N；本板無 D4/GPIO27） |
| D5 | 0 | ❌ strapping |
| D8 | 5 | ❌ strapping |
| D9 | 2 | ❌ strapping（也是內置 LED） |
| D12 | **4** | ✅（**OneWire 用**） |
| D13 | 12 | ❌ strapping（關鍵陷阱） |

### 1.3 相機（DFR1154 ESP32-S3）

見 `camera/src/main.cpp:17-18, 202-218`。

| 信號 | 引腳 | 用途 |
|------|------|------|
| IR 燈控制 | GPIO47 | 數字 OUT，HIGH=ON |
| LED 白光控制 | GPIO3 | 數字 OUT，HIGH=ON |
| 攝像頭 D0~D7 / XCLK / PCLK / VSYNC / HREF / SCCB | 16/18/21/17/14/7/6/4 + xclk=5 + pclk=15 + vsync=1 + href=2 + sda=8/scl=9 | OV3660 8-bit 並行數據（DFR1154 實際傳感器；紅外補光 940 nm，GPIO47） |

相機引腳固定，**不要改**——這些是 DFR1154 廠商硬連接的板內走線，改 `camera_config_t` 引腳會失配導致無圖像。

---

## 2. PWM / LEDC 參數

定義見 `src/main.cpp:14-23`。

| 通道 | 宏 | 引腳 | 頻率 | 分辨率 | 範圍 | 寫函數 |
|------|-----|------|------|--------|------|--------|
| ch0 | `FAN_CH` | GPIO18 | 25000 Hz | 10-bit (`PWM_RES`) | 0~1023 | `setFan(s)`，輸入 0~255 然後 ×4 |
| ch1 | `TEC_L_CH` | GPIO26 | 25000 Hz | 10-bit (`TEC_PWM_RES`) | 0~1023 | `setTec(cool,heat)` / `setTecPwm(power,isCool)` |
| ch2 | `TEC_R_CH` | GPIO25 | 25000 Hz | 10-bit (`TEC_PWM_RES`) | 0~1023 | 同上 |

### 2.1 輸入語義

| 函數 | 輸入語義 | 實際 PWM 值 | 適用 |
|------|----------|------------|------|
| `setFan(int s)` | `s ∈ [0,255]`，會被 `constrain` 截斷 | `ledcWrite(FAN_CH, s*4)` → 0~1020 | 風扇百分比 |
| `setTec(int cool, int heat)` | 8-bit 0~255 | `constrain(val*4,0,1023)` | 手動 / `/test` 端點 |
| `setTecPwm(float power, bool isCool)` | 0.0~1.0 歸一化 | `(int)(power*1023)`，`constrain[0,1023]` | 自動 bang-bang |

### 2.2 關鍵衍生參數

| 項 | 宏 / 全局 | 值 | 位置 | 說明 |
|----|----------|----|----|----|
| EMA 濾波係數 | `EMA_ALPHA` | `0.5f` | `main.cpp:89` | 新讀值佔 50%，舊濾波值佔 50%；越大越逼真但越噪 |
| 風扇延遲關閉時間 | `FAN_AFTERRUN_MS` | `10000` ms | `main.cpp:94` | TEC 關後保持 78% 風扇 10 秒 |
| 風扇延遲檔速 | `FAN_AFTERRUN_SPEED` | `200` PWM | `main.cpp:95` | ≈78%（200/255） |
| 狀態保存去抖 | `STATE_SAVE_DEBOUNCE_MS` | `750` ms | `main.cpp:81` | `/control` 寫參數後 0.75s 內不再寫 EEPROM |
| 看門狗超時 | `esp_task_wdt_init(7, true)` | **7 秒** | `main.cpp:1050` | ⚠️ 舊註釋寫 3 秒，**實際是 7 秒**，以代碼為準 |
| OneWire 轉換等待 | `millis()-convStart >= 750` | 750 ms | `main.cpp:324` | DS18B20 12-bit 轉換時間硬約束 |
| 感測週期 | `millis()-lastRead >= 2000` | 2000 ms | `main.cpp:1209` | loop 內觸發；疊加 750ms 轉換等待與調度開銷，**實測約 4s**（見 ARCHITECTURE） |
| 重掃間隔 | `millis()-lastScan >= 10000` | 10 s | `main.cpp:1205` | 任一感測器缺失時才掃 |
| OLED 檢查 | `millis()-lastOledCheck >= 5000` | 5 s | `main.cpp:1212` | 熱插拔檢查 |
| OLED 刷新 | `millis()-lastOled >= 2000` | 2 s | `main.cpp:1224` | 畫面更新 |
| mDNS 重查 | `resolveInterval` | 5 s（未確認）/60 s（確認） | `main.cpp:1231` | 相機 IP 刷新 |

### 2.3 文檔與代碼不符處（風扇）

> ⚠️ 實測冷端風扇降速比全速好（08-01：100% 時出風口 18.6°C，手動 52% 時 14.87°C），但 **`controlTemp()` 製冷分支實際寫 `setFan(255)`（全速）**。

注意數據只能說明"降速優於全速"，沒做過檔位掃描，52% 不一定是最優點。**這部分代碼未同步**（收尾鎖版決定不修）。詳見 [../LIMITATIONS.md](../LIMITATIONS.md) #34。若要同步：
- 把製冷分支 `setFan(255)` 改為 `setFan(round(255*0.52))`（≈133）或引入常量 `COOL_FAN_PCT=52`。
- 加熱分支 `setFan(255)` 維持不變（全速合理）。
- 演示對策：網頁手動把風扇設到 52%（`fanManual`）。

---

## 3. 溫度閾值與安全保護

### 3.1 全局閾值變量

| 變量 | 默認值 | API 範圍（constrain） | UI 滑條 min/max | 用途 | 位置 |
|------|--------|---------------------|----------------|------|------|
| `targetTemp` | `28.0` | `10.0 ~ 40.0` | 10/40/step 0.1 | 單目標溫度 °C | `main.cpp:60` |
| `hysteresis` | `0.5` | `0.01 ~ 3.0` | 0.01/3/step 0.01 | 維持死區 ±hysteresis °C | `main.cpp:61` |
| `safeMin` | `5.0` | `0 ~ 20` | 0/20/step 0.5 | 巢穴最低安全溫度（動物保護） | `main.cpp:84` |
| `safeMax` | `35.0` | `20 ~ 50` | 20/50/step 0.5 | 巢穴最高安全溫度 | `main.cpp:85` |
| `ventMax` | `50.0` | `40 ~ 80` | 40/80/step 1 | 出風口斷電閾值（硬件保護） | `main.cpp:86` |

> ⚠️ **`ventMax` 範圍在 `handleControl()` 中是 `40~80`**（README 舊文本曾寫 `30~80`，現已同步為 40~80）。在 `loadState()` 中 clamp 也是 `40.0~80.0`。請以**40~80 為準**。
>
> ⚠️ `safeMin` 限在 `0~20`、`safeMax` 限在 `20~50`，兩個範圍不相交，所以經 `/control` **設不反**。真正的風險是代碼沒校驗 `targetTemp` 是否落在 `safeMin~safeMax` 之間：目標在區間外時會一直走"巢穴極端溫度"分支，TEC 永遠關著（另有 EEPROM 損壞讀出畸形值的情況，見 #35）。詳見 [../LIMITATIONS.md](../LIMITATIONS.md) #36。

### 3.2 校準偏移

| 變量 | 默認 | API 範圍 | 位置 | 用途 |
|------|------|----------|------|------|
| `nestOffset` | 0 | `-5.0 ~ 5.0` | `main.cpp:98`, 433-437 | 巢穴 DS18B20 校準 |
| `roomOffset` | 0 | `-5.0 ~ 5.0` | `82`, 287-291 | 活動區 |
| `ventOffset` | 0 | `-5.0 ~ 5.0` | `82`, 293-295 | 出風口 |

偏移在 `readSensor()` 中疊加到 raw 讀值，參見 `main.cpp:345`。**對濾波值和 `readTemps[]` 原始值都生效**（同一 `raw[i] += offs` 後分別走濾波與 readTemps）。控制、`/data`、OLED、CSV 用的都是濾波值，`readTemps[]` 目前沒有接口輸出。

### 3.3 閾值生效優先級

`controlTemp()` 五段保護順序（**最高優先級在最前**）：

1. NAN 守衛（任一溫度 NAN → nanCount++；≥3 次 → emergencyStop + saveState）
2. `ventT >= ventMax` → emergencyStop + saveState（不可逆，需手動重啟）
3. `nestT < safeMin || nestT > safeMax` → 關 TEC + 風扇（255 / 60，根據高溫或低溫）
4. 熱節流 `ventT > ventMax - 10` → `throttle = (ventMax - ventT)/10`，乘到 TEC 功率上
5. bang-bang：`targetTemp ± hysteresis` 死區外 → 全功率；死區內 → 關 TEC + 風扇延遲

**調參注意**：
- 想要更精細控制：縮小 `hysteresis`（如 0.1）。代價是死區邊緣頻繁切換。
- 想要更平緩：加大 `hysteresis`（如 1.0）。代價是穩態偏差更大。
- 切 PI 控制見 §3.5。

### 3.4 BME680 / 防結露常量（2026-10-04 新增）

| 參數 | 變量 | 默認 | 位置 | 說明 |
|------|------|------|------|------|
| 溫溼壓讀取週期 | `BME_INTERVAL_MS` | `1000` ms | `main.cpp:57` | 1 Hz，加熱器關閉 |
| Gas 量測間隔 | `BME_GAS_EVERY` | `10` | `main.cpp:58` | 每 10 次讀取開一次加熱器（320°C/150ms）量 Gas，即 10 s 一次 |
| Gas 燒機期 | `BME_BURNIN_MS` | `600000` ms | `main.cpp:63` | 上電前 10 分鐘 Gas 值勿引用 |
| Gas 基線 EMA | `BME_GAS_ALPHA` | `0.02f` | `main.cpp:64` | `bmeGasRel` 的分母 |
| 結露警示進入餘量 | `COND_ON_MARGIN` | `1.0f` °C | `main.cpp:70` | 出風口 ≤ 露點 + 1°C → `condRisk=true` |
| 結露警示退出餘量 | `COND_OFF_MARGIN` | `2.0f` °C | `main.cpp:71` | 出風口 > 露點 + 2°C → `condRisk=false`（中間保持，遲滯） |
| BME 數據過期 | `COND_STALE_MS` | `30000` ms | `main.cpp:72` | BME 離線或 30 s 無成功讀取 → `condRisk` 強制 false |

`condRisk` **僅警示**（`/data` 字段、網頁橫幅、OLED `DEW`），不進入 `controlTemp()`，不寫 EEPROM。出風口氣溫只是冷端表面的代理量，警示可能偏晚。

---

### 3.5 切回 PI 控制（正式蟄眠實驗前）

07-23 從 PI 改成 bang-bang，是因為地面測試溫差太小（室溫 29–30°C、目標 28°C），PI 怎麼調都不夠積極。正式實驗是 24 → 15°C，溫差大，冷量也夠了（銅片 + 冷端風扇降速後出風口 14.87°C），可以換回 PI 讓溫度變化平緩一些。

下面是步驟草稿，**沒有在現行代碼上編譯驗證過**，改完先 `pio run -e esp32dev`，上機前先空箱跑一遍。

1. 在 `EMA_ALPHA` 附近加變量：

```cpp
float coolIntegral = 0, heatIntegral = 0;
const float KI = 0.05f;           // 07-23 調到的值
const float KI_MAX = 0.5f;
const float maxHeatRate = 1.0f;   // °C/min；只有想模擬自然復溫才需要，論文流程是人工快速復溫
const float maxCoolRate = 0.5f;   // °C/min
float prevNestT = NAN;
unsigned long lastControlTime = 0;
```

2. 把 `controlTemp()` 裡 `throttle` 計算之後的 `// === 製冷/加熱/維持 ===` 整段換成：

```cpp
float dtMin = (millis() - lastControlTime) / 60000.0f;
lastControlTime = millis();
float coolDiff = nestT - (targetTemp + hysteresis);
float heatDiff = (targetTemp - hysteresis) - nestT;

if (coolDiff > 0) {
  heatIntegral = 0;
  if (coolDiff < 2.0) coolIntegral += coolDiff * KI;   // 接近目標才積分，防 windup
  coolIntegral = constrain(coolIntegral, -KI_MAX, KI_MAX);
  float power = constrain(coolDiff / 5.0 + coolIntegral, 0.15, 1.0);
  if (coolDiff > 3.0 && !isnan(prevNestT) && dtMin > 0) {   // 溫差 >3°C 才限速
    float rate = (nestT - prevNestT) / dtMin;
    if (rate < 0 && -rate > maxCoolRate) power *= maxCoolRate / (-rate);
  }
  setTecPwm(power * throttle, true);
  if (!fanManual) setFan(100 + (int)(155 * power));    // 冷端風扇轉速另見 §2.3 / #34
} else if (heatDiff > 0) {
  coolIntegral = 0;
  if (heatDiff < 2.0) heatIntegral += heatDiff * KI;
  heatIntegral = constrain(heatIntegral, -KI_MAX, KI_MAX);
  float power = constrain(heatDiff / 5.0 + heatIntegral, 0.15, 1.0);
  if (heatDiff > 3.0 && !isnan(prevNestT) && dtMin > 0) {
    float rate = (nestT - prevNestT) / dtMin;
    if (rate > maxHeatRate) power *= maxHeatRate / rate;
  }
  setTecPwm(power * throttle, false);
  if (!fanManual) setFan(80 + (int)(175 * power));
} else {
  coolIntegral *= 0.95;
  heatIntegral *= 0.95;
  // ……保留原來的 setTecPwm(0) 和風扇 after-run 邏輯
}
prevNestT = nestT;
```

3. `throttle` 乘在 PI 輸出上，保留出風口接近 `ventMax` 時的降功率；`ventT >= ventMax` 急停不受影響。
4. `controlTemp()` 除了 `readSensor()` 還會被每次 `/control` 請求調用，兩次調用間隔可能很短，速率估計會抖。介意的話把 `prevNestT` / `lastControlTime` 的更新挪到 `readSensor()` 那條路徑上。
5. 要回到 bang-bang：刪掉這些變量，把這一段恢復原樣。

## 4. EEPROM 地址映射

主 ESP `EEPROM.begin(256)`，flash 模擬，**`EEPROM.commit()` 才真正寫**。映射見 `main.cpp:474-520`。

| 地址偏移 | 長度 | 字段 | C++ 存取 | 寫入時機 |
|----------|------|------|----------|---------|
| 0 | 1 byte | `0xAA` 有效標記 | `write(0,0xAA)` | 每次 saveState |
| 1 | 1 byte | `systemOn` (0/1) | `write(1,...)` | 系統開關；emergencyStop；
| 2 | 4 bytes | `targetTemp` (float) | `put(2,...)` | `/control` 寫 targetTemp |
| 6 | 4 bytes | `hysteresis` (float) | `put(6,...)` | `/control` 寫 hysteresis |
| 10 | 4 bytes | `safeMin` (float) | `put(10,...)` | `/control` |
| 14 | 4 bytes | `safeMax` (float) | `put(14,...)` | `/control` |
| 18 | 4 bytes | `ventMax` (float) | `put(18,...)` | `/control` |
| 22 | 4 bytes | `nestOffset` (float) | `put(22,...)` | `/control` 寫 nestOff |
| 26 | 4 bytes | `roomOffset` (float) | `put(26,...)` | `/control` 寫 roomOff |
| 30 | 4 bytes | `ventOffset` (float) | `put(30,...)` | `/control` 寫 ventOff |
| 34 | 1 byte | `wifiMode` (0/1) | `write(34,...)` | `/control` 寫 wifi |
| 35 | 33 bytes | `wifiSSID[33]` | `put(35,...)` | `/control` 寫 ssid |
| 68 | 65 bytes | `wifiPass[65]` | `put(68,...)` | `/control` 寫 pass |
| **68+65=133** | **已用 133 bytes**，餘 123 字節空白 | — | — | — |

> `EEPROM.begin(256)` 映射的 256 字節足夠覆蓋 133 字節使用。新增字段請從 133 起排，**注意 EEPROM 物理扇區壽命**（約 10 萬次寫滿額度的擦寫）。`scheduleStateSave()` 750ms 的去抖正是為降低碎片化寫入。

### 4.1 loadState 的安全邏輯

```text
if EEPROM[0] != 0xAA → 使用代碼默認值，return
else 讀全部字段
  · ventMax 越界 → clamp 到 [40, 80]        （main.cpp:504）
  · wifiMode 越界 (>1) → 設為 0              （main.cpp:509）
  · SSID 空 ('\0') 或 0xFF（未寫flash）→ 用默認 "OPhone 12" / "qwer1234"
```

**注意**：`targetTemp/hysteresis/safeMin/safeMax/offset` 等浮點字段 **沒有越界 clamp**——如果 flash 損壞填入畸形值，可能直接被加載用於控制。唯一例外是 `ventMax`。這是潛在風險，見 [../LIMITATIONS.md](../LIMITATIONS.md) #35（鎖版不修，演示前確認設備正常即可）。

---

## 5. WiFi 配置

主 ESP 支持兩種 WiFi 模式：

| 模式 | `wifiMode` | AP | STA | 設置命令 | 切換行為 |
|------|-----------|----|-----|----------|---------|
| 純 AP（默認） | 0 | `ESP32-TEMP` / `12345678` (192.168.4.1) | ❌ | `POST /control?wifi=0` | `setWifiModeAndRestart(0)` → saveState+delay(200)+ESP.restart |
| STA+AP 備援 | 1 | 失敗才開 `ESP32-TEMP` | ✅ 連 `wifiSSID/wifiPass` | `POST /control?wifi=1&ssid=XXX&pass=YYY` | 同上 |

STA 默認憑據：`OPhone 12` / `qwer1234`，當 EEPROM 中 SSID 為空或 0xFF 時回退。

相機固件無 EEPROM 配置，**帳密硬編碼在 `camera/src/main.cpp:12-15`**：
- `AP_SSID = "OPhone 12"`、`AP_PASS = "qwer1234"`（首選）
- `FALLBACK_SSID = "ESP32-TEMP"`、`FALLBACK_PASS = "12345678"`（回退）

修改相機 WiFi 必須**重新燒錄**。

### 5.1 主 mDNS 名稱

| 名稱 | 固件 | 說明 |
|------|------|------|
| `esp32-tec` | 主 ESP | `WiFi.setHostname("esp32-tec")`；但 `MDNS.begin()` 僅在 STA+AP 備援模式且 STA 成功時調用，純 AP 模式下主 ESP **無 mDNS** |
| `esp32-cam` | 相機 | `MDNS.begin("esp32-cam")`（連接成功後執行）；主 ESP 通過 `MDNS.queryHost("esp32-cam")` 解析 |

---

## 6. 調參導引

### 6.1 我要改目標溫度

**改法**：瀏覽器滑條 → 前端 `queueControl('targetTemp',...)` 300ms 防抖 → `POST /control?targetTemp=X` → `handleControl()` `constrain(10,40)` → `scheduleStateSave()` → `controlTemp()` 即時生效。

**邊界**：
- API clamp `[10, 40]`；UI 輸入框 `min=10 max=40 step=0.1`；slider 同。
- 若你想設 < 10 或 > 40，必須改 `handleControl` 的 constrain 才能寫進 backend（前端會被 HTML 輸入框擋住，但用 curl 直接發參數可繞過——backend 仍會截斷）。
- 數值用 `toFloat()` 解析；傳非數字 → `0.0f`，會被 clamp 到 10（**不是拒絕請求！**）。

### 6.2 我要改出風口斷電閾值

`POST /control?ventMax=XX`，範圍 **40~80**（API 文檔舊文本寫 30~80，**實際 40 起**）。
- 太低（接近環境溫度）會誤斷電；太高失去保護意義。
- `loadState()` 會再 clamp 一次到 40~80，所以即使 EEPROM 損壞也不會出範圍。

### 6.3 我要改校準偏移

`POST /control?nestOff=-0.3&roomOff=0&ventOff=0.2`，每項 `-5~5`。
生效時機：**下一次 `readSensor()` 讀數時**（而非下一次 `controlTemp`），因為偏移在 `readSensor()` 中累加到 raw 讀值後才進入濾波器。

如果斷線時 `*(filtPtrs[i]) = NAN`，下次恢復時**首次讀值直接設為 raw（不經過 EMA）**（`main.cpp:346-347`），所以重新校準後只需一次讀數就能"重置濾波"，斷線反而是恢復校準的契機。

### 6.4 我要換感測器

更新 `src/main.cpp:36-38` 的 `DeviceAddress` 常量（8 字節十六進制），重新燒錄。換下來的舊 ROM 不會被清出 EEPROM（EEPROM 不存 ROM 位址，ROM 是代碼常量）。

獲取新 ROM：調試時看啟動 Serial 的 `doScan()` 輸出，`[?]` 標記的就是未識別的設備，後面跟的 16 位十六進制串即為 ROM。

### 6.5 我要改風扇最速製冷

實測冷端風扇降速比全速好（52% 那次出風口 14.87°C，未做檔位掃描）；**默認代碼製冷分支寫 255 是全速**。如要套實測結論，要麼把 `setFan(255)` 改為 `setFan(133)`（≈52%），要麼新增常量 `COOL_FAN_PCT`。

### 6.6 我要加新參數

最小改動路徑：
1. 全局變量 + 默認值 + 宏範圍。
2. `handleControl()` 加 `if (server.hasArg("xxx")) { xxx = constrain(server.arg("xxx").toFloat(), LO, HI); changed = true; }`。
3. `saveState()` 加 `EEPROM.put(ADDR, xxx);`，選 `ADDR ≥ 133` 的新地址。
4. `loadState()` 加 `EEPROM.get(ADDR, xxx);`，並加越界 clamp（**推薦效仿 ventMax 的處理**，避免 flash 損壞填畸形值）。
5. 前端 `INDEX[]` HTML 加 `<input type="range">` 與 `setXxx` 函數，加 `queueControl('xxx',...)`。
6. `/data` JSON 輸出加字段。
7. `handleData()` `snprintf` 模板同步加。

---

## 7. 單位與硬約束速查

| 量 | 單位 | 合法範圍 | 物理硬約束 | 軟約束 |
|----|------|---------|----------|--------|
| DS18B20 溫度 | °C | `-55 ~ +85` | 12-bit 分辨率 0.0625°C，轉換 ≤750ms | 讀值超出 -55~85 → 判 NAN |
| PWM 分辨率 | bit | 10 | LEDC 上限 20 bit，本系統取 10 | `setTec` 輸入 8-bit 放大 4 倍 |
| FAN PWM 範圍 | bit value | 0~1023 (內部) | — | `setFan(0~255)` → ×4 寫到 10-bit |
| TEC PWM 範圍 | bit value | 0~1023 | — | `setTecPwm(0~1)` → ×1023 |
| EEPROM 容量 | byte | 256 | flash 模擬 | 已用 133 byte |
| Serial baud | bps | 115200 | — | 見 platformio.ini |
| WiFi AP SSID | str | ≤32 字節 | IEEE 11 | `wifiSSID[33]` |
| WiFi 密碼 | str | 8~63 字節 WPA2 | — | `wifiPass[65]` |
| H 網橋 EN 電壓 | — | HIGH=ON | — | 默認 LOW（關 TEC） |

---

## 8. 前端 UI 約束 vs 後端 constrain 對齊表

> 此表對每個參數列出 **HTML 輸入框 min/max/step** 與 **`handleControl()` 中 `constrain` 範圍** 是否一致。**不一致項必須修正**

| 參數 | HTML `min/max/step` | 後端 `constrain` | 一致？ | 備註 |
|------|---------------------|------------------|--------|------|
| targetTemp | 10/40/0.1 | `(10, 40)` | ✅ | NaN 輸入會被 clamp 到 10（非拒絕） |
| hysteresis | 0.01/3/0.01 | `(0.01, 3.0)` | ✅ | |
| safeMin | 0/20/0.5 | `(0, 20)` | ✅ | 與 safeMax 無交叉校驗 |
| safeMax | 20/50/0.5 | `(20, 50)` | ✅ | 同上 |
| ventMax | **40**/80/1 | `(40, 80)` | ✅（與實際代碼一致） | README 舊文本曾寫 30，現已同步 |
| nestOff/roomOff/ventOff | —（無 UI，僅 API） | `(-5, 5)` | — | UI 無此滑條，靠 `prompt()` 或 curl |
| wifi | 0/1 | `toInt()`，無 constrain | ✅ | `loadState` 會把 `>1` reset |
| fanS（手動風扇） | 0/100/1 | `0~255`（×255/100 轉換） | ✅ | UI 0-100% → API 0-255 |
| pollS（更新間隔） | 1/10/1 | — | — | 僅前端，不發後端 |

---

## 9. 相機固件配置

`camera/src/main.cpp` 幾乎全部硬編碼：

| 參數 | 值 | 位置 |
|------|-----|------|
| 主 WiFi SSID/PSK | `"OPhone 12"` / `"qwer1234"` | `:12-13` |
| 回退 WiFi SSID/PSK | `"ESP32-TEMP"` / `"12345678"` | `:14-15` |
| IR 引腳 | 47 | `:17` |
| LED 引腳 | 3 | `:18` |
| 串流端口 | 80 | `:126` |
| 控制端口 | 81 | `:145` |
| ctrl_port | 32769 | `:146`（與默認 32768 錯開） |
| 最大併發客戶端 | 4 | `:127, 147` |
| send_wait_timeout | 1 s | `:130` |
| 流無幀超時 | 10000 ms | `:48` |
| 幀大小 | VGA 640×480（無 PSRAM 時 QVGA） | `:227, :231` |
| JPEG quality | 16 | `:228` |
| xclk | 20 MHz | `:221` |
| fb_count | 2（PSRAM）/ 1（無 PSRAM） | `:225, :233` |
| grab_mode | `CAMERA_GRAB_LATEST` | `:223` |
| 傳感器後處理 | `vflip=1, hmirror=0, brightness=1, contrast=1, exposure_ctrl=1, agc_gain=0, gain_ctrl=1, gainceiling=2, ae_level=1` | `:243-255` |

**這些參數都沒有 UI 調整入口**，要改只能重新燒錄。

---

## 10. 常見誤配置排查

| 現象 | 可能原因 | 檢查方向 |
|------|----------|---------|
| 上電 OLED 顯示但 Serial 無 DS18B20 就緒 | OneWire 接到 D13（GPIO12）strapping，flash 電壓讀錯變磚邊緣 | 切到 D12（GPIO4），換前先做factory 檢查 |
| `deviceCount=0` | WiFi 中斷搶佔 OneWire 時隙（`doScan` 在 `WiFi.softAP` 之後跑） | 確認 `doScan()` 在 `WiFi.mode(...)` 之前調用 |
| 製冷方向反了 | TEC 線被對調但代碼認為原方向 | 看 `/test?cool=255&heat=0&en=1` 實際冷熱面對應 |
| `ventMax=30` API 被截到 40 | API 實際下限 40（README 舊文本誤） | 改為 40 或更高 |
| 設 0.001 hysteresis 看似有效但 TEC 頻繁啟停 | 死區太小 | 寫日誌看切換頻率，調到 0.1~0.5 |
| 誤關瀏覽頁時數據全失 | `allData` 僅在瀏覽器內存 | 已知缺陷，未來需 SD 卡/SPIFFS |
| 重啟後參數丟失 | `EEPROM.read(0) != 0xAA`，或寫時未 `commit()` | 看啟動 Serial `[EEPROM]` 行 |
| 切到 STA 模式失敗 | SSID/密碼錯或對端非 2.4GHz | 部分手機熱點 5GHz 不兼容；切回 AP 用 `/control?wifi=0` |
| 相機一直加載失敗 | mDNS 未解析 + IP 是 192.168.4.2 但相機不在線 | STA 模式下檢查 `esp32-cam.local`；純 AP 下相機會連不上 |

---

> 本文檔基於 `src/main.cpp`（1240 行，含 EEPROM 段與 BME688）、`camera/src/main.cpp`、`platformio.ini`、`README.md` 靜態分析撰寫，**源碼未被修改**（2026-10-04 解凍改動見文首增補說明）。任何閾值或 EEPROM 地址改動請同步本文。