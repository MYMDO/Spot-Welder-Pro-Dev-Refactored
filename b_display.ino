/***************************************************************************************************
*  Spot-Welder-Pro — OLED display & UI rendering                                                    *
*                                                                                                  *
*  All drawing happens here. This file MUST NOT perform I2C sensor reads — those belong in          *
*  d_hardware.ino. v4.0.x had a hidden `INA.getCurrent_mA()` call inside drawStatusLine() that     *
*  fired an I2C transaction on every UI redraw; v4.1 removes it and uses the cached                 *
*  `batteryVoltage` value updated by checkForLowVoltageEvent().                                    *
*                                                                                                  *
*  All text constants live in PROGMEM (Spot-Welder-Pro.h) and are passed as __FlashStringHelper*.  *
***************************************************************************************************/

/*==================================================================================================
*  Text primitives
*================================================================================================*/
void setTextProp(uint8_t size, int16_t x, int16_t y, uint16_t color, bool invert)
{
    display.setTextSize(size);
    display.setCursor(x, y);
    if (invert)
        display.setTextColor(BLACK, color);
    else
        display.setTextColor(color);
}

void drawText(uint8_t size, int16_t x, int16_t y, const __FlashStringHelper* text)
{
    setTextProp(size, x, y);
    display.print(text);
}

void drawValueWithUnits(uint8_t size, int16_t x, int16_t y, const char* value,
                        const __FlashStringHelper* units)
{
    setTextProp(size, x, y);
    display.print(value);
    display.print(units);
}

/*==================================================================================================
*  Status line — top row of every primary screen
*  -----------------------------------------------------------------------------------------
*  v4.0.x bug: this function called `INA.getCurrent_mA()` directly, triggering an I2C
*  transaction on every redraw. That made the display flicker-prone and violated the
*  "no I2C in the rendering layer" rule. v4.1 reads only cached globals.
*================================================================================================*/
void drawStatusLine()
{
    char str[16];
    drawText(1, 0, 1, pData.pFlags.en_autoPulse ? FPSTR(LS_AUTO_BAR) : FPSTR(LS_MANUAL_BAR));

    // The cached `batteryVoltage` is in mV; display in V with 2 decimals → pass centivolts.
    drawValueWithUnits(1, SSD1306_LCDWIDTH - CHAR_WIDTH_PX * 6, 1,
                       valStr(str, batteryVoltage / 10, ValueFormat::BATTERY_VOLTS),
                       FPSTR(LS_VUNITS));

    display.drawLine(0, CHAR_HEIGHT_PX + 3, SSD1306_LCDWIDTH - 1, CHAR_HEIGHT_PX + 3, WHITE);
}

/*==================================================================================================
*  Non-blocking alarm overlays
*  -----------------------------------------------------------------------------------------
*  These draw a black box over the lower portion of the main screen and overlay an alarm
*  message + the live voltage. They are called from displayMainScreen() when the
*  corresponding alarm flag is set, so the alarm is visible without leaving the main screen.
*================================================================================================*/
void displayHighVoltageWarning()
{
    char str[16];
    display.fillRect(0, 15, SCREEN_WIDTH, 34, BLACK);
    setTextProp(2, (SSD1306_LCDWIDTH - (10 * CHAR_WIDTH_PX * 2)) / 2 + 12, 20, WHITE);
    display.print(FPSTR(LS_HIGHV));
    setTextProp(1, (SSD1306_LCDWIDTH - (12 * CHAR_WIDTH_PX)) / 2, 38, WHITE);
    display.print(FPSTR(LS_CURRLABEL));
    display.print(valStr(str, batteryVoltage / 10, ValueFormat::BATTERY_VOLTS));
    display.print(FPSTR(LS_VUNITS));
}

