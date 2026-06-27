/***************************************************************************************************
*  Spot-Welder-Pro — Firmware entry point                                                          *
*                                                                                                  *
*  Responsibilities of this file:                                                                  *
*    - Define global objects (INA226, Adafruit_SSD1306)                                            *
*    - Define global runtime state (mState, mEvent, batteryVoltage, alarm flags)                   *
*    - Atomic access helpers for ISR-shared variables (mEvent, lastActiveTime)                     *
*    - setup() — pin directions, I2C, OLED, INA226, EEPROM, splash, WDT                            *
*    - loop()  — WDT kick, FSM tick, EEPROM tick                                                   *
*    - isr()   — encoder interrupt (FALLING edge on PIN_CLK)                                       *
*                                                                                                  *
*  All hardware-specific functions live in d_hardware.ino.                                         *
*  All FSM logic lives in a_state_machine.ino.                                                     *
*  All rendering lives in b_display.ino.                                                           *
*  All persistence + audio lives in c_eeprom_sound.ino.                                            *
***************************************************************************************************/

#define _Spot_Welder_Pro

#include <avr/wdt.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>
#include <INA226.h>
#include "Spot-Welder-Pro.h"

/*==================================================================================================
*  Global objects
*================================================================================================*/
INA226 INA(I2C_ADDR_INA226);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN, OLED_I2C_CLOCK_HZ);

/*==================================================================================================
*  Global runtime state
*  -----------------------------------------------------------------------------------------
*  Only state that must be visible across modules lives here. Everything else is file-local
*  `static` to keep the global namespace lean.
*================================================================================================*/
progData    pData;                                // Persistent settings (mirrored in EEPROM)
State       mState         = State::MAIN_SCREEN;  // Current FSM state
celsius_t   TCelsius       = 0;                   // Last measured temperature (°C)
millivolts_t batteryVoltage = DEF_NOM_BATT_V;     // Last measured battery voltage (mV)

bool        sysMenu                = false;       // True while inside the system menu tree
bool        highVoltageAlarmActive = false;       // Non-blocking HV alarm flag (display-only)
bool        lowVoltageAlarmActive  = false;       // Non-blocking LV alarm flag (display-only)

// Volatile — written by ISR, read by main loop. Access ONLY through the atomic helpers below.
volatile unsigned long lastActiveTime = 0;
volatile Event         mEvent         = Event::NONE;

/*==================================================================================================
*  Atomic access helpers  (ISR ↔ main-loop contract)
*  -----------------------------------------------------------------------------------------
*  ATmega328P is an 8-bit MCU — single-byte reads/writes are atomic, but multi-byte and
*  read-modify-write operations are not. The encoder ISR writes both `mEvent` (1 byte) and
*  `lastActiveTime` (4 bytes). The main loop reads+clears `mEvent` and reads `lastActiveTime`.
*  Without protection, an ISR firing between a 4-byte load and store would corrupt the value.
*================================================================================================*/
[[nodiscard]] Event atomicReadEvent() {
    cli();
    Event ev = mEvent;
    sei();
    return ev;
}

[[nodiscard]] Event atomicReadAndClearEvent() {
    cli();
    Event ev = mEvent;
    mEvent = Event::NONE;
    sei();
    return ev;
}

void atomicSetEvent(Event ev) {
    cli();
    mEvent = ev;
    sei();
}

[[nodiscard]] unsigned long atomicReadLastActiveTime() {
    cli();
    unsigned long t = lastActiveTime;
    sei();
    return t;
}

void atomicSetLastActiveTime(unsigned long t) {
    cli();
    lastActiveTime = t;
    sei();
}

/*==================================================================================================
*  Boot-time MCUSR cleanup
*  -----------------------------------------------------------------------------------------
*  Runs as a `.init3` constructor (before main()). Clears the reset-cause register and
*  disables the WDT so it cannot fire during setup(). The WDT is re-enabled at the end of
*  setup() — see wdt_enable(WDTO_250MS) below.
*================================================================================================*/
void reset_mcusr(void) __attribute__((naked)) __attribute__((section(".init3")));
void reset_mcusr(void) {
    MCUSR = 0;
    wdt_disable();
}

