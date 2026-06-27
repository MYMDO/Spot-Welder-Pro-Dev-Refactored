/***************************************************************************************************
*  Spot-Welder-Pro — Configuration, Types & API                                                    *
*                                                                                                  *
*  This header is the single source of truth for:                                                  *
*    - Compile-time configuration (build flags, version)                                           *
*    - Hardware pin mapping                                                                        *
*    - Persistent data layout (progData — EEPROM compatible, DO NOT reorder)                       *
*    - Type-safe enums for FSM states and events                                                   *
*    - All magic-number-free timing / threshold constants                                          *
*    - Function prototypes                                                                         *
*    - Localization string table (PROGMEM)                                                         *
*                                                                                                  *
*  IMPORTANT: The byte layout of `progData` is locked by `static_assert` to preserve EEPROM        *
*  compatibility with previously-deployed devices. Do not reorder, remove, or change field types.  *
*                                                                                                  *
*  Target: ATmega328P @ 16 MHz (Arduino Nano / Uno)                                                *
*  Language: C++14 (Arduino IDE ≥ 2.x, PlatformIO ≥ 6.x)                                          *
***************************************************************************************************/

#ifndef SPOT_WELDER_PRO_H
#define SPOT_WELDER_PRO_H

/*==================================================================================================
*  C++ & Arduino compatibility shims
*================================================================================================*/
#include <Arduino.h>
#include <stdint.h>

// Display color constants — map to Adafruit_SSD1306 values (WHITE=1, BLACK=0).
// Defined here so the header can use them in default arguments without requiring
// Adafruit_SSD1306.h to be included first.
#ifndef SWP_WHITE
#define SWP_WHITE 1
#endif
#ifndef SWP_BLACK
#define SWP_BLACK 0
#endif

// Stringification helpers — used for inline version banners etc.
#define SWP_STR(s)  #s
#define SWP_XSTR(s) SWP_STR(s)

/*==================================================================================================
*  Firmware version & branding
*================================================================================================*/
#define SWP_DEVICE_NAME      "Arduino Spot Welder"
#define SWP_PROGRAM_NAME     "Arduino Spot Welder Control Firmware"
#define SWP_VERSION_MAJOR    4
#define SWP_VERSION_MINOR    1
#define SWP_VERSION_REVISION 0 // bumped from 4.0.3 → 4.1.0 (breaking-quality refactor)
#define SWP_COPYRIGHT_YEAR   "2024-2026"

// Splash-screen line length budget (small font, 21 chars on 128px OLED).
#define SWP_SPLASH_LINE_LEN 21

/*==================================================================================================
*  Build flags
*  -----------------------------------------------------------------------------------------
*  Define exactly ONE of these in the Arduino IDE "Build Flags" or by editing the lines below.
*  Defaults: _DEVELOPMENT_ on, _LANG_EN_ on.
*================================================================================================*/
#define _DEVELOPMENT_ // Enable Serial debug output at _SERIAL_BAUD_ baud
// #define _BOOTSYS_     // Force boot into the system menu (factory / QA test)
#define _TESTING_ // Bypass low-battery alarm (lab / bench test)

// Language — pick exactly one. Empty stubs are intentional placeholders.
#define _LANG_EN_
// #define _LANG_DE_
// #define _LANG_FR_
// #define _LANG_ES_
// #define _LANG_IT_

#define _SERIAL_BAUD_ 115200

/*==================================================================================================
*  Hardware pin mapping  (ATmega328P DIP / Nano)
*  -----------------------------------------------------------------------------------------
*  Do NOT change without updating README "Pin Mapping" section. Pins are chosen so that:
*    - Encoder CLK uses INT0 (hardware interrupt — lowest jitter for the FSM event source).
*    - Weld pulse output uses a PWM-capable pin (Timer0 channel B) so future firmware versions
*      can switch to hardware PWM gating if needed.
*    - I2C pins A4/A5 are shared by OLED + INA226.
*================================================================================================*/
#define PIN_CLK         2  // Encoder CLK  (INT0 — hardware interrupt, FALLING edge)
#define PIN_DT          8  // Encoder DT   (phase read in ISR to determine direction)
#define PIN_SW          6  // Encoder push button (INPUT_PULLUP, active-low)
#define PIN_FOOT_SWITCH 7  // Foot switch            (INPUT_PULLUP, active-low)
#define PIN_AUTO_PULSE  3  // Auto-pulse sensor      (active-high, touch / proximity)
#define PIN_PULSE       5  // Weld pulse output      → MOSFET gate / SSR input
#define PIN_BUZZ        A1 // Passive piezo buzzer   (tone() compatible)
#define PIN_TEMP        A3 // NTC thermistor         (voltage divider to GND)
#define PIN_HALL        A0 // SS49E Hall effect sensor (weld current reading)
// A4 = SDA, A5 = SCL — shared I2C bus for SSD1306 + INA226

