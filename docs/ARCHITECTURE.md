# 架构文档（ARCHITECTURE）

> 本文档对 ESP32 TEC 蟄眠實驗溫控系統做完整的架构层面梳理，涵盖两个独立固件目标的模块划分、数据流、状态机与并发模型。可独立阅读，无需先读其他文档。
>
> 源码分析基础（只读，未做任何修改）：`src/main.cpp`（1120 行，主 ESP）、`camera/src/main.cpp`（275 行，相机）、`platformio.ini` / `camera/platformio.ini`。

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

本系统由**两台完全独立的 ESP32 固件**组成，无直接电气或总线连接，仅通过 IP 网络松耦合（主 ESP 在 STA+AP 备援模式下与相机同处一个 WiFi；纯 AP 模式下两者完全断开，相机由用户手机热点承载）。

```text
┌─────────────── 硬件层 ───────────────┐   ┌──────────── 网络层 ───────────┐   ┌─── 客户端 ───┐
│ 3× DS18B20 (OneWire, GPIO4)        │   │ 主 ESP WiFi                   │   │ 浏览器 SPA   │
│ TEC H-bridge (TEC_EN/LPWM/RPWM)    │◀▶│  · 纯 AP: ESP32-TEMP          │◀▶│ (仪表板)      │
│ FAN PWM (GPIO18, LEDC ch0)         │   │  · STA+AP 备援: 连指定 WiFi   │   └──────────────┘
│ SSD1306 OLED (I2C 21/22)           │   │ HTTP server port 80           │
│ EEPROM (flash 模拟, 256B)         │   └───────────────────────────────┘
└─────────────────────────────────────┘            ▲
                   ▲                                │ mDNS esp32-cam
                   │ 控制信号                        │ (仅 STA 模式可解析)
                   │                                ▼
┌─────────────── 主 ESP 固件 src/main.cpp ────────────────────────────┐
│ setup() → loop() 单线程 Arduino 调度                                  │
│  · 感测层 readSensor()/controlTemp()  · HTTP 控制层 /control /test  │
│  · 安全层 emergencyStop()/双层 NAN 守卫 · 显示层 updateOLED()        │
└──────────────────────────────────────────────────────────────────────┘

┌──────── 相机固件 camera/src/main.cpp（独立第二个 ESP32-S3）─────────┐
│ WiFi STA（连手机热点 OPhone 12，失败回退 ESP32-TEMP AP）             │
│ 双 httpd server: port 80 串流 / port 81 控制（IR/LED）               │
│ 每个串流客户端一个独立 task（free-threaded）                         │
└──────────────────────────────────────────────────────────────────────┘
```

**关键点：** 两台 ESP32 **不共享任何硬件资源**。相机 IR/LED 在浏览器中由前端**直接发请求到相机 81 端口**（`toggleIR()`/`toggleLED()`），主 ESP 仅在 STA+AP 备援模式且能解析 mDNS 时作为代理中转（见 `src/main.cpp:1013-1042` 的 `/light` 处理器）。这是为了避免串流占用控制端点导致灯光无反应而做的设计取舍。

---

## 2. 两固件边界

| 维度 | 主 ESP（TEC 溫控） | 相机（行为观测） |
|------|---------------------|-----------------|
| 板型 | FireBeetle 2 ESP32-E (`esp32dev` / `dfrobot_firebeetle2_esp32e`) | DFRobot DFR1154 ESP32-S3 (`dfrobot_dfr1154`)，16MB Flash + 8MB OPI PSRAM |
| 主源文件 | `src/main.cpp`（1120 行） | `camera/src/main.cpp`（275 行） |
| 框架 | Arduino + Arduino-ESP32 core | Arduino + Arduino-ESP32 core + esp32-camera |
| WiFi 角色 | 默认 AP（`ESP32-TEMP` / 192.168.4.1）；可切 STA+AP 备援 | STA（连 `OPhone 12` 手机热点，失败回退 `ESP32-TEMP`） |
| HTTP | 单 `WebServer` (port 80)，事件回调式 | 双 `httpd` (port 80 串流 / port 81 控制)，每客户端独立 task |
| 持久状态 | EEPROM 256B（save/loadState） | 无 |
| 任务并发 | 单 `loop()` 主任务 + 看门狗；WiFi/HTTP/tls 由 core 内部 task 承载 | 主 loop 仅重连 WiFi；串流/控制由 httpd 内部 task 池承载 |
| 构建命令 | `pio run` | `pio run -e esp32s3cam`（独立 env） |

