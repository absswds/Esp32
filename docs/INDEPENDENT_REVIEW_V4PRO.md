# 独立代码审查报告（INDEPENDENT_REVIEW_V4PRO）

> 审查人角色：独立代码审查员（第三方，非原生成 AI）。
> 审查对象：
> 1. `docs/ARCHITECTURE.md`、`docs/CONFIG_GUIDE.md`、`docs/EXPERIMENTS.md` 三份文档的事实准确性（对照 `src/main.cpp`、`camera/src/main.cpp`、`platformio.ini`、`camera/platformio.ini` 与 7 个 CSV 原始数据）。
> 2. `test/` 下 `test_pure_logic.cpp`、`test_control_policy.cpp`、`test_eeprom_config.cpp` 与 `test/README_TESTS.md`（逻辑一致性、编译风险、mock 合理性）。
>
> 方法：只读源码与 CSV，逐项比对文档断言；对 7 个 CSV 用脚本统计首末行、min/max、风扇档集合、状态序列、重复时戳。**未修改任何现有文件**（本文件为唯一新增）。
>
> 结论速览：三份文档整体质量高（EEPROM 地址映射、PWM 参数、ROM 地址、相机配置全部核对无误），但存在 **1 个会直接导致测试失败的错误、1 个时序分析错误、1 个实验数据时序位置错误**，以及若干轻微行号/措辞问题。测试骨架 **从未被真正编译运行**，且其中一个"可跑过"的测试文件内含必然失败的断言。

---

## 0. 验证方法说明

- 源码：完整通读 `src/main.cpp`（1120 行）、`camera/src/main.cpp`（275 行）、`platformio.ini`、`camera/platformio.ini`、`test/mocks/Arduino.h`。
- CSV：对 7 个 CSV 用 Python 脚本计算数据行数、首末时间戳、三列温度 min/max 及对应时间戳、风扇档集合、状态迁移序列（折叠连续重复）、重复时戳计数。
- 环境事实：本机 `g++` 不存在（`which g++` 无结果），`.pio/build/native/` 下仅有 `unity_config.c/h` 与 scons 签名，**无任何 `.o` / 测试可执行产物**——证明 `pio test -e native` 从未成功编译过测试。

---

## 1. 核对表（ARCHITECTURE.md）

