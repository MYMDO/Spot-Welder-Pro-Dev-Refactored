/***************************************************************************************************
*  Spot-Welder-Pro — EEPROM persistence, audio, and value formatting                              *
*                                                                                                  *
*  Responsibilities:                                                                               *
*    - valStr()                  : centralised numeric → string formatting (PROGMEM format strs)   *
*    - loadEEPROM() / resetEEPROM(): read / factory-default the persisted settings                 *
*    - updateEEPROM()            : debounced write-back, honours the dirty flag set by markEEPROMDirty() *
*    - calculateChecksum()       : CRC-16/MODBUS integrity check                                   *
*    - Boot_Sound / playBeep / alarm sounds : piezo feedback                                      *
*    - reboot()                  : WDT-triggered software reset                                    *
*                                                                                                  *
*  EEPROM layout (frozen, do NOT reorder — see Spot-Welder-Pro.h):                                 *
*    [0..3]   EE_UNIQUE_ID   (uint32_t magic — validates EEPROM was initialised by us)             *
*    [4..29]  progData       (26 bytes — see static_assert in header)                              *
*    [30..31] CRC-16/MODBUS  (uint16_t checksum over progData bytes)                               *
*================================================================================================*/

#include <avr/wdt.h>

// Declared in Spot-Welder-Pro.h (defined in a_state_machine.ino) — set by markEEPROMDirty()
// whenever a setting changes, read here to decide whether to do a debounced early save.

/*==================================================================================================
*  valStr — centralised numeric → ASCII formatting
*  -----------------------------------------------------------------------------------------
*  Every display call site that needs to render a number goes through here. Using a single
*  function means: format strings live in PROGMEM (no SRAM cost), there is exactly one
*  default case for unknown formats, and adding a new format is a one-line change.
*================================================================================================*/
char* valStr(char* str, uint16_t val, ValueFormat fType)
{
    switch (fType) {
    case ValueFormat::BATTERY_ALARM:
        sprintf_P(str, PSTR("%u.%u"), val / 10, val % 10);
        break; // 7.5 (decivolts)
    case ValueFormat::TEMP_ALARM:
        sprintf_P(str, PSTR("%4u"), val);
        break; // "  65"
    case ValueFormat::BATTERY_VOLTS:
        sprintf_P(str, PSTR("%2u.%02u"), val / 100, val % 100);
        break; // "12.45"
    case ValueFormat::BATTERY_AMPS:
        sprintf_P(str, PSTR("%5u"), val);
        break; // "  500"
    case ValueFormat::WELD_COUNT:
        sprintf_P(str, PSTR("%5u"), val);
        break; // "   42"
    case ValueFormat::TEMPERATURE:
        sprintf_P(str, PSTR("%5u"), val);
        break; // "   25"
    case ValueFormat::PULSE_DELAY:
        sprintf_P(str, PSTR("%3u"), val);
        break; // "  5"  (ms)
    case ValueFormat::SHORT_PULSE:
        sprintf_P(str, PSTR("%3u"), val);
        break; // "  0"  (%)
    case ValueFormat::AUTO_DELAY:
        sprintf_P(str, PSTR("%u.%u"), val / 10, val % 10);
        break; // "2.0"  (seconds)
    default:
        str[0] = '?';
        str[1] = '\0';
        break; // Defensive
    }
    return str;
}

/*==================================================================================================
*  CRC-16/MODBUS — integrity check over progData
*  -----------------------------------------------------------------------------------------
*  Polynomial 0xA001 (reflected 0x8005), init 0xFFFF. Same algorithm as Modbus RTU.
*  Detects any single-bit error, any two-bit error, any burst ≤16 bits, and 99.998% of
*  longer bursts — plenty for catching EEPROM corruption from brown-outs / write wear-out.
*================================================================================================*/
uint16_t calculateChecksum(const progData& data)
{
    uint16_t crc = 0xFFFF;
    const byte* p = reinterpret_cast<const byte*>(&data);
    for (size_t i = 0; i < sizeof(progData); ++i) {
        crc ^= p[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x0001) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
        }
    }
    return crc;
}