两固件**没有任何源码共享**（无 `include/` 公共头、无 `lib/` 共用库，两者目前均只有 PlatformIO 模板 README）。换感测器或调阈值需重刷主 ESP；相机配置（WiFi 帐密、引脚）则硬编码在 `camera/src/main.cpp` 顶部。

---

## 3. 主 ESP 模块划分

`src/main.cpp` 是单文件 Arduino sketch，按职责可在阅读上划分为 6 个模块（**代码本身没有物理拆分**，所有函数与全局都在同一编译单元，靠注释分节组织）。下表给出每个模块的入口、关键函数、依赖与产出。

### 3.1 全局配置与常量（`main.cpp:13-86`）

| 项 | 值 | 备注 |
|----|----|------|
| `FAN_PIN=18`, `TEC_EN=19`, `TEC_LPWM=26`, `TEC_RPWM=25`, `DS18B20_PIN=4` | GPIO 编号 | FireBeetle 板标 D-pin ≠ GPIO，详见 CONFIG_GUIDE |
| `PWM_FREQ=25000` Hz, `PWM_RES=10`, `TEC_PWM_RES=10` | LEDC 配置 | 风扇与 TEC 一致均 10-bit |
| `FAN_CH=0`, `TEC_L_CH=1`, `TEC_R_CH=2` | LEDC 通道分配 | 三路独立 |
| `EMA_ALPHA=0.5f` | 温度滤波系数 | 滑动平均 |
| `FAN_AFTERRUN_MS=10000`, `FAN_AFTERRUN_SPEED=200` | 风扇延迟关闭 | ~78% |
| `STATE_SAVE_DEBOUNCE_MS=750` | 状态保存去抖 | 避免抖动频繁写 EEPROM |
| DS18B20 ROM 常数 `nestAddr/roomAddr/ventAddr` | 8 字节 DeviceAddress | ROM 出厂位址认人 |
| 安全阈值默认 `safeMin=5.0`, `safeMax=35.0`, `ventMax=50.0` | °C | 可被 `/control` 覆盖并持久化 |

### 3.2 感测层（`main.cpp:224-270, 875-920`）

- `doScan()` — OneWire/Dallas 总线扫描，按 ROM 比对给三颗感测器赋角色；处理 `cnt==0` 回退路径并打印 raw ROM 便于排错。
- `readSensor()` — **非阻塞转换状态机**：先 `requestTemperatures()` 立即返回置 `convPending=true`；下次 loop 迭代等到 `millis()-convStart >= 750` 才真正读 3 个温度。读后做范围合法化（`-55~85°C` 外判 NAN）、校准偏移叠加、EMA 滤波，最后写入 `nestT/roomT/ventT` 和 `readTemps[]`，并调用 `controlTemp()` 闭环。
- `sameAddr()` — 8 字节逐位比较，ROM 识别基础。

**输入：DS18B20 OneWire 总线；输出：3 个滤波后温度 + raw 数组。**

### 3.3 控制层（`main.cpp:93-222`）

控制层是**单点真相来源** `controlTemp()`，被 `readSensor()` 和 `handleControl()` 两处调用以立即生效新阈值。

