# 单元测试说明（README_TESTS）

最近一次：2026-10-04，`pio test -e native` → **41/41 通过**（09-26 时是 37/37，10-04 新增 4 个防结露 `condRiskNext` 用例）。

`src/main.cpp` 是单文件 Arduino sketch，感测、控制、HTTP、HTML、OLED、EEPROM 都在一起，还依赖 `WebServer` / `OneWire` / `DallasTemperature` / `U8g2` / `ledc` 等 Arduino API，**没法直接 `#include` 到电脑上编译**。所以这里的测试是"镜像"：把要测的逻辑复制一份到测试文件里测，不直接测源码。改了源码对应的逻辑，镜像要手动同步。

| 文件 | 策略 | 说明 |
|------|------|------|
| `test/test_pure_logic.cpp` | **可执行测试** | 镜像若干**无 Arduino 依赖的纯算法片段**（sameAddr、EMA 滤波、温度合法化、NaN 去抖、EEPROM 地址布局契约、防结露迟滞），用标准 C++17 直接编译验证 |
| `test/test_control_policy.cpp` | 骨架 + mock | 复刻 `controlTemp()` 状态机为可测逻辑结构；提供 `mock_setFan` / `mock_setTecPwm` / `mock_emergencyStop` / `mock_saveState` 等替身，断言输入输出对 |
| `test/test_eeprom_config.cpp` | 骨架 + 桩 | 复刻 `saveState()` / `loadState()` 与 `handleControl()` 的 constrain 范围，验证往返一致性与越界 clamp |
| `test/mocks/Arduino.h` | mock 桩（header-only） | 提供 `Serial` / `EEPROM` / `digitalRead/Write` / `ledcWrite` / `millis` 等 Arduino API 占位行为；`String` / `delay` 类最小兼容实现 |

PlatformIO native 会把 `test/` 下所有 `test_*.cpp` 链接成**一个**程序，所以 `main()` / `setUp()` / `tearDown()` 只能定义一次（统一在 `test_pure_logic.cpp`），`test_control_policy.cpp` 通过 `resetControlMocks()` 暴露 mock 重置入口。

---

## 运行方式

**前置依赖**：`platform = native` 用本机的 C/C++ 编译器。Windows 装 MinGW-w64（如 WinLibs）或 MSYS2，并把 `bin` 目录加进 PATH；macOS 装 Xcode Command Line Tools；Linux 装 `build-essential`。没装编译器时会报 `gcc 不是内部或外部命令`，装上即可。首次运行会自动拉取 `native` 平台与 `Unity@2.6.1`。

```bash
# 运行所有 native 测试
pio test -e native

# 运行单个测试文件
pio test -e native -f test_pure_logic

# 详细输出
pio test -e native --verbose
```

`platformio.ini` 的 `[env:native]` 段：`platform = native`、`test_framework = unity`、`build_flags = -std=c++17 -I test/mocks -I test`。

> 杀毒软件（如 360）可能把生成的 `.pio/build/native/program.exe` 误报为木马，把 `.pio` 目录加入信任即可。

---

## 当前测试覆盖范围

### ✅ 已可直接跑过（test_pure_logic.cpp）

| 算法 | 镜像位置 | 用例 |
|------|---------|------|
| `sameAddr` ROM 字节比较 | `main.cpp:987-990` | 3 个用例（相等/末位异/首字节异） |
| EMA 滤波（α=0.5） | `main.cpp:346-351` | 3 个用例（首帧初始化 / α=0.5 / 单帧尖刺衰减） |
| DS18B20 范围合法化 | `main.cpp:339-344` | 6 个用例（断线/超范围/边界/NaN 透传） |
| NaN 计数去抖 | `main.cpp:177-190`, `:1190` | 3 个用例（恢复路径 / 3 次触发 / 0 glitch） |
| EEPROM 地址布局契约 | `main.cpp:474-491` | 1 个用例（确保布局常量未被改坏） |
| 防结露警示迟滞 `condRiskNext` | `main.cpp:344-349`（2026-10-04） | 4 个用例（`test_cond_enter_at_dp_plus_1` 露点+1 进入 / `test_cond_hysteresis_holds` 1–2°C 间保持 / `test_cond_exit_above_dp_plus_2` 露点+2 以上退出 / `test_cond_nan_clears` NaN 清除） |

### 🟡 骨架就绪但需策略调整（test_control_policy.cpp, test_eeprom_config.cpp）

测试逻辑已就绪并通过镜像控制/配置语义验证；**但源码当前无法直链**——这是因为：