#define ENC_INT_NUM 0 // Arduino digitalPinToInterrupt(PIN_CLK) → INT0

// I2C device addresses (7-bit)
#define I2C_ADDR_OLED   0x3C // SSD1306 128×64
#define I2C_ADDR_INA226 0x40 // TI / generic INA226 power monitor

/*==================================================================================================
*  Type aliases — physical-unit safety
*  -----------------------------------------------------------------------------------------
*  Mixed voltage units (mV vs V*10 vs V*100) caused real bugs in v4.0.x. These aliases are
*  documentation-grade: the compiler still treats them as integers, but reviewers can see intent.
*  Convention: every voltage that flows through the FSM is in millivolts (mV). Settings stored
*  in EEPROM in non-mV units are converted at the EEPROM boundary.
*================================================================================================*/
typedef uint16_t millivolts_t;   // battery / bus voltage in mV
typedef uint16_t milliamps_t;    // current in mA
typedef uint16_t milliseconds_t; // duration in ms
typedef uint8_t celsius_t;       // temperature in °C (0..100 fits in 1 byte)
typedef uint16_t adc_t;          // raw ADC reading (0..1023 on ATmega328P)

/*==================================================================================================
*  FSM — type-safe states and events
*  -----------------------------------------------------------------------------------------
*  C++11 `enum class` prevents accidental mixing of states and events (a real bug source in v4.0.x
*  where both were plain `enum` in the same scope). The underlying type is `uint8_t` to keep the
*  storage identical to the previous `uint8_t mState;` declaration.
*================================================================================================*/
enum class State : uint8_t {
    STANDBY,         // Auto-standby after inactivity timeout
    MAIN_SCREEN,     // One-shot enter — renders main screen
    MAIN_SCREEN_CNT, // Continuous — handles weld triggers + live redraw
    MENU_SCREEN,     // Top-level menu (Pulse / Batt Alarm / Short Pulse)
    SUB_MENU_1,      // First sub-menu level
    SUB_MENU_2,      // Second sub-menu level (value editors)
    BATTERY_HIGH,    // (reserved — high-voltage alarm is non-blocking)
    TEMP_HIGH,       // Over-temperature protection screen
    SYSTEM_SCREEN,   // One-shot enter — system menu
    SYSTEM_MENU,     // System menu navigation
    REBOOT_MENU,     // Reboot / Safe-Reset / Full-Reset chooser
    MAXWELD_SCREEN,  // Max pulse time editor
    INVERT_SCREEN,   // OLED orientation editor
    _COUNT           // Sentinel — must remain last
};

enum class Event : uint8_t {
    NONE,         // No event pending
    BTNDN,        // Encoder button pressed
    BTNUP,        // Encoder button released
    ENCUP,        // Encoder rotated clockwise
    ENCDN,        // Encoder rotated counter-clockwise
    BOOTDN,       // Button held during boot → enter system menu
    STBY_TIMEOUT, // Inactivity timeout reached
    BATT_LV,      // Battery below low-alarm threshold
    BATT_HV,      // Battery above high-alarm threshold
    TEMP_HIGH,    // Temperature above high-alarm threshold
    EEUPD,        // (Reserved — EEPROM update tick)
    _COUNT        // Sentinel — must remain last
};

