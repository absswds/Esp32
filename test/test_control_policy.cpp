// PlatformIO Unity 测试骨架：controlTemp 状态机策略
//
// **此文件为骨架**（skeleton）。完整跑通需要把 src/main.cpp 中的 controlTemp() 拆分到
// lib/teccore/control.cpp 并暴露纯函数接口；当前因 src/main.cpp 含 WebServer/OneWire/
// ledc 等 Arduino API 依赖无法直接 #include 到 native 编译。
// 详见 docs/CODE_REVIEW.md "结构改进" 节与 docs/ARCHITECTURE.md §3.3。
//
// ===== 需要 mock 的部分 =====
//     setFan(int)        — 替换为：把 PWM 值记录到 g_mock_fan（已在此文件用 lambda 代替）
//     setTecPwm(...)    — 替换为：把 cool/heat PWM 记录到 g_mock_cool/g_mock_heat
//     setTec(...)       — 同上（手動通道，不在本骨架范围）
//     digitalWrite(TEC_EN, ...) — 已有 mocks/Arduino.h 桩
//     Serial.printf(...)       — 已有桩（无操作）
//     emergencyStop/saveState  — 此处用桩计数（不写真实 EEPROM）
// ===========================

#include <unity.h>
#include <cmath>
#include <cstdio>
#include <cstdint>

// ============ 简化 mock：记录 setFan / setTecPwm 输出 ============
static int g_mock_fan = -1;
static struct { int cool; int heat; } g_mock_tec { -1, -1 };
static int g_mock_emergency = 0;
static int g_mock_savecalls = 0;

// 供统一 setUp（test_pure_logic.cpp）在每用例前重置本文件 mock 状态
void resetControlMocks(void) {
    g_mock_fan = -1;
    g_mock_tec.cool = -1;
    g_mock_tec.heat = -1;
    g_mock_emergency = 0;
    g_mock_savecalls = 0;
}

static void mock_setFan(int s) { g_mock_fan = s; }
static void mock_setTecPwm(float power, bool isCool) {
    int pwm = (int)(power * 1023); if (pwm < 0) pwm = 0; if (pwm > 1023) pwm = 1023;
    if (isCool) { g_mock_tec.cool = pwm; g_mock_tec.heat = 0; }
    else        { g_mock_tec.cool = 0; g_mock_tec.heat = pwm; }
}
static void mock_emergencyStop() { g_mock_emergency++; }
static void mock_saveState() { g_mock_savecalls++; }

// ============ 反映 controlTemp 的全局决策输入 ============
// 镜像 src/main.cpp 中被 controlTemp 直接读取的全局变量集合。
struct ControlInputs {
    bool systemOn = false;
    bool tecManual = false;
    bool manualMode = false;
    float nestT = NAN, roomT = NAN, ventT = NAN;
    float targetTemp = 28.0f, hysteresis = 0.5f;
    float safeMin = 5.0f, safeMax = 35.0f, ventMax = 50.0f;
    bool fanManual = false;
    // cooling/heating / coolingHeating 标志位（用于检测"刚切出"）—— 简化
};

// ============ [MIRROR] controlTemp 决策核心 —— 镜像 main.cpp:154-222 ============
// 警告：这是为了可测性的简化复刻；真正源码同步需用 diff 校验两版差异。
static void controlTempLogic(ControlInputs& st, int& nanCount) {
    if (!st.systemOn || st.tecManual || st.manualMode) return;
    // guard 0: 任一 NAN → nanCount++
    if (std::isnan(st.nestT) || std::isnan(st.roomT) || std::isnan(st.ventT)) {
        nanCount++;
        if (nanCount >= 3) { mock_emergencyStop(); mock_saveState(); }
        return;
    } else if (nanCount > 0) {
        nanCount = 0;
    }
    // guard 1: 出風口过热
    if (st.ventT >= st.ventMax) {
        mock_emergencyStop(); mock_saveState();
        return;
    }
    // guard 2: 巢穴极端
    if (st.nestT < st.safeMin || st.nestT > st.safeMax) {
        mock_setTecPwm(0, true);
        if (!st.fanManual) mock_setFan(st.nestT > st.safeMax ? 255 : 60);
        return;
    }
    // 热节流
    float throttle = 1.0f;
    if (st.ventT > st.ventMax - 10) {
        throttle = st.ventMax - st.ventT;
        if (throttle < 0) throttle = 0; if (throttle > 1) throttle = 1;
        throttle = throttle / 10.0f * (10.0f); // (ventMax-ventT)/10
        // 简化重算：(ventMax - ventT) / 10
        throttle = (st.ventMax - st.ventT) / 10.0f;
        if (throttle < 0) throttle = 0; if (throttle > 1) throttle = 1;
    }
    // bang-bang
    if (st.nestT > st.targetTemp + st.hysteresis) {
        mock_setTecPwm(throttle, true);
        if (!st.fanManual) mock_setFan(255);
    } else if (st.nestT < st.targetTemp - st.hysteresis) {
        mock_setTecPwm(throttle, false);
        if (!st.fanManual) mock_setFan(255);
    } else {
        mock_setTecPwm(0, true);
        if (!st.fanManual) mock_setFan(0);
    }
}

// ============ setUp / tearDown 统一由 test_pure_logic.cpp 提供 ============

// ============ 测试用例 ============

