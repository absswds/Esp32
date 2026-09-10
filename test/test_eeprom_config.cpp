// PlatformIO Unity 测试骨架：EEPROM save/load 与 API constrain 边界
//
// **此文件为骨架**（skeleton）。完整跑通需要把 src/main.cpp 中的 saveState()/loadState()
// 与 handleControl() 的 constrain 逻辑提取到独立可测单元，并能链接 mocks/Arduino.h 的
// EEPROM 桩。当前 src/main.cpp 内嵌 WebServer 等 Arduino API 依赖无法直接 #include。
// 详见 docs/CODE_REVIEW.md "结构改进" 与 docs/CONFIG_GUIDE.md §4/§6。
//
// ===== 需要 mock 的部分 =====
//     EEPROM（Arduino）         — 使用 mocks/Arduino.h 中的 _EEPROMStub（256 字节内存映像）
//     WebServer.hasArg/arg      — 需补 mocks/WebServer.h（本骨架未引入）
//     Serial.printf             — 已有桩（无操作）
// ===========================

#include <unity.h>
#include <cmath>
#include <cstdint>
#include <cstring>

// ============ 反映配置全局状态（镜像 src/main.cpp 全局变量） ============
struct CfgState {
    bool systemOn = false;
    float targetTemp = 28.0f, hysteresis = 0.5f;
    float safeMin = 5.0f, safeMax = 35.0f, ventMax = 50.0f;
    float nestOffset = 0, roomOffset = 0, ventOffset = 0;
    uint8_t wifiMode = 0;
    char wifiSSID[33] = "";
    char wifiPass[65] = "";
};

// ============ [MIRROR] saveState —— 镜像 main.cpp:373-390 ============
#include "Arduino.h"   // 复用 _EEPROMStub 桩

static void saveStateLogic(const CfgState& s) {
    uint8_t buf[256];
    std::memset(buf, 0xFF, sizeof(buf));
    buf[0] = 0xAA;
    buf[1] = s.systemOn ? 1 : 0;
    std::memcpy(&buf[2],  &s.targetTemp,  4);
    std::memcpy(&buf[6],  &s.hysteresis,   4);
    std::memcpy(&buf[10], &s.safeMin,      4);
    std::memcpy(&buf[14], &s.safeMax,      4);
    std::memcpy(&buf[18], &s.ventMax,      4);
    std::memcpy(&buf[22], &s.nestOffset,   4);
    std::memcpy(&buf[26], &s.roomOffset,   4);
    std::memcpy(&buf[30], &s.ventOffset,   4);
    buf[34] = s.wifiMode;
    std::memcpy(&buf[35], s.wifiSSID, sizeof(s.wifiSSID));   // 33
    std::memcpy(&buf[68], s.wifiPass, sizeof(s.wifiPass));   // 65
    // copy into 桩 EEPROM.mem
    std::memcpy(EEPROM.mem, buf, sizeof(buf));
}

// ============ [MIRROR] loadState —— 镜像 main.cpp:392-419 ============
static CfgState loadStateLogic() {
    CfgState s;            // 代码默认值
    if (EEPROM.mem[0] != 0xAA) return s;
    s.systemOn = EEPROM.mem[1] == 1;
    std::memcpy(&s.targetTemp,  &EEPROM.mem[2],  4);
    std::memcpy(&s.hysteresis,  &EEPROM.mem[6],  4);
    std::memcpy(&s.safeMin,     &EEPROM.mem[10], 4);
    std::memcpy(&s.safeMax,     &EEPROM.mem[14], 4);
    std::memcpy(&s.ventMax,     &EEPROM.mem[18], 4);
    // clamp ventMax 镜像
    if (s.ventMax < 40.0f) s.ventMax = 40.0f;
    if (s.ventMax > 80.0f) s.ventMax = 80.0f;
    std::memcpy(&s.nestOffset,  &EEPROM.mem[22], 4);
    std::memcpy(&s.roomOffset, &EEPROM.mem[26], 4);
    std::memcpy(&s.ventOffset, &EEPROM.mem[30], 4);
    s.wifiMode = EEPROM.mem[34];
    if (s.wifiMode > 1) s.wifiMode = 0;
    std::memcpy(s.wifiSSID, &EEPROM.mem[35], sizeof(s.wifiSSID));
    std::memcpy(s.wifiPass, &EEPROM.mem[68], sizeof(s.wifiPass));
    if (s.wifiSSID[0] == '\0' || (uint8_t)s.wifiSSID[0] == 0xFF) {
        std::strcpy(s.wifiSSID, "OPhone 12");
        std::strcpy(s.wifiPass, "qwer1234");
    }
    return s;
}

// ============ [MIRROR] API constrain 范围 —— 镜像 handleControl() main.cpp:296-365 ============
static float clampTargetTemp(float v) { return v < 10.0f ? 10.0f : (v > 40.0f ? 40.0f : v); }
static float clampHyst(float v)        { return v < 0.01f ? 0.01f : (v > 3.0f ? 3.0f : v); }
static float clampSafeMin(float v)    { return v < 0.0f ? 0.0f : (v > 20.0f ? 20.0f : v); }
static float clampSafeMax(float v)    { return v < 20.0f ? 20.0f : (v > 50.0f ? 50.0f : v); }
static float clampVentMax(float v)    { return v < 40.0f ? 40.0f : (v > 80.0f ? 80.0f : v); }
static float clampOffset(float v)     { return v < -5.0f ? -5.0f : (v > 5.0f ? 5.0f : v); }