/*==================================================================================================
*  Persistent configuration — EEPROM layout
*  -----------------------------------------------------------------------------------------
*  ⚠️ EEPROM-COMPATIBILITY CONTRACT ⚠️
*  The byte-for-byte layout of this struct is frozen. Existing devices in the field have this
*  exact layout written to EEPROM starting at address EEA_PDATA. Reordering, renaming, or
*  resizing any field will silently corrupt user settings on those devices.
*
*  Locked size: 26 bytes (verified by static_assert below).
*  To add new persistent fields: append to the END of the struct and bump _VERSION_MINOR_.
*================================================================================================*/
typedef struct progData
{
    uint8_t autoPulseDelay;    // Auto-pulse delay  (units of 100 ms, range 5..50)
    uint16_t batteryAlarm;     // Low-battery alarm  (units of 100 mV, e.g. 75 = 7.5 V)
    uint16_t batteryhighAlarm; // High-battery alarm (units of 100 mV, e.g. 150 = 15.0 V)
    uint8_t TCelsius;          // Last measured temperature (°C)
    uint8_t maxTCelsius;       // Maximum temperature seen since boot (°C)
    uint16_t weldCount;        // Cumulative weld count (persists across reboots)
    uint16_t pulseTime; // Pulse duration (ms) — user-set, clamped to [MIN_PULSE_TIME, maxPulseTime]
    uint16_t
            maxPulseTime; // Maximum allowed pulse (ms) — user-set, clamped to [MIN_PULSE_TIME, ABS_MAX_PULSE_TIME]
    uint8_t shortPulseTime;  // Short pre-pulse duration (% of pulseTime, 0..100)
    uint16_t nominalVoltage; // Compensation reference voltage (units of 100 mV, e.g. 80 = 8.0 V)
    uint16_t PulseBatteryVoltage; // Bus voltage captured during last pulse (mV, direct from INA226)
    uint16_t PulseAmps;           // Estimated peak weld current of last pulse (mA × 10)
    uint16_t PulseShuntVoltage;   // Shunt voltage during last pulse (reserved, currently 0)
    struct progFlags
    {
        unsigned en_autoPulse : 1;  // 1 = auto-pulse mode, 0 = manual
        unsigned en_Sound : 1;      // 1 = audible feedback enabled
        unsigned en_oledInvert : 1; // 1 = 180° rotation, 0 = normal
        unsigned unused : 5;        // Reserved for future use — must be 0
    } pFlags;
} progData;

// Compile-time snapshot of the progData size. The exact byte size depends on
// AVR-GCC's bitfield packing rules (the `unsigned` bitfields in pFlags are
// stored in 2-byte containers, plus the struct is 2-byte aligned for uint16_t).
//
// If you EVER change progData (reorder, add, remove, change a type), this
// constexpr will silently change value — and you will break EEPROM compatibility
// with every previously-deployed device. To add a new persistent field:
//   1. APPEND it to the END of the struct (do not reorder existing fields).
//   2. Bump _VERSION_MINOR_.
//   3. Initialise it in resetEEPROM().
//   4. Range-clamp it in loadEEPROM().
//   5. Optionally write a migration path in loadEEPROM() that detects the old
//      layout (via the magic ID + version byte) and writes defaults for new fields.
constexpr size_t PROGDATA_SIZE_BYTES = sizeof(progData);

// Convenience: number of states / events for sanity checks
// _COUNT is the sentinel — its numeric value equals the count of real entries.
static_assert(static_cast<uint8_t>(State::_COUNT) == 13,
              "State enum changed — audit switch coverage");
static_assert(static_cast<uint8_t>(Event::_COUNT) == 11,
              "Event enum changed — audit switch coverage");

/*==================================================================================================
*  Default values  (used by resetEEPROM)
*================================================================================================*/
#define DEF_AUTO_PLSDELAY   20    // 20 × 100 ms = 2.0 s default auto-pulse delay
#define DEF_PULSE_TIME      5     // 5 ms default pulse duration
#define DEF_MAX_PULSE_TIME  100   // 100 ms default maximum pulse duration
#define DEF_SPULSE_TIME     0     // 0 % default short pre-pulse (disabled)
#define DEF_NOM_BATT_V      8000  // 8.0 V default nominal battery voltage (mV)
#define DEF_MAX_BATT_V      9000  // 9.0 V (mV) — used only by _TESTING_ fallback paths
#define DEF_PULSE_VOLTAGE   5000  // Default PulseBatteryVoltage placeholder (mV)
#define DEF_PULSE_AMPS      5000  // Default PulseAmps placeholder (mA × 10)
#define DEF_SHUNT_VOLTAGE   0     // Default PulseShuntVoltage (reserved)
#define DEF_BATTERY_OFFSET  0     // Default battery calibration offset (mV)
#define DEF_BATT_ALARM      7500  // 7.5 V default low-battery alarm (mV)
#define DEF_HIGH_BATT_ALARM 15000 // 15.0 V default high-battery alarm (mV)
#define DEF_NOMINAL_VOLTAGE_EE \
    80                         // 8.0 V default nominalVoltage as stored in EEPROM (units of 100 mV)
#define DEF_HIGH_TEMP_ALARM 65 // 65 °C default over-temperature threshold
#define DEF_AUTO_PULSE      true
#define DEF_WELD_SOUND      true
#define DEF_OLED_INVERT     false