void displayLowVoltageWarning()
{
    char str[16];
    display.fillRect(0, 15, SCREEN_WIDTH, 34, BLACK);
    setTextProp(2, (SSD1306_LCDWIDTH - (9 * CHAR_WIDTH_PX * 2)) / 2 + 12, 20, WHITE);
    display.print(FPSTR(LS_LOWV));
    setTextProp(1, (SSD1306_LCDWIDTH - (12 * CHAR_WIDTH_PX)) / 2, 38, WHITE);
    display.print(FPSTR(LS_CURRLABEL));
    display.print(valStr(str, batteryVoltage / 10, ValueFormat::BATTERY_VOLTS));
    display.print(FPSTR(LS_VUNITS));
}

/*==================================================================================================
*  Main screen
*================================================================================================*/
void displayMainScreen(bool signaled)
{
    char str[16];
    display.clearDisplay();
    drawStatusLine();

    // Big pulse-time readout (size=4 → 24px tall digits)
    setTextProp(4, 0, 16 + CHAR_HEIGHT_PX / 2);
    display.print(valStr(str, pData.pulseTime, ValueFormat::PULSE_DELAY));

    // "ms" label, inverted when a pulse is firing (visual feedback for auto-pulse delay)
    setTextProp(2, 12 * CHAR_WIDTH_PX, 32, WHITE, signaled);
    display.print(FPSTR(LS_MS));

    // Weld counter (top-right)
    drawValueWithUnits(1, SSD1306_LCDWIDTH - CHAR_WIDTH_PX * 6, 20,
                       valStr(str, pData.weldCount, ValueFormat::WELD_COUNT), FPSTR(LS_WELDS));

    // Last-pulse statistics (bottom row)
    char pulseAmpsStr[8];
    drawText(1, 0, 56, FPSTR(LS_PULSE));
    drawValueWithUnits(1, SSD1306_LCDWIDTH - CHAR_WIDTH_PX * 14, 56,
                       valStr(pulseAmpsStr, pData.PulseAmps, ValueFormat::BATTERY_AMPS),
                       FPSTR(LS_AUNITS));
    drawValueWithUnits(1, SSD1306_LCDWIDTH - CHAR_WIDTH_PX * 6, 56,
                       valStr(str, pData.PulseBatteryVoltage / 10, ValueFormat::BATTERY_VOLTS),
                       FPSTR(LS_VUNITS));

    // Non-blocking alarm overlays
    if (highVoltageAlarmActive)
        displayHighVoltageWarning();
    else if (lowVoltageAlarmActive)
        displayLowVoltageWarning();

    display.display();
}

/*==================================================================================================
*  Menu pages — Type 1 (three labelled rows + selection cursor)
*================================================================================================*/
void displayMenuType1(const __FlashStringHelper* title, const __FlashStringHelper* line1,
                      const __FlashStringHelper* line2, const __FlashStringHelper* line3,
                      uint8_t SelectedItem)
{
    display.clearDisplay();
    if (title == nullptr) {
        drawStatusLine();
    } else {
        setTextProp(1, 1, 1, WHITE);
        display.print(title);
        display.drawLine(0, CHAR_HEIGHT_PX + 3, SSD1306_LCDWIDTH - 1, CHAR_HEIGHT_PX + 3, WHITE);
    }

    setTextProp(2, 2, 16, WHITE, SelectedItem == 0);
    display.print(line1);
    setTextProp(2, 2, 16 + 2 * CHAR_HEIGHT_PX + 1, WHITE, SelectedItem == 1);
    display.print(line2);
    setTextProp(2, 2, 16 + 4 * CHAR_HEIGHT_PX + 2, WHITE, SelectedItem == 2);
    display.print(line3);

    // 2×2 px selection box at the left edge of the highlighted row
    int16_t boxY = (SelectedItem == 0)   ? 16
                   : (SelectedItem == 1) ? 16 + 2 * CHAR_HEIGHT_PX + 1
                                         : 16 + 4 * CHAR_HEIGHT_PX + 2;
    display.drawRect(0, boxY, 2, 2 * CHAR_HEIGHT_PX, WHITE);
    display.display();
}

