# 单元测试说明（README_TESTS）

## ✅ 运行方式（2026-08-13 实测通过）

```bash
# 前置：Windows 需要 gcc/g++（native 测试环境用本机编译器）。
# 例：winlibs MinGW-w64（本机示例 C:\Users\binbi\tools\mingw64，GCC 16.2.0；请按自己机器路径调整）
export PATH="/c/Users/binbi/tools/mingw64/bin:$PATH"   # ← 换成你机器上的安装路径
cd D:/binbi/Desktop/test
pio test -e native
# 结果：37/37 PASSED（1.5s）
```

> **2026-10-04 新机复测：** pio（`~/.local/bin/pio`）+ WinLibs g++ 已装，`pio test -e native` → **41 test cases: 41 succeeded**（新增 4 个防结露 `condRiskNext` 用例）。

注意：PlatformIO native 会把 `test/` 下所有 `test_*.cpp` 链接为**一个**程序，
因此 `main()` / `setUp()` / `tearDown()` 只允许定义一次（统一在 `test_pure_logic.cpp`），
`test_control_policy.cpp` 通过 `resetControlMocks()` 暴露 mock 重置入口。

本目录下维护**纯逻辑模块**的 PlatformIO Unity 单元测试骨架与测试说明。
**约束**：测试**只读分析** `src/` 现有代码、不修改源码运行；CSV / `.pio/` / `.opencode/` / `node_modules` 不触碰。

源码分析显示，`src/main.cpp` 是单一 Arduino sketch，所有逻辑（感测、控制、HTTP、HTML、OLED、EEPROM）混在同一文件且依赖大量 Arduino API（`WebServer` / `OneWire` / `DallasTemperature` / `U8g2` / `ledc`）。
**这使得当代 `src/main.cpp` 无法直接 `#include` 到 native 测试环境编译**。
因此本目录的测试采用两种策略：

| 文件 | 策略 | 说明 |
|------|------|------|
| `test/test_pure_logic.cpp` | **可执行测试** | 镜像若干**无 Arduino 依赖的纯算法片段**（sameAddr、EMA 滤波、温度合法化、NaN 去抖、EEPROM 地址布局契约），用标准 C++17 直接编译验证 |
| `test/test_control_policy.cpp` | 骨架 + mock | 复刻 `controlTemp()` 状态机为可测逻辑结构；提供 `mock_setFan` / `mock_setTecPwm` / `mock_emergencyStop` / `mock_saveState` 等替身，断言输入输出对 |
| `test/test_eeprom_config.cpp` | 骨架 + 桩 | 复刻 `saveState()` / `loadState()` 与 `handleControl()` 的 constrain 范围，验证往返一致性与越界 clamp |
| `test/mocks/Arduino.h` | mock 桩（header-only） | 提供 `Serial` / `EEPROM` / `digitalRead/Write` / `ledcWrite` / `millis` 等 Arduino API 占位行为；`String` / `delay` 类最小兼容实现 |

---

## 运行方式

> **前置依赖**：`platform = native` 依赖系统已安装的 C/C++ 编译器（`gcc` / `g++`）。
> Windows 上需先装 MinGW-w64 或 MSYS2；macOS 装 Xcode Command Line Tools；Linux 装 `build-essential`。
> 本仓库当前所在的 Windows 主机在 2026-08-13 审查时**未装 g++**，当时 `pio test -e native` 会因 `gcc 不是内部或外部命令` 报错退出——
> mingw64 是之后装的（见上节 37/37 通过记录），时间线如此，两处都成立。
> 这不是测试本身的问题，安装编译器后即可正常运行。项目首次运行 `pio test` 会自动拉取 `native` 平台与 `Unity@2.6.1` 库。

```bash
# 运行所有 native 测试
pio test -e native

# 运行单个测试文件
pio test -e native -f test_pure_logic

# 详细输出
pio test -e native --verbose
```

`platformio.ini` 已新增 `[env:native]` 段：
- `platform = native`，`framework = unity`
- `build_flags = -std=c++17 -I test/mocks`
- 测试源被 PlatformIO 全量扫描入构建。

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
| `loop()` 顶部"双层 NAN 守卫"的 `!dsOk` 立即断电路径 | 高 | OneWire/Dallas 库 mock |
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

> 本骨架严格不改 `src/`，所有 mirror 实现是纯算法的语义副本；断言失败仅作为提示而非直接证明源码回归——需要 diff 校验两版差异以最终定位。建议在完成 src 重构后切换到 `#include` 真源并 ~95%删除 mirror 副本。