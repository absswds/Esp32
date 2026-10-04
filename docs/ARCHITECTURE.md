# 架构文档（ARCHITECTURE）

> 本文档对 ESP32 TEC 蟄眠實驗溫控系統做完整的架构层面梳理，涵盖两个独立固件目标的模块划分、数据流、状态机与并发模型。可独立阅读，无需先读其他文档。
>
> 源码分析基础（只读，未做任何修改）：`src/main.cpp`（1240 行，主 ESP，含 2026-09-19 `abc8f9a` BME688 接入；**2026-10-04 解冻后约 1340 行**：换装 BME680、1 Hz 读取、防结露警示 `condRisk`、断电自动恢复——相关小节已更新，其余行号仍为 09-26 版本，请先 grep）、`camera/src/main.cpp`（275 行，相机）、`platformio.ini` / `camera/platformio.ini`。行号于 2026-09-26 按当前版本重新核对。

---

## 目录

1. [系统全景](#1-系统全景)
2. [两固件边界](#2-两固件边界)
3. [主 ESP 模块划分](#3-主-esp-模块划分)
4. [相机固件模块划分](#4-相机固件模块划分)
5. [数据流](#5-数据流)
6. [状态机](#6-状态机)
7. [并发与实时性模型](#7-并发与实时性模型)
8. [存储与持久化](#8-存储与持久化)
9. [故障域与降级路径](#9-故障域与降级路径)
10. [已知架构限制](#10-已知架构限制)

---

## 1. 系统全景

本系统由**两台完全独立的 ESP32 固件**组成，无直接电气或总线连接，仅通过 IP 网络松耦合（相机优先连手机热点 `OPhone 12`，连不上 10 秒后回退连主 ESP 的 `ESP32-TEMP` AP；主 ESP 在 STA+AP 备援模式下与相机同处手机热点，纯 AP 模式下相机经回退也能连到主 ESP 的 AP）。

```text
┌─────────────── 硬件层 ───────────────┐   ┌──────────── 网络层 ───────────┐   ┌─── 客户端 ───┐
│ 3× DS18B20 (OneWire, GPIO4)        │   │ 主 ESP WiFi                   │   │ 浏览器 SPA   │
│ TEC H-bridge (TEC_EN/LPWM/RPWM)    │◀▶│  · 纯 AP: ESP32-TEMP          │◀▶│ (仪表板)      │
│ FAN PWM (GPIO18, LEDC ch0)         │   │  · STA+AP 备援: 连指定 WiFi   │   └──────────────┘
│ SSD1306 OLED (I2C 21/22, 0x3C)     │   │ HTTP server port 80           │
│ BME680 (同 I2C, 0x76/0x77, 仅监测) │   └───────────────────────────────┘
│ EEPROM (flash 模拟, 256B)         │
└─────────────────────────────────────┘            ▲
                   ▲                                │ mDNS esp32-cam
                   │ 控制信号                        │ (仅 STA 模式可解析)
                   │                                ▼
┌─────────────── 主 ESP 固件 src/main.cpp ────────────────────────────┐
│ setup() → loop() 单线程 Arduino 调度                                  │
│  · 感测层 readSensor()/controlTemp()  · HTTP 控制层 /control /test  │
│  · 安全层 emergencyStop()/NAN 计数守卫 · 显示层 updateOLED()         │
│  · 环境监测 bmeInit()/bmeTick()（BME680，不参与控制）                │
└──────────────────────────────────────────────────────────────────────┘

┌──────── 相机固件 camera/src/main.cpp（独立第二个 ESP32-S3）─────────┐
│ WiFi STA（连手机热点 OPhone 12，失败回退 ESP32-TEMP AP）             │
│ 双 httpd server: port 80 串流 / port 81 控制（IR/LED）               │
│ 每个 httpd 实例只有 1 个 server task（串流占住 80 口时 80 口不接新请求）│
└──────────────────────────────────────────────────────────────────────┘
```

**关键点：** 两台 ESP32 **不共享任何硬件资源**。相机 IR/LED 在浏览器中由前端**直接发请求到相机 81 端口**（`toggleIR()`/`toggleLED()`）。前端只在 `camIP` 为空时才退回主 ESP 的 `/light` 代理（见 `src/main.cpp:1132-1161`），但 `/data` 恒返回 `camIP`（未解析到时为默认 `192.168.4.2`），所以**实际上前端永远直连、代理路径走不到**；且该代理连的是相机 **80 口**，而 `/light` 注册在 81 口，本身也是坏的（见 CLAUDE.md 相机章节 / LIMITATIONS #34–36 所记代码问题）。串流与控制分端口，是为了避免串流占住 80 口导致灯光无反应。

---

## 2. 两固件边界

| 维度 | 主 ESP（TEC 溫控） | 相机（行为观测） |
|------|---------------------|-----------------|
| 板型 | `board = esp32dev`（实物为 FireBeetle 2 ESP32-E；`platformio.ini` 未指定专用 variant） | DFRobot DFR1154 ESP32-S3 (`dfrobot_dfr1154`)，16MB Flash + 8MB OPI PSRAM |
| 主源文件 | `src/main.cpp`（1240 行） | `camera/src/main.cpp`（275 行） |
| 框架 | Arduino + Arduino-ESP32 core | Arduino + Arduino-ESP32 core + esp32-camera |
| WiFi 角色 | 默认 AP（`ESP32-TEMP` / 192.168.4.1）；可切 STA+AP 备援 | STA（连 `OPhone 12` 手机热点，失败回退 `ESP32-TEMP`） |
| HTTP | 单 `WebServer` (port 80)，`loop()` 内 `handleClient()` 同步分派 | 双 `httpd` (port 80 串流 / port 81 控制)，每实例 1 个 server task |
| 持久状态 | EEPROM 256B（save/loadState） | 无 |
| 任务并发 | 单 `loop()` 主任务 + 看门狗；WiFi/LWIP 协议栈由 core 内部 task 承载，HTTP handler 在 `loop()` 中执行 | 主 loop 仅重连 WiFi；串流、控制各由自己 httpd 实例的 server task 执行 |
| 构建命令 | `pio run` | `cd camera && pio run -e esp32s3cam`（独立 PlatformIO 项目，根目录执行会报 `UnknownEnvNamesError`） |

两固件**没有任何源码共享**（无 `include/` 公共头、无 `lib/` 共用库，两者目前均只有 PlatformIO 模板 README）。换感测器或调阈值需重刷主 ESP；相机配置（WiFi 帐密、引脚）则硬编码在 `camera/src/main.cpp` 顶部。

---

## 3. 主 ESP 模块划分

`src/main.cpp` 是单文件 Arduino sketch，按职责可在阅读上划分为 7 个模块（**代码本身没有物理拆分**，所有函数与全局都在同一编译单元，靠注释分节组织）。下表给出每个模块的入口、关键函数、依赖与产出。

### 3.1 全局配置与常量（`main.cpp:14-101`）

| 项 | 值 | 备注 |
|----|----|------|
| `FAN_PIN=18`, `TEC_EN=19`, `TEC_LPWM=26`, `TEC_RPWM=25`, `DS18B20_PIN=4` | GPIO 编号 | FireBeetle 板标 D-pin ≠ GPIO，详见 CONFIG_GUIDE |
| `PWM_FREQ=25000` Hz, `PWM_RES=10`, `TEC_PWM_RES=10` | LEDC 配置 | 风扇与 TEC 一致均 10-bit |
| `FAN_CH=0`, `TEC_L_CH=1`, `TEC_R_CH=2` | LEDC 通道分配 | 三路独立 |
| `EMA_ALPHA=0.5f` | 温度滤波系数 | 指数移动平均（EMA） |
| `FAN_AFTERRUN_MS=10000`, `FAN_AFTERRUN_SPEED=200` | 风扇延迟关闭 | ~78% |
| `STATE_SAVE_DEBOUNCE_MS=750` | 状态保存去抖 | 避免抖动频繁写 EEPROM |
| DS18B20 ROM 常数 `nestAddr/roomAddr/ventAddr` | 8 字节 DeviceAddress | ROM 出厂位址认人 |
| 安全阈值默认 `safeMin=5.0`, `safeMax=35.0`, `ventMax=50.0` | °C | 可被 `/control` 覆盖并持久化 |
| `BME_INTERVAL_MS=1000`, `BME_GAS_EVERY=10`, `BME_BURNIN_MS=600000`, `BME_GAS_ALPHA=0.02f` | BME680 温湿压读取周期（1 Hz）/ 每 N 次量一次 Gas / Gas 烧机期 / Gas 基线 EMA | 见 3.7 |
| `COND_ON_MARGIN=1.0f`, `COND_OFF_MARGIN=2.0f`, `COND_STALE_MS=30000` | 防结露警示进入/退出余量（°C，相对露点）/ BME 数据过期判定 | 见 3.7 |

### 3.2 感测层（`main.cpp:316-362, 992-1037`）

- `doScan()` — OneWire/Dallas 总线扫描，按 ROM 比对给三颗感测器赋角色；处理 `cnt==0` 回退路径并打印 raw ROM 便于排错。
- `readSensor()` — **非阻塞转换状态机**：先 `requestTemperatures()` 立即返回置 `convPending=true`；下次 loop 迭代等到 `millis()-convStart >= 750` 才真正读 3 个温度。读后做范围合法化（`-55~85°C` 外判 NAN；注意判据是 `> 85.0`，**85.0 本身被接受**，而 85°C 正是 DS18B20 的上电复位值，见 LIMITATIONS #37）、校准偏移叠加、EMA 滤波，最后写入 `nestT/roomT/ventT`（滤波值，`/data` 输出的就是它）和 `readTemps[]`（偏移后未滤波值，目前没有任何接口对外输出），并调用 `controlTemp()` 闭环。
- 断线值：`getTempC(addr)` 读不到时返回 `DEVICE_DISCONNECTED_C`（-127），代码里 `== DEVICE_DISCONNECTED_F` 的比较不会命中，实际靠 `< -55` 下限判 NAN。
- `sameAddr()` — 8 字节逐位比较，ROM 识别基础。

**输入：DS18B20 OneWire 总线；输出：3 个滤波后温度 + raw 数组。**

### 3.3 控制层（`main.cpp:109-238`）

控制层是**单点真相来源** `controlTemp()`，被 `readSensor()` 和 `handleControl()` 两处调用以立即生效新阈值。

- `setFan(int s)` — 风扇占空比 0-255 → `ledcWrite` 10-bit（×4）。
- `setTec(int cool, int heat)` — **手动 / `/test` 通道**：8-bit 0-255 放大 4 倍写到 TEC_L/R 通道，同步置 `cooling/heating` 标志。
- `setTecPwm(float power, bool isCool)` — **自动 bang-bang 通道**：归一化 0-1.0 → 0-1023 PWM，单向驱动（另一方向写 0）。
- `controlTemp()` — 安全 + bang-bang 五段式决策（详见 [6.1](#61-controltemp-状态机)）。
- `emergencyStop()` / `stopAll()` / `startAll()` — 系统级状态切换，含风扇延迟逻辑。**`TEC_EN` 只在 `startAll()`（及 `/test?en=1`）中拉高**；`setup()` 先把它拉低，2026-10-04 起若 `loadState()` 恢复出 `systemOn=true`，`setup()` 末尾会自动调用 `startAll()`（断电自动恢复，LIMITATIONS #31，未上机验证）。`emergencyStop()` 已把 `systemOn=false` 存入 EEPROM，所以紧急停机不会被重启复活。

### 3.4 网络层（`main.cpp:364-555, 1126-1181`）

- `WebServer server(80)` — 5 个路由 + 2 个 inline lambda 路由（共 7 个）：
  - `GET /` → `handleRoot()` 返回 `PROGMEM` 内嵌的 SPA HTML（`main.cpp:557-935`，约 380 行 inline HTML+CSS+JS）。
  - `GET /data` → `handleData()` JSON 状态快照（温度为滤波值；含 `bmeOk/bmeT/bmeH/bmeP/bmeGas/bmeGasRel/bmeDP`，BME 不在线时为 `null`）。
  - `POST /control` → `handleControl()` 参数写入 + `scheduleStateSave()` / `saveState()`，再 `controlTemp()` 即时生效。
  - `GET /test` → `handleTest()` 手动 PWM 测试，置 `tecManual/fanManual` 标志。
  - `GET /diag` → `handleDiag()` 诊断 JSON。
  - `GET /light`（inline） → 设计上是相机不可直连时的代理；乐观更新本地 `lightIR/lightLED` 缓存，再尽最大努力同步到相机 `camIP:80`。**已知缺陷**：相机的 `/light` 在 81 口，连 80 口会 404；且前端总拿得到 `camIP`，实际不会走这条路径。
  - `GET /camenable`（inline） → 切换 `camEnabled` 标志并回 JSON。

### 3.5 显示层（`main.cpp:941-983, 1212-1224`）

- `updateOLED()` — U8g2 `firstPage()/nextPage()` 分页绘制，6 段布局（巢穴/活动/出风口温度 + 状态 + 风扇% + 目标 + 模式），每 2 秒刷新。
- 主循环每 5 秒 `Wire.beginTransmission(0x3C)` 探测，OLED 断线自动 `u8g2.begin()` 重初始化（热插拔恢复，commit `e9c6829`）。

### 3.6 持久化层（`main.cpp:469-527`）

- `scheduleStateSave()` — 去抖调度：设 `stateSavePending=true`+`stateSaveDue`。
- `saveState()` — 写 EEPROM 256B（详细地址见 CONFIG_GUIDE）。
- `loadState()` — 上电恢复，含 `0xAA` 有效标记、`ventMax` 越界 clamp、`wifiMode>1` reset、SSID 0xFF 兼容默认回退。
- `setWifiModeAndRestart(mode)` — `saveState()` + `delay(200)` + `ESP.restart()`，用于 WiFi 模式切换。

### 3.7 环境监测层 BME680（`main.cpp:48-73, 263-359`，2026-09-19 `abc8f9a` 接入，2026-10-04 改 1 Hz + 防结露警示）

- `bmeInit(quiet)` — 在 `setup()` 中 `Wire.begin(21,22)` 之后、`u8g2.begin()` 之前调用；依次探测 0x76 / 0x77，设过采样、IIR 与气体加热器 320°C/150ms。用 Adafruit BME680 驱动（实际装的就是 BME680），**未用 BSEC**，因此没有 IAQ / eCO₂，只有气体电阻。
- `bmeTick()` — 每次 `loop()` 调用的非阻塞状态机：`beginReading()` 记下完成时刻，到点才 `endReading()` 取值；每 1 s（`BME_INTERVAL_MS`）读一次温湿压，加热器平时关闭（`setGasHeater(0,0)`），每 `BME_GAS_EVERY`=10 次才开加热器量一次 Gas（即 10 s 一次，降低自热）；成功读取时更新 `lastBmeOk`。连续 3 次 `beginReading` 失败标记离线，离线后每 60 s 重试 `bmeInit(true)`（可热插拔）；单次 `endReading` 失败沿用上一笔。
- 派生量：露点（Magnus 公式）；`bmeGasRel` = 气体电阻 ÷ 慢速 EMA 基线（`BME_GAS_ALPHA=0.02`）×100%。上电前 10 分钟（`BME_BURNIN_MS`）为燒機期，Gas 值不应引用。
- 防结露警示：`updateCondRisk()` 每次 `loop()` 调用，用纯函数 `condRiskNext(prev, ventT, bmeDP)` 做迟滞——出风口 ≤ 露点 + `COND_ON_MARGIN`(1°C) 进入、> 露点 + `COND_OFF_MARGIN`(2°C) 退出；BME 离线或数据超过 `COND_STALE_MS`(30 s) 未更新时强制为 false。**仅警示，不影响 `controlTemp()`**。出风口气温只是冷端表面的代理量（冷端表面更冷），警示可能偏晚。
- 输出：`/data` 的 `bmeOk/bmeT/bmeH/bmeP/bmeGas/bmeGasRel/bmeDP/condRisk` 字段；网页 BME 面板 + 结露风险横幅；OLED Vent 行 `DEW` 标记；CSV 后 6 列。
- **与控制完全解耦**：不参与 `controlTemp()`、阈值、安全逻辑，也不写 EEPROM。

---

## 4. 相机固件模块划分

`camera/src/main.cpp` 极简，按职责可分 3 块：

| 模块 | 函数 | 位置 | 说明 |
|------|------|------|------|
| 串流服务 | `stream_handler()` | `camera/src/main.cpp:30-78` | MJPEG multipart 帧循环；每帧前用 `send(fd, &probe, 0, MSG_DONTWAIT)` 探测客户端是否断开；10 秒无帧发出超时退出；**始终 `esp_camera_fb_return(fb)`** 防帧缓存泄漏 |
| 快照服务 | `capture_handler()` | `82-91` | 单帧 JPEG 抓取，用于不发 multipart 的客户端 |
| 控制 / 状态 | `light_handler()` / `status_handler()` | `95-119` | IR/LED GPIO 控制与状态 JSON；GET query 参数解析 |
| 启动 | `startCameraServer()` | `123-161` | 起 2 个 `httpd` 实例，stream(80) 与 control(81)；`send_wait_timeout=1` 避免慢客户端阻塞 5 秒；`ctrl_port=32769` 与默认 32768 错开 |
| WiFi | `connectWiFi()` | `163-193` | 两级回退：`OPhone 12` → `ESP32-TEMP`；连接成功后 `(re)startCameraServer()` + `MDNS.begin("esp32-cam")`（重连时只 `httpd_stop` 了串流 server，控制 server 未停，再次启动会打印 "Control server failed!"，旧实例仍在工作） |
| setup / loop | `setup()` / `loop()` | `197-275` | camera_config 全字段硬编码；PSRAM 缺失回退 QVGA DRAM；`set_vflip/hmirror/brightness/contrast` 等传感器调参写死；主 loop 仅监测 WiFi 掉线重连 |

### 4.1 传感器配置（`camera/src/main.cpp:202-228`）

VGA 640×480 / JPEG quality 16 / `CAMERA_GRAB_LATEST` / `fb_count=2` / `fb_location=PSRAM` / `xclk=20MHz`。
**无 PSRAM 时** (`!psramFound()`) 回退 DRAM + 单帧缓冲，避免 DMA 崩溃（这是 README 反复强调"必须用 `dfrobot_dfr1154` 板型"的根本原因）。注意：代码里的 QVGA 回退实际无效——`esp_camera_init()` 之后无条件 `s->set_framesize(s, FRAMESIZE_VGA)` 又改回了 VGA，只有 `fb_location`/`fb_count` 的回退生效（锁版未改）。

---

## 5. 数据流

### 5.1 主 ESP 主回路数据流

```text
            ┌──────────── OneWire GPIO4 ───────────┐
            │ 3× DS18B20 (ROM 识别)                │
            └──────────────┬─────────────────────────┘
                           │ requestTemperatures (750ms 非阻塞)
                           ▼
        ┌────────── readSensor() ──────────┐
        │ 范围合法化 → 偏移 → EMA 滤波       │
        └──────────────┬─────────────────────┘
                       │ nestT/roomT/ventT (全局)
                       ▼
        ┌────────── controlTemp() ─────────┐  ◀── http 触发（仅 /control；/test 不调用它）
        │ 5 段安全 + bang-bang 决策          │
        └──────┬───────────────┬────────────┘
   制冷/加热    │               │ 风扇
               ▼               ▼
        ledcWrite(TEC_L/R)  ledcWrite(FAN)
        digitalWrite(TEC_EN)

        旁路输出：
        ┌────────── bmeTick()（BME680，1 Hz）──┐ →  bmeT/H/P/Gas/DP，只进 /data 与 CSV，不进控制
        ┌────────── /data JSON ──────────┐  →  浏览器仪表板
        └────────── updateOLED() ────────┘  →  SSD1306
        └────────── EEPROM saveState() ──┘  →  flash 持久化
```

### 5.2 HTTP 控制流

- **写参数**：浏览器 `fetch('/control?...',{method:'POST'})` → `handleControl()` 把参数写入全局变量 → `scheduleStateSave()`（750ms 去抖）→ `controlTemp()` 即时生效。
- **系统开关**：`handleControl("system=1")` → `startAll()`（同步置 `systemOn=true`、复位 `nanCount=0`、关闭手动态）→ 立即 `saveState()`（不走去抖，确保断电不丢）。
- **手动 PWM 测试**：`/test?cool=..&heat=..&en=..&fan=..` → 直接写 PWM。带 `cool`/`heat` 时置 `tecManual`，之后 `controlTemp()` 直接 return（被 `if(!systemOn||tecManual||manualMode) return` 拦截，**所有保护一并跳过**）；只带 `fan` 时只置 `fanManual`，TEC 仍自动控制、风扇保持手动值直到停止/重开系统（52% 用法即此）。网页"製冷/加熱"小按钮 = `/control?manual=1` + `/test?cool|heat=200&en=1`（TEC 约 78%）。

### 5.3 浏览器 ↔ 相机 直连流

```text
浏览器                        主 ESP /camenable         相机 (port 80/81)
   │  GET /data camEnabled=true ────────▶                   │
   │  ◀── camIP (mDNS esp32-cam 解析) ──────────────────── │
   │                                                       │
   │  ──── <img src=http://camIP/stream> ─────────────────▶(port 80 MJPEG)
   │  ──── fetch http://camIP:81/light?ir=.. ─────────────▶(port 81 控制)
   │   （仅当 camIP 为空才退到主 ESP /light 代理；但 /data 恒返回 camIP
   │    ——未解析时为默认 192.168.4.2——所以实际永远直连，代理不会被用到）
```

纯 AP 模式下主 ESP 不运行 `MDNS.begin()`，`camIP` 保持默认 `192.168.4.2`。相机回退连入 `ESP32-TEMP` 时通常拿到 .2，但若手机/电脑先连上 AP 占用了 .2，相机会拿到别的地址而网页仍去连 .2（**推断**，未实测）。

### 5.4 显示流

主回路中 OLED 每 2 秒读全局温度 + 状态绘制；浏览器侧每 1 秒（串流时 5 秒）轮询 `/data` 推入 `H`（600 滑动窗口）和 `allData`（10000 上限）数组，Canvas 重绘趋势图。注意：温度值本身约每 4 秒才更新一次，CSV 中相邻行多为重复值；网页"更新頻率"滑条会在下一次轮询被 `ms=camEnabled?5000:1000` 覆盖，实际无效（前端已知缺陷）。

---

## 6. 状态机

### 6.1 `controlTemp()` 状态机

这是全系统的**核心决策机**，被 `readSensor()`（每完成一次读数，约 4 秒一次）和 `handleControl()`（每次 `/control` 请求）调用。返回前会被下列前置 guard 之一拦截：

```text
guard 0  systemOn==false || tecManual || manualMode  → return（不动作；手动模式下以下所有保护都不执行）
guard 1  isnan(nestT)||isnan(roomT)||isnan(ventT)    → nanCount++
            └ nanCount>=3 → emergencyStop()+saveState()+return（连续 3 次读数 ≈ 12 s）
            └ 否则 return（容忍 1~2 次瞬态跳变）
guard 2  ventT >= ventMax (默认 50°C)                → emergencyStop()+saveState()+return
guard 3  nestT < safeMin || nestT > safeMax           → setTecPwm(0)+风扇 255(过热) 或 60≈24%(过冷) +return
─────────────────────────────────────────────────────────
热节流   if ventT > ventMax-10 → throttle = (ventMax-ventT)/10  ∈[0,1]
决策    nestT > targetTemp+hysteresis → setTecPwm(throttle,true) + setFan(255)
        nestT < targetTemp-hysteresis → setTecPwm(throttle,false)+ setFan(255)
        else（在死区内）→ setTecPwm(0) + 风扇 after-run 延迟逻辑
```

**重要：** 处于死区时如检测到刚从制冷/加热切出（`cooling||heating` 仍为 true），启动 `fanAfterRunTimer`；之后 10 秒内每帧保持 `FAN_AFTERRUN_SPEED=200`(~78%)，超过 10 秒关扇。

> ⚠️ 注意 README 提到"製冷時風扇應以 52% 運行"的實測發現，但**當前 `controlTemp()` 中製冷分支仍寫 `setFan(255)` 全速**（`main.cpp:215-220`）——這是已知的代碼與文檔偏差，鎖版凍結不改，演示時手動設 52%，詳 [LIMITATIONS.md](../LIMITATIONS.md) #34 與 [CONFIG_GUIDE.md](CONFIG_GUIDE.md) §2.3。

### 6.2 风扇 after-run 状态机（`loop()` 内独立）

```text
状态：fanAfterRunTimer == 0      → 不在 after-run
状态：fanAfterRunTimer > 0 且 !fanManual
   ├─ millis()-timer < 10s → setFan(200)
   └─ millis()-timer >= 10s → setFan(0) + 重置 timer=0
```

启动点：`controlTemp()` 进入死区且检测到刚切出冷/热；`stopAll()` 在 `fanSpeed>0` 时也置 `fanAfterRunTimer = millis()`。`startAll()` 会清零 timer 取消延迟。

### 6.3 非阻塞感测状态机（`readSensor()`）

```text
convPending=false → requestTemperatures(); convPending=true; convStart=millis(); return
convPending=true  → if millis()-convStart < 750 return   (还未到)
                  → else 读 3 温度、滤波、controlTemp(); convPending=false
```

由 `loop()` 每 2 秒触发一次 `readSensor()`：第一次调用只发起转换就返回，下一次（2 秒后）才读取——所以**一次完整读数约 4 s**（旧文档写的 2.75 s 有误）。这是隐含的采样率限制，也是 NAN 连续 3 次 ≈ 12 s 的原因。CSV 实测：P1/P2 温度约每 4 s 变一次，P3 约每 5 s。

### 6.4 NAN 守卫：实际只有一道

`loop()` 顶部有一行看似"后备"的守卫：

```cpp
if (systemOn && sensorInit && nanCount >= 3) { emergencyStop(); saveState(); }
```

但 `nanCount` **只在 `controlTemp()` 内递增**，而且 `controlTemp()` 在达到 3 时已经自己调用了 `emergencyStop()`。因此这行守卫**不构成第二道保险**：

- 手动模式（`tecManual/manualMode`）下 `controlTemp()` 第一行就 return，`nanCount` 根本不会递增，loop 守卫也永远不会触发——**手动模式下没有任何断线保护**（LIMITATIONS #8）。
- `!dsOk` 时 `readSensor()` 第一行就 return，同样不会递增 `nanCount`。

**断线缺口的实际范围**（LIMITATIONS #36）：`dsOk` 只在 `doScan()` 里改变，而 `doScan()` 只在已有探头缺失时才每 10 秒重跑。
- 开机三颗都识别到、运行中总线整体断开 → `dsOk` 仍为 true，`getTempC()` 返回 -127 → 被 `< -55` 判 NAN → 正常走 nanCount → 约 12 s 急停。✅
- 已有探头缺失、重扫时发现 **0 个设备** → `dsOk=false` → `readSensor()` 直接 return → `nanCount` 不再增长（只有 `/control` 请求触发的 `controlTemp()` 还会让它 +1）→ **不会急停**，TEC 保持上次状态。❌

### 6.5 EEPROM 保存状态机

```text
触发  → stateSavePending=true; stateSaveDue=millis()+750
loop 中 if pending && (long)(millis()-due)>=0 → saveState(); pending=false
例外：/control 带 system 参数时、以及紧急停机的两个调用点（controlTemp 内 / loop 守卫）
      会直接调 saveState() 走立即路径（不经去抖）；emergencyStop() 本身不调 saveState()
```

---

## 7. 并发与实时性模型

### 7.1 主 ESP：单 loop 主任务 + Arduino-ESP32 后端 task

主 ESP 没有显式多任务。所有控制、感测、HTTP、显示都在 `loop()` 串行执行。Arduino-ESP32 core 在背后运行 WiFi stack、LWIP、mDNS 等内部 task，但 **HTTP handler 不在这些 task 里执行**：`loop()` 调 `server.handleClient()`，由 `WebServer` 在 loop 任务内同步分派到 `handleControl()` 等 user handler。因此 handler 与 `readSensor()`/`controlTemp()` 不会并发，不存在数据竞争。

**关键的时序要求：**
- `loop()` 顶部 `esp_task_wdt_reset()` 必须在 **7 秒**内执行（`esp_task_wdt_init(7, true)`，`main.cpp:1050`；紧邻的源码注释仍写"3 秒"，已过时，以代码 7 秒为准）。
- OneWire 时序由 `paulstoffregen/OneWire` 库用 `micros()` 软件循环保证，对 WiFi 中断极敏感。基于此 README 强调"`doScan()` 必须 `WiFi.softAP()` 之前执行"。
- HTTP 处理是非阻塞的，单个请求不应 >100ms，否则后续 IO 会被卡。`handleLight` 中的相机代理同步连接最长等 2 秒，可能影响响应性（用 `esp_task_wdt_reset()` 中间喂狗避免重启）。

### 7.2 相机：每个 httpd 实例一个 server task

ESP-IDF `esp_http_server` 的每个实例只有**一个 server task**，在它的循环里轮询监听 socket 与控制 socket，并在该 task 内调用 URI handler（[官方文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_http_server.html)）。`stream_handler()` 是 `while(true)` 阻塞式，直到客户端断开或 10 秒无帧超时才返回——**串流进行期间，80 口这个实例不能再服务第二路 `/stream` 或 `/capture`**。控制端点放在 81 口的独立实例（独立 server task、`ctrl_port=32769`）正是为了不被串流卡住。`max_open_sockets=4` 只是允许同时保持 4 个连接，**不等于能同时推 4 路流**（相机固件第 2 行"Each client connection gets its own task"的注释是错的）。

`capture_handler()` 与 `stream_handler()` 在同一实例，所以串流期间 `/capture` 会排队等待，而不是并发抢帧。

### 7.3 跨固件松耦合

主 ESP 与相机之间**无 RPC 心跳、无同步锁**。相机掉线仅靠：
- 主 ESP 主循环每 5s~60s `MDNS.queryHost("esp32-cam")` 重新解析（仅 STA+AP 模式下成立）。
- 浏览器侧 `camErr()` 重试 2 秒后 `camPoll()` 重连。

---

## 8. 存储与持久化

| 介质 | 内容 | 容量 | 接口 |
|------|------|------|------|
| 主 ESP EEPROM（flash 模拟） | 系统状态 + 安全阈值 + 校准偏移 + WiFi 配置 | 256 字节映射，已用 0–132（共 133 字节） | `EEPROM.begin(256)` / `put` / `get` / `commit` |
| 主 ESP PROGMEM | SPA 仪表板 HTML/CSS/JS | ~12KB（INDEX[] PROGMEM R"HTML(...)"） | `server.send_P` |
| 浏览器内存 | `H[]` 600 + `allData[]` 10000 | 单条 3 温度 + 风扇/状态 + 6 个 BME 字段 + 时间戳，约 1–2 MB 上限 | JS 全局变量 |
| 相机 | 摄像头帧缓冲 PSRAM | 2 个 VGA 帧 + JPEG 中间缓冲 | `esp_camera_fb_get/return` |
| 相机 | WiFi 帐密 | 硬编码字符串常量 | 无持久化 |

EEPROM 详细地址映射见 [CONFIG_GUIDE.md]。

---

## 9. 故障域与降级路径

下表列每个潜在故障点、当前处置与残余风险，便于运维时定位问题域。

| # | 故障 | 检测位置 | 当前处置 | 残余风险 |
|---|------|----------|----------|----------|
| F1 | 任一 DS18B20 断线（NAN） | `controlTemp()` guard1（loop 那行守卫不构成第二道，见 §6.4） | 连续 3 次读数 NAN（约 12 s）紧急断电 | 单次瞬态跳变被忽略（设计），约 12 s 内仍按旧状态运行；**手动模式下完全不生效**（LIMITATIONS #8） |
| F2 | OneWire 总线无设备（`!dsOk`） | `doScan()` `cnt==0` | 重新 doScan 每 10s | 缺口范围较窄（§6.4）：只有在已有探头缺失、重扫发现 0 设备后，`readSensor()` 顶端 `if(!dsOk) return` 才会让 `nanCount` 停止增长 → **不急停**，旧 TEC 状态残留（LIMITATIONS #36，锁版未修）；三颗都在线时整体掉线仍会走 NAN 路径急停 |
| F3 | 出風口过热 `ventT>=ventMax` | `controlTemp()` guard2 | 紧急断电 + saveState | 出风口探头 NAN 时 guard1 先行，guard2 不执行；在 nanCount 累到 3 之前（约 12 s）TEC 继续按上次决策运行；手动模式下不检查 |
| F4 | H-bridge MOSFET fail-short | 无软件检测 | 仅靠 ventT 反馈 | 软件无法干预，需硬件温度保险（LIMITATIONS #32） |
| F5 | 主 ESP `loop()` 卡死 / 断电重启 | TWDT (7s 实际) | 看门狗复位 → setup → loadState 恢复参数与 `systemOn` | `setup()` 早期 `digitalWrite(TEC_EN, LOW)` 关 TEC；2026-10-04 起 `systemOn=true` 时 `setup()` 末尾自动 `startAll()` 恢复运行（LIMITATIONS #31，未上机验证）；紧急停机已存 `systemOn=false`，不会被复活 |
| F6 | WiFi AP 启动失败 | 无显式检查 | 无降级，OLED/Serial 仍工作 | 远程监控失效，但本地控制台不受影响 |
| F7 | WiFi 模式切换（STA+AP）连不上 | `setup()` 内 20 次重试 | 失败回退 AP | 切换后总要 `ESP.restart()`，期间短时离线 |
| F8 | OLED 离线 | `loop()` 5s I2C 探测 | 自动 `u8g2.begin()` 重连 | 良好 |
| F9 | 浏览器关页 / ESP 复位 | 无 | `allData` 丢失 | 已知缺陷（LIMITATIONS #5） |
| F10 | 相机 WiFi 掉线 | `loop()` 5s 检测 | `connectWiFi()` 重连 | 重连只 `httpd_stop` 了 80 口串流 server；81 口控制 server 未停，再次 `startCameraServer()` 时 81 口启动失败（打印 "Control server failed!"），旧实例仍在工作，功能不受影响 |
| F11 | 串流慢客户端 | `send_wait_timeout=1` + send(fd,0) probe | 1 秒超时断开 | 不再阻塞其他客户端 |
| F12 | 帧缓冲泄漏（发送失败） | `esp_camera_fb_return(fb)` 总是调用 | 防止 PSRAM 耗尽 | 已修复 |
| F13 | DS18B20 上电复位值 85.0°C（探头接触不良、掉电重连时出现） | 范围检查 `raw > 85.0` 才判 NAN，**85.0 本身被接受** | 无 | 85°C 进入 EMA（断线后首值不经滤波直接等于 85）；出风口会误触 `ventT>=ventMax` 急停，巢穴会误入极端分支。0826 CSV 有实例：16:22:57 出风 56.16°C 触发一次误急停（LIMITATIONS #37） |
| F14 | BME680 离线 | `bmeTick()` 连续 3 次 `beginReading` 失败 | 标记离线，每 60 s 重试 | 与控制无关，`/data` 字段变 `null`；`condRisk` 强制为 false（数据过期 30 s 同理） |

---

## 10. 已知架构限制

以下是从架构层面（非细节 bug）观察到的限制，会限制后续可扩展性：

1. **单文件巨 sketch**：`src/main.cpp` 约 1340 行包含感测、控制、HTTP、HTML、OLED、EEPROM、BME680，模块边界靠注释组织，不利于单元测试与并行开发。这是本系统**只能写 mirror 测试、无法直接测源码**（见 `test/README_TESTS.md`）的最大根因。
2. **无感测/控制抽象层**：温度读、决策、PWM 写三件事在 `controlTemp()` 内耦合，无法在 PC 上跑逻辑测试（必须 mock OneWire/Dallas/WebServer/ledc 全套 Arduino-API）。
3. **状态散落全局变量**：`systemOn/cooling/heating/nanCount` 等十余个全局变量无封装，任何函数都能改（目前 HTTP handler 与感测都在 `loop()` 任务内串行执行，没有并发竞争；但若日后改成多任务就需要加锁）。
4. **HTML/CSS/JS 在 PROGMEM 内 raw 字符串中**：~380 行前端代码无法 lint、无法类型检查、无法复用组件，靠 `R"HTML(...)"` 维护极脆弱。
5. **相机固件无 OTA、无配置接口**：换 WiFi SSID/密码、改 IR/LED 引脚都要重新烧录，与主 ESP 的可配置 WiFi 模式形成体验落差。
6. **两固件无共享库**：`include/`、`lib/` 均为空模板，养成"放了也不会被编译"的盲区。
7. **TWO `httpd` 的 ctrl_port 错开靠硬编码 32769**：未做枚举常量化，未来扩第三个 server 时易撞端口。
8. **mDNS 名称硬编码**：`esp32-tec` 与 `esp32-cam` 在两固件分别硬编码，改名必须两边同步，且同一网络内不能与其他设备重名。

---

## 附：源码定位速查

| 关心点 | 位置 |
|--------|------|
| 引脚定义 | `src/main.cpp:14-24` |
| ROM 位址 | `src/main.cpp:36-38` |
| BME680 全局与常量（含 `COND_*`） | `src/main.cpp:48-73` |
| `bmeInit` / `bmeTick` / `condRiskNext` / `updateCondRisk` | `src/main.cpp:263-359` |
| 安全阈值默认 | `src/main.cpp:84-86` |
| `setFan` / `setTec` / `setTecPwm` | `main.cpp:109-168` |
| `stopAll`/`startAll`/`emergencyStop` | `main.cpp:125-152` |
| `controlTemp`（核心决策） | `main.cpp:170-238` |
| `bmeInit` / `bmeTick` | `main.cpp:241-308` |
| `readSensor`（非阻塞状态机） | `main.cpp:316-362` |
| `handleData` / `handleControl` / `handleDiag` / `handleTest` | `main.cpp:364-555` |
| HTTP 路由注册 | `main.cpp:1126-1181` |
| 仪表板 HTML | `main.cpp:557-935` |
| `scheduleStateSave` / `saveState` / `loadState` / `setWifiModeAndRestart` | `main.cpp:469-527` |
| `doScan` | `main.cpp:992-1037` |
| `setup`（含看门狗/EEPROM/I2C 扫描/BME/WiFi 模式） | `main.cpp:1039-1183` |
| `loop`（看门狗/WS/NAN 守卫/风扇 after-run/重扫/读取/BME/OLED/状态保存/mDNS 重查） | `main.cpp:1185-1240` |
| `updateOLED` | `main.cpp:941-983` |
| 相机引脚/PIN_IR/PIN_LED | `camera/src/main.cpp:17-18` |
| 相机 WiFi 帐密常量 | `camera/src/main.cpp:12-15` |
| 相机 `camera_config_t` | `camera/src/main.cpp:202-228` |
| 相机 `stream_handler` | `camera/src/main.cpp:30-78` |
| 相机 `startCameraServer` | `camera/src/main.cpp:123-161` |
| 相机 `connectWiFi` | `camera/src/main.cpp:163-193` |

---

> 本文档基于 `src/main.cpp`（1240 行，`abc8f9a`）与 `camera/src/main.cpp` 的静态分析撰写，源码本身**未被修改**；2026-09-26 按当前源码复核并更新行号。任何逻辑改动请同步更新本文的相应小节。