// guard 0：systemOff → 无动作
void test_noop_when_system_off(void) {
    ControlInputs st; st.systemOn = false;
    int nan = 0;
    controlTempLogic(st, nan);
    TEST_ASSERT_EQUAL_INT(-1, g_mock_fan);
    TEST_ASSERT_EQUAL_INT(-1, g_mock_tec.cool);
}

// guard 0：tecManual → 无动作
void test_noop_when_tecManual(void) {
    ControlInputs st; st.systemOn = true; st.tecManual = true;
    int nan = 0;
    controlTempLogic(st, nan);
    TEST_ASSERT_EQUAL_INT(-1, g_mock_fan);
}

// guard 0：manualMode → 无动作
void test_noop_when_manualMode(void) {
    ControlInputs st; st.systemOn = true; st.manualMode = true;
    int nan = 0;
    controlTempLogic(st, nan);
    TEST_ASSERT_EQUAL_INT(-1, g_mock_fan);
}

// guard 0：任一 NAN，1~2 次不动作，3 次紧急
void test_nan_combust_after_three(void) {
    ControlInputs st; st.systemOn = true;
    st.nestT = NAN; st.roomT = 25; st.ventT = 25;
    int nan = 0;
    controlTempLogic(st, nan); TEST_ASSERT_EQUAL_INT(0, g_mock_emergency);
    controlTempLogic(st, nan); TEST_ASSERT_EQUAL_INT(0, g_mock_emergency);
    controlTempLogic(st, nan); TEST_ASSERT_EQUAL_INT(1, g_mock_emergency);
    TEST_ASSERT_EQUAL_INT(1, g_mock_savecalls);
}

// guard 1：出風口过热 → 紧急
void test_vent_overtemp_trigger(void) {
    ControlInputs st; st.systemOn = true;
    st.nestT = 28; st.roomT = 26; st.ventT = 51; st.ventMax = 50;
    int nan = 0;
    controlTempLogic(st, nan);
    TEST_ASSERT_EQUAL_INT(1, g_mock_emergency);
}

// guard 2：巢穴过冷 → 风扇 60
void test_nest_below_safeMin_fan60(void) {
    ControlInputs st; st.systemOn = true;
    st.nestT = 3; st.roomT = 20; st.ventT = 25; st.safeMin = 5;
    int nan = 0;
    controlTempLogic(st, nan);
    TEST_ASSERT_EQUAL_INT(0, g_mock_tec.cool);     // TEC 关
    TEST_ASSERT_EQUAL_INT(60, g_mock_fan);
}

// guard 2：巢穴过热 → 风扇 255
void test_nest_above_safeMax_fan255(void) {
    ControlInputs st; st.systemOn = true;
    st.nestT = 36; st.roomT = 26; st.ventT = 30; st.safeMax = 35;
    int nan = 0;
    controlTempLogic(st, nan);
    TEST_ASSERT_EQUAL_INT(255, g_mock_fan);
}

// 制冷分支
void test_cooling_branch(void) {
    ControlInputs st; st.systemOn = true;
    st.nestT = 30; st.targetTemp = 28; st.hysteresis = 0.5;   // > 28.5 制冷
    st.roomT = 26; st.ventT = 25;
    int nan = 0;
    controlTempLogic(st, nan);
    TEST_ASSERT_TRUE(g_mock_tec.cool > 0);
    TEST_ASSERT_EQUAL_INT(0, g_mock_tec.heat);
    TEST_ASSERT_EQUAL_INT(255, g_mock_fan);
}

// 加热分支
void test_heating_branch(void) {
    ControlInputs st; st.systemOn = true;
    st.nestT = 26; st.targetTemp = 28; st.hysteresis = 0.5;   // < 27.5 加热
    st.roomT = 26; st.ventT = 25;
    int nan = 0;
    controlTempLogic(st, nan);
    TEST_ASSERT_TRUE(g_mock_tec.heat > 0);
    TEST_ASSERT_EQUAL_INT(0, g_mock_tec.cool);
}

// 维持死区 → TEC 关、风扇关
void test_deadzone_idle(void) {
    ControlInputs st; st.systemOn = true;
    st.nestT = 28.0; st.targetTemp = 28.0; st.hysteresis = 0.5;
    st.roomT = 26; st.ventT = 25;
    int nan = 0;
    controlTempLogic(st, nan);
    TEST_ASSERT_EQUAL_INT(0, g_mock_tec.cool);
    TEST_ASSERT_EQUAL_INT(0, g_mock_tec.heat);
    TEST_ASSERT_EQUAL_INT(0, g_mock_fan);
}

// 热节流：ventT 接近 ventMax → 功率 < full
void test_thermal_throttle(void) {
    ControlInputs st; st.systemOn = true;
    st.nestT = 30; st.targetTemp = 28; st.hysteresis = 0.5;   // 制冷
    st.roomT = 26; st.ventT = 47; st.ventMax = 50;            // ventT 在 (40,50] 内
    int nan = 0;
    controlTempLogic(st, nan);
    // throttle = (50-47)/10 = 0.3 → cool PWM ≈ 306 (0.3*1023)
    TEST_ASSERT_INT_WITHIN(20, 306, g_mock_tec.cool);
}

// fanManual → 制冷不变更风扇写
void test_fan_manual_blocks_setFan(void) {
    ControlInputs st; st.systemOn = true; st.fanManual = true;
    st.nestT = 30; st.targetTemp = 28; st.hysteresis = 0.5;
    st.roomT = 26; st.ventT = 25;
    int nan = 0;
    controlTempLogic(st, nan);
    TEST_ASSERT_EQUAL_INT(-1, g_mock_fan);
}