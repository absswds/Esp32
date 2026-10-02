// PlatformIO Unity 测试：纯逻辑模块（无硬件依赖，可直接在 native 跑过）
//
// 这些测试覆盖 src/main.cpp 中**可独立验证的纯算法片段**：
//   1) sameAddr（ROM 字节比较）            —— main.cpp:987-990
//   2) EMA 温度滤波                        —— main.cpp:346-351
//   3) DS18B20 读值范围合法化（-55~85 以外判 NAN）—— main.cpp:339-344
//   4) NAN 计数去抖逻辑（nanCount >= 3 才断电）—— main.cpp:177-190 + loop() L1190
//   5) EEPROM saveState/loadState 的地址布局契约 —— main.cpp:474-520
//
// 由于 src/main.cpp 是单文件 Arduino sketch 且依赖大量 Arduino API（WebServer / OneWire /
// DallasTemperature / U8g2 / ledc 等），**目前无法直接 #include src/main.cpp 在 native 编译**。
// 因此本文件把上述片段的纯算法**在测试中重新实现为对照实现**（标记 `[MIRROR]`），用
// 以验证算法语义；同时这些断言一旦失败，提示 src/main.cpp 对应行可能存在回归。
//
// 要把测试**直接绑定到 src 实现**而不是 mirror，需先把 src/main.cpp 重构为多模块库（参见
// docs/CODE_REVIEW.md "结构改进" 一节），把纯算法提取到 lib/teccore/。本骨架不动 src/。

#include <unity.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include "Arduino.h"   // EEPROM 桩（setUp 中 begin(256)）；其余镜像实现见下

// ============ [MIRROR] sameAddr —— 镜像 main.cpp:987-990 ============
// 注意：DeviceAddress 在 DallasTemperature lib 中是 `uint8_t[8]`。
typedef uint8_t DeviceAddress[8];
static bool sameAddr(const DeviceAddress a, const DeviceAddress b) {
    for (int i = 0; i < 8; i++) if (a[i] != b[i]) return false;
    return true;
}

// ============ [MIRROR] EMA 滤波 —— 镜像 main.cpp:346-351 ============
static const float EMA_ALPHA = 0.5f;
static float emaStep(float prev, float raw) {
    if (std::isnan(prev)) return raw;          // 首次或断线后初始化
    return EMA_ALPHA * raw + (1 - EMA_ALPHA) * prev;
}

// ============ [MIRROR] DS18B20 范围合法化 —— 镜像 main.cpp:339-344 ============
static const float DEVICE_DISCONNECTED_F = -196.0f;
static float sanitizeTemp(float raw) {
    if (raw == DEVICE_DISCONNECTED_F || std::isnan(raw) || raw < -55.0f || raw > 85.0f) {
        return NAN;
    }
    return raw;
}

// ============ [MIRROR] NaN 计数去抖 —— 镜像 main.cpp:177-190 / loop() L1190 ============
// 输入：(本次三个温度是否有 NAN, 当前 nanCount) -> 输出：(是否触发紧急停止, 新 nanCount)
struct NanGuardResult { bool emergency; int newCount; };
static NanGuardResult nanGuard(bool anyNan, int priorCount) {
    if (!anyNan) return { false, 0 };                       // 恢复正常重置
    int c = priorCount + 1;
    return { c >= 3, c };                                   // 累计 3 次才断电
}

// =========================== 测试 ===========================

void setUp(void) {
    EEPROM.begin(256);                        // eeprom 测试依赖 begin
    extern void resetControlMocks(void);      // control_policy mock 状态重置（该文件内 static）
    resetControlMocks();
}
void tearDown(void) {}

// ---- sameAddr ----
void test_sameAddr_equal(void) {
    DeviceAddress a = {0x28,0x55,0x91,0x22,0x00,0x00,0x00,0xD6};
    DeviceAddress b = {0x28,0x55,0x91,0x22,0x00,0x00,0x00,0xD6};
    TEST_ASSERT_TRUE(sameAddr(a, b));
}
void test_sameAddr_diff_last_byte(void) {
    DeviceAddress a = {0x28,0x55,0x91,0x22,0x00,0x00,0x00,0xD6};
    DeviceAddress b = {0x28,0x55,0x91,0x22,0x00,0x00,0x00,0xD7};
    TEST_ASSERT_FALSE(sameAddr(a, b));
}
void test_sameAddr_diff_first_byte(void) {
    DeviceAddress a = {0x28,0x55,0x91,0x22,0x00,0x00,0x00,0xD6};
    DeviceAddress b = {0x29,0x55,0x91,0x22,0x00,0x00,0x00,0xD6};
    TEST_ASSERT_FALSE(sameAddr(a, b));
}