| # | 断言 | 源码/数据依据 | 结论 |
|---|------|---------------|------|
| A1 | 主 `src/main.cpp` 1120 行 | 实际 1120 行 | ✅ |
| A2 | 相机 `camera/src/main.cpp` 275 行 | 实际 275 行 | ✅ |
| A3 | 引脚 `FAN=18, TEC_EN=19, TEC_LPWM=26, TEC_RPWM=25, DS18B20=4` | `main.cpp:13-23` | ✅ |
| A4 | `PWM_FREQ=25000, PWM_RES=10, TEC_PWM_RES=10`，`FAN_CH=0/TEC_L_CH=1/TEC_R_CH=2` | `main.cpp:17-22` | ✅ |
| A5 | `EMA_ALPHA=0.5f`、`FAN_AFTERRUN_MS=10000`、`FAN_AFTERRUN_SPEED=200`、`STATE_SAVE_DEBOUNCE_MS=750` | `main.cpp:73,78,79,65` | ✅ |
| A6 | ROM 地址 nest/room/vent 三个 `DeviceAddress` | `main.cpp:35-37` | ✅ |
| A7 | 安全阈值默认 `safeMin=5.0, safeMax=35.0, ventMax=50.0` | `main.cpp:68-70` | ✅ |
| A8 | `setFan` 0-255→`ledcWrite` ×4（10-bit） | `main.cpp:93-96` | ✅ |
| A9 | `setTec` 手动 8-bit→×4；`setTecPwm` 归一化 0-1→×1023 | `main.cpp:98-107,138-152` | ✅ |
| A10 | `controlTemp()` 位于 `main.cpp:154-222` | 实际 | ✅ |
| A11 | 网络路由：5 个常规 + `/light` `/camenable` 两个 inline lambda | `main.cpp:1007-1061` | ✅ |
| A12 | OLED `updateOLED` 每 2 秒刷新，5 秒探测重连 | `main.cpp:1092,1104` | ✅ |
| A13 | `saveState/loadState/scheduleStateSave` 于 `main.cpp:368-426` | 实际 | ✅ |
| A14 | 相机模块行号（stream/capture/light/status/startServer/connectWiFi/setup） | `camera/main.cpp:30-275` | ✅ |
| A15 | 相机 `xclk=20MHz, VGA, quality16, GRAB_LATEST, fb_count=2, PSRAM` | `camera/main.cpp:202-228` | ✅ |
| A16 | 浏览器 1 秒轮询（串流 5 秒）；`H` 600 窗口 / `allData` 10000 上限 | `main.cpp:741,744-747` | ✅ |
| A17 | `controlTemp` 状态机（guard0/NAN/vent/nest/throttle/bang-bang）顺序 | `main.cpp:154-222` | ✅ |
| A18 | 风扇 after-run 10 秒 78%（`FAN_AFTERRUN_SPEED=200`） | `main.cpp:207-220,1074-1082` | ✅ |
| A19 | 非阻塞转换状态机 750ms | `main.cpp:224-233` | ✅ |
| A20 | 双层 NAN 守卫（`loop()` L1071 后备） | `main.cpp:1071` | ✅ |
| A21 | 看门狗 `esp_task_wdt_init(7, true)` 为 7 秒（非 CLAUDE.md 的 3 秒） | `main.cpp:933` | ✅（文档正确指出偏差） |
| A22 | EEPROM 256B，已用 0-133 | `main.cpp:373-419` | ✅ |
| A23 | **感测周期 "2+0.75≈2.75s"** | 实际为 **~4s**（见下方错误 E-1） | ❌ |
| A24 | F2 故障描述 "`controlTemp()` 顶端 `if(!dsOk) return`" | `!dsOk` 检查在 `readSensor()` L225，不在 `controlTemp()` | ❌（E-2） |
| A25 | 制冷分支仍写 `setFan(255)` 全速（与 CLAUDE.md 52% 建议不符） | `main.cpp:201` | ✅（文档正确指出偏差） |
| A26 | mDNS 重查 5s（未确认）/60s（确认） | `main.cpp:1111` | ✅ |

---

## 2. 核对表（CONFIG_GUIDE.md）