- `src/main.cpp` 单文件 sketch；含 `WebServer server(80)` 等永久实例化对象，链接会报多重定义或缺少本体。
- 控制/持久化逻辑依赖 `ledcWrite` / `digitalWrite` / `Serial` 等 Arduino API，需 mock。
- 这些 skeleton 测试**通过 mirror 实现先行验证算法**；当后续重构把纯逻辑提取到 `lib/teccore/` 时，可直接切到 `#include` 真源而不需重写断言。

---

## 未来要让测试直接基于 `src/` 而非 mirror，需要做的重构

详见 [../docs/ARCHITECTURE.md] §10「已知架构限制」。要点：

1. **拆 `src/main.cpp` 为多模块**：
   - `lib/teccore/control.{h,cpp}`：`controlTemp()` / `setTecPwm()` / `setFan()` / `emergencyStop()`
   - `lib/teccore/sensor.{h,cpp}`：`readSensor()` 状态机
   - `lib/teccore/persist.{h,cpp}`：`saveState/loadState`
   - `lib/teccore/policy.{h,cpp}`：纯函数（sameAddr、EMA、范围合法化、NaN guard）
   - `src/main.cpp` 仅留 Arduino setup/loop + HTTP 路由装配
2. **依赖倒置**：把 `setFan` / `setTecPwm` 抽象为函数指针或接口，便于 mock 注入。
3. **WebServer mock**：补 `test/mocks/WebServer.h` 提供 `hasArg` / `arg` / `send` 用于 HTTP handler 单元测试。
4. **OneWire/Dallas mock**：相机等若需要测可补 `mocks/OneWire.h`，但当前主 ESP 测试不涉及，可延后。

---

## 需 mock 的部分汇总

| 模块 | 关键 Arduino/库 API | mock 提供方 | 状态 |
|------|---------------------|------------|------|
| 控制 PWM 输出 | `ledcWrite` / `digitalWrite` | `mocks/Arduino.h` | ✅ 桩已就绪 |
| 持久化 | `EEPROM` | `mocks/Arduino.h`（256 字节内存映像 + `put/get/commit/instance`） | ✅ |
| 时序 | `millis` / `delay` | `mocks/Arduino.h`（`g_fake_millis` 可推进） | ✅ millis，❌ delay 暂未桩 |
| 日志 | `Serial.printf` | `mocks/Arduino.h`（空操作） | ✅ |
| HTTP 路由 | `WebServer.hasArg/arg/send` | — | ❌ 待补 `mocks/WebServer.h`（骨架阶段不必要） |
| 感测总线 | `OneWire` / `DallasTemperature` | — | ❌ 暂未提供，sensor 状态机测试需补 |
| OLED | `U8g2` | — | ❌ 不可单元测，应留在硬件集成测 |

骨架阶段（本目录现状）已覆盖关键纯逻辑；其余依赖较重的部分在加 mock 或重构前以 mirror 方式先行验证。

---

## 已知未覆盖路径（待补测试）

| 路径 | 优先级 | 需何种 mock |
|------|--------|------------|
| `!dsOk` 时 `readSensor()` 提前返回、`nanCount` 不再增长、不急停的缺口（LIMITATIONS #36） | 高 | OneWire/Dallas 库 mock |
| `loop()` 风扇 after-run 计时状态机 | 中 | millis 桩 + control 状态结构重组 |
| `handleControl` 非 POST 请求回 405 | 低 | WebServer mock |
| `setTec`/`setTecPwm` 钳制到 `[0,1023]` | 中 | control 拆分后直接断言 |
| `setFan(s)` 把 0~255 ×4 到 0~1020 的"非对称映射"边界 | 中 | control 拆分后断言 |
| `safeMin < safeMax` 交叉校验（**目前代码无此校验**） | 高 | 加 constrain 配套函数后补 |
| `targetTemp` 非数字输入 `toFloat()` 返回 0 而后被错误 clamp 到下限 | 高 | WebServer mock + handleControl 拆分 |
| `readSensor` 转换状态机过渡帧 `convPending` 切换 | 中 | Dallas mock + millis |

---

## 测试编写约定

- 文件名前缀 `test_` + 模块名（`.cpp`）。
- 每个函数 `void test_xxx(void)`；`main` 用 `UNITY_BEGIN()`/`UNITY_END()`。
- 镜像段用注释 `// [MIRROR] 镜像 main.cpp:LINE` 标注来源行，便于源码改动反查。
- 新增 tests 请在 [../docs/ARCHITECTURE.md] §10「已知架构限制」同步登记期望覆盖的源码行。

> 镜像测试通过不等于源码没问题：镜像和源码不同步时，测试照样会过。改了 `src/main.cpp` 里被镜像的逻辑，记得同步改测试。等以后把纯逻辑拆到 `lib/` 里，就可以直接 `#include` 源码、删掉镜像副本。