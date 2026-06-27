/***************************************************************************************************
*  Spot-Welder-Pro — Hardware abstraction layer                                                     *
*                                                                                                  *
*  This file is the ONLY place that talks to:                                                      *
*    - INA226 (I2C)                                                                                *
*    - Hall-effect sensor (analogRead on PIN_HALL)                                                 *
*    - NTC thermistor (analogRead on PIN_TEMP)                                                     *
*    - Weld pulse MOSFET (digitalWrite on PIN_PULSE)                                               *
*    - I2C bus clock control                                                                       *
*                                                                                                  *
*  Why: separating hardware access from FSM logic makes the FSM unit-testable in a simulator,      *
*  and lets us swap sensor ICs (e.g. INA219 → INA226) without touching the state machine.          *
*                                                                                                  *
*  Safety notes:                                                                                   *
*    - sendWeldPulse() forces PIN_PULSE low at entry AND at exit — defense in depth.               *
*    - The pulse timing loop uses micros() busy-wait (NOT delay()) so the WDT can be reset          *
*      inside the loop. The loop body must remain side-effect free.                                *
*    - ADC sampling runs ASYNC during the pulse — the conversion is started BEFORE the pulse        *
*      fires and read AFTER the pulse ends, so the ADC sampling time does not extend the pulse.     *
*================================================================================================*/

#include <avr/wdt.h>

/*==================================================================================================
*  NTC thermistor lookup table  (PROGMEM)
*  -----------------------------------------------------------------------------------------
*  ADC value (10-bit, 0..1023) vs temperature in °C, sampled every 5 °C from 0 °C to 100 °C.
*  Generated from a 10kΩ NTC + 10kΩ pull-up divider on 5 V, with the standard Beta equation.
*  Linear interpolation between adjacent points keeps the resolution to ±0.5 °C — sufficient
*  for over-temperature protection at 65 °C.
*================================================================================================*/
const uint16_t ntc_adc_table[] PROGMEM = {
        786, 736, 683, 626, 569, 512, 457, 405, 357, 313, //  0..45 °C
        273, 238, 207, 180, 156, 136, 118, 103, 90,  79,  // 50..95 °C
        69                                                // 100 °C
};
static constexpr uint8_t NTC_TABLE_SIZE = sizeof(ntc_adc_table) / sizeof(ntc_adc_table[0]);

/*==================================================================================================
*  I2C bus clock management
*  -----------------------------------------------------------------------------------------
*  Two speeds are used: NORMAL (100 kHz) for UI / battery sampling, and FAST (800 kHz) for
*  the INA226 read that happens immediately after a weld pulse — that read must complete
*  before the battery voltage sag recovers, so we want it as fast as the bus will tolerate.
*  v4.0.x left the clock at 800 kHz forever after the first pulse, which made OLED redraws
*  electrically noisier. v4.1 restores the normal clock after the critical section.
*================================================================================================*/
void setI2CClock(uint32_t hz)
{
    Wire.setClock(hz);
}

void enterCriticalPulseMode()
{
    setI2CClock(I2C_FAST_CLOCK_HZ);
}

void exitCriticalPulseMode()
{
    setI2CClock(I2C_NORMAL_CLOCK_HZ);
}

/*==================================================================================================
*  INA226 initialization with bounded retry
*  -----------------------------------------------------------------------------------------
*  The original loop had an unbounded retry that could hang a device with a faulty INA226.
*  We retry exactly INA226_INIT_RETRIES times with INA226_INIT_RETRY_MS between attempts,
*  then reboot — the WDT will eventually recover the device if even reboot fails.
*================================================================================================*/
void initINA226()
{
    uint8_t retries = INA226_INIT_RETRIES;
    bool ok = false;
    while (retries-- > 0) {
        if (INA.begin()) {
            ok = true;
            break;
        }
        delay(INA226_INIT_RETRY_MS);
    }
    if (!ok) {
#ifdef _DEVELOPMENT_
        Serial.println(F("INA226 init failed after retries — rebooting"));
#endif
        delay(MESSAGE_BLOCK_MS);
        reboot();
    }
    INA.setMaxCurrentShunt(INA_MAX_CURRENT_A, INA_SHUNT_RESISTANCE_OHM);
    INA.setAverage(INA_AVERAGING_MODE);
}