| # | 断言 | 源码依据 | 结论 |
|---|------|---------|------|
| C1 | 引脚映射表（含 D12=GPIO4、D13=GPIO12 strapping 陷阱） | `main.cpp:13-23` + CLAUDE.md | ✅ |
| C2 | PWM 三通道 25000Hz / 10-bit / 0-1023 | `main.cpp:17-22` | ✅ |
| C3 | `setFan` 0-255×4、`setTec` ×4、`setTecPwm` ×1023 | `main.cpp:93-152` | ✅ |
| C4 | EMA=0.5、AFTERRUN 10s/200、去抖 750ms、看门狗 7s、OneWire 750ms、重扫 10s、OLED 5s/2s、mDNS 5s/60s | 各对应行 | ✅ |
| C5 | **感测周期 "2.75s"** | 实际 ~4s | ❌（E-1） |
| C6 | 阈值默认值与 constrain 范围（target 10-40、hyst 0.01-3、safeMin 0-20、safeMax 20-50、ventMax **40-80**） | `main.cpp:313-331` | ✅ |
| C7 | "ventMax 实际 40~80，README/CLAUDE.md 写 30 是旧文本" | `main.cpp:330` 为 `constrain(...,40,80)` | ✅（文档正确指出） |
| C8 | "safeMin/safeMax 无交叉校验" | `main.cpp:321-327` 无 `safeMin<safeMax` 检查 | ✅（文档正确指出） |
| C9 | 校准偏移默认 0、范围 -5~5 | `main.cpp:82,333-344` | ✅（值正确） |
| C10 | **偏移行号 "283-285 / 287-291 / 293-295"** | 实际 offset 处理在 `main.cpp:333-344`（283-295 是 handleData） | ❌（E-3） |
| C11 | 阈值五段优先级顺序 | `main.cpp:154-222` | ✅ |
| C12 | EEPROM 地址映射（0/1/2/6/10/14/18/22/26/30/34/35/68 → 133） | `main.cpp:373-390` 逐字段核对 | ✅（完全正确） |
| C13 | loadState 越界处理：仅 ventMax clamp [40,80]、wifiMode>1→0、SSID 0xFF 回退默认 | `main.cpp:403,408,412-415` | ✅ |
| C14 | "浮点字段仅 ventMax 例外有 clamp，其余无" | `main.cpp:398-410` | ✅ |
| C15 | WiFi 两模式 + AP 凭据 + STA 默认凭据 | `main.cpp:970-1005,412-415` | ✅ |
| C16 | mDNS 名称 esp32-tec / esp32-cam，纯 AP 下主 ESP 无 mDNS | `main.cpp:968,983,189(cam)` | ✅ |
| C17 | §7 单位/硬约束（-55~85、12-bit 0.0625、EEPROM 256、baud 115200、SSID≤32、pass 8-63） | 各定义 | ✅ |
| C18 | §8 前端 vs 后端 constrain 对齐表 | HTML 458-818 vs `handleControl` | ✅ |
| C19 | 相机配置表（SSID/pass、IR47/LED3、port 80/81、ctrl_port 32769、max 4、send_wait 1s、timeout 10000、VGA/QVGA、quality16、xclk 20MHz、fb 2/1、grab LATEST、sensor 后处理） | `camera/main.cpp` 逐项核对 | ✅（后处理表漏列 `saturation=0`/`aec2=0`，见 E-7） |

---

## 3. 核对表（EXPERIMENTS.md）