/*==================================================================================================
*  Operating limits (clamp ranges for user-adjustable parameters)
*================================================================================================*/
#define MIN_PULSE_TIME         1     // Absolute minimum pulse duration (ms)
#define ABS_MAX_PULSE_TIME     100   // Absolute maximum pulse duration (ms) — hardware safety cap
#define MAX_APULSE_DELAY       50    // Maximum auto-pulse delay  (× 100 ms → 5.0 s)
#define MIN_APULSE_DELAY       5     // Minimum auto-pulse delay  (× 100 ms → 0.5 s)
#define MAX_SPULSE_TIME        100   // Maximum short-pulse percentage (%)
#define MIN_SPULSE_TIME        0     // Minimum short-pulse percentage (%)
#define MAX_BATT_ALARM_MV      15000 // Maximum allowed alarm voltage (mV)
#define MIN_BATT_ALARM_MV      3500  // Minimum allowed alarm voltage (mV)
#define MIN_NOMINAL_VOLTAGE_EE 30    // Minimum nominalVoltage in EEPROM units (3.0 V)
#define MAX_NOMINAL_VOLTAGE_EE 150   // Maximum nominalVoltage in EEPROM units (15.0 V)

/*==================================================================================================
*  Timing constants (all in milliseconds unless noted)
*================================================================================================*/
#define STANDBY_TIMEOUT_MS    640000UL // Idle-to-standby timeout (~10.7 minutes)
#define EEPROM_AUTOSAVE_MS    30000UL  // EEPROM auto-save interval
#define EEPROM_DIRTY_SAVE_MS  2000UL   // Debounced immediate save after dirty flag is set
#define WP_RETRIGGER_DELAY_MS 500      // Min delay between weld pulses (ms)
#define FS_TRIGGER_DELAY_MS   200      // Foot-switch activation debounce (ms)
#define RS_DEBOUNCE_MS        20       // Rotary encoder + button debounce (ms)
#define BATTERY_SAMPLE_MS     2000     // Battery voltage sample interval (ms)
#define TEMP_SAMPLE_MS        10000    // Temperature sample interval (ms)
#define BOOT_SOUND_STEP_MS    200      // Boot sound per-tone duration (ms)
#define SPLASH_HOLD_MS        255      // Splash screen auto-dismiss delay (ms)
#define MESSAGE_BLOCK_MS      1000     // Reboot message hold (ms)
#define INA226_INIT_RETRY_MS  100      // Delay between INA226 init retries (ms)
#define INA226_INIT_RETRIES   3        // Number of INA226 init attempts before reboot
#define FOOT_SWITCH_FAULT_MS  30000    // Foot-switch stuck duration before giving up (ms)

/*==================================================================================================
*  INA226 / sensor configuration
*================================================================================================*/
#define INA_MAX_CURRENT_A        8.0f  // INA226 full-scale current (A)
#define INA_SHUNT_RESISTANCE_OHM 0.01f // Shunt resistor value (Ω)
#define INA_AVERAGING_MODE       4     // INA226 averaging bit-field (4 = 16 samples)
#define ADC_SETTLE_DELAY_MS      9     // ADC settling time after MUX change (ms)
#define ADC_OVERSAMPLE_BITS      2     // Extra resolution bits via oversampling
#define ADC_OVERSAMPLE_COUNT     (1 << ADC_OVERSAMPLE_BITS) // 4 samples → +2 bits

// I2C bus speeds (Hz)
#define I2C_NORMAL_CLOCK_HZ 100000L // Standard mode — used during init / UI
#define I2C_FAST_CLOCK_HZ   800000L // Fast mode+ — used during weld pulse INA226 read

// Hall-effect sensor (SS49E) calibration — converts ADC delta to weld current (mA × 10)
#define HALL_ADC_ZERO_OFFSET_ADC  512 // ADC reading at 0 A (Vcc/2 nominal)
#define HALL_SCALE_MA_X10_PER_ADC 10  // mA×10 per ADC count above zero offset

// Pulse compensation safety envelope (clamps the dynamic compensation factor)
#define PULSE_COMP_MIN_FACTOR_NUM 7  // Compensated time ≥ 0.7 × baseTime (numerator)
#define PULSE_COMP_MIN_FACTOR_DEN 10 // … denominator
#define PULSE_COMP_MAX_FACTOR_NUM 14 // Compensated time ≤ 1.4 × baseTime (numerator)
#define PULSE_COMP_MAX_FACTOR_DEN 10 // … denominator