/*==================================================================================================
*  Sensor readers
*================================================================================================*/
millivolts_t readBatteryVoltage()
{
    return INA.getBusVoltage_mV();
}

milliamps_t readBatteryCurrent()
{
    int32_t ma = INA.getCurrent_mA();
    return (ma < 0) ? static_cast<milliamps_t>(-ma) : static_cast<milliamps_t>(ma);
}

adc_t readHallSensorAvg(uint8_t samples)
{
    if (samples == 0) samples = 1;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < samples; ++i) {
        sum += analogRead(PIN_HALL);
    }
    return static_cast<adc_t>(sum / samples);
}

celsius_t readTemperatureCelsius(adc_t adc)
{
    if (adc >= pgm_read_word(&ntc_adc_table[0])) return 0;
    if (adc <= pgm_read_word(&ntc_adc_table[NTC_TABLE_SIZE - 1])) return 100;

    for (uint8_t i = 0; i < NTC_TABLE_SIZE - 1; ++i) {
        adc_t y0 = pgm_read_word(&ntc_adc_table[i]);
        adc_t y1 = pgm_read_word(&ntc_adc_table[i + 1]);
        if (adc <= y0 && adc > y1) {
            uint8_t t0 = i * 5;
            return t0 + static_cast<uint8_t>(5UL * (y0 - adc) / (y0 - y1));
        }
    }
    return 100;
}

/*==================================================================================================
*  Periodic background checks  (called from stateMachine every loop tick)
*================================================================================================*/
void checkForLowVoltageEvent()
{
    static unsigned long lastBVTime = 1; // 1 ensures first-tick execution
    if ((millis() - lastBVTime) < BATTERY_SAMPLE_MS) return;
    lastBVTime = millis();

    batteryVoltage = readBatteryVoltage();

#ifdef _TESTING_
    batteryVoltage = DEF_NOM_BATT_V;
#endif

    // High-voltage alarm — non-blocking visual indication, no state transition.
    millivolts_t hvThreshold = static_cast<millivolts_t>(pData.batteryhighAlarm) * 100;
    if (batteryVoltage > hvThreshold) {
        if (!highVoltageAlarmActive) {
            highVoltageAlarmActive = true;
            if (pData.pFlags.en_Sound) playHighVoltageAlarmSound();
        }
    } else {
        highVoltageAlarmActive = false;
    }

    // Low-voltage alarm — non-blocking visual indication, no state transition.
    millivolts_t lvThreshold = static_cast<millivolts_t>(pData.batteryAlarm) * 100;
    if (batteryVoltage < lvThreshold) {
        if (!lowVoltageAlarmActive) {
            lowVoltageAlarmActive = true;
            if (pData.pFlags.en_Sound) playLowVoltageAlarmSound();
        }
    } else {
        lowVoltageAlarmActive = false;
    }
}

void checkTemp()
{
    static unsigned long lastTTime = 0;
    if ((millis() - lastTTime) < TEMP_SAMPLE_MS) return;
    lastTTime = millis();

    adc_t bitwertNTC = analogRead(PIN_TEMP);
    TCelsius = readTemperatureCelsius(bitwertNTC);

    if (TCelsius > pData.maxTCelsius) pData.maxTCelsius = TCelsius;

    if (TCelsius > DEF_HIGH_TEMP_ALARM) {
        atomicSetEvent(Event::TEMP_HIGH);
    }
}

void checkForSleepEvent()
{
    if ((atomicReadLastActiveTime() + STANDBY_TIMEOUT_MS) < millis()) {
        atomicSetEvent(Event::STBY_TIMEOUT);
    }
}

void checkForBtnEvent()
{
    static unsigned long lastBtnTime = 0;
    static bool lastBtnState = BUTTON_UP;
    bool thisBtnState = btnState();

    if ((millis() - lastBtnTime) < RS_DEBOUNCE_MS) return;
    if (thisBtnState == lastBtnState) return;

    atomicSetEvent(thisBtnState == BUTTON_UP ? Event::BTNUP : Event::BTNDN);
    unsigned long now = millis();
    atomicSetLastActiveTime(now);
    lastBtnTime = now;
    lastBtnState = thisBtnState;
}