| # | 断言 | CSV 实测 | 结论 |
|---|------|---------|------|
| X1 | 7 个 CSV 行数 616/296/609/310/636/315/2793 | 脚本实测一致 | ✅ |
| X2 | 时长 10:15/4:43/10:08/5:08/10:35/5:14/47:06 | 首末时间戳一致 | ✅ |
| X3 | A1 制冷：起 29.5→终 24.9，Δ-4.6，速率 0.448 | 实测 nest 29.5→24.9 | ✅ |
| X4 | A1 巢穴 min 24.9@17:51:45 / max 29.8@17:43:29 | 实测一致 | ✅ |
| X5 | A1 出風口 25.4→29.8、活動區 25.0~29.4 | 实测 vent 25.4~29.8、room 25.0~29.4 | ✅ |
| X6 | A1 风档 {0,39,100}、状态 維持→製冷→維持 | 实测一致 | ✅ |
| X7 | A3 制冷：起 29.1→终 24.9，Δ-4.7，速率 0.464，状态未回維持 | 实测 nest 29.1→24.9，seq=['維持','製冷'] | ✅ |
| X8 | A3 巢穴 min 24.8@16:15:14 / max 29.5@16:07:58、出風口 25.0~29.8、活動區 24.8~29.6 | 实测一致 | ✅ |
| X9 | A2 加热：24.9→31.9，Δ+7.0，速率 1.484 | 实测一致 | ✅ |
| X10 | A2 出風口 25.5→31.5(+6.0)、风档 {0,39,100} | 实测 vent 25.5→31.5 | ✅ |
| X11 | **A2 活動區 "25.0~31.0 (+6.0)"** | 起始 room=25.1（min=25.0），起始→终止应为 +5.9 | ⚠️（E-8） |
| X12 | **A2 "起始连续 3 行时戳重复 17:53:48"** | 实测 17:53:48 出现 **13 行** | ❌（E-4） |
| X13 | A4 加热：24.9→31.6，Δ+6.7，速率 1.309，出風口 25.3→31.9(+6.6) | 实测一致 | ✅ |
| X14 | **A4 異常"製冷"位置**："末尾出现製冷"、"維持→加熱→短暫製冷→維持" | 实测 seq=['維持','製冷','加熱','維持']，製冷在**开头**（16:18:31-32，2 行） | ❌（E-5） |
| X15 | B1 8.5V 制冷：29.39→25.77=-3.62，出風口 29.27→26.13=-3.14，活動區 25.94~29.25 | 实测一致 | ✅ |
| X16 | B2 8.5V 加热：25.75→30.71=+4.96，速率 0.948，风档 {100,78} | 实测一致（seq=['加熱','維持']，78% 结尾） | ✅ |
| X17 | §3.3 横向对比（出風口 min 25.4/26.13/25.5/26.13，加速率 0.448/0.345/1.484/0.948） | 实测一致 | ✅ |
| X18 | C 组：2793 行、47:06、起 nest 26.34 终 22.19、vent 14.87 | 实测一致 | ✅ |
| X19 | C §4.1 里程碑表（11:42:00/11:54:20/12:00:38/12:06:05/12:25:27/12:28:03/12:29:06） | 逐点实测一致（见下） | ✅ |
| X20 | **C §4.1 "12:28:03 min … 活動區 19.75"** | 12:28:03 实际 room=**19.81**（19.75 为全局最低，出现在别处） | ⚠️（E-6） |
| X21 | C §4.2 风档：100%→18.67、50%→17.30、52%→14.87、0%→20.93 | 实测：100% 档最低 **18.63**、50% 档最低 **17.33**、52%=14.87、0% 最高 20.93 | ⚠️（E-9） |
| X22 | C §4.4 首次≤15.0@12:25:27、首次≤14.95@12:25:39、43 分钟 | 实测一致 | ✅ |
| X23 | C §4.3 出風口 14.87 vs 巢穴 22.19 Δ≈7.3 | 实测 7.32 | ✅ |
| X24 | §5 横向对比 C Δ-4.2、最低出風口 14.87 | 实测 26.34-22.19=-4.15 | ✅ |
| X25 | **C "末两行 12:29:05"（§4.5 #3 / §7.3）** | 末尾序列为 …12:29:05、12:29:05、**12:29:06**（末行是 12:29:06） | ⚠️（E-10） |
| X26 | §1 "A/B 用 7/22 格式" | A 系 7/22，B 系为 **7/23** | ⚠️（E-11） |
| X27 | 数据精度 A 1 位小数 / B、C 2 位小数 / C 含上午下午 | 实测一致 | ✅ |

C §4.1 里程碑逐点验证（脚本精确匹配）：
- `11:42:00` fan=100 vent=25.04 nest=26.34 room=25.85 ✅
- `11:54:20` fan=50 vent=18.67 nest=23.34 ✅
- `12:00:38` fan=78 vent=19.51 nest=23.79 → `12:00:49` fan=0 → `12:01:36` fan=100 vent=20.93 nest=24.14 ✅（"78%→0%→100%" 属实）
- `12:06:05` fan=52 vent=18.68 nest=23.56 ✅
- `12:25:27` fan=52 vent=15.00 nest=22.31 ✅
- `12:28:03` fan=52 vent=14.87 nest=22.19 ✅（唯活動區 19.81 与文档 19.75 不符）
- `12:29:06` fan=52 vent=14.94 nest=22.19 room=19.89 ✅

---

## 4. 核对表（test/ 与 README_TESTS.md）