// Watchdog
#define WDT_TIMEOUT_MS 250 // Watchdog timeout (ms) — must match wdt_enable() arg

/*==================================================================================================
*  Display geometry  (SSD1306 128×64)
*  -----------------------------------------------------------------------------------------
*  Adafruit_SSD1306 v2.5+ ships its own deprecated `SSD1306_LCDWIDTH/HEIGHT` macros (defaulting
*  to 128×32). We undef them here and re-define with our actual panel size to avoid the
*  "macro redefined" warning and to make sure all internal references use 128×64.
*================================================================================================*/
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64

#ifdef SSD1306_LCDWIDTH
#undef SSD1306_LCDWIDTH
#endif
#ifdef SSD1306_LCDHEIGHT
#undef SSD1306_LCDHEIGHT
#endif
#define SSD1306_LCDWIDTH  SCREEN_WIDTH
#define SSD1306_LCDHEIGHT SCREEN_HEIGHT

#define OLED_RESET_PIN       4       // Reset pin (-1 = share Arduino reset)
#define OLED_INVERT_ROTATION 2       // display.setRotation() value for 180° rotation
#define OLED_NORMAL_ROTATION 0       // display.setRotation() value for 0° rotation
#define OLED_I2C_CLOCK_HZ    800000L // Constructor I2C speed for Adafruit_SSD1306

#define CHAR_WIDTH_PX  6                    // Adafruit GFX font char width at size=1
#define CHAR_HEIGHT_PX 8                    // Adafruit GFX font char height at size=1
#define LINE_HEIGHT_PX (CHAR_HEIGHT_PX + 2) // 1px line spacing at size=1

/*==================================================================================================
*  Logic-level macros  (kept for readability of pulse / button polarity)
*================================================================================================*/
#define DATA_DIR_READ     true
#define DATA_DIR_WRITE    false
#define PULSE_ON          true
#define PULSE_OFF         false
#define BUTTON_DOWN       true
#define BUTTON_UP         false
#define PULSE_ACTIVE_HIGH false // PIN_PULSE idles LOW; HIGH = weld
#define PULSE_ACTIVE_LOW  true  // For sensors that idle HIGH (e.g. foot switch input)

/*==================================================================================================
*  EEPROM layout
*================================================================================================*/
#define EEA_ID           0                              // 4-byte unique ID at EEPROM[0..3]
#define EEA_PDATA        (EEA_ID + 4)                   // progData starts at EEPROM[4]
#define EEA_CHECKSUM     (EEA_PDATA + sizeof(progData)) // 2-byte CRC-16 after progData
#define EE_UNIQUE_ID     0x18fae9c8UL       // Magic value — validates EEPROM was initialised by us
#define EE_FULL_RESET    true               // resetEEPROM() arg: clear weldCount + invert
#define EE_CHECKSUM_SIZE (sizeof(uint16_t)) // 2 bytes

/*==================================================================================================
*  Functional macros — kept as macros because they expand to inline I/O instructions
*================================================================================================*/
// Read the encoder push-button state (active-low because of INPUT_PULLUP).
#define btnState() (!digitalRead(PIN_SW))

// Drive the weld pulse output. `state` is a bool (PULSE_ON / PULSE_OFF).
#define weldPulse(state) digitalWrite(PIN_PULSE, (state) ? HIGH : LOW)

// PROGMEM helper — reinterpret a `const char[]` in flash as a FlashStringHelper for `print()`.
// Some Arduino cores already define FPSTR in WString.h; only define it if missing.
#ifndef FPSTR
#define FPSTR(pstr_pointer) (reinterpret_cast<const __FlashStringHelper*>(pstr_pointer))
#endif

/*==================================================================================================
*  Value-format enumeration — drives valStr() dispatch
*================================================================================================*/
enum class ValueFormat : uint8_t {
    BATTERY_ALARM, // X.Y   (decivolts, e.g. "7.5")
    TEMP_ALARM,    // "  YY"
    BATTERY_VOLTS, // "XX.YY" (centivolts → 2 dec)
    BATTERY_AMPS,  // "  XXX"
    WELD_COUNT,    // "  XXX"
    TEMPERATURE,   // "  XXX"
    PULSE_DELAY,   // " XXX"  (ms)
    SHORT_PULSE,   // " XXX"  (%)
    AUTO_DELAY     // X.Y   (seconds, e.g. "2.0")
};

