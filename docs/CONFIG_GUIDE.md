# 配置指南（CONFIG_GUIDE）

> 本文档汇总本系统**所有可配置项**：引脚、PWM 参数、温度阈值、EEPROM 地址映射、调参说明、单位与硬约束。可独立阅读，涵盖主 ESP 与相机两固件。源码分析基础为只读模式，**未做任何修改**。
>
> 如果只想知道"我现在改这个值会发生什么、改之前要注意什么"，请直接跳到 §6 "调参导引"。
>
> **状态（2026-09-10 收尾锁版）：** 引脚/阈值/EEPROM 布局已按现行 `src/main.cpp`（1120 行）核对；代码已知问题集中在 [../LIMITATIONS.md](../LIMITATIONS.md) #34~#36，锁版冻结不再改代码。

---

## 目录

1. [引脚映射](#1-引脚映射)
2. [PWM / LEDC 参数](#2-pwm--ledc-参数)
3. [温度阈值与安全保护](#3-温度阈值与安全保护)
4. [EEPROM 地址映射](#4-eeprom-地址映射)
5. [WiFi 配置](#5-wifi-配置)
6. [调参导引](#6-调参导引)
7. [单位与硬约束速查](#7-单位与硬约束速查)
8. [前端 UI 约束 vs 后端 constrain 对齐表](#8-前端-ui-约束-vs-后端-constrain-对齐表)
9. [相机固件配置](#9-相机固件配置)
10. [常见误配置排查](#10-常见误配置排查)

---

## 1. 引脚映射

### 1.1 主 ESP（FireBeetle 2 ESP32-E，board `esp32dev`）

定义见 `src/main.cpp:13-23`。

| 信号 | 宏 | GPIO | FireBeetle 板标 | 类型 | 用途 |
|------|-----|------|------------------|------|------|
| FAN | `FAN_PIN` | 18 | SCK | LEDC ch0 PWM 输出 | 风扇 PWM |
| TEC EN | `TEC_EN` | 19 | MISO | 数字 OUT | H-bridge 使能，HIGH=ON |
| TEC LPWM | `TEC_LPWM` | 26 | D3 | LEDC ch1 PWM 输出 | 制冷方向（**线已对调**，详见 README 注意事项） |
| TEC RPWM | `TEC_RPWM` | 25 | D2 | LEDC ch2 PWM 输出 | 加热方向 |
| DS18B20 | `DS18B20_PIN` | 4 | **D12** | OneWire 双向 | 3 颗并联 + 4.7kΩ 上拉到 3.3V |
| LED 灯带 | `LED_PIN` | 27 | D4 | 数字/PWM OUT | **待装机**：经 IRLZ44N 驱动 12V COB 灯带（2026-09-10 锁版） |
| OLED SDA | — | 21 | SDA | I2C 双向 | SSD1306 数据 |
| OLED SCL | — | 22 | SCL | I2C 双向 | SSD1306 时钟 |
| BME688 | — | 21/22 | SDA/SCL | I2C 双向 | **待装机**：与 OLED 并线（同址 0x76/0x77 冲突时需改址） |

> ⚠️ **板标 D13=GPIO12 是 strapping pin**：接 4.7kΩ 上拉会把 ESP32 误认为 1.8V flash 电压，可能致砖。**OneWire 必须接 D12（GPIO4），切勿接 D13**。
>
> ⚠️ **D5/D8/D9 是 strapping pin**（GPIO0/5/2），上电瞬间电平被采样，勿用于 OneWire 或带上拉的关键 IO。

### 1.2 FireBeetle 2 ESP32-E D-pin ↔ GPIO 对照

主要 D-pin 与 GPIO **不一致**，源码中所有引脚号都是 GPIO 而非板标。常见映射：

| 板标 | GPIO | 可安全用？ |
|------|------|-----------|
| D2 | 25 | ✅ |
| D3 | 26 | ✅ |
| D4 | 27 | ✅（**LED 灯带**经 IRLZ44N） |
| D5 | 0 | ❌ strapping |
| D7 | 13 | ✅ |
| D8 | 5 | ❌ strapping |
| D9 | 2 | ❌ strapping（也是内置 LED） |
| D12 | **4** | ✅（**OneWire 用**） |
| D13 | 12 | ❌ strapping（关键陷阱） |

### 1.3 相机（DFR1154 ESP32-S3）

见 `camera/src/main.cpp:17-18, 202-218`。

| 信号 | 引脚 | 用途 |
|------|------|------|
| IR 灯控制 | GPIO47 | 数字 OUT，HIGH=ON |
| LED 白光控制 | GPIO3 | 数字 OUT，HIGH=ON |
| 摄像头 D0~D7 / XCLK / PCLK / VSYNC / HREF / SCCB | 16/18/21/17/14/7/6/4 + xclk=5 + pclk=15 + vsync=1 + href=2 + sda=8/scl=9 | OV2640 8-bit 并行数据 |

相机引脚固定，**不要改**——这些是 DFR1154 厂商硬连接的板内走线，改 `camera_config_t` 引脚会失配导致无图像。

---

## 2. PWM / LEDC 参数

定义见 `src/main.cpp:13-22`。

| 通道 | 宏 | 引脚 | 频率 | 分辨率 | 范围 | 写函数 |
|------|-----|------|------|--------|------|--------|
| ch0 | `FAN_CH` | GPIO18 | 25000 Hz | 10-bit (`PWM_RES`) | 0~1023 | `setFan(s)`，输入 0~255 然后 ×4 |
| ch1 | `TEC_L_CH` | GPIO26 | 25000 Hz | 10-bit (`TEC_PWM_RES`) | 0~1023 | `setTec(cool,heat)` / `setTecPwm(power,isCool)` |
| ch2 | `TEC_R_CH` | GPIO25 | 25000 Hz | 10-bit (`TEC_PWM_RES`) | 0~1023 | 同上 |

### 2.1 输入语义

| 函数 | 输入语义 | 实际 PWM 值 | 适用 |
|------|----------|------------|------|
| `setFan(int s)` | `s ∈ [0,255]`，会被 `constrain` 截断 | `ledcWrite(FAN_CH, s*4)` → 0~1020 | 风扇百分比 |
| `setTec(int cool, int heat)` | 8-bit 0~255 | `constrain(val*4,0,1023)` | 手动 / `/test` 端点 |
| `setTecPwm(float power, bool isCool)` | 0.0~1.0 归一化 | `(int)(power*1023)`，`constrain[0,1023]` | 自动 bang-bang |

### 2.2 关键衍生参数

| 项 | 宏 / 全局 | 值 | 位置 | 说明 |
|----|----------|----|----|----|
| EMA 滤波系数 | `EMA_ALPHA` | `0.5f` | `main.cpp:73` | 新读值占 50%，旧滤波值占 50%；越大越逼真但越噪 |
| 风扇延迟关闭时间 | `FAN_AFTERRUN_MS` | `10000` ms | `main.cpp:78` | TEC 关后保持 78% 风扇 10 秒 |
| 风扇延迟档速 | `FAN_AFTERRUN_SPEED` | `200` PWM | `main.cpp:79` | ≈78%（200/255） |
| 状态保存去抖 | `STATE_SAVE_DEBOUNCE_MS` | `750` ms | `main.cpp:65` | `/control` 写参数后 0.75s 内不再写 EEPROM |
| 看门狗超时 | `esp_task_wdt_init(7, true)` | **7 秒** | `main.cpp:933` | ⚠️ 旧注释写 3 秒，**实际是 7 秒**，以代码为准 |
| OneWire 转换等待 | `millis()-convStart >= 750` | 750 ms | `main.cpp:232` | DS18B20 12-bit 转换时间硬约束 |
| 感测周期 | `millis()-lastRead >= 2000` | 2000 ms | `main.cpp:1090` | loop 内触发；叠加 750ms 转换等待与调度开销，**实测约 4s**（见 ARCHITECTURE） |
| 重扫间隔 | `millis()-lastScan >= 10000` | 10 s | `main.cpp:1086` | 任一感测器缺失时才扫 |
| OLED 检查 | `millis()-lastOledCheck >= 5000` | 5 s | `main.cpp:1092` | 热插拔检查 |
| OLED 刷新 | `millis()-lastOled >= 2000` | 2 s | `main.cpp:1104` | 画面更新 |
| mDNS 重查 | `resolveInterval` | 5 s（未确认）/60 s（确认） | `main.cpp:1111` | 相机 IP 刷新 |

### 2.3 文档与代码不符处（风扇）

> ⚠️ README "溫控邏輯詳解" 写"製冷時風扇應以 PWM 50–52% 運行（實測最優）"，但 **`controlTemp()` 制冷分支实际写 `setFan(255)`（全速）**（`main.cpp:199-204`）。

实测发现 52% 才是最优，但**这部分代码未同步**（收尾锁版决定不修）。详见 [../LIMITATIONS.md](../LIMITATIONS.md) #34。若要同步：
- 把制冷分支 `setFan(255)` 改为 `setFan(round(255*0.52))`（≈133）或引入常量 `COOL_FAN_PCT=52`。
- 加热分支 `setFan(255)` 维持不变（全速合理）。
- 演示对策：网页手动把风扇设到 52%（`fanManual`）。

---

## 3. 温度阈值与安全保护

### 3.1 全局阈值变量

| 变量 | 默认值 | API 范围（constrain） | UI 滑条 min/max | 用途 | 位置 |
|------|--------|---------------------|----------------|------|------|
| `targetTemp` | `28.0` | `10.0 ~ 40.0` | 10/40/step 0.1 | 单目标温度 °C | `main.cpp:44` |
| `hysteresis` | `0.5` | `0.01 ~ 3.0` | 0.01/3/step 0.01 | 维持死区 ±hysteresis °C | `main.cpp:45` |
| `safeMin` | `5.0` | `0 ~ 20` | 0/20/step 0.5 | 巢穴最低安全温度（动物保护） | `main.cpp:68` |
| `safeMax` | `35.0` | `20 ~ 50` | 20/50/step 0.5 | 巢穴最高安全温度 | `main.cpp:69` |
| `ventMax` | `50.0` | `40 ~ 80` | 40/80/step 1 | 出风口断电阈值（硬件保护） | `main.cpp:70` |

> ⚠️ **`ventMax` 范围在 `handleControl()` 中是 `40~80`**（README 旧文本曾写 `30~80`，现已同步为 40~80）。在 `loadState()` 中 clamp 也是 `40.0~80.0`。请以**40~80 为准**。
>
> ⚠️ `safeMin` API 写 `0~20`、`safeMax` 写 `20~50`——两者**可能交叉**（例如 safeMin=18, safeMax=10），代码**未校验** safeMin < safeMax，会进入不安全区域。详见 [../LIMITATIONS.md](../LIMITATIONS.md) #36。

### 3.2 校准偏移

| 变量 | 默认 | API 范围 | 位置 | 用途 |
|------|------|----------|------|------|
| `nestOffset` | 0 | `-5.0 ~ 5.0` | `main.cpp:82`, 283-285 | 巢穴 DS18B20 校准 |
| `roomOffset` | 0 | `-5.0 ~ 5.0` | `82`, 287-291 | 活动区 |
| `ventOffset` | 0 | `-5.0 ~ 5.0` | `82`, 293-295 | 出风口 |

偏移在 `readSensor()` 中叠加到 raw 读值，参见 `main.cpp:253`。**对控制用滤波值和显示用原始值都生效**（同一 `raw[i] += offs` 后分别走滤波与 readTemps）。

### 3.3 阈值生效优先级

`controlTemp()` 五段保护顺序（**最高优先级在最前**）：

1. NAN 守卫（任一温度 NAN → nanCount++；≥3 次 → emergencyStop + saveState）
2. `ventT >= ventMax` → emergencyStop + saveState（不可逆，需手动重启）
3. `nestT < safeMin || nestT > safeMax` → 关 TEC + 风扇（255 / 60，根据高温或低温）
4. 热节流 `ventT > ventMax - 10` → `throttle = (ventMax - ventT)/10`，乘到 TEC 功率上
5. bang-bang：`targetTemp ± hysteresis` 死区外 → 全功率；死区内 → 关 TEC + 风扇延迟

**调参注意**：
- 想要更精细控制：缩小 `hysteresis`（如 0.1）。代价是死区边缘频繁切换。
- 想要更平缓：加大 `hysteresis`（如 1.0）。代价是稳态偏差更大。
- 切 PI 控制需在 `controlTemp()` 新增 `coolIntegral/heatIntegral/KI/maxRate` 等变量（本地开发笔记有逐步步骤，未随库发布）。

---

## 4. EEPROM 地址映射

主 ESP `EEPROM.begin(256)`，flash 模拟，**`EEPROM.commit()` 才真正写**。映射见 `main.cpp:373-419`。

| 地址偏移 | 长度 | 字段 | C++ 存取 | 写入时机 |
|----------|------|------|----------|---------|
| 0 | 1 byte | `0xAA` 有效标记 | `write(0,0xAA)` | 每次 saveState |
| 1 | 1 byte | `systemOn` (0/1) | `write(1,...)` | 系统开关；emergencyStop；
| 2 | 4 bytes | `targetTemp` (float) | `put(2,...)` | `/control` 写 targetTemp |
| 6 | 4 bytes | `hysteresis` (float) | `put(6,...)` | `/control` 写 hysteresis |
| 10 | 4 bytes | `safeMin` (float) | `put(10,...)` | `/control` |
| 14 | 4 bytes | `safeMax` (float) | `put(14,...)` | `/control` |
| 18 | 4 bytes | `ventMax` (float) | `put(18,...)` | `/control` |
| 22 | 4 bytes | `nestOffset` (float) | `put(22,...)` | `/control` 写 nestOff |
| 26 | 4 bytes | `roomOffset` (float) | `put(26,...)` | `/control` 写 roomOff |
| 30 | 4 bytes | `ventOffset` (float) | `put(30,...)` | `/control` 写 ventOff |
| 34 | 1 byte | `wifiMode` (0/1) | `write(34,...)` | `/control` 写 wifi |
| 35 | 33 bytes | `wifiSSID[33]` | `put(35,...)` | `/control` 写 ssid |
| 68 | 65 bytes | `wifiPass[65]` | `put(68,...)` | `/control` 写 pass |
| **68+65=133** | **已用 133 bytes**，余 123 字节空白 | — | — | — |

> 栈空间 `256` 足够覆盖 133 字节使用。新增字段请从 133 起排，**注意 EEPROM 物理扇区寿命**（约 10 万次写满额度的擦写）。`scheduleStateSave()` 750ms 的去抖正是为降低碎片化写入。

### 4.1 loadState 的安全逻辑

```text
if EEPROM[0] != 0xAA → 使用代码默认值，return
else 读全部字段
  · ventMax 越界 → clamp 到 [40, 80]        （main.cpp:403）
  · wifiMode 越界 (>1) → 设为 0              （main.cpp:408）
  · SSID 空 ('\0') 或 0xFF（未写flash）→ 用默认 "OPhone 12" / "qwer1234"
```

**注意**：`targetTemp/hysteresis/safeMin/safeMax/offset` 等浮点字段 **没有越界 clamp**——如果 flash 损坏填入畸形值，可能直接被加载用于控制。唯一例外是 `ventMax`。这是潜在风险，见 [../LIMITATIONS.md](../LIMITATIONS.md) #35（锁版不修，演示前确认设备正常即可）。

---

## 5. WiFi 配置

主 ESP 支持两种 WiFi 模式：

| 模式 | `wifiMode` | AP | STA | 设置命令 | 切换行为 |
|------|-----------|----|-----|----------|---------|
| 纯 AP（默认） | 0 | `ESP32-TEMP` / `12345678` (192.168.4.1) | ❌ | `POST /control?wifi=0` | `setWifiModeAndRestart(0)` → saveState+delay(200)+ESP.restart |
| STA+AP 备援 | 1 | 失败才开 `ESP32-TEMP` | ✅ 连 `wifiSSID/wifiPass` | `POST /control?wifi=1&ssid=XXX&pass=YYY` | 同上 |

STA 默认凭据：`OPhone 12` / `qwer1234`，当 EEPROM 中 SSID 为空或 0xFF 时回退。

相机固件无 EEPROM 配置，**帐密硬编码在 `camera/src/main.cpp:12-15`**：
- `AP_SSID = "OPhone 12"`、`AP_PASS = "qwer1234"`（首选）
- `FALLBACK_SSID = "ESP32-TEMP"`、`FALLBACK_PASS = "12345678"`（回退）

修改相机 WiFi 必须**重新烧录**。

### 5.1 主 mDNS 名称

| 名称 | 固件 | 说明 |
|------|------|------|
| `esp32-tec` | 主 ESP | `WiFi.setHostname("esp32-tec")`；但 `MDNS.begin()` 仅在 STA+AP 备援模式且 STA 成功时调用，纯 AP 模式下主 ESP **无 mDNS** |
| `esp32-cam` | 相机 | `MDNS.begin("esp32-cam")`（连接成功后执行）；主 ESP 通过 `MDNS.queryHost("esp32-cam")` 解析 |

---

## 6. 调参导引

### 6.1 我要改目标温度

**改法**：浏览器滑条 → 前端 `queueControl('targetTemp',...)` 300ms 防抖 → `POST /control?targetTemp=X` → `handleControl()` `constrain(10,40)` → `scheduleStateSave()` → `controlTemp()` 即时生效。

**边界**：
- API clamp `[10, 40]`；UI 输入框 `min=10 max=40 step=0.1`；slider 同。
- 若你想设 < 10 或 > 40，必须改 `handleControl` 的 constrain 才能写进 backend（前端会被 HTML 输入框挡住，但用 curl 直接发参数可绕过——backend 仍会截断）。
- 数值用 `toFloat()` 解析；传非数字 → `0.0f`，会被 clamp 到 10（**不是拒绝请求！**）。

### 6.2 我要改出风口断电阈值

`POST /control?ventMax=XX`，范围 **40~80**（API 文档旧文本写 30~80，**实际 40 起**）。
- 太低（接近环境温度）会误断电；太高失去保护意义。
- `loadState()` 会再 clamp 一次到 40~80，所以即使 EEPROM 损坏也不会出范围。

### 6.3 我要改校准偏移

`POST /control?nestOff=-0.3&roomOff=0&ventOff=0.2`，每项 `-5~5`。
生效时机：**下一次 `readSensor()` 读数时**（而非下一次 `controlTemp`），因为偏移在 `readSensor()` 中累加到 raw 读值后才进入滤波器。

如果断线时 `*(filtPtrs[i]) = NAN`，下次恢复时**首次读值直接设为 raw（不经过 EMA）**（`main.cpp:254-255`），所以重新校准后只需一次读数就能"重置滤波"，断线反而是恢复校准的契机。

### 6.4 我要换感测器

更新 `src/main.cpp:35-37` 的 `DeviceAddress` 常量（8 字节十六进制），重新烧录。换下来的旧 ROM 不会被清出 EEPROM（EEPROM 不存 ROM 位址，ROM 是代码常量）。

获取新 ROM：调试时看启动 Serial 的 `doScan()` 输出，`[?]` 标记的就是未识别的设备，后面跟的 16 位十六进制串即为 ROM。

### 6.5 我要改风扇最速制冷

实测冷端风扇 50-52% 反而出风口最低；**默认代码制冷分支写 255 是全速**。如要套实测结论，要么把 `setFan(255)` 改为 `setFan(133)`（≈52%），要么新增常量 `COOL_FAN_PCT`。

### 6.6 我要加新参数

最小改动路径：
1. 全局变量 + 默认值 + 宏范围。
2. `handleControl()` 加 `if (server.hasArg("xxx")) { xxx = constrain(server.arg("xxx").toFloat(), LO, HI); changed = true; }`。
3. `saveState()` 加 `EEPROM.put(ADDR, xxx);`，选 `ADDR ≥ 133` 的新地址。
4. `loadState()` 加 `EEPROM.get(ADDR, xxx);`，并加越界 clamp（**推荐效仿 ventMax 的处理**，避免 flash 损坏填畸形值）。
5. 前端 `INDEX[]` HTML 加 `<input type="range">` 与 `setXxx` 函数，加 `queueControl('xxx',...)`。
6. `/data` JSON 输出加字段。
7. `handleData()` `snprintf` 模板同步加。

---

## 7. 单位与硬约束速查

| 量 | 单位 | 合法范围 | 物理硬约束 | 软约束 |
|----|------|---------|----------|--------|
| DS18B20 温度 | °C | `-55 ~ +85` | 12-bit 分辨率 0.0625°C，转换 ≤750ms | 读值超出 -55~85 → 判 NAN |
| PWM 分辨率 | bit | 10 | LEDC 上限 20 bit，本系统取 10 | `setTec` 输入 8-bit 放大 4 倍 |
| FAN PWM 范围 | bit value | 0~1023 (内部) | — | `setFan(0~255)` → ×4 写到 10-bit |
| TEC PWM 范围 | bit value | 0~1023 | — | `setTecPwm(0~1)` → ×1023 |
| EEPROM 容量 | byte | 256 | flash 模拟 | 已用 133 byte |
| Serial baud | bps | 115200 | — | 见 platformio.ini |
| WiFi AP SSID | str | ≤32 字节 | IEEE 11 | `wifiSSID[33]` |
| WiFi 密码 | str | 8~63 字节 WPA2 | — | `wifiPass[65]` |
| H 网桥 EN 电压 | — | HIGH=ON | — | 默认 LOW（关 TEC） |

---

## 8. 前端 UI 约束 vs 后端 constrain 对齐表

> 此表对每个参数列出 **HTML 输入框 min/max/step** 与 **`handleControl()` 中 `constrain` 范围** 是否一致。**不一致项必须修正**

| 参数 | HTML `min/max/step` | 后端 `constrain` | 一致？ | 备注 |
|------|---------------------|------------------|--------|------|
| targetTemp | 10/40/0.1 | `(10, 40)` | ✅ | NaN 输入会被 clamp 到 10（非拒绝） |
| hysteresis | 0.01/3/0.01 | `(0.01, 3.0)` | ✅ | |
| safeMin | 0/20/0.5 | `(0, 20)` | ✅ | 与 safeMax 无交叉校验 |
| safeMax | 20/50/0.5 | `(20, 50)` | ✅ | 同上 |
| ventMax | **40**/80/1 | `(40, 80)` | ✅（与实际代码一致） | README 旧文本曾写 30，现已同步 |
| nestOff/roomOff/ventOff | —（无 UI，仅 API） | `(-5, 5)` | — | UI 无此滑条，靠 `prompt()` 或 curl |
| wifi | 0/1 | `toInt()`，无 constrain | ✅ | `loadState` 会把 `>1` reset |
| fanS（手动风扇） | 0/100/1 | `0~255`（×255/100 转换） | ✅ | UI 0-100% → API 0-255 |
| pollS（更新间隔） | 1/10/1 | — | — | 仅前端，不发后端 |

---

## 9. 相机固件配置

`camera/src/main.cpp` 几乎全部硬编码：

| 参数 | 值 | 位置 |
|------|-----|------|
| 主 WiFi SSID/PSK | `"OPhone 12"` / `"qwer1234"` | `:12-13` |
| 回退 WiFi SSID/PSK | `"ESP32-TEMP"` / `"12345678"` | `:14-15` |
| IR 引脚 | 47 | `:17` |
| LED 引脚 | 3 | `:18` |
| 串流端口 | 80 | `:126` |
| 控制端口 | 81 | `:145` |
| ctrl_port | 32769 | `:146`（与默认 32768 错开） |
| 最大并发客户端 | 4 | `:127, 147` |
| send_wait_timeout | 1 s | `:130` |
| 流无帧超时 | 10000 ms | `:48` |
| 帧大小 | VGA 640×480（无 PSRAM 时 QVGA） | `:227, :231` |
| JPEG quality | 16 | `:228` |
| xclk | 20 MHz | `:221` |
| fb_count | 2（PSRAM）/ 1（无 PSRAM） | `:225, :233` |
| grab_mode | `CAMERA_GRAB_LATEST` | `:223` |
| 传感器后处理 | `vflip=1, hmirror=0, brightness=1, contrast=1, exposure_ctrl=1, agc_gain=0, gain_ctrl=1, gainceiling=2, ae_level=1` | `:243-255` |

**这些参数都没有 UI 调整入口**，要改只能重新烧录。

---

## 10. 常见误配置排查

| 现象 | 可能原因 | 检查方向 |
|------|----------|---------|
| 上电 OLED 显示但 Serial 无 DS18B20 就绪 | OneWire 接到 D13（GPIO12）strapping，flash 电压读错变砖边缘 | 切到 D12（GPIO4），换前先做factory 检查 |
| `deviceCount=0` | WiFi 中断抢占 OneWire 时隙（`doScan` 在 `WiFi.softAP` 之后跑） | 确认 `doScan()` 在 `WiFi.mode(...)` 之前调用 |
| 制冷方向反了 | TEC 线被对调但代码认为原方向 | 看 `/test?cool=255&heat=0&en=1` 实际冷热面对应 |
| `ventMax=30` API 被截到 40 | API 实际下限 40（README 旧文本误） | 改为 40 或更高 |
| 设 0.001 hysteresis 看似有效但 TEC 频繁启停 | 死区太小 | 写日志看切换频率，调到 0.1~0.5 |
| 误关浏览页时数据全失 | `allData` 仅在浏览器内存 | 已知缺陷，未来需 SD 卡/SPIFFS |
| 重启后参数丢失 | `EEPROM.read(0) != 0xAA`，或写时未 `commit()` | 看启动 Serial `[EEPROM]` 行 |
| 切到 STA 模式失败 | SSID/密码错或对端非 2.4GHz | 部分手机热点 5GHz 不兼容；切回 AP 用 `/control?wifi=0` |
| 相机一直加载失败 | mDNS 未解析 + IP 是 192.168.4.2 但相机不在线 | STA 模式下检查 `esp32-cam.local`；纯 AP 下相机会连不上 |

---

> 本文档基于 `src/main.cpp`（1120 行，含 EEPROM 段）、`camera/src/main.cpp`、`platformio.ini`、`README.md` 静态分析撰写，**源码未被修改**。任何阈值或 EEPROM 地址改动请同步本文。