/*==================================================================================================
*  Menu pages — Type 2 (title + single value + units)
*================================================================================================*/
void displayMenuType2(const __FlashStringHelper* title, const char* value,
                      const __FlashStringHelper* units)
{
    display.clearDisplay();
    setTextProp(1, 1, 1, WHITE);
    display.print(title);
    display.drawLine(0, CHAR_HEIGHT_PX + 3, SSD1306_LCDWIDTH - 1, CHAR_HEIGHT_PX + 3, WHITE);

    setTextProp(2, 2, 16 + LINE_HEIGHT_PX);
    if (value == nullptr) {
        display.print(units);
    } else {
        display.print(value);
        drawText(1, 2, 16 + 3 * LINE_HEIGHT_PX, units);
    }
    display.display();
}

/*==================================================================================================
*  Full-screen battery / temperature status pages
*================================================================================================*/
void displayBatteryStatus(const __FlashStringHelper* statusText)
{
    char str[8];
    display.clearDisplay();
    drawStatusLine();
    drawText(2, (SSD1306_LCDWIDTH - (sizeof(LS_BATTERY) - 1) * CHAR_WIDTH_PX * 2) / 2, 16,
             FPSTR(LS_BATTERY));
    uint8_t statusLen = strlen_P(reinterpret_cast<PGM_P>(statusText));
    drawText(2, (SSD1306_LCDWIDTH - statusLen * CHAR_WIDTH_PX * 2) / 2, 16 + 2 * LINE_HEIGHT_PX,
             statusText);
    drawValueWithUnits(1, (SSD1306_LCDWIDTH - 1 * CHAR_WIDTH_PX) / 2, 16 + 4 * LINE_HEIGHT_PX,
                       valStr(str, batteryVoltage, ValueFormat::BATTERY_VOLTS), FPSTR(LS_VUNITS));
    display.display();
}

void displayTemperatureStatus(const __FlashStringHelper* statusText,
                              const __FlashStringHelper* adviceText)
{
    char str[8];
    display.clearDisplay();
    drawStatusLine();
    drawValueWithUnits(2, (SSD1306_LCDWIDTH - (sizeof(LS_HIGHT) - 1) * CHAR_WIDTH_PX * 2) / 2, 16,
                       valStr(str, TCelsius, ValueFormat::TEMPERATURE), FPSTR(LS_TUNITS));
    uint8_t statusLen = strlen_P(reinterpret_cast<PGM_P>(statusText));
    drawText(2, (SSD1306_LCDWIDTH - statusLen * CHAR_WIDTH_PX * 2) / 2, 16 + 2 * LINE_HEIGHT_PX,
             statusText);
    uint8_t adviceLen = strlen_P(reinterpret_cast<PGM_P>(adviceText));
    drawText(1, (SSD1306_LCDWIDTH - adviceLen * CHAR_WIDTH_PX) / 2, 16 + 4 * LINE_HEIGHT_PX,
             adviceText);
    display.display();
}

void displayLowBattery()
{
    displayBatteryStatus(FPSTR(LS_LOWV));
}

void displayHighTemperature()
{
    displayTemperatureStatus(FPSTR(LS_HIGHT), FPSTR(LS_COOL));
}