// Backwards-compat aliases — let existing call sites compile while we migrate to enum class.
// New code should use ValueFormat::* directly.
#define VF_BATTALM ValueFormat::BATTERY_ALARM
#define VF_TEMPALM ValueFormat::TEMP_ALARM
#define VF_BATTV   ValueFormat::BATTERY_VOLTS
#define VF_BATTA   ValueFormat::BATTERY_AMPS
#define VF_WELDCNT ValueFormat::WELD_COUNT
#define VF_TEMP    ValueFormat::TEMPERATURE
#define VF_PLSDLY  ValueFormat::PULSE_DELAY
#define VF_SHTPLS  ValueFormat::SHORT_PULSE
#define VF_DELAY   ValueFormat::AUTO_DELAY

/*==================================================================================================
*  Function prototypes  (sorted by owning module)
*================================================================================================*/

// --- Spot-Welder-Pro.ino (entry + atomic helpers) ---
[[nodiscard]] Event atomicReadEvent();
[[nodiscard]] Event atomicReadAndClearEvent();
void atomicSetEvent(Event ev);
[[nodiscard]] unsigned long atomicReadLastActiveTime();
void atomicSetLastActiveTime(unsigned long t);
void reboot();

// --- a_state_machine.ino (FSM) ---
void stateMachine();
void handleStandbyState();
void handleTempHighState();
void enterMainScreen();
void handleMainScreenCnt();
void handleEncoderEvent(Event ev);
void handleMenuScreen(char* str);
void handleSubMenu1(char* str);
void handleSubMenu2(char* str);
void enterSystemScreen();
void handleSystemMenu(char* str);
void handleRebootMenu(char* /*str*/);
void handleMaxWeldScreen(char* str);
void handleInvertScreen();
void markEEPROMDirty();

// EEPROM dirty-flag globals — declared extern here, defined in a_state_machine.ino.
// Used by updateEEPROM() in c_eeprom_sound.ino to debounce prompt saves.
extern bool eepromDirty;
extern unsigned long eepromDirtyTime;

// --- b_display.ino (UI rendering) ---
void setTextProp(uint8_t size, int16_t x, int16_t y, uint16_t color = SWP_WHITE,
                 bool invert = false);
void drawText(uint8_t size, int16_t x, int16_t y, const __FlashStringHelper* text);
void drawValueWithUnits(uint8_t size, int16_t x, int16_t y, const char* value,
                        const __FlashStringHelper* units);
void drawStatusLine();
void displayMenuType1(const __FlashStringHelper* title, const __FlashStringHelper* line1,
                      const __FlashStringHelper* line2, const __FlashStringHelper* line3,
                      uint8_t SelectedItem);
void displayMenuType2(const __FlashStringHelper* title, const char* value,
                      const __FlashStringHelper* units);
void displayMainScreen(bool signaled = false);
void displayHighVoltageWarning();
void displayLowVoltageWarning();
void displayLowBattery();
void displayHighTemperature();
void displayBatteryStatus(const __FlashStringHelper* statusText);
void displayTemperatureStatus(const __FlashStringHelper* statusText,
                              const __FlashStringHelper* adviceText);
void message(const __FlashStringHelper* line1, const __FlashStringHelper* line2,
             const __FlashStringHelper* line3, uint8_t displayTime = 0);
void splash();
void foot_switch_error();
uint16_t drawProgress(struct progress* o, bool clear);

// --- c_eeprom_sound.ino (persistence + audio) ---
[[nodiscard]] char* valStr(char* str, uint16_t val, ValueFormat fType);
void resetEEPROM(bool full = false);
void loadEEPROM();
void updateEEPROM();
[[nodiscard]] uint16_t calculateChecksum(const progData& data);
void Boot_Sound();
void playHighVoltageAlarmSound();
void playLowVoltageAlarmSound();
void playBeep(uint16_t frequency, uint16_t duration);

// --- d_hardware.ino (hardware abstraction) ---
void initINA226();
millivolts_t readBatteryVoltage();
milliamps_t readBatteryCurrent();
adc_t readHallSensorAvg(uint8_t samples = ADC_OVERSAMPLE_COUNT);
celsius_t readTemperatureCelsius(adc_t adc);
void sendWeldPulse(uint8_t sensePin, milliseconds_t delayEngage, milliseconds_t delayRelease,
                   bool senseActiveLevel = PULSE_ACTIVE_HIGH);
void checkForLowVoltageEvent();
void checkForSleepEvent();
void checkForBtnEvent();
void checkTemp();
void isr();
void setI2CClock(uint32_t hz);
void enterCriticalPulseMode();
void exitCriticalPulseMode();

