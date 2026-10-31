# 單元測試說明（README_TESTS）

最近一次：2026-10-04，`pio test -e native` → **41/41 通過**（09-26 時是 37/37，10-04 新增 4 個防結露 `condRiskNext` 用例）。

`src/main.cpp` 是單文件 Arduino sketch，感測、控制、HTTP、HTML、OLED、EEPROM 都在一起，還依賴 `WebServer` / `OneWire` / `DallasTemperature` / `U8g2` / `ledc` 等 Arduino API，**沒法直接 `#include` 到電腦上編譯**。所以這裡的測試是"鏡像"：把要測的邏輯複製一份到測試文件裡測，不直接測源碼。改了源碼對應的邏輯，鏡像要手動同步。

| 文件 | 策略 | 說明 |
|------|------|------|
| `test/test_pure_logic.cpp` | **可執行測試** | 鏡像若干**無 Arduino 依賴的純算法片段**（sameAddr、EMA 濾波、溫度合法化、NaN 去抖、EEPROM 地址佈局契約、防結露遲滯），用標準 C++17 直接編譯驗證 |
| `test/test_control_policy.cpp` | 骨架 + mock | 復刻 `controlTemp()` 狀態機為可測邏輯結構；提供 `mock_setFan` / `mock_setTecPwm` / `mock_emergencyStop` / `mock_saveState` 等替身，斷言輸入輸出對 |
| `test/test_eeprom_config.cpp` | 骨架 + 樁 | 復刻 `saveState()` / `loadState()` 與 `handleControl()` 的 constrain 範圍，驗證往返一致性與越界 clamp |
| `test/mocks/Arduino.h` | mock 樁（header-only） | 提供 `Serial` / `EEPROM` / `digitalRead/Write` / `ledcWrite` / `millis` 等 Arduino API 佔位行為；`String` / `delay` 類最小兼容實現 |

PlatformIO native 會把 `test/` 下所有 `test_*.cpp` 鏈接成**一個**程序，所以 `main()` / `setUp()` / `tearDown()` 只能定義一次（統一在 `test_pure_logic.cpp`），`test_control_policy.cpp` 通過 `resetControlMocks()` 暴露 mock 重置入口。

---

## 運行方式

**前置依賴**：`platform = native` 用本機的 C/C++ 編譯器。Windows 裝 MinGW-w64（如 WinLibs）或 MSYS2，並把 `bin` 目錄加進 PATH；macOS 裝 Xcode Command Line Tools；Linux 裝 `build-essential`。沒裝編譯器時會報 `gcc 不是內部或外部命令`，裝上即可。首次運行會自動拉取 `native` 平台與 `Unity@2.6.1`。

```bash
# 運行所有 native 測試
pio test -e native

# 運行單個測試文件
pio test -e native -f test_pure_logic

# 詳細輸出
pio test -e native --verbose
```

`platformio.ini` 的 `[env:native]` 段：`platform = native`、`test_framework = unity`、`build_flags = -std=c++17 -I test/mocks -I test`。

> 殺毒軟件（如 360）可能把生成的 `.pio/build/native/program.exe` 誤報為木馬，把 `.pio` 目錄加入信任即可。

---

## 當前測試覆蓋範圍

### ✅ 已可直接跑過（test_pure_logic.cpp）

| 算法 | 鏡像的源碼函數 | 用例 |
|------|---------|------|
| `sameAddr` ROM 字節比較 | `sameAddr()` | 3 個用例（相等/末位異/首字節異） |
| EMA 濾波（α=0.5） | `readSensor()` | 3 個用例（首幀初始化 / α=0.5 / 單幀尖刺衰減） |
| DS18B20 範圍合法化 | `readSensor()` | 6 個用例（斷線/超範圍/邊界/NaN 透傳） |
| NaN 計數去抖 | `controlTemp()` 保護 0、`loop()` 守衛 | 3 個用例（恢復路徑 / 3 次觸發 / 0 glitch） |
| EEPROM 地址佈局契約 | `saveState()` / `loadState()` | 1 個用例（確保佈局常量未被改壞） |
| 防結露警示遲滯 `condRiskNext` | `condRiskNext()`（2026-10-04） | 4 個用例（`test_cond_enter_at_dp_plus_1` 露點+1 進入 / `test_cond_hysteresis_holds` 1–2°C 間保持 / `test_cond_exit_above_dp_plus_2` 露點+2 以上退出 / `test_cond_nan_clears` NaN 清除） |

### 🟡 骨架就緒但需策略調整（test_control_policy.cpp, test_eeprom_config.cpp）

測試邏輯已就緒並通過鏡像控制/配置語義驗證；**但源碼當前無法直鏈**——這是因為：