| # | 断言 | 依据 | 结论 |
|---|------|------|------|
| T1 | `test_pure_logic.cpp` sameAddr 镜像 = `main.cpp:870-873` | 逐字节比对一致 | ✅ |
| T2 | EMA 镜像 = `main.cpp:254-258` | 一致 | ✅ |
| T3 | sanitize 镜像 = `main.cpp:247-251`（-55/85 边界、断线、NaN） | 一致 | ✅ |
| T4 | nanGuard 镜像 = `main.cpp:161-174`（≥3 断电、恢复清零） | 一致 | ✅ |
| T5 | **EEPROM 布局契约测试 `TEST_ASSERT_EQUAL_INT(6, 34 - 32)`** | `34-32=2`，即断言 `6==2` → **必然失败** | ❌（E-12，严重） |
| T6 | README_TESTS 称 `test_pure_logic.cpp` "✅ 已可直接跑过" | 与 T5 矛盾，且从未实际编译 | ❌（E-13，严重） |
| T7 | README_TESTS 引用 "`test/mocks/Arduino.h` + `Arduino.cpp`" | `test/mocks/` 下**只有 Arduino.h，无 Arduino.cpp** | ❌（E-14） |
| T8 | README_TESTS "❌ delay 暂未桩" | mock `Arduino.h:69` 已提供 `inline void delay(...)` 空操作 | ❌（E-15） |
| T9 | README_TESTS 描述 ini 为 "`framework = unity`" | 实际 ini 为 `test_framework = unity`，且 build_flags 漏列 `-I test` | ⚠️（E-16） |
| T10 | `test_control_policy.cpp` 死区分支镜像 | 真实源码死区会触发 fan after-run（保持 200 达 10s），镜像简化为 `setFan(0)` | ⚠️（E-17） |
| T11 | `test_control_policy.cpp` 头部注释"使用 mocks/Arduino.h 桩" | 文件未 `#include "Arduino.h"`，自带 lambda mock | ⚠️（E-18） |
| T12 | `test_control_policy.cpp` 热节流段冗余死代码（76-79 行被 80 行覆盖） | 逻辑结果正确但含 4 行废代码 | ⚠️（E-19） |
| T13 | `test_eeprom_config.cpp` saveState/loadState 镜像 = `main.cpp:373-419` | 逐字段一致（含 ventMax clamp、wifiMode reset、SSID 0xFF 回退） | ✅ |
| T14 | `test_eeprom_config.cpp` constrain 镜像 = `handleControl` 范围 | 一致 | ✅ |
| T15 | 三个测试文件均自定义 `main()` + `setUp/tearDown` | PlatformIO native 默认 runner 会生成 main，可能多定义冲突 | ⚠️（E-20） |

---

## 5. 错误清单（按严重度分级）

### 🔴 严重（会导致测试失败 / 严重误导）

- **E-12 / E-13｜测试断言错误 + "可跑过"声称不实**
  - `test/test_pure_logic.cpp:150`：`TEST_ASSERT_EQUAL_INT(6, 34 - 32)` 求值为 `6 == 2`，**运行必失败**（注释写"hysteresis 从 6 开始"，但 `34-32=2`）。
  - `test/README_TESTS.md:46` 明确声称该文件 "✅ 已可直接跑过"，与实际矛盾。
  - 佐证：本机无 g++，`.pio/build/native/` 无任何 `.o` 或测试二进制，测试**从未被编译运行**过——"可跑过"是未经验证的断言。
  - 附带问题：整个 `test_eeprom_layout_constants`（147-153 行）只是字面量自比较（`0==0`、`1==1`…），不引用任何源码共享常量，**不构成真正的"契约测试"**，唯一非平凡的一行还写错了。

### 🟠 中等（事实性/时序/位置错误）

- **E-1｜感测周期分析错误（两份文档同错）**
  - `docs/ARCHITECTURE.md:249` 与 `docs/CONFIG_GUIDE.md:102` 均写"实际感测周期接近 2+0.75≈2.75s"。
  - 实际：`readSensor()` 由 `loop()` 每 2 秒调用一次；第一次调用 `requestTemperatures()` 并 `return`，下一次调用（2 秒后，远超 750ms）才真正读温并 `controlTemp()`。因此"请求→读取"跨两次 2 秒调用，**完整周期是 ~4 秒**，不是 2.75s。
  - 文档的隐含模型（认为 750ms 是门控因素）错误；门控因素是 2 秒的 readSensor 调用节奏。