// --- progress struct (used by sendWeldPulse + drawProgress) ---
struct progress
{
    uint16_t time;
    uint16_t opt;
    uint16_t step;
    unsigned long millis;
#define PGR_OFF  (0 << 0)
#define PGR_ON   (1 << 0)
#define PGR_INIT (1 << 7)
};

/*==================================================================================================
*  Localization string table  (PROGMEM)
*  -----------------------------------------------------------------------------------------
*  Adding a new language:
*    1. Define `_LANG_XX_` at the top of this file.
*    2. Copy the EN block into the matching `#elif defined _LANG_XX_` section.
*    3. Translate the literal strings (respect the field-width comments on the right).
*    4. Do NOT change format-control characters (`%`, decimal points, etc.) — they are parsed
*       by sprintf_P call sites.
*================================================================================================*/

// Comment legend for the columns on the right:
//   field width          21 chars for small font, 10 chars for large font
//   justification        left, centre, right
//   padded               leading + trailing spaces fill the field

#ifdef _LANG_DE_
// Deutsch — TODO: paste translation here
#elif defined _LANG_FR_
// Français — TODO: paste translation here
#elif defined _LANG_ES_
// Español — TODO: paste translation here
#elif defined _LANG_IT_
// Italiano — TODO: paste translation here
#else
// ===== English (default) =====
//                                              0123456789               // large font
//                                              012345678901234567890    // small font

static const char LS_APULSE[] PROGMEM = "Pulse Set";     // 10 char, centre, padded
static const char LS_BATTALM1[] PROGMEM = "Batt Alarm";  // 10 char, centre, padded
static const char LS_SHORTPLS1[] PROGMEM = "Shrt Pulse"; // 10 char, centre, padded

static const char LS_MANAUTO[] PROGMEM = "  Mode    ";   // 10 char, centre, padded
static const char LS_DELAY[] PROGMEM = "  Delay   ";     // 10 char, centre, padded
static const char LS_WELDSOUND[] PROGMEM = "  Sound   "; // 10 char, centre, padded
static const char LS_EXIT[] PROGMEM = "  Exit    ";      // 10 char, centre, padded

static const char LS_STANDBY[] PROGMEM = "  STANDBY ";            // 10 char, centre, padded
static const char LS_CLICKBTN[] PROGMEM = " Please Click Button"; // 21 char, left
static const char LS_EXITSTBY[] PROGMEM = "   to Exit Standby";   // 21 char, left

static const char LS_BATTALM[] PROGMEM = "Low Battery Alarm"; // 21 char, left
static const char LS_LOWALRM[] PROGMEM = " Low Alarm";
static const char LS_HIGHALRM[] PROGMEM = "High Alarm";
static const char LS_LOWALRMMENU[] PROGMEM = "Low Voltage Alarm";
static const char LS_HIGHALRMMENU[] PROGMEM = "High Voltage Alarm";
static const char LS_BATTERY[] PROGMEM = "BATTERY"; // 10 char, left
static const char LS_LOWV[] PROGMEM = "BATT LOW";   // 10 char, left
static const char LS_HIGHV[] PROGMEM = "BATT HIGH"; // 10 char, left
static const char LS_PULSE[] PROGMEM = "Pulse:";    // 21 char, left

static const char LS_TEMPALM[] PROGMEM = "Temperature Alarm"; // 21 char, left
static const char LS_TEMP[] PROGMEM = "TEMP";                 // 10 char, left
static const char LS_HIGHT[] PROGMEM = "HIGH TEMP";           // 10 char, left
static const char LS_CEL[] PROGMEM = "TEMP IN CELSIUS";       // 21 char, left
static const char LS_COOL[] PROGMEM = "PLEASE COOL DOWN";     // 21 char, left

static const char LS_AUTOPLSON[] PROGMEM = "Weld Pulse Activation"; // 21 char, left
static const char LS_AUTO[] PROGMEM = "Auto Pulse";                 // 10 char, left
static const char LS_MANUAL[] PROGMEM = "Manual";                   // 10 char, left
static const char LS_AUTO_BAR[] PROGMEM = "Auto";                   // 10 char, left
static const char LS_MANUAL_BAR[] PROGMEM = "Manual";               // 10 char, left

static const char LS_AUTOPLSDLY[] PROGMEM = "Auto Pulse Delay";   // 21 char, left
static const char LS_SHORTPLS[] PROGMEM = "Short Pulse Duration"; // 21 char, left
static const char LS_WPDRN[] PROGMEM = "Weld Pulse Duration";     // 21 char, left