- `src/main.cpp` 單文件 sketch；含 `WebServer server(80)` 等永久實例化對象，鏈接會報多重定義或缺少本體。
- 控制/持久化邏輯依賴 `ledcWrite` / `digitalWrite` / `Serial` 等 Arduino API，需 mock。
- 這些 skeleton 測試**通過 mirror 實現先行驗證算法**；當後續重構把純邏輯提取到 `lib/teccore/` 時，可直接切到 `#include` 真源而不需重寫斷言。

---

## 未來要讓測試直接基於 `src/` 而非 mirror，需要做的重構

詳見 [../docs/ARCHITECTURE.md] §10「已知架構限制」。要點：

1. **拆 `src/main.cpp` 為多模塊**：
   - `lib/teccore/control.{h,cpp}`：`controlTemp()` / `setTecPwm()` / `setFan()` / `emergencyStop()`
   - `lib/teccore/sensor.{h,cpp}`：`readSensor()` 狀態機
   - `lib/teccore/persist.{h,cpp}`：`saveState/loadState`
   - `lib/teccore/policy.{h,cpp}`：純函數（sameAddr、EMA、範圍合法化、NaN guard）
   - `src/main.cpp` 僅留 Arduino setup/loop + HTTP 路由裝配
2. **依賴倒置**：把 `setFan` / `setTecPwm` 抽象為函數指針或接口，便於 mock 注入。
3. **WebServer mock**：補 `test/mocks/WebServer.h` 提供 `hasArg` / `arg` / `send` 用於 HTTP handler 單元測試。
4. **OneWire/Dallas mock**：相機等若需要測可補 `mocks/OneWire.h`，但當前主 ESP 測試不涉及，可延後。

---

## 需 mock 的部分彙總

| 模塊 | 關鍵 Arduino/庫 API | mock 提供方 | 狀態 |
|------|---------------------|------------|------|
| 控制 PWM 輸出 | `ledcWrite` / `digitalWrite` | `mocks/Arduino.h` | ✅ 樁已就緒 |
| 持久化 | `EEPROM` | `mocks/Arduino.h`（256 字節內存映像 + `put/get/commit/instance`） | ✅ |
| 時序 | `millis` / `delay` | `mocks/Arduino.h`（`g_fake_millis` 可推進） | ✅ millis，❌ delay 暫未樁 |
| 日誌 | `Serial.printf` | `mocks/Arduino.h`（空操作） | ✅ |
| HTTP 路由 | `WebServer.hasArg/arg/send` | — | ❌ 待補 `mocks/WebServer.h`（骨架階段不必要） |
| 感測總線 | `OneWire` / `DallasTemperature` | — | ❌ 暫未提供，sensor 狀態機測試需補 |
| OLED | `U8g2` | — | ❌ 不可單元測，應留在硬件集成測 |

骨架階段（本目錄現狀）已覆蓋關鍵純邏輯；其餘依賴較重的部分在加 mock 或重構前以 mirror 方式先行驗證。

---

## 已知未覆蓋路徑（待補測試）

| 路徑 | 優先級 | 需何種 mock |
|------|--------|------------|
| `!dsOk` 時 `readSensor()` 提前返回、`nanCount` 不再增長、不急停的缺口（LIMITATIONS #36） | 高 | OneWire/Dallas 庫 mock |
| `loop()` 風扇 after-run 計時狀態機 | 中 | millis 樁 + control 狀態結構重組 |
| `handleControl` 非 POST 請求回 405 | 低 | WebServer mock |
| `setTec`/`setTecPwm` 鉗制到 `[0,1023]` | 中 | control 拆分後直接斷言 |
| `setFan(s)` 把 0~255 ×4 到 0~1020 的"非對稱映射"邊界 | 中 | control 拆分後斷言 |
| `safeMin < safeMax` 交叉校驗（**目前代碼無此校驗**） | 高 | 加 constrain 配套函數後補 |
| `targetTemp` 非數字輸入 `toFloat()` 返回 0 而後被錯誤 clamp 到下限 | 高 | WebServer mock + handleControl 拆分 |
| `readSensor` 轉換狀態機過渡幀 `convPending` 切換 | 中 | Dallas mock + millis |

---

## 測試編寫約定

- 文件名前綴 `test_` + 模塊名（`.cpp`）。
- 每個函數 `void test_xxx(void)`；`main` 用 `UNITY_BEGIN()`/`UNITY_END()`。
- 鏡像段用註釋 `// [MIRROR] 鏡像 main.cpp:LINE` 標註來源行，便於源碼改動反查。
- 新增 tests 請在 [../docs/ARCHITECTURE.md] §10「已知架構限制」同步登記期望覆蓋的源碼行。

> 鏡像測試通過不等於源碼沒問題：鏡像和源碼不同步時，測試照樣會過。改了 `src/main.cpp` 裡被鏡像的邏輯，記得同步改測試。等以後把純邏輯拆到 `lib/` 裡，就可以直接 `#include` 源碼、刪掉鏡像副本。