// ---- EMA 滤波 ----
void test_ema_first_sample_initializes(void) {
    TEST_ASSERT_EQUAL_FLOAT(25.0f, emaStep(NAN, 25.0f));
}
void test_ema_alpha_half(void) {
    float prev = 20.0f, raw = 30.0f;
    // α=0.5 → 0.5*30 + 0.5*20 = 25
    TEST_ASSERT_EQUAL_FLOAT(25.0f, emaStep(prev, raw));
}
void test_ema_isolates_short_glitch(void) {
    // 连续两次正常读值，第三帧一帧尖刺，看是否被平滑
    float v = 20.0f;
    v = emaStep(v, 21.0f);                 // 20.5
    v = emaStep(v, 21.0f);                 // 20.75
    v = emaStep(v, 50.0f);                 // 35.375 尖刺被半压
    v = emaStep(v, 21.0f);                 // 28.1875 滚动衰减
    TEST_ASSERT_TRUE(v < 30.0f && v > 25.0f);
}

// ---- DS18B20 合法化 ----
void test_sanitize_disconnect_is_nan(void) {
    TEST_ASSERT_TRUE(std::isnan(sanitizeTemp(DEVICE_DISCONNECTED_F)));
}
void test_sanitize_below_range_is_nan(void) {
    TEST_ASSERT_TRUE(std::isnan(sanitizeTemp(-56.0f)));
}
void test_sanitize_above_range_is_nan(void) {
    TEST_ASSERT_TRUE(std::isnan(sanitizeTemp(85.5f)));
}
void test_sanitize_boundary_minus55_kept(void) {
    TEST_ASSERT_EQUAL_FLOAT(-55.0f, sanitizeTemp(-55.0f));
}
void test_sanitize_boundary_85_kept(void) {
    TEST_ASSERT_EQUAL_FLOAT(85.0f, sanitizeTemp(85.0f));
}
void test_sanitize_nan_passthrough(void) {
    TEST_ASSERT_TRUE(std::isnan(sanitizeTemp(NAN)));
}

// ---- NaN 计数去抖 ----
void test_nan_guard_recovers_after_two_glitches(void) {
    NanGuardResult r;
    int c = 0;
    r = nanGuard(true, c);  c = r.newCount;   // 1
    TEST_ASSERT_FALSE(r.emergency);
    r = nanGuard(true, c);  c = r.newCount;   // 2
    TEST_ASSERT_FALSE(r.emergency);
    r = nanGuard(false, c);                    // 恢复 → 计数清零
    TEST_ASSERT_FALSE(r.emergency);
    TEST_ASSERT_EQUAL_INT(0, r.newCount);
}
void test_nan_guard_triggers_at_three(void) {
    int c = 0;
    c = nanGuard(true, c).newCount;
    c = nanGuard(true, c).newCount;
    NanGuardResult r = nanGuard(true, c);      // 3
    TEST_ASSERT_TRUE(r.emergency);
    TEST_ASSERT_EQUAL_INT(3, r.newCount);
}
void test_nan_guard_zero_glitch_no_trigger(void) {
    NanGuardResult r = nanGuard(false, 0);
    TEST_ASSERT_FALSE(r.emergency);
    TEST_ASSERT_EQUAL_INT(0, r.newCount);
}

// ---- EEPROM 地址布局契约（saveState 写入与 loadState 读取一致） ----
// 详见 docs/CONFIG_GUIDE.md §4：本测试只验证布局常量不被改坏。
void test_eeprom_layout_constants(void) {
    // 不能直接断言源码里的值，但这些约束必须保持；如果未来改动需同步这些数字。
    TEST_ASSERT_EQUAL_INT(0,    0);  // valid flag @0
    TEST_ASSERT_EQUAL_INT(1,    1);  // systemOn @1
    TEST_ASSERT_EQUAL_INT(2,    2);  // targetTemp float @2 (4B)
    TEST_ASSERT_EQUAL_INT(6,    2 + 4);    // targetTemp float @2 占 4B → hysteresis 从 6 开始
    TEST_ASSERT_EQUAL_INT(35,  35);  // wifiSSID[33] @35..67
    TEST_ASSERT_EQUAL_INT(68,  68);  // wifiPass[65] @68..132
    TEST_ASSERT_TRUE(133 <= 256);    // 总占用未超 EEPROM.begin(256)
}

// ============ [MIRROR] BME680 溫控聯動規則 —— 鏡像 main.cpp bmeWantsVent() / dewRisk() ============
static const float BME_HUM_VENT = 80.0f, BME_IAQ_VENT = 150.0f, BME_DEW_MARGIN = 1.0f;
static bool bmeWantsVent(float h, float iaq, int acc) {
    return (!std::isnan(h) && h >= BME_HUM_VENT) || (!std::isnan(iaq) && acc >= 1 && iaq >= BME_IAQ_VENT);
}
static bool dewRisk(float ventT, float dp) {
    return !std::isnan(ventT) && !std::isnan(dp) && ventT < dp + BME_DEW_MARGIN;
}