/*==================================================================================================
*  setup() — one-time initialization
*================================================================================================*/
void setup() {
#if defined(_DEVELOPMENT_) || defined(_BOOTSYS_)
    Serial.begin(_SERIAL_BAUD_);
#endif

    // --- Pin directions -----------------------------------------------------
    pinMode(PIN_PULSE,       OUTPUT);
    pinMode(PIN_BUZZ,        OUTPUT);
    pinMode(PIN_CLK,         INPUT);
    pinMode(PIN_DT,          INPUT);
    pinMode(PIN_SW,          INPUT_PULLUP);
    pinMode(PIN_FOOT_SWITCH, INPUT_PULLUP);
    pinMode(PIN_AUTO_PULSE,  INPUT);

    // Explicit idle states — defensive: a fresh ATmega pin reads 0, which on PIN_PULSE
    // means "weld off", but be explicit so the intent is in the source.
    digitalWrite(PIN_PULSE,       LOW);
    digitalWrite(PIN_SW,          HIGH);  // redundant with INPUT_PULLUP, kept for clarity
    digitalWrite(PIN_FOOT_SWITCH, HIGH);  // redundant with INPUT_PULLUP, kept for clarity
    weldPulse(PULSE_OFF);                // belt-and-suspenders: ensure MOSFET gate is low

    // --- I2C + OLED ---------------------------------------------------------
    Wire.begin();
    setI2CClock(I2C_NORMAL_CLOCK_HZ);

    display.begin(SSD1306_SWITCHCAPVCC, I2C_ADDR_OLED);
    display.clearDisplay();
    display.display();
    display.setRotation(pData.pFlags.en_oledInvert ? OLED_INVERT_ROTATION : OLED_NORMAL_ROTATION);

    // --- Encoder interrupt --------------------------------------------------
    attachInterrupt(ENC_INT_NUM, isr, FALLING);
    atomicSetLastActiveTime(millis());

    // --- Boot event: button held → enter system menu ------------------------
#ifdef _BOOTSYS_
    atomicSetEvent(Event::BOOTDN);
#else
    atomicSetEvent(btnState() == BUTTON_DOWN ? Event::BOOTDN : Event::NONE);
#endif

    // --- EEPROM -------------------------------------------------------------
    loadEEPROM();

    // Re-apply OLED rotation now that we have the persisted flag.
    display.setRotation(pData.pFlags.en_oledInvert ? OLED_INVERT_ROTATION : OLED_NORMAL_ROTATION);
    splash();

    // --- Foot-switch stuck-at-boot detection --------------------------------
    if (!digitalRead(PIN_FOOT_SWITCH)) {
        foot_switch_error();
        reboot();
    }

    batteryVoltage = DEF_NOM_BATT_V;
    Boot_Sound();

#ifdef _DEVELOPMENT_
    Serial.println();
    Serial.println(F("========================================"));
    Serial.println(F(" Spot Welder Pro — boot diagnostics"));
    Serial.println(F("========================================"));
    Serial.print(F("Firmware  : v")); Serial.print(SWP_VERSION_MAJOR); Serial.print('.'); Serial.print(SWP_VERSION_MINOR); Serial.print('.'); Serial.println(SWP_VERSION_REVISION);
    Serial.print(F("Build date: ")); Serial.println(__DATE__);
    Serial.print(F("Pulse V   : ")); Serial.println(pData.PulseBatteryVoltage);
    Serial.print(F("Pulse A   : ")); Serial.println(pData.PulseAmps);
    Serial.print(F("BattAlarm : ")); Serial.println(pData.batteryAlarm);
    Serial.print(F("WeldCount : ")); Serial.println(pData.weldCount);
    Serial.print(F("AutoDelay : ")); Serial.println(pData.autoPulseDelay);
    Serial.print(F("MaxPulse  : ")); Serial.println(pData.maxPulseTime);
    Serial.print(F("ShrtPulse : ")); Serial.println(pData.shortPulseTime);
    Serial.print(F("AutoPulse : ")); Serial.println(pData.pFlags.en_autoPulse ? "On" : "Off");
    Serial.print(F("Sound     : ")); Serial.println(pData.pFlags.en_Sound ? "On" : "Off");
    Serial.print(F("InvertOLED: ")); Serial.println(pData.pFlags.en_oledInvert ? "Invert" : "Normal");
    Serial.print(F("PulseTime : ")); Serial.println(pData.pulseTime);
    Serial.println(F("----------------------------------------"));
#endif

    // --- INA226 with bounded retry ------------------------------------------
    initINA226();

    // Prime the batteryVoltage cache with a real reading.
    batteryVoltage = readBatteryVoltage();
    if (batteryVoltage == 0) batteryVoltage = DEF_NOM_BATT_V;

    // --- Last line of setup: enable the watchdog ----------------------------
    // Anything that hangs the main loop for >250 ms from this point will hard-reset the MCU.
    wdt_enable(WDTO_250MS);
}

/*==================================================================================================
*  loop() — super-cycle
*================================================================================================*/
void loop() {
    wdt_reset();          // Prove the CPU is alive every iteration
    stateMachine();       // FSM tick (handles events, draws UI, fires pulses)
    updateEEPROM();       // Persist changed settings (debounced; honours dirty flag)
}

/*==================================================================================================
*  isr() — encoder FALLING-edge interrupt (INT0)
*  -----------------------------------------------------------------------------------------
*  Reads PIN_DT phase to decide direction. The ISR is kept extremely short: it does ONE
*  digitalRead, ONE atomic 1-byte write to mEvent, and ONE atomic 4-byte write to
*  lastActiveTime. No I2C, no Serial, no malloc.
*================================================================================================*/
void isr() {
    static volatile unsigned long lastInterruptTime = 0;
    unsigned long now = millis();

    if ((now - lastInterruptTime) > RS_DEBOUNCE_MS) {
        lastInterruptTime = now;
        // Atomic 1-byte write — safe on AVR, but go through the helper for consistency.
        mEvent = digitalRead(PIN_DT) ? Event::ENCUP : Event::ENCDN;
        // 4-byte write — MUST be atomic (this is the bug fixed in v4.1).
        uint8_t oldSREG = SREG;
        cli();
        lastActiveTime = now;
        SREG = oldSREG;
    }
}

// EOF — Spot-Welder-Pro.ino