- **E-5｜A4 异常"製冷"时序位置写反**
  - `docs/EXPERIMENTS.md:78,83` 写"維持 → 加熱 → 短暫製冷 → 維持"、"A4 末尾出现製冷"。
  - 实测 `P1-无铜片/P1-无铜片_0722_12703vs12706对比_12706加热.csv` 状态序列为 `['維持','製冷','加熱','維持']`，"製冷"出现在**开头**（16:18:31-32，仅 2 行，位于維持与加熱之间），而非末尾。

### 🟡 轻微（行号/措辞/四舍五入/引用缺失文件）

- **E-2** `docs/ARCHITECTURE.md:319`（F2）：`!dsOk` 检查在 `readSensor()` L225，而非文档所说 `controlTemp()` 顶端。实质结论（无 !dsOk 紧急断电）仍正确。
- **E-3** `docs/CONFIG_GUIDE.md:138-140`：校准偏移行号 "283-285/287-291/293-295" 错误，实际在 `main.cpp:333-344`（283-295 属 `handleData`）。
- **E-4** `docs/EXPERIMENTS.md:76,259`：A2 起始重复时戳 "3 行" 实为 **13 行**（17:53:48）。
- **E-6** `docs/EXPERIMENTS.md:158`：里程碑 "12:28:03 活動區 19.75" 实际 19.81（19.75 为全局最低，出现在别处）。
- **E-7** `docs/CONFIG_GUIDE.md:326`：相机 sensor 后处理表漏列 `set_saturation(s,0)` 与 `set_aec2(s,0)`（`camera/main.cpp:249,251`）。
- **E-8** `docs/EXPERIMENTS.md:74`：A2 活動區 "25.0~31.0 (+6.0)" 用 min(25.0) 而非起始值(25.1)，起始→终止应为 +5.9。
- **E-9** `docs/EXPERIMENTS.md:165-167`：风扇档最低温 "100%→18.67 / 50%→17.30" 与实测 18.63 / 17.33 有 0.03-0.04°C 出入（四舍五入/取值点不一致）。
- **E-10** `docs/EXPERIMENTS.md:194,259`：C "末两行 12:29:05" 不准确，末尾是 …12:29:05、12:29:05、12:29:06（末行 12:29:06）。
- **E-11** `docs/EXPERIMENTS.md:34`："A/B 用 7/22 格式" 中 B 系列实为 7/23。
- **E-14** `test/README_TESTS.md:15`：引用不存在的 `test/mocks/Arduino.cpp`。
- **E-15** `test/README_TESTS.md:88`："delay 暂未桩" 与 `mocks/Arduino.h:69` 已提供 delay 空操作桩矛盾。
- **E-16** `test/README_TESTS.md:38`：描述 ini 为 "`framework = unity`"，实际是 `test_framework = unity`；build_flags 漏列 `-I test`。
- **E-17** `test/test_control_policy.cpp:90-93`：死区镜像简化为 `setFan(0)`，未复刻真实源码的 fan after-run（保持 200 达 10 秒）；`test_deadzone_idle` 断言 fan==0 与真实源码行为不符。
- **E-18** `test/test_control_policy.cpp:11-14` 注释声称使用 `mocks/Arduino.h` 桩，实际未 include、自带 lambda mock。
- **E-19** `test/test_control_policy.cpp:76-79` 热节流段 4 行冗余赋值（被 L80 覆盖），结果正确但含废代码。
- **E-20** 三个测试文件均自定义 `main()`；PlatformIO native 默认 Unity runner 也会生成 `main()`，存在重复定义链接风险（因从未编译，未能实测定论）。

---

## 6. 测试可编译性评估