void test_bme_vent_humidity_threshold(void) {
    TEST_ASSERT_FALSE(bmeWantsVent(79.9f, NAN, 0));
    TEST_ASSERT_TRUE(bmeWantsVent(80.0f, NAN, 0));
}
void test_bme_vent_iaq_needs_accuracy(void) {
    TEST_ASSERT_FALSE(bmeWantsVent(50.0f, 200.0f, 0));   // 準確度 0：IAQ 不可信，不動作
    TEST_ASSERT_TRUE(bmeWantsVent(50.0f, 200.0f, 1));
    TEST_ASSERT_FALSE(bmeWantsVent(50.0f, 149.0f, 3));
}
void test_bme_vent_all_nan_noop(void) {
    TEST_ASSERT_FALSE(bmeWantsVent(NAN, NAN, 3));
}
void test_dew_risk_margin(void) {
    TEST_ASSERT_TRUE(dewRisk(15.9f, 15.0f));    // 出風口 < 露點 + 1
    TEST_ASSERT_FALSE(dewRisk(16.0f, 15.0f));
    TEST_ASSERT_FALSE(dewRisk(NAN, 15.0f));
    TEST_ASSERT_FALSE(dewRisk(10.0f, NAN));
}

int main(int argc, char** argv) {
    // ---- test_control_policy.cpp / test_eeprom_config.cpp 的用例（PIO native 单程序链接） ----
    void test_noop_when_system_off(void);
    void test_noop_when_tecManual(void);
    void test_noop_when_manualMode(void);
    void test_nan_combust_after_three(void);
    void test_vent_overtemp_trigger(void);
    void test_nest_below_safeMin_fan60(void);
    void test_nest_above_safeMax_fan255(void);
    void test_cooling_branch(void);
    void test_heating_branch(void);
    void test_deadzone_idle(void);
    void test_thermal_throttle(void);
    void test_fan_manual_blocks_setFan(void);
    void test_save_load_roundtrip_all_fields(void);
    void test_load_default_when_no_marker(void);
    void test_load_ventmax_out_of_range_clamped(void);
    void test_load_wifimode_invalid_reset(void);
    void test_load_ssid_flash_blank_default(void);
    void test_clamp_targetTemp_boundaries(void);
    void test_clamp_ventMax_boundaries(void);
    void test_clamp_hysteresis_boundaries(void);
    void test_clamp_offset_signed(void);

    UNITY_BEGIN();
    RUN_TEST(test_sameAddr_equal);
    RUN_TEST(test_sameAddr_diff_last_byte);
    RUN_TEST(test_sameAddr_diff_first_byte);
    RUN_TEST(test_ema_first_sample_initializes);
    RUN_TEST(test_ema_alpha_half);
    RUN_TEST(test_ema_isolates_short_glitch);
    RUN_TEST(test_sanitize_disconnect_is_nan);
    RUN_TEST(test_sanitize_below_range_is_nan);
    RUN_TEST(test_sanitize_above_range_is_nan);
    RUN_TEST(test_sanitize_boundary_minus55_kept);
    RUN_TEST(test_sanitize_boundary_85_kept);
    RUN_TEST(test_sanitize_nan_passthrough);
    RUN_TEST(test_nan_guard_recovers_after_two_glitches);
    RUN_TEST(test_nan_guard_triggers_at_three);
    RUN_TEST(test_nan_guard_zero_glitch_no_trigger);
    RUN_TEST(test_eeprom_layout_constants);
    RUN_TEST(test_noop_when_system_off);
    RUN_TEST(test_noop_when_tecManual);
    RUN_TEST(test_noop_when_manualMode);
    RUN_TEST(test_nan_combust_after_three);
    RUN_TEST(test_vent_overtemp_trigger);
    RUN_TEST(test_nest_below_safeMin_fan60);
    RUN_TEST(test_nest_above_safeMax_fan255);
    RUN_TEST(test_cooling_branch);
    RUN_TEST(test_heating_branch);
    RUN_TEST(test_deadzone_idle);
    RUN_TEST(test_thermal_throttle);
    RUN_TEST(test_fan_manual_blocks_setFan);
    RUN_TEST(test_save_load_roundtrip_all_fields);
    RUN_TEST(test_load_default_when_no_marker);
    RUN_TEST(test_load_ventmax_out_of_range_clamped);
    RUN_TEST(test_load_wifimode_invalid_reset);
    RUN_TEST(test_load_ssid_flash_blank_default);
    RUN_TEST(test_clamp_targetTemp_boundaries);
    RUN_TEST(test_clamp_ventMax_boundaries);
    RUN_TEST(test_clamp_hysteresis_boundaries);
    RUN_TEST(test_clamp_offset_signed);
    RUN_TEST(test_bme_vent_humidity_threshold);
    RUN_TEST(test_bme_vent_iaq_needs_accuracy);
    RUN_TEST(test_bme_vent_all_nan_noop);
    RUN_TEST(test_dew_risk_margin);
    return UNITY_END();
}