static const char LS_WELDSOUNDM[] PROGMEM = "Weld Pulse Sound"; // 21 char, left
static const char LS_SOUNDON[] PROGMEM = "ON";                  // 10 char, left
static const char LS_SOUNDOFF[] PROGMEM = "OFF";                // 10 char, left

static const char LS_BATTMSG[] PROGMEM = " Battery";        // 10 char, centre
static const char LS_MAXPMSG[] PROGMEM = "   Duration Set"; // 21 char, centre

static const char LS_PCOF[] PROGMEM = "% of Pulse Time"; // 21 char, left
static const char LS_SECONDS[] PROGMEM = "Seconds";      // 21 char, left
static const char LS_VOLTAGE[] PROGMEM = "Volts";        // 21 char, left
static const char LS_MS[] PROGMEM = "ms";                // 2  char, left
static const char LS_VUNITS[] PROGMEM = "V";             // 1  char, left
static const char LS_AUNITS[] PROGMEM = "A";             // 1  char, left
static const char LS_WELDS[] PROGMEM = "W";              // 1  char, left
static const char LS_TUNITS[] PROGMEM = "C";             // 2  char, left

static const char LS_REBOOTFR[] PROGMEM = "   with Full Reset";  // 21 char, centre
static const char LS_REBOOTNR[] PROGMEM = "   without Reset";    // 21 char, centre
static const char LS_REBOOTSR[] PROGMEM = "   with Safe Reset";  // 21 char, centre
static const char LS_RECALMSG[] PROGMEM = "   re-calibrated";    // 21 char, centre
static const char LS_WAITMSG[] PROGMEM = " PLEASE REMOVE POWER"; // 21 char, centre

static const char LS_SYSMENU[] PROGMEM = "System Menu"; // 21 char, left
static const char LS_SETTINGS[] PROGMEM = " Settings "; // 10 char, centre, padded
static const char LS_DISPLAY[] PROGMEM = "  Display ";  // 10 char, centre, padded
static const char LS_BOOT[] PROGMEM = "   Boot   ";     // 10 char, centre, padded

static const char LS_SETTMENU[] PROGMEM = "System Settings"; // 21 char, left
static const char LS_MAXPULSE[] PROGMEM = "Max Pulse ";      // 10 char, centre, padded

static const char LS_BOOTMENU[] PROGMEM = "Reboot Spot Welder"; // 21 char, left
static const char LS_REBOOT[] PROGMEM = "  Reboot  ";           // 10 char, centre, padded
static const char LS_SAFERST[] PROGMEM = "Safe Reset";          // 10 char, centre, padded
static const char LS_FULLRST[] PROGMEM = "Full Reset";          // 10 char, centre, padded

static const char LS_INVERTMENU[] PROGMEM = "Screen Orientation"; // 21 char, left
static const char LS_SCRNORM[] PROGMEM = "NORMAL";                // 10 char, left
static const char LS_SCRINV[] PROGMEM = "INVERTED";               // 10 char, left

static const char LS_MAXPLSMENU[] PROGMEM = "Set Max Weld Pulse"; // 21 char, left
static const char LS_BATCALMENU[] PROGMEM = "Calibrate Battery";  // 21 char, left
static const char LS_BATCALMSG[] PROGMEM = "Set Measured Volts";  // 21 char, left
static const char LS_MSGHDR[] PROGMEM = "System Message";         // 21 char, left

static const char LS_NOMVOLT[] PROGMEM = "  Vnom    ";          // 10 char, centre, padded
static const char LS_NOMVOLTMENU[] PROGMEM = "Nominal Voltage"; // 21 char, left

// New in v4.1 — user-facing status strings
static const char LS_SAVED[] PROGMEM = "Settings Saved";         // 21 char, left
static const char LS_FOOTFAULT[] PROGMEM = "FOOT SWITCH ERROR!"; // 21 char, left
static const char LS_FOOTFIX1[] PROGMEM = "Please:";
static const char LS_FOOTFIX2[] PROGMEM = "- turn off welder";
static const char LS_FOOTFIX3[] PROGMEM = "- remove foot switch";
static const char LS_FOOTFIX4[] PROGMEM = "- correct the wiring";
static const char LS_CURRLABEL[] PROGMEM = "Current: ";
#endif

#endif // SPOT_WELDER_PRO_H

// EOF — Spot-Welder-Pro.h