/*==================================================================================================
*  sendWeldPulse — the safety-critical path
*  -----------------------------------------------------------------------------------------
*  Sequence:
*    1. Defensive: force PIN_PULSE LOW on entry (no assumptions about previous state).
*    2. Wait for sensePin to be stably active (with progress bar for auto-pulse, fixed delay
*       for foot switch). The WDT is reset every iteration inside this wait.
*    3. Optional short pre-pulse (cleans the nickel surface).
*    4. Compute the voltage-compensated main pulse duration (integer arithmetic, no float).
*    5. Switch I2C to FAST mode, prime the Hall ADC.
*    6. Fire the main pulse: drive PIN_PULSE HIGH, busy-wait on micros() for the exact
*       duration, then drive PIN_PULSE LOW. Reset the WDT inside the wait loop.
*    7. After pulse-off, read the Hall ADC + INA226 bus voltage (the ADC was started BEFORE
*       the pulse fired, so the conversion time does NOT extend the pulse).
*    8. Wait for sensePin to release (foot switch up / auto-pulse sensor clear). The WDT is
*       reset inside this loop and the MOSFET is held LOW on every iteration — if the user
*       stands on the pedal for 30 seconds, the MOSFET stays OFF.
*    9. Show the release-progress bar, increment weld count, mark activity time.
*   10. Defensive: force PIN_PULSE LOW on exit and restore the normal I2C clock.
*================================================================================================*/
void sendWeldPulse(uint8_t sensePin, milliseconds_t delayEngage, milliseconds_t delayRelease,
                   bool senseActiveLevel)
{
    struct progress wait;
    bool activePinState = (senseActiveLevel == PULSE_ACTIVE_HIGH);

    // 1. Defensive: ensure MOSFET is off before we begin any timing.
    weldPulse(PULSE_OFF);

#ifdef _DEVELOPMENT_
    Serial.println(F("Weld pulse sequence started"));
#endif

    // 2. Engage wait
    if (sensePin == PIN_AUTO_PULSE) {
        wait.opt = PGR_ON;
        wait.time = delayEngage;
        while (!drawProgress(&wait, false)) {
            wdt_reset();
            if (digitalRead(PIN_AUTO_PULSE) != activePinState) {
                drawProgress(&wait, true);
                return; // User released before the engage delay elapsed — abort cleanly.
            }
        }
    } else {
        // Foot switch: chunked delay so the WDT can be reset every 50 ms.
        milliseconds_t remaining = delayEngage;
        while (remaining >= 50) {
            wdt_reset();
            delay(50);
            remaining -= 50;
        }
        if (remaining > 0) {
            wdt_reset();
            delay(remaining);
        }
    }

    if (pData.pFlags.en_Sound) playBeep(1500, 100);

    // 3. Short pre-pulse (dual-pulse mode)
    if (pData.shortPulseTime > 0 && pData.pulseTime > 3) {
        unsigned long shortPulseDelay = max(
                1UL, (static_cast<unsigned long>(pData.pulseTime) * pData.shortPulseTime) / 100);
        wdt_reset();
        weldPulse(PULSE_ON);
        delay(static_cast<unsigned long>(shortPulseDelay));
        weldPulse(PULSE_OFF);
        wdt_reset();
        delay(shortPulseDelay);
    }

    // 4. Voltage-compensated main pulse duration (integer arithmetic, P = V²/R model).
    //    nominalVoltage is stored in EEPROM as units of 100 mV (e.g. 80 = 8.0 V).
    //    batteryVoltage is in mV. We scale both to centivolts (100 mV) for the ratio.
    uint32_t vnom_cv = static_cast<uint32_t>(pData.nominalVoltage); // centivolts
    uint32_t vact_cv =
            (static_cast<uint32_t>(batteryVoltage) + 50) / 100; // mV → centivolts, rounded
    if (vact_cv < 10) vact_cv = (DEF_NOM_BATT_V / 100);         // sanity floor (0.1 V)

    uint32_t vnom2 = vnom_cv * vnom_cv;
    uint32_t vact2 = vact_cv * vact_cv;
    uint32_t compTime32 = (static_cast<uint32_t>(pData.pulseTime) * vnom2) / vact2;

    // Clamp the compensation factor to [0.7, 1.4] × baseTime — prevents runaway when the
    // battery is unusually low (comp would over-extend) or unusually high (comp would under-fire).
    milliseconds_t minTime = (static_cast<uint32_t>(pData.pulseTime) * PULSE_COMP_MIN_FACTOR_NUM +
                              PULSE_COMP_MIN_FACTOR_DEN - 1) /
                             PULSE_COMP_MIN_FACTOR_DEN;
    milliseconds_t maxTime = (static_cast<uint32_t>(pData.pulseTime) * PULSE_COMP_MAX_FACTOR_NUM) /
                             PULSE_COMP_MAX_FACTOR_DEN;
    milliseconds_t compensatedPulseTime = static_cast<milliseconds_t>(compTime32);
    if (compensatedPulseTime < minTime) compensatedPulseTime = minTime;
    if (compensatedPulseTime > maxTime) compensatedPulseTime = maxTime;

#ifdef _DEVELOPMENT_
    Serial.print(F("Base="));
    Serial.print(pData.pulseTime);
    Serial.print(F("ms  Vbatt="));
    Serial.print(batteryVoltage / 1000);
    Serial.print('.');
    Serial.print((batteryVoltage % 1000) / 100);
    Serial.print(F("V  Tcomp="));
    Serial.print(compensatedPulseTime);
    Serial.println(F("ms"));
#endif

    // 5. Enter critical-pulse mode (fast I2C, prime ADC).
    enterCriticalPulseMode();
    adc_t nominalGauss = readHallSensorAvg(ADC_OVERSAMPLE_COUNT);
    delay(ADC_SETTLE_DELAY_MS);
    wdt_reset();

    // 6. Fire the main pulse. Use micros() busy-wait — NOT delay() — so we can reset
    //    the WDT inside the loop and the pulse length is exact to ~4 µs.
    uint32_t pulseStart_us = micros();
    uint32_t pulseDuration_us = static_cast<uint32_t>(compensatedPulseTime) * 1000UL;

    weldPulse(PULSE_ON);
    while ((micros() - pulseStart_us) < pulseDuration_us) {
        // The loop body MUST stay minimal. Resetting the WDT here costs ~6 cycles
        // and is well below the pulse precision budget.
        wdt_reset();
    }
    weldPulse(PULSE_OFF);

    wdt_reset();

    // 7. Read sensors AFTER pulse-off — the ADC was primed above so its settling is
    //    already accounted for; the conversion below takes ~112 µs but does not
    //    extend the pulse because the pulse already ended.
    adc_t pulseGauss = readHallSensorAvg(ADC_OVERSAMPLE_COUNT);
    millivolts_t busVoltageDuring = readBatteryVoltage();

    pData.PulseBatteryVoltage = busVoltageDuring;
    // Hall sensor delta → estimated weld current in mA×10. Named constants replace the
    // magic `* 10` from v4.0.x.
    pData.PulseAmps =
            (pulseGauss > nominalGauss)
                    ? static_cast<uint16_t>(static_cast<uint32_t>(pulseGauss - nominalGauss) *
                                            HALL_SCALE_MA_X10_PER_ADC)
                    : 0;

    // 8. Wait for sensePin to release. Critical safety property: the MOSFET is held OFF
    //    on every iteration, so even if the user stands on the pedal indefinitely the
    //    device will NOT fire another pulse — the loop just keeps the WDT alive.
    while (digitalRead(sensePin) == activePinState) {
        wdt_reset();
        weldPulse(PULSE_OFF);
    }

    // 9. Release-progress bar + bookkeeping.
    wait.opt = PGR_OFF;
    wait.time = delayRelease;
    while (!drawProgress(&wait, false)) {
        wdt_reset();
    }

    pData.weldCount++;
    atomicSetLastActiveTime(millis());
    markEEPROMDirty();

    // 10. Defensive cleanup.
    weldPulse(PULSE_OFF);
    exitCriticalPulseMode();
}

// EOF — d_hardware.ino
