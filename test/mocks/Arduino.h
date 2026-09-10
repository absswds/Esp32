// 最小 Arduino.h 桩头（仅 native 单元测试用）—— header-only
// 提供测试骨架引用 Arduino 类型/宏所需的最低定义；不实现任何真实硬件行为。
// 配合 test/README_TESTS.md 阅读各 mock 的边界。
//
// 设计时刻意做成 header-only：PlatformIO native 默认只编译 test/test_*.cpp，
// 不自动编译 mocks/*.cpp；inline 变量/函数避免多重定义同时单一全局实例。
#ifndef UNIT_TEST_ARDUINO_STUB_H
#define UNIT_TEST_ARDUINO_STUB_H

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

// ---- Arduino 基本类型 ----
typedef uint8_t byte;
typedef bool boolean;
#define HIGH 1
#define LOW  0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

// ---- String 桩 ----
class String {
public:
    String() = default;
    String(const char* s) : buf_(s ? s : "") {}
    const char* c_str() const { return buf_.c_str(); }
    size_t length() const { return buf_.size(); }
    String& operator=(const char* s) { buf_ = s ? s : ""; return *this; }
    int toInt() const { return (int)std::strtol(buf_.c_str(), nullptr, 10); }
    float toFloat() const { return (float)std::strtod(buf_.c_str(), nullptr); }
    String substring(int from) const { return String(buf_.substr(from).c_str()); }
    String substring(int from, int to) const { return String(buf_.substr(from, to - from).c_str()); }
    int indexOf(const char* s) const { auto p = buf_.find(s); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const String& s) const { return indexOf(s.c_str()); }
    operator const char*() const { return c_str(); }
private:
    std::string buf_;
};

// ---- Serial 桩 ----
struct _SerialStub {
    void begin(long) {}
    void printf(const char*, ...) {}
    void print(const char*) {}
    void print(const String&) {}
    void println(const char* = "") {}
    void println(const String&) {}
};
inline _SerialStub Serial;

// ---- LEDC / GPIO 桩 ----
struct LedcCall { uint8_t channel; int value; };
inline LedcCall g_ledc_last { 0xFF, 0 };
inline uint8_t g_digital[40] { 0 };
inline uint8_t g_pin_mode[40] { 0 };
inline uint32_t g_fake_millis = 0;

inline uint32_t millis() { return g_fake_millis; }
inline void setMillis(uint32_t v) { g_fake_millis = v; }

inline void pinMode(uint8_t pin, uint8_t mode) { if (pin < 40) g_pin_mode[pin] = mode; }
inline void digitalWrite(uint8_t pin, uint8_t value) { if (pin < 40) g_digital[pin] = value; }
inline int digitalRead(uint8_t pin) { return pin < 40 ? g_digital[pin] : 0; }
inline void delay(unsigned long /*ms*/) {}

inline void ledcSetup(uint8_t, uint32_t, uint8_t) {}
inline void ledcAttachPin(uint8_t, uint8_t) {}
inline void ledcWrite(uint8_t channel, int value) { g_ledc_last.channel = channel; g_ledc_last.value = value; }

// ---- constrain 模版 ----
template<class T> T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
template<class T> T min(T a, T b) { return a < b ? a : b; }
template<class T> T max(T a, T b) { return a > b ? a : b; }

// ---- EEPROM 桩（256 字节内存映像） ----
struct _EEPROMStub {
    static const size_t SIZE = 256;
    uint8_t mem[SIZE];
    bool begun = false;
    void begin(size_t /*sz*/) { begun = true; std::memset(mem, 0xFF, SIZE); }
    uint8_t read(size_t addr) { return mem[addr]; }
    void write(size_t addr, uint8_t v) { mem[addr] = v; }
    template<class T> void put(size_t addr, const T& v) { std::memcpy(&mem[addr], &v, sizeof(T)); }
    template<class T> void get(size_t addr, T& v) { std::memcpy(&v, &mem[addr], sizeof(T)); }
    void commit() {}
    void end() {}
};
inline _EEPROMStub EEPROM;

#endif // UNIT_TEST_ARDUINO_STUB_H