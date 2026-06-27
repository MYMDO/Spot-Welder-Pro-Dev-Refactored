// Minimal Arduino stub for syntax-checking Spot-Welder-Pro firmware.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <algorithm>
using std::max;
using std::min;

// --- Arduino core types & macros ---
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define FALLING 2
#define RISING 3
#define CHANGE 1
#define WHITE 1
#define BLACK 0
#define SSD1306_SWITCHCAPVCC 1
#define WDTO_15MS 0
#define WDTO_250MS 1
#define A0 14
#define A1 15
#define A2 16
#define A3 17
#define A4 18
#define A5 19

#define HEX 16
#define DEC 10

typedef uint8_t byte;
typedef bool boolean;
typedef const char* PGM_P;

class __FlashStringHelper;
#define F(x) (reinterpret_cast<const __FlashStringHelper*>(x))
#define FPSTR(x) (reinterpret_cast<const __FlashStringHelper*>(x))
#define PSTR(x) (x)
#define PROGMEM
inline uint16_t pgm_read_word(const uint16_t* p) { return *p; }
inline uint8_t  pgm_read_byte(const uint8_t* p)  { return *p; }
inline size_t strlen_P(PGM_P s) { return std::strlen(s); }
inline int sprintf_P(char* buf, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    int r = std::vsnprintf(buf, 64, fmt, args);
    va_end(args); return r;
}

// String class (just enough to compile)
class String {
public:
    String() {}
    String(const char*) {}
};

// --- Arduino API stubs ---
void pinMode(uint8_t, uint8_t);
void digitalWrite(uint8_t, uint8_t);
int  digitalRead(uint8_t);
int  analogRead(uint8_t);
void attachInterrupt(uint8_t, void(*)(), uint8_t);
unsigned long millis();
unsigned long micros();
void delay(unsigned long);
void delayMicroseconds(unsigned int);
void tone(uint8_t, unsigned int, unsigned long);
void noTone(uint8_t);
void cli();
void sei();
extern uint8_t SREG;
uint8_t SREG = 0;
extern volatile uint8_t MCUSR_reg;
volatile uint8_t MCUSR_reg = 0;
#define MCUSR MCUSR_reg

// Serial
class Serial_ {
public:
    void begin(unsigned long) {}
    void print(const char*) {}
    void print(int) {}
    void print(int, int) {}
    void print(unsigned int) {}
    void print(unsigned int, int) {}
    void print(unsigned long) {}
    void print(unsigned long, int) {}
    void print(long) {}
    void print(long, int) {}
    void print(char) {}
    void print(const __FlashStringHelper*) {}
    void println() {}
    void println(const char*) {}
    void println(int) {}
    void println(unsigned int) {}
    void println(unsigned long) {}
    void println(unsigned long, int) {}
    void println(long) {}
    void println(const __FlashStringHelper*) {}
};
extern Serial_ Serial;

// --- AVR wdt.h stub ---
namespace avr_wdt_stub {
    inline void wdt_enable(uint8_t) {}
    inline void wdt_disable() {}
    inline void wdt_reset() {}
}
#define wdt_enable avr_wdt_stub::wdt_enable
#define wdt_disable avr_wdt_stub::wdt_disable
#define wdt_reset avr_wdt_stub::wdt_reset

// --- Wire library stub ---
class TwoWire {
public:
    void begin() {}
    void setClock(uint32_t) {}
    void beginTransmission(uint8_t) {}
    uint8_t endTransmission() { return 0; }
};
extern TwoWire Wire;

// --- Adafruit GFX + SSD1306 stub ---
class Adafruit_SSD1306 {
public:
    Adafruit_SSD1306(int, int, TwoWire*, int, long) {}
    bool begin(uint8_t, uint8_t) { return true; }
    void clearDisplay() {}
    void display() {}
    void setTextColor(uint16_t) {}
    void setTextColor(uint16_t, uint16_t) {}
    void setCursor(int16_t, int16_t) {}
    void setTextSize(uint8_t) {}
    size_t print(const char*) { return 0; }
    size_t print(const __FlashStringHelper*) { return 0; }
    size_t print(int) { return 0; }
    size_t print(unsigned int) { return 0; }
    size_t print(unsigned long) { return 0; }
    size_t print(long) { return 0; }
    size_t print(char) { return 0; }
    void drawLine(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
    void drawRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
    void fillRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
    void setRotation(uint8_t) {}
};

// --- EEPROM stub ---
class EEPROMClass {
public:
    template<typename T> void put(int, const T&) {}
    template<typename T> void get(int, T&) {}
    int length() { return 1024; }
};
extern EEPROMClass EEPROM;

// --- INA226 stub ---
class INA226 {
public:
    INA226(uint8_t) {}
    bool begin() { return true; }
    void setMaxCurrentShunt(float, float) {}
    void setAverage(uint8_t) {}
    uint16_t getBusVoltage_mV() { return 8000; }
    int32_t getCurrent_mA() { return 0; }
};