// ============ setUp / tearDown 统一由 test_pure_logic.cpp 提供（EEPROM.begin 在其中） ============

// ============ save/load 往返测试 ============
void test_save_load_roundtrip_all_fields(void) {
    CfgState src;
    src.systemOn = true;
    src.targetTemp = 15.0f; src.hysteresis = 0.25f;
    src.safeMin = 3.0f; src.safeMax = 40.0f; src.ventMax = 60.0f;
    src.nestOffset = -0.7f; src.roomOffset = 0.2f; src.ventOffset = 1.1f;
    src.wifiMode = 1;
    std::strcpy(src.wifiSSID, "MyLab");
    std::strcpy(src.wifiPass, "pw12345");

    saveStateLogic(src);
    CfgState dst = loadStateLogic();

    TEST_ASSERT_TRUE(dst.systemOn);
    TEST_ASSERT_EQUAL_FLOAT(src.targetTemp, dst.targetTemp);
    TEST_ASSERT_EQUAL_FLOAT(src.hysteresis,  dst.hysteresis);
    TEST_ASSERT_EQUAL_FLOAT(src.safeMin,     dst.safeMin);
    TEST_ASSERT_EQUAL_FLOAT(src.safeMax,     dst.safeMax);
    TEST_ASSERT_EQUAL_FLOAT(src.ventMax,     dst.ventMax);
    TEST_ASSERT_EQUAL_FLOAT(src.nestOffset,  dst.nestOffset);
    TEST_ASSERT_EQUAL_FLOAT(src.roomOffset,  dst.roomOffset);
    TEST_ASSERT_EQUAL_FLOAT(src.ventOffset,  dst.ventOffset);
    TEST_ASSERT_EQUAL_INT(src.wifiMode, dst.wifiMode);
    TEST_ASSERT_EQUAL_STRING(src.wifiSSID, dst.wifiSSID);
    TEST_ASSERT_EQUAL_STRING(src.wifiPass, dst.wifiPass);
}

// ============ loadState 默认路径（无有效标记） ============
void test_load_default_when_no_marker(void) {
    std::memset(EEPROM.mem, 0xFF, 256);
    CfgState s = loadStateLogic();
    TEST_ASSERT_FALSE(s.systemOn);
    TEST_ASSERT_EQUAL_FLOAT(28.0f, s.targetTemp);
}

// ============ loadState ventMax 越界 clamp ============
void test_load_ventmax_out_of_range_clamped(void) {
    CfgState src; src.ventMax = 99.0f;
    saveStateLogic(src);
    CfgState dst = loadStateLogic();
    TEST_ASSERT_EQUAL_FLOAT(80.0f, dst.ventMax);
}

// ============ loadState wifiMode 越界 reset ============
void test_load_wifimode_invalid_reset(void) {
    CfgState src; src.wifiMode = 7;
    saveStateLogic(src);
    CfgState dst = loadStateLogic();
    TEST_ASSERT_EQUAL_INT(0, dst.wifiMode);
}

// ============ loadState SSID 0xFF 回退默认 ============
void test_load_ssid_flash_blank_default(void) {
    std::memset(EEPROM.mem, 0xFF, 256);
    EEPROM.mem[0] = 0xAA;  // 有效但 SSID 未写过
    CfgState s = loadStateLogic();
    TEST_ASSERT_EQUAL_STRING("OPhone 12", s.wifiSSID);
    TEST_ASSERT_EQUAL_STRING("qwer1234", s.wifiPass);
}

// ============ API constrain 边界 ============
void test_clamp_targetTemp_boundaries(void) {
    TEST_ASSERT_EQUAL_FLOAT(10.0f, clampTargetTemp(-5.0f));
    TEST_ASSERT_EQUAL_FLOAT(10.0f, clampTargetTemp(9.99f));
    TEST_ASSERT_EQUAL_FLOAT(40.0f, clampTargetTemp(50.0f));
    TEST_ASSERT_EQUAL_FLOAT(28.0f, clampTargetTemp(28.0f));
}
void test_clamp_ventMax_boundaries(void) {
    TEST_ASSERT_EQUAL_FLOAT(40.0f, clampVentMax(30.0f));   // ✱ README 旧文本 30，代码下限 40
    TEST_ASSERT_EQUAL_FLOAT(80.0f, clampVentMax(100.0f));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, clampVentMax(50.0f));
}
void test_clamp_hysteresis_boundaries(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.01f, clampHyst(0.001f));
    TEST_ASSERT_EQUAL_FLOAT(3.0f, clampHyst(5.0f));
}
void test_clamp_offset_signed(void) {
    TEST_ASSERT_EQUAL_FLOAT(-5.0f, clampOffset(-10.0f));
    TEST_ASSERT_EQUAL_FLOAT(5.0f, clampOffset(7.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, clampOffset(0.0f));
}

// 注（2026-09-10 收尾鎖版）：以下三項當時未補，鎖版凍結不再重構，保留現狀。
//                                       非数字输入 toFloat() 返回 0 被错误 clamp 到下限（见 CODE_REVIEW）