- `setFan(int s)` — 风扇占空比 0-255 → `ledcWrite` 10-bit（×4）。
- `setTec(int cool, int heat)` — **手动 / `/test` 通道**：8-bit 0-255 放大 4 倍写到 TEC_L/R 通道，同步置 `cooling/heating` 标志。
- `setTecPwm(float power, bool isCool)` — **自动 bang-bang 通道**：归一化 0-1.0 → 0-1023 PWM，单向驱动（另一方向写 0）。
- `controlTemp()` — 安全 + bang-bang 五段式决策（详见 [6.1](#61-controltemp-状态机)）。
- `emergencyStop()` / `stopAll()` / `startAll()` — 系统级状态切换，含风扇延迟逻辑。

### 3.4 网络层（`main.cpp:279-454, 1007-1063`）

- `WebServer server(80)` — 5 个路由 + 2 个 inline lambda 路由：
  - `GET /` → `handleRoot()` 返回 `PROGMEM` 内嵌的 SPA HTML（`main.cpp:456-818`，约 360 行 inline HTML+CSS+JS）。
  - `GET /data` → `handleData()` JSON 状态快照。
  - `POST /control` → `handleControl()` 参数写入 + `scheduleStateSave()` / `saveState()`，再 `controlTemp()` 即时生效。
  - `GET /test` → `handleTest()` 手动 PWM 测试，置 `tecManual/fanManual` 标志。
  - `GET /diag` → `handleDiag()` 诊断 JSON。
  - `GET /light`（inline） → 主 ESP 仅在相机不可直连时作为代理；乐观更新本地 `lightIR/lightLED` 缓存，再尽最大努力同步到相机 `camIP:80`。
  - `GET /camenable`（inline） → 切换 `camEnabled` 标志并回 JSON。

### 3.5 显示层（`main.cpp:824-866, 1091-1104`）

- `updateOLED()` — U8g2 `firstPage()/nextPage()` 分页绘制，6 段布局（巢穴/活动/出风口温度 + 状态 + 风扇% + 目标 + 模式），每 2 秒刷新。
- 主循环每 5 秒 `Wire.beginTransmission(0x3C)` 探测，OLED 断线自动 `u8g2.begin()` 重初始化（热插拔恢复，commit `e9c6829`）。

### 3.6 持久化层（`main.cpp:368-426`）

- `scheduleStateSave()` — 去抖调度：设 `stateSavePending=true`+`stateSaveDue`。
- `saveState()` — 写 EEPROM 256B（详细地址见 CONFIG_GUIDE）。
- `loadState()` — 上电恢复，含 `0xAA` 有效标记、`ventMax` 越界 clamp、`wifiMode>1` reset、SSID 0xFF 兼容默认回退。
- `setWifiModeAndRestart(mode)` — `saveState()` + `delay(200)` + `ESP.restart()`，用于 WiFi 模式切换。

---

## 4. 相机固件模块划分

`camera/src/main.cpp` 极简，按职责可分 3 块：

| 模块 | 函数 | 位置 | 说明 |
|------|------|------|------|
| 串流服务 | `stream_handler()` | `camera/src/main.cpp:30-78` | MJPEG multipart 帧循环；每帧前用 `send(fd, &probe, 0, MSG_DONTWAIT)` 探测客户端是否断开；10 秒无帧发出超时退出；**始终 `esp_camera_fb_return(fb)`** 防帧缓存泄漏 |
| 快照服务 | `capture_handler()` | `82-91` | 单帧 JPEG 抓取，用于不发 multipart 的客户端 |
| 控制 / 状态 | `light_handler()` / `status_handler()` | `95-119` | IR/LED GPIO 控制与状态 JSON；GET query 参数解析 |
| 启动 | `startCameraServer()` | `123-161` | 起 2 个 `httpd` 实例，stream(80) 与 control(81)；`send_wait_timeout=1` 避免慢客户端阻塞 5 秒；`ctrl_port=32769` 与默认 32768 错开 |
| WiFi | `connectWiFi()` | `163-193` | 两级回退：`OPhone 12` → `ESP32-TEMP`；连接成功后 `(re)startCameraServer()` + `MDNS.begin("esp32-cam")` |
| setup / loop | `setup()` / `loop()` | `197-275` | camera_config 全字段硬编码；PSRAM 缺失回退 QVGA DRAM；`set_vflip/hmirror/brightness/contrast` 等传感器调参写死；主 loop 仅监测 WiFi 掉线重连 |

### 4.1 传感器配置（`camera/src/main.cpp:202-228`）

VGA 640×480 / JPEG quality 16 / `CAMERA_GRAB_LATEST` / `fb_count=2` / `fb_location=PSRAM` / `xclk=20MHz`。
**无 PSRAM 时** (`!psramFound()`) 自动回退 QVGA + DRAM + 单帧缓冲，避免 DMA 崩溃（这是 README 反复强调"必须用 `dfrobot_dfr1154` 板型"的根本原因）。

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
        ┌────────── controlTemp() ─────────┐  ◀── http 触发（/control /test）
        │ 5 段安全 + bang-bang 决策          │
        └──────┬───────────────┬────────────┘
   制冷/加热    │               │ 风扇
               ▼               ▼
        ledcWrite(TEC_L/R)  ledcWrite(FAN)
        digitalWrite(TEC_EN)

        旁路输出：
        ┌────────── /data JSON ──────────┐  →  浏览器仪表板
        └────────── updateOLED() ────────┘  →  SSD1306
        └────────── EEPROM saveState() ──┘  →  flash 持久化
```

### 5.2 HTTP 控制流

- **写参数**：浏览器 `fetch('/control?...',{method:'POST'})` → `handleControl()` 把参数写入全局变量 → `scheduleStateSave()`（750ms 去抖）→ `controlTemp()` 即时生效。
- **系统开关**：`handleControl("system=1")` → `startAll()`（同步置 `systemOn=true`、复位 `nanCount=0`、关闭手动态）→ 立即 `saveState()`（不走去抖，确保断电不丢）。
- **手动 PWM 测试**：`/test?cool=..&heat=..&en=..&fan=..` → 试验性写入并置 `tecManual/fanManual`，自动 `controlTemp()` 直接 return（被 `if(tecManual||manualMode) return` 拦截）。

### 5.3 浏览器 ↔ 相机 直连流

```text
浏览器                        主 ESP /camenable         相机 (port 80/81)
   │  GET /data camEnabled=true ────────▶                   │
   │  ◀── camIP (mDNS esp32-cam 解析) ──────────────────── │
   │                                                       │
   │  ──── <img src=http://camIP/stream> ─────────────────▶(port 80 MJPEG)
   │  ──── fetch http://camIP:81/light?ir=.. ─────────────▶(port 81 控制)
   │   （若 camIP 为空或纯 AP 模式，退路由到主 ESP /light 代理）
```

### 5.4 显示流

主回路中 OLED 每 2 秒读全局温度 + 状态绘制；浏览器侧每 1 秒（串流时 5 秒）轮询 `/data` 推入 `H`（600 滑动窗口）和 `allData`（10000 上限）数组，Canvas 重绘趋势图。

---

## 6. 状态机

### 6.1 `controlTemp()` 状态机

这是全系统的**核心决策机**，被 `readSensor()`（每 2 秒）和 `handleControl()`（每次参数写入）调用。返回前会被三个前置 guard 之一拦截：

```text
guard 0  systemOn==false || tecManual || manualMode  → return（不动作）
guard 1  isnan(nestT)||isnan(roomT)||isnan(ventT)    → nanCount++
            └ nanCount>=3 → emergencyStop()+saveState()+return
            └ 否则 return（容忍 1~2 次瞬态跳变）
guard 2  ventT >= ventMax (默认 50°C)                → emergencyStop()+saveState()+return
guard 3  nestT < safeMin || nestT > safeMax           → setTecPwm(0)+风扇 255 或 60 +return
─────────────────────────────────────────────────────────
热节流   if ventT > ventMax-10 → throttle = (ventMax-ventT)/10  ∈[0,1]
决策    nestT > targetTemp+hysteresis → setTecPwm(throttle,true) + setFan(255)
        nestT < targetTemp-hysteresis → setTecPwm(throttle,false)+ setFan(255)
        else（在死区内）→ setTecPwm(0) + 风扇 after-run 延迟逻辑
```

**重要：** 处于死区时如检测到刚从制冷/加热切出（`cooling||heating` 仍为 true），启动 `fanAfterRunTimer`；之后 10 秒内每帧保持 `FAN_AFTERRUN_SPEED=200`(~78%)，超过 10 秒关扇。

> ⚠️ 注意 README 提到"製冷時風扇應以 52% 運行"的實測發現，但**當前 `controlTemp()` 中製冷分支仍寫 `setFan(255)` 全速**（`main.cpp:199-204`）——這是已知的代碼與文檔偏差，鎖版凍結不改，演示時手動設 52%，詳 [LIMITATIONS.md](../LIMITATIONS.md) #34 與 [CONFIG_GUIDE.md](CONFIG_GUIDE.md) §2.3。

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

由 `loop()` 每 2 秒触发一次 `readSensor()`；读完一次后再等下次 loop 进入完成读取分支。两次 `readSensor()` 之间还叠加 750ms 转换等待与 loop 调度开销，**实测感测周期约 4s（V4PRO E-1 订正，原 2.75s 有误）**，这是隐含的采样率限制。

### 6.4 NOR 信号：双 NAN 守卫

为防止 `controlTemp()` 内 nanCount 计数器因不在自动模式等路径被绕过，`loop()` 顶部有一个**独立的后备守卫**：

```cpp
if (systemOn && sensorInit && nanCount >= 3) { emergencyStop(); saveState(); }
```

它与 `controlTemp()` 内的 `nanCount>=3 → emergencyStop()` 形成**冗余的双层保险**——即使 `controlTemp()` 因 `tecManual/manualMode` 提前 return，主循环仍能在 `nanCount≥3` 时强制断电。

但 `!dsOk`（总线无设备）触发立即断电的逻辑**仅在第 4 步重扫处处理**，并不直接进入 emergencyStop —— 详见 [故障域](#9-故障域与降级路径)。

### 6.5 EEPROM 保存状态机

```text
触发  → stateSavePending=true; stateSaveDue=millis()+750
loop 中 if pending && (long)(millis()-due)>=0 → saveState(); pending=false
例外：system 开关、emergencyStop 调 saveState() 走立即路径（不经去抖）
```

---

## 7. 并发与实时性模型

### 7.1 主 ESP：单 loop 主任务 + Arduino-ESP32 后端 task

主 ESP 没有显式多任务。所有控制、感测、HTTP、显示都在 `loop()` 串行执行。Arduino-ESP32 core 在背后运行 WiFi stack、LWIP、mDNS 等内部 task，调用回调时（如 `/control`）会在 core 的 task 上下文上回调，由 `WebServer` 内部把请求转交给 user handler——**不是中断**。

**关键的时序要求：**
- `loop()` 顶部 `esp_task_wdt_reset()` 必须在 3 秒内执行（实际配置 `esp_task_wdt_init(7, true)` 是 7 秒，注意 `main.cpp:933` 写 7 而舊註釋說 3 秒——以代碼 7 秒為準）。
- OneWire 时序由 `paulstoffregen/OneWire` 库用 `micros()` 软件循环保证，对 WiFi 中断极敏感。基于此 README 强调"`doScan()` 必须 `WiFi.softAP()` 之前执行"。
- HTTP 处理是非阻塞的，单个请求不应 >100ms，否则后续 IO 会被卡。`handleLight` 中的相机代理同步连接最长等 2 秒，可能影响响应性（用 `esp_task_wdt_reset()` 中间喂狗避免重启）。

### 7.2 相机：每客户端独立 task

`esp_http_server` 的架构是每个 URI handler 在独立 task 上下文执行。`stream_handler()` 是 `while(true)` 阻塞式，直到客户端断开或 10 秒超时才返回——**这不会阻塞 control(81) 端口**，因为那是另一个 `httpd` 实例。`max_open_sockets=4` 同时支持 4 个流客户端。

`capture_handler()` 用 `esp_camera_fb_get()` 抓帧，**与 streaming handler 共享摄像头帧队列**——并发抓帧与流可能会临时抢占帧资源，但 `CAMERA_GRAB_LATEST` + `fb_count=2` 缓解了大部分抖动。

### 7.3 跨固件松耦合

主 ESP 与相机之间**无 RPC 心跳、无同步锁**。相机掉线仅靠：
- 主 ESP 主循环每 5s~60s `MDNS.queryHost("esp32-cam")` 重新解析（仅 STA+AP 模式下成立）。
- 浏览器侧 `camErr()` 重试 2 秒后 `camPoll()` 重连。

---

## 8. 存储与持久化

| 介质 | 内容 | 容量 | 接口 |
|------|------|------|------|
| 主 ESP EEPROM（flash 模拟） | 系统状态 + 安全阈值 + 校准偏移 + WiFi 配置 | 256 字节映射，已用 0-133 字节 | `EEPROM.begin(256)` / `put` / `get` / `commit` |
| 主 ESP PROGMEM | SPA 仪表板 HTML/CSS/JS | ~12KB（INDEX[] PROGMEM R"HTML(...)"） | `server.send_P` |
| 浏览器内存 | `H[]` 600 + `allData[]` 10000 | 单条 ~8 个 number + 时间戳，数十 KB | JS 全局变量 |
| 相机 | 摄像头帧缓冲 PSRAM | 2 个 VGA 帧 + JPEG 中间缓冲 | `esp_camera_fb_get/return` |
| 相机 | WiFi 帐密 | 硬编码字符串常量 | 无持久化 |

EEPROM 详细地址映射见 [CONFIG_GUIDE.md]。

---

## 9. 故障域与降级路径

下表列每个潜在故障点、当前处置与残余风险，便于运维时定位问题域。

| # | 故障 | 检测位置 | 当前处置 | 残余风险 |
|---|------|----------|----------|----------|
| F1 | 任一 DS18B20 断线（NAN） | `controlTemp()` guard1 + `loop()` 双 NAN 守卫 | nanCount≥3 紧急断电 | 单次瞬态跳变被忽略（设计），但 6s 内仍可能在错误读值下控制 |
| F2 | OneWire 总线无设备（`!dsOk`） | `doScan()` `cnt==0` | 重新 doScan 每 10s | **无紧急断电**：`readSensor()` L225 顶端 `if(!dsOk) return` 直接退出，`controlTemp()` 不执行，旧 TEC 状态可能残留（LIMITATIONS #36，未修） |
| F3 | 出風口过热 `ventT>=ventMax` | `controlTemp()` guard2 | 紧急断电 + saveState | 若 ventT 也是 NAN，guard1 先行，guard2 不执行；若 ventNAN 但 nestNAN 时 nanCount 还没累到 3，仍可能继续加热 |
| F4 | H-bridge MOSFET fail-short | 无软件检测 | 仅靠 ventT 反馈 | 软件无法干预，需硬件温度保险（LIMITATIONS #32） |
| F5 | 主 ESP `loop()` 卡死 | TWDT (7s 实际) | 看门狗复位 → setup → loadState 恢复 | 复位期间 TEC 处于上电状态，但 `setup()` 第一行 `digitalWrite(TEC_EN, LOW)` 关 TEC，相对安全 |
| F6 | WiFi AP 启动失败 | 无显式检查 | 无降级，OLED/Serial 仍工作 | 远程监控失效，但本地控制台不受影响 |
| F7 | WiFi 模式切换（STA+AP）连不上 | `setup()` 内 20 次重试 | 失败回退 AP | 切换后总要 `ESP.restart()`，期间短时离线 |
| F8 | OLED 离线 | `loop()` 5s I2C 探测 | 自动 `u8g2.begin()` 重连 | 良好 |
| F9 | 浏览器关页 / ESP 复位 | 无 | `allData` 丢失 | 已知缺陷（LIMITATIONS #5） |
| F10 | 相机 WiFi 掉线 | `loop()` 5s 检测 | `connectWiFi()` 重连 | 重连会 `httpd_stop` 再 `startCameraServer`，避免端口冲突 |
| F11 | 串流慢客户端 | `send_wait_timeout=1` + send(fd,0) probe | 1 秒超时断开 | 不再阻塞其他客户端 |
| F12 | 帧缓冲泄漏（发送失败） | `esp_camera_fb_return(fb)` 总是调用 | 防止 PSRAM 耗尽 | 已修复 |

---

## 10. 已知架构限制

以下是从架构层面（非细节 bug）观察到的限制，会限制后续可扩展性：

1. **单文件巨 sketch**：`src/main.cpp` 1120 行包含感测、控制、HTTP、HTML、OLED、EEPROM，模块边界靠注释组织，不利于单元测试与并行开发。**这是本系统测试不可写**（见 test 章节说明）的最大根因。
2. **无感测/控制抽象层**：温度读、决策、PWM 写三件事在 `controlTemp()` 内耦合，无法在 PC 上跑逻辑测试（必须 mock OneWire/Dallas/WebServer/ledc 全套 Arduino-API）。
3. **状态散落全局变量**：`systemOn/cooling/heating/nanCount` 等十余个全局变量无封装，跨函数修改无原子性保证（被 core 后台 task 在 handleControl 中改写时与 loop 读取存在数据竞争，但单核 Arduino 上通常无重大隐患）。
4. **HTML/CSS/JS 在 PROGMEM 内 raw 字符串中**：~360 行前端代码无法 lint、无法类型检查、无法复用组件，靠 `R"HTML(...)"` 维护极脆弱。
5. **相机固件无 OTA、无配置接口**：换 WiFi SSID/密码、改 IR/LED 引脚都要重新烧录，与主 ESP 的可配置 WiFi 模式形成体验落差。
6. **两固件无共享库**：`include/`、`lib/` 均为空模板，养成"放了也不会被编译"的盲区。
7. **TWO `httpd` 的 ctrl_port 错开靠硬编码 32769**：未做枚举常量化，未来扩第三个 server 时易撞端口。
8. **mDNS 名称硬编码**：`esp32-tec` 与 `esp32-cam` 在两固件分别硬编码，禁止冲突。

---

## 附：源码定位速查

| 关心点 | 位置 |
|--------|------|
| 引脚定义 | `src/main.cpp:13-23` |
| ROM 位址 | `src/main.cpp:35-37` |
| 安全阈值默认 | `src/main.cpp:68-70` |
| `setFan` / `setTec` / `setTecPwm` | `main.cpp:93-152` |
| `stopAll`/`startAll`/`emergencyStop` | `main.cpp:109-136` |
| `controlTemp`（核心决策） | `main.cpp:154-222` |
| `readSensor`（非阻塞状态机） | `main.cpp:224-270` |
| `handleData` / `handleControl` / `handleTest` / `handleDiag` | `main.cpp:279-454` |
| HTTP 路由注册 | `main.cpp:1007-1063` |
| 仪表板 HTML | `main.cpp:456-818` |
| `saveState` / `loadState` / `scheduleStateSave` | `main.cpp:368-426` |
| `doScan` | `main.cpp:875-920` |
| `setup`（含看门狗/EEPROM/I2C 扫描/WiFi 模式） | `main.cpp:922-1064` |
| `loop`（看门狗/WS/NAN 后备/风扇 after-run/重扫/读取/OLED/状态保存/mDNS 重查） | `main.cpp:1066-1120` |
| `updateOLED` | `main.cpp:824-866` |
| 相机引脚/PIN_IR/PIN_LED | `camera/src/main.cpp:17-18` |
| 相机 WiFi 帐密常量 | `camera/src/main.cpp:12-15` |
| 相机 `camera_config_t` | `camera/src/main.cpp:202-228` |
| 相机 `stream_handler` | `camera/src/main.cpp:30-78` |
| 相机 `startCameraServer` | `camera/src/main.cpp:123-161` |
| 相机 `connectWiFi` | `camera/src/main.cpp:163-193` |

---

> 本文档基于 `src/main.cpp` 与 `camera/src/main.cpp` 的静态分析撰写，源码本身**未被修改**。任何逻辑改动请同步更新本文的相应小节。