/*==================================================================================================
*  Modal message box  (title + 3 lines, optional auto-dismiss)
*================================================================================================*/
void message(const __FlashStringHelper* line1, const __FlashStringHelper* line2,
             const __FlashStringHelper* line3, uint8_t displayTime)
{
    display.clearDisplay();
    drawText(1, (SSD1306_LCDWIDTH - (sizeof(LS_MSGHDR) - 1) * CHAR_WIDTH_PX) / 2, 1,
             FPSTR(LS_MSGHDR));
    display.drawLine(0, CHAR_HEIGHT_PX + 3, SSD1306_LCDWIDTH - 1, CHAR_HEIGHT_PX + 3, WHITE);
    drawText(2, 1, 16, line1);
    drawText(1, 1, 16 + 2 * LINE_HEIGHT_PX, line2);
    drawText(1, 1, 16 + 3 * LINE_HEIGHT_PX, line3);
    display.display();
    if (displayTime) {
        // Chunked delay so the WDT can be reset every 50 ms during the wait.
        unsigned long remaining = static_cast<unsigned long>(displayTime) * 1000UL;
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
}

/*==================================================================================================
*  Splash screen — shown at boot, dismissible by pressing the encoder button
*================================================================================================*/
void splash()
{
    display.clearDisplay();
    display.display();
    drawText(1, 1, 1, F(SWP_DEVICE_NAME));
    drawText(1, 1, 16,
             F("Ver " SWP_XSTR(SWP_VERSION_MAJOR) "." SWP_XSTR(SWP_VERSION_MINOR) "." SWP_XSTR(
                     SWP_VERSION_REVISION) " " __DATE__));
    drawText(1, 1, 16 + 2 * LINE_HEIGHT_PX, F("Copyright (c) " SWP_COPYRIGHT_YEAR));
    display.display();

    // Auto-dismiss after SPLASH_HOLD_MS OR immediately if the user presses the button.
    uint16_t timer = 0;
    while (btnState() != BUTTON_DOWN && timer < SPLASH_HOLD_MS) {
        wdt_reset();
        delay(10);
        timer += 10;
    }
    while (btnState() == BUTTON_DOWN)
        wdt_reset(); // wait for release

    display.clearDisplay();
    display.display();
}

/*==================================================================================================
*  Foot-switch stuck-at-boot error screen
*================================================================================================*/
void foot_switch_error()
{
    display.clearDisplay();
    display.display();
    drawText(1, 1, 1, FPSTR(LS_FOOTFAULT));
    drawText(1, 1, 16, FPSTR(LS_FOOTFIX1));
    drawText(1, 1, 16 + LINE_HEIGHT_PX, FPSTR(LS_FOOTFIX2));
    drawText(1, 1, 16 + 2 * LINE_HEIGHT_PX, FPSTR(LS_FOOTFIX3));
    drawText(1, 1, 16 + 3 * LINE_HEIGHT_PX, FPSTR(LS_FOOTFIX4));
    display.display();

    unsigned long startMs = millis();
    while (!digitalRead(PIN_FOOT_SWITCH)) {
        wdt_reset();
        if ((millis() - startMs) > FOOT_SWITCH_FAULT_MS) break;
    }
}

/*==================================================================================================
*  Progress bar — used by sendWeldPulse() for engage + release delays
*  -----------------------------------------------------------------------------------------
*  Moved here from a_state_machine.ino in v4.1 — it is a pure rendering primitive and
*  belongs with the rest of the UI code.
*================================================================================================*/
uint16_t drawProgress(struct progress* o, bool clear)
{
    constexpr uint16_t steps = 126;
    constexpr uint16_t height = 8;
    constexpr uint16_t x = SSD1306_LCDWIDTH - steps - 1;
    constexpr uint16_t y = 55;

    if (clear) {
        display.fillRect(x - 1, y - 1, steps + 2, height + 2, BLACK);
        return 0;
    }

    if (!(o->opt & PGR_INIT)) {
        display.drawRect(x - 1, y - 1, steps + 2, height + 2, WHITE);
        display.fillRect(x, y, steps, height, (o->opt & PGR_ON) ? BLACK : WHITE);
        display.display();
        o->step = 1;
        o->millis = millis();
        o->opt |= PGR_INIT;
        return 0;
    }

    uint16_t elapsed = millis() - o->millis;
    uint16_t interval = o->time / steps;
    uint16_t expectedStep = (interval > 0) ? (elapsed / interval) : steps;

    if (expectedStep > o->step) {
        o->step = expectedStep;
        display.fillRect(x, y, o->step, height, (o->opt & PGR_ON) ? WHITE : BLACK);
        display.display();
    }
    return (o->step >= steps) ? 1 : 0;
}

// EOF — b_display.ino