### 6.1 结论
**当前三个测试文件均未被编译验证过**。本机无 g++/MinGW，`.pio/build/native/` 仅生成 `unity_config.c/h`，无对象文件。因此：
1. **确定性失败**：`test_pure_logic.cpp:150`（`6==2`）一旦运行必然断言失败，与 "✅ 已可直接跑过" 矛盾。
2. **潜在链接风险**：三文件各自定义 `main()`，若 PlatformIO native 采用单一 runner 编译全部测试文件，会产生"重复定义 main / setUp / tearDown"错误；若每个文件独立成可执行则无碍。因未实测，只能标注为"需验证"。
3. **可独立编译的部分**：`test_control_policy.cpp` 与 `test_eeprom_config.cpp` 逻辑内部自洽（镜像内部一致），若分别编译，其断言应通过（除上述 main 冲突问题外）。`test_eeprom_config.cpp` 依赖 `-I test/mocks` 头（ini 已配置）。
4. **mock 合理性**：`test/mocks/Arduino.h` 为 header-only，`inline` 变量避免 ODR 冲突，思路正确；EEPROM 用 256 字节内存映像 + `put/get/commit` 语义贴合真实 Arduino EEPROM；`millis` 提供 `g_fake_millis` 可推进。但 `delay` 是空操作（不是时间推进），无法测 after-run 时序——与 README_TESTS 的"delay 暂未桩"描述相反。

### 6.2 README_TESTS 的核心诚实性结论
README_TESTS 已诚实承认"本机未装 g++，测试未运行"，这是优点；但它同时给出 "✅ 已可直接跑过（test_pure_logic.cpp）" 的**未经验证的肯定断言**，且该文件含必然失败的断言，属于误导。建议改为"静态编写完成，尚未实跑验证"，并先修 `:150` 行。

---

## 7. 附带观察（超出文档准确性范围，供参考）

- **主 ESP `/light` 代理端口疑似 bug**：`src/main.cpp:1020` 代理到 `camIP:80`，但相机 `/light` 在 **81** 端口（`camera/main.cpp:145`）。文档 `ARCHITECTURE.md:116` 如实描述了源码行为（"camIP:80"），但该行为本身与相机控制端口 81 不一致——前端直连用 `:81`（`main.cpp:696`），代理却用 `:80`，代理路径实际不可用。
- **board 命名**：`platformio.ini` 实际用 `board = esp32dev`，而 CLAUDE.md 强调应 `dfrobot_firebeetle2_esp32e`。CONFIG_GUIDE 写 esp32dev（与 ini 一致），ARCHITECTURE 写两者皆可，此差异继承自 CLAUDE.md，非本文档新引入。
- `loadState()` 未对 `targetTemp/hysteresis/safeMin/safeMax/offset` 做越界 clamp（仅 `ventMax`）——两份文档均正确指出，且与 CODE_REVIEW.md CR-01 一致，属源码既有风险。

---

## 8. 总体评价

| 维度 | 评价 |
|------|------|
| 文档事实准确性（源码层） | **优秀**：EEPROM 地址映射（0-133 逐字段）、PWM/LEDC 参数、ROM 地址、引脚、相机全配置、阈值 constrain 范围、loadState 越界处理等硬事实全部核对无误，且主动指出 ventMax 40/80、看门狗 7 秒、制冷风扇 255 等"文档 vs 代码/旧文档"偏差。 |
| 文档事实准确性（实验数据层） | **良好**：7 个 CSV 的行数、时长、起止温度、Δ、速率、风档、状态序列绝大部分精确一致；主要问题集中在"异常状态的位置"（A4 製冷）与少量四舍五入/取值点不一致。 |
| 时序/状态机描述 | **有一处实质错误**：感测周期 2.75s（应为 ~4s），两份文档同错，说明该推断未经实测验证。 |
| 测试质量 | **偏弱**：镜像测试忠实度高，但存在必然失败的断言、引用不存在的文件、"可跑过"未验证断言、镜像与源码在 after-run 行为上的偏差；且全程未实际编译。 |
| 诚实性 | 中上：文档多处主动标注"此文档与代码/旧文档偏差"，是加分项；但测试"可跑过"的断言与"未装编译器"的事实冲突，扣分。 |

**一句话结论**：三份文档的"硬事实"（地址/引脚/参数/阈值）经得起逐行核对，可信度高；薄弱点在"动态行为推断"（感测周期）与"实验异常时序定位"（A4 製冷），以及测试骨架中一个必然失败的断言和"从未实跑"却声称"可跑过"的问题。建议优先修复 `test_pure_logic.cpp:150`，并订正两处 2.75s→4s 的时序描述。