/*==================================================================================================
*  resetEEPROM — factory-default the persisted settings
*  -----------------------------------------------------------------------------------------
*  full == true  → Full Reset: clear weldCount, restore default OLED orientation.
*  full == false → Safe Reset: keep weldCount, keep OLED orientation.
*  Uses EEPROM.put() which internally calls update() — only writes bytes that actually
*  changed, so a reset does NOT burn an EEPROM write cycle on every byte.
*================================================================================================*/
void resetEEPROM(bool full)
{
    pData.autoPulseDelay = DEF_AUTO_PLSDELAY;
    pData.PulseBatteryVoltage = DEF_PULSE_VOLTAGE;
    pData.PulseAmps = DEF_PULSE_AMPS;
    pData.PulseShuntVoltage = DEF_SHUNT_VOLTAGE;
    pData.batteryAlarm = DEF_BATT_ALARM / 100;
    pData.batteryhighAlarm = DEF_HIGH_BATT_ALARM / 100;
    pData.nominalVoltage = DEF_NOMINAL_VOLTAGE_EE;
    pData.weldCount = full ? 0 : pData.weldCount;
    pData.pulseTime = DEF_PULSE_TIME;
    pData.maxPulseTime = DEF_MAX_PULSE_TIME;
    pData.shortPulseTime = DEF_SPULSE_TIME;
    pData.pFlags.en_autoPulse = DEF_AUTO_PULSE;
    pData.pFlags.en_Sound = DEF_WELD_SOUND;
    pData.pFlags.en_oledInvert = full ? DEF_OLED_INVERT : pData.pFlags.en_oledInvert;

    EEPROM.put(EEA_PDATA, pData);
    EEPROM.put(EEA_CHECKSUM, calculateChecksum(pData));
    EEPROM.put(EEA_ID, static_cast<uint32_t>(EE_UNIQUE_ID));

#if defined(_DEVELOPMENT_) || defined(_BOOTSYS_)
    Serial.println(full ? F("EEPROM: full factory reset")
                        : F("EEPROM: safe reset (kept weldCount + orientation)"));
#endif
}

/*==================================================================================================
*  loadEEPROM — read & validate the persisted settings
*  -----------------------------------------------------------------------------------------
*  If either the unique-ID or the CRC fails to validate, we reset to factory defaults.
*  After loading we also clamp every value to its valid range — this catches corrupted-
*  but-CRC-valid data (e.g. if a future firmware version writes a new field that an old
*  firmware then loads).
*================================================================================================*/
void loadEEPROM()
{
    uint32_t uniqueID;
    uint16_t storedChecksum;
    EEPROM.get(EEA_ID, uniqueID);
    EEPROM.get(EEA_PDATA, pData);
    EEPROM.get(EEA_CHECKSUM, storedChecksum);

    uint16_t calculatedChecksum = calculateChecksum(pData);

    if (calculatedChecksum != storedChecksum || uniqueID != EE_UNIQUE_ID) {
#ifdef _DEVELOPMENT_
        Serial.print(F("EEPROM mismatch (stored=0x"));
        Serial.print(storedChecksum, HEX);
        Serial.print(F(" calc=0x"));
        Serial.print(calculatedChecksum, HEX);
        Serial.println(F(") — restoring defaults"));
#endif
        resetEEPROM(EE_FULL_RESET);
    }

    // --- Range-clamp every persisted field ------------------------------------
    if (pData.nominalVoltage < MIN_NOMINAL_VOLTAGE_EE ||
        pData.nominalVoltage > MAX_NOMINAL_VOLTAGE_EE) {
        pData.nominalVoltage = DEF_NOMINAL_VOLTAGE_EE;
    }
    if (pData.batteryAlarm < MIN_BATT_ALARM_MV / 100 ||
        pData.batteryAlarm > MAX_BATT_ALARM_MV / 100) {
        pData.batteryAlarm = DEF_BATT_ALARM / 100;
    }
    if (pData.batteryhighAlarm < MIN_BATT_ALARM_MV / 100 ||
        pData.batteryhighAlarm > MAX_BATT_ALARM_MV / 100) {
        pData.batteryhighAlarm = DEF_HIGH_BATT_ALARM / 100;
    }
    if (pData.batteryAlarm >= pData.batteryhighAlarm) {
        pData.batteryAlarm = DEF_BATT_ALARM / 100;
        pData.batteryhighAlarm = DEF_HIGH_BATT_ALARM / 100;
    }
    if (pData.pulseTime < MIN_PULSE_TIME || pData.pulseTime > ABS_MAX_PULSE_TIME) {
        pData.pulseTime = DEF_PULSE_TIME;
    }
    if (pData.maxPulseTime < MIN_PULSE_TIME || pData.maxPulseTime > ABS_MAX_PULSE_TIME) {
        pData.maxPulseTime = DEF_MAX_PULSE_TIME;
    }
    if (pData.pulseTime > pData.maxPulseTime) pData.pulseTime = pData.maxPulseTime;
    if (pData.shortPulseTime > MAX_SPULSE_TIME) pData.shortPulseTime = DEF_SPULSE_TIME;
    if (pData.autoPulseDelay < MIN_APULSE_DELAY || pData.autoPulseDelay > MAX_APULSE_DELAY) {
        pData.autoPulseDelay = DEF_AUTO_PLSDELAY;
    }
}

/*==================================================================================================
*  updateEEPROM — periodic + dirty-flag-driven write-back
*  -----------------------------------------------------------------------------------------
*  Two save triggers:
*    (1) Periodic: every EEPROM_AUTOSAVE_MS (30 s), regardless of dirty flag.
*        Catches any drift we missed flagging.
*    (2) Debounced: when markEEPROMDirty() was called and at least
*        EEPROM_DIRTY_SAVE_MS (2 s) have passed since the dirty flag was set.
*        Catches user menu edits promptly without spamming EEPROM on every encoder tick.
*
*  EEPROM.put() internally uses update() — only bytes that actually changed get written,
*  so this is gentle on the EEPROM write-cycle budget (~100k cycles per cell).
*================================================================================================*/
void updateEEPROM()
{
    static unsigned long lastAutoSave = 0;
    unsigned long now = millis();

    bool periodicDue = (now - lastAutoSave) >= EEPROM_AUTOSAVE_MS;
    bool dirtyDue = eepromDirty && ((now - eepromDirtyTime) >= EEPROM_DIRTY_SAVE_MS);

    if (!periodicDue && !dirtyDue) return;

    lastAutoSave = now;

    uint16_t storedChecksum;
    EEPROM.get(EEA_CHECKSUM, storedChecksum);
    uint16_t calculatedChecksum = calculateChecksum(pData);

    if (calculatedChecksum == storedChecksum) {
        // Nothing to write — the cached pData matches what's already in EEPROM.
        eepromDirty = false;
#ifdef _DEVELOPMENT_
        Serial.print(F("EEPROM OK (crc=0x"));
        Serial.print(calculatedChecksum, HEX);
        Serial.println(')');
#endif
        return;
    }

    EEPROM.put(EEA_PDATA, pData);
    EEPROM.put(EEA_CHECKSUM, calculatedChecksum);
    eepromDirty = false;

#ifdef _DEVELOPMENT_
    Serial.print(F("EEPROM written: 0x"));
    Serial.print(storedChecksum, HEX);
    Serial.print(F(" → 0x"));
    Serial.println(calculatedChecksum, HEX);
#endif
}

/*==================================================================================================
*  Piezo buzzer — sounds & alarms
*  -----------------------------------------------------------------------------------------
*  playBeep() uses tone() (non-blocking on AVR) followed by a short delay so the caller
*  can stack notes without overlapping them. The delay() here is intentional — sound
*  sequences are short (≤1 s) and the WDT is reset before the blocking wait.
*================================================================================================*/
void playBeep(uint16_t frequency, uint16_t duration)
{
    wdt_reset();
    tone(PIN_BUZZ, frequency, duration);
    delay(duration / 2);
    noTone(PIN_BUZZ);
}

void Boot_Sound()
{
    playBeep(1000, BOOT_SOUND_STEP_MS);
    playBeep(2000, BOOT_SOUND_STEP_MS);
    playBeep(3000, BOOT_SOUND_STEP_MS);
    playBeep(2000, BOOT_SOUND_STEP_MS);
}

void playHighVoltageAlarmSound()
{
    playBeep(2000, 150);
    delay(50);
    playBeep(2500, 150);
    delay(50);
    playBeep(3000, 200);
}

void playLowVoltageAlarmSound()
{
    playBeep(1500, 150);
    delay(50);
    playBeep(1000, 150);
    delay(50);
    playBeep(500, 200);
}

/*==================================================================================================
*  reboot — software reset via the watchdog
*  -----------------------------------------------------------------------------------------
*  Enable the shortest WDT interval (15 ms) and spin forever. The WDT will fire and reset
*  the MCU. reset_mcusr() runs at next boot to clear MCUSR and disable the WDT again.
*================================================================================================*/
void reboot()
{
    wdt_enable(WDTO_15MS);
    while (true) { /* wait for WDT reset */
    }
}

// EOF — c_eeprom_sound.ino
