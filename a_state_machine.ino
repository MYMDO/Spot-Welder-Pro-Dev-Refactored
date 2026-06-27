/***************************************************************************************************
*  Spot-Welder-Pro — Finite state machine                                                          *
*                                                                                                  *
*  This file contains the FSM dispatcher and all state-handler functions. It is PURE LOGIC:        *
*    - No I2C calls                                                                               *
*    - No analogRead / digitalWrite                                                               *
*    - No delay() in critical paths                                                               *
*                                                                                                  *
*  All hardware access is delegated to d_hardware.ino.                                             *
*  All rendering is delegated to b_display.ino.                                                    *
*  All persistence is delegated to c_eeprom_sound.ino.                                             *
*                                                                                                  *
*  The FSM has 13 states and 12 events. State transitions are driven by `mEvent`, an               *
*  ISR-fed volatile that the main loop reads atomically. See Spot-Welder-Pro.h for the enum defs.  *
***************************************************************************************************/

/*==================================================================================================
*  Menu state — file-local
*  -----------------------------------------------------------------------------------------
*  Encapsulates all the "where am I in the menu tree" variables that v4.0.x kept as scattered
*  globals. Grouping them makes the menu logic much easier to follow.
*================================================================================================*/
struct MenuState {
    uint8_t selectedMenu     = 0;  // Cursor position within the current menu page
    uint8_t selectedMainMenu = 0;  // Which top-level menu was entered
    uint8_t selectedSubMenu  = 0;  // Which sub-menu item is being edited
    bool    btn              = false;  // Latched "button-down seen" flag for the reboot menu
};
static MenuState menuState;

/*==================================================================================================
*  EEPROM dirty flag — set whenever a setting changes, consumed by updateEEPROM()
*  -----------------------------------------------------------------------------------------
*  v4.0.x only saved to EEPROM every 30 s. If the user changed a setting and power was lost
*  within that window, the change was lost. v4.1 keeps the 30 s autosave but ALSO saves
*  promptly (within EEPROM_DIRTY_SAVE_MS) after a setting changes, so a power cut at the
*  worst loses ~2 s of menu navigation rather than ~30 s.
*
*  Declared `extern` in Spot-Welder-Pro.h so c_eeprom_sound.ino can read them.
*================================================================================================*/
bool          eepromDirty     = false;
unsigned long eepromDirtyTime = 0;

void markEEPROMDirty() {
    eepromDirty     = true;
    eepromDirtyTime = millis();
}

/*==================================================================================================
*  stateMachine — top-level dispatcher (called every loop() iteration)
*================================================================================================*/
void stateMachine() {
    char str[8];

    Event currentEvent = atomicReadEvent();

    if (currentEvent == Event::BOOTDN) {
        mState = State::SYSTEM_SCREEN;
        atomicSetEvent(Event::NONE);
    } else {
        // Run the four periodic background checks. Each is throttled internally by its
        // own timer, so calling them every loop tick costs essentially nothing.
        checkForBtnEvent();
        checkForSleepEvent();
        checkForLowVoltageEvent();
        checkTemp();

        currentEvent = atomicReadEvent();

        // Pre-dispatch event filtering: some events always cause a state transition
        // regardless of the current state.
        switch (currentEvent) {
            case Event::STBY_TIMEOUT:
                mState = State::STANDBY;
                atomicSetEvent(Event::NONE);
                return;
            case Event::TEMP_HIGH:
                if (!sysMenu) mState = State::TEMP_HIGH;
                atomicSetEvent(Event::NONE);
                return;
            case Event::BTNUP:
                // BTNUP is consumed everywhere except in the reboot menu, where the
                // release-after-press is the trigger for the destructive action.
                if (mState != State::REBOOT_MENU) atomicSetEvent(Event::NONE);
                break;
            default:
                break;
        }
    }

    // Dispatch on the current state.
    switch (mState) {
        case State::STANDBY:          handleStandbyState();    break;
        case State::TEMP_HIGH:        handleTempHighState();   break;
        case State::MAIN_SCREEN:      enterMainScreen();       break;
        case State::MAIN_SCREEN_CNT:  handleMainScreenCnt();   break;
        case State::MENU_SCREEN:      handleMenuScreen(str);   break;
        case State::SUB_MENU_1:       handleSubMenu1(str);     break;
        case State::SUB_MENU_2:       handleSubMenu2(str);     break;
        case State::SYSTEM_SCREEN:    enterSystemScreen();     break;
        case State::SYSTEM_MENU:      handleSystemMenu(str);   break;
        case State::REBOOT_MENU:      handleRebootMenu(str);   break;
        case State::MAXWELD_SCREEN:   handleMaxWeldScreen(str);break;
        case State::INVERT_SCREEN:    handleInvertScreen();    break;
        default: break;  // Defensive — unknown state does nothing, WDT keeps ticking.
    }
}

/*==================================================================================================
*  State handlers — STANDBY / TEMP_HIGH
*  -----------------------------------------------------------------------------------------
*  Both states wait for a button press to return to the previous screen tree. They share
*  the same structure: render the message, watch for BTNDN, return to MAIN_SCREEN or
*  SYSTEM_SCREEN depending on which tree the user was in.
*================================================================================================*/
void handleStandbyState() {
    if (atomicReadEvent() == Event::BTNDN) {
        mState = sysMenu ? State::SYSTEM_SCREEN : State::MAIN_SCREEN;
    }
    message(FPSTR(LS_STANDBY), FPSTR(LS_CLICKBTN), FPSTR(LS_EXITSTBY));
    atomicSetEvent(Event::NONE);
}

void handleTempHighState() {
    if (atomicReadEvent() == Event::BTNDN) {
        mState = sysMenu ? State::SYSTEM_SCREEN : State::MAIN_SCREEN;
    }
    displayHighTemperature();
    atomicSetEvent(Event::NONE);
}

/*==================================================================================================
*  State handlers — MAIN_SCREEN / MAIN_SCREEN_CNT
*  -----------------------------------------------------------------------------------------
*  MAIN_SCREEN is a one-shot "enter" state that initialises UI state and drops into
*  MAIN_SCREEN_CNT, which is the steady-state home screen: it listens for weld triggers,
*  encoder turns (which adjust pulseTime), and a button press (which opens the menu).
*================================================================================================*/
void enterMainScreen() {
    mState = State::MAIN_SCREEN_CNT;
    sysMenu = false;
    menuState.selectedMenu = 0;
    displayMainScreen();
}

void handleMainScreenCnt() {
    static bool autoPulseTriggered = false;

    // Suppress weld firing while a low-battery alarm is active.
    if (lowVoltageAlarmActive) {
        autoPulseTriggered = false;
        displayMainScreen();
        return;
    }

    // --- Weld triggers (priority order: auto-pulse > foot switch) -------------
    if (digitalRead(PIN_AUTO_PULSE) && pData.pFlags.en_autoPulse) {
        if (!autoPulseTriggered) {
            autoPulseTriggered = true;
            displayMainScreen(true);  // invert "ms" label as a visual cue
            if (pData.pFlags.en_Sound) playBeep(1000, 100);
            sendWeldPulse(PIN_AUTO_PULSE,
                          static_cast<milliseconds_t>(pData.autoPulseDelay) * 100,
                          WP_RETRIGGER_DELAY_MS,
                          PULSE_ACTIVE_HIGH);
            displayMainScreen();
        }
    } else if (!digitalRead(PIN_FOOT_SWITCH)) {
        sendWeldPulse(PIN_FOOT_SWITCH, FS_TRIGGER_DELAY_MS, WP_RETRIGGER_DELAY_MS, PULSE_ACTIVE_LOW);
    } else {
        autoPulseTriggered = false;
    }

    // --- UI events ----------------------------------------------------------
    Event ev = atomicReadEvent();
    if (ev == Event::BTNDN) {
        mState = State::MENU_SCREEN;
        menuState.selectedMenu = 0;
        atomicSetEvent(Event::NONE);
        displayMenuType1(nullptr, FPSTR(LS_APULSE), FPSTR(LS_BATTALM1), FPSTR(LS_SHORTPLS1), 0);
        return;
    }

    if (ev == Event::ENCUP || ev == Event::ENCDN) {
        handleEncoderEvent(ev);
        markEEPROMDirty();
        atomicSetEvent(Event::NONE);
    }

    displayMainScreen();
}

void handleEncoderEvent(Event ev) {
    if (ev == Event::ENCUP) {
        pData.pulseTime = (pData.pulseTime < pData.maxPulseTime)
                            ? pData.pulseTime + 1 : pData.maxPulseTime;
    } else {
        pData.pulseTime = (pData.pulseTime > MIN_PULSE_TIME)
                            ? pData.pulseTime - 1 : MIN_PULSE_TIME;
    }
}

/*==================================================================================================
*  State handlers — MENU_SCREEN / SUB_MENU_1 / SUB_MENU_2
*  -----------------------------------------------------------------------------------------
*  Three-level menu tree:
*    MENU_SCREEN   →  Pulse Set | Batt Alarm | Shrt Pulse     (3 items)
*    SUB_MENU_1    →  depends on which top-level item was chosen
*    SUB_MENU_2    →  value editor for the chosen sub-item
*================================================================================================*/
void handleMenuScreen(char *str) {
    Event ev = atomicReadEvent();
    if (ev == Event::BTNDN) {
        mState = State::SUB_MENU_1;
        menuState.selectedMainMenu = menuState.selectedMenu;

        if (menuState.selectedMainMenu == 0) {
            displayMenuType1(FPSTR(LS_APULSE), FPSTR(LS_MANAUTO),
                             FPSTR(LS_DELAY), FPSTR(LS_WELDSOUND), menuState.selectedMenu);
        } else if (menuState.selectedMainMenu == 1) {
            displayMenuType1(FPSTR(LS_BATTALM), FPSTR(LS_LOWALRM),
                             FPSTR(LS_HIGHALRM), FPSTR(LS_EXIT), 0);
        } else if (menuState.selectedMainMenu == 2) {
            displayMenuType2(FPSTR(LS_SHORTPLS),
                             valStr(str, pData.shortPulseTime, ValueFormat::SHORT_PULSE),
                             FPSTR(LS_PCOF));
        }

        atomicSetEvent(Event::NONE);
        menuState.selectedMenu = 0;

    } else if (ev == Event::ENCUP || ev == Event::ENCDN) {
        if (ev == Event::ENCDN) {
            menuState.selectedMenu = (menuState.selectedMenu == 0) ? 2 : menuState.selectedMenu - 1;
        } else {
            menuState.selectedMenu = (menuState.selectedMenu == 2) ? 0 : menuState.selectedMenu + 1;
        }
        atomicSetEvent(Event::NONE);
        displayMenuType1(nullptr, FPSTR(LS_APULSE), FPSTR(LS_BATTALM1),
                         FPSTR(LS_SHORTPLS1), menuState.selectedMenu);
    }
}

void handleSubMenu1(char *str) {
    Event ev = atomicReadEvent();
    if (ev == Event::BTNDN) {
        if (menuState.selectedMainMenu == 0) {
            mState = State::SUB_MENU_2;
            menuState.selectedSubMenu = menuState.selectedMenu;

            if (menuState.selectedSubMenu == 0) {
                displayMenuType2(FPSTR(LS_AUTOPLSON), nullptr,
                                 pData.pFlags.en_autoPulse ? FPSTR(LS_AUTO) : FPSTR(LS_MANUAL));
            } else if (menuState.selectedSubMenu == 1) {
                displayMenuType2(FPSTR(LS_AUTOPLSDLY),
                                 valStr(str, pData.autoPulseDelay, ValueFormat::AUTO_DELAY),
                                 FPSTR(LS_SECONDS));
            } else if (menuState.selectedSubMenu == 2) {
                displayMenuType2(FPSTR(LS_WELDSOUNDM), nullptr,
                                 pData.pFlags.en_autoPulse ? FPSTR(LS_SOUNDON) : FPSTR(LS_SOUNDOFF));
            } else if (menuState.selectedSubMenu == 3) {
                displayMenuType2(FPSTR(LS_NOMVOLTMENU),
                                 valStr(str, pData.nominalVoltage, ValueFormat::BATTERY_ALARM),
                                 FPSTR(LS_VOLTAGE));
            }

        } else if (menuState.selectedMainMenu == 1) {
            if (menuState.selectedMenu < 2) {
                mState = State::SUB_MENU_2;
                menuState.selectedSubMenu = menuState.selectedMenu;
                if (menuState.selectedSubMenu == 0) {
                    displayMenuType2(FPSTR(LS_LOWALRMMENU),
                                     valStr(str, pData.batteryAlarm, ValueFormat::BATTERY_ALARM),
                                     FPSTR(LS_VOLTAGE));
                } else {
                    displayMenuType2(FPSTR(LS_HIGHALRMMENU),
                                     valStr(str, pData.batteryhighAlarm, ValueFormat::BATTERY_ALARM),
                                     FPSTR(LS_VOLTAGE));
                }
            } else {
                mState = State::MAIN_SCREEN;
            }

        } else {
            mState = State::MAIN_SCREEN;
        }

        atomicSetEvent(Event::NONE);
        menuState.selectedMenu = 0;

    } else if (ev == Event::ENCUP || ev == Event::ENCDN) {
        if (menuState.selectedMainMenu == 0) {
            if (ev == Event::ENCDN) {
                menuState.selectedMenu = (menuState.selectedMenu == 0) ? 3 : menuState.selectedMenu - 1;
            } else {
                menuState.selectedMenu = (menuState.selectedMenu == 3) ? 0 : menuState.selectedMenu + 1;
            }

            if (menuState.selectedMenu < 3) {
                displayMenuType1(FPSTR(LS_APULSE), FPSTR(LS_MANAUTO),
                                 FPSTR(LS_DELAY), FPSTR(LS_WELDSOUND), menuState.selectedMenu);
            } else {
                displayMenuType1(FPSTR(LS_APULSE), FPSTR(LS_DELAY),
                                 FPSTR(LS_WELDSOUND), FPSTR(LS_NOMVOLT), 2);
            }

        } else if (menuState.selectedMainMenu == 1) {
            if (ev == Event::ENCDN) {
                menuState.selectedMenu = (menuState.selectedMenu == 0) ? 2 : menuState.selectedMenu - 1;
            } else {
                menuState.selectedMenu = (menuState.selectedMenu == 2) ? 0 : menuState.selectedMenu + 1;
            }
            displayMenuType1(FPSTR(LS_BATTALM), FPSTR(LS_LOWALRM),
                             FPSTR(LS_HIGHALRM), FPSTR(LS_EXIT), menuState.selectedMenu);

        } else if (menuState.selectedMainMenu == 2) {
            if (ev == Event::ENCDN) {
                pData.shortPulseTime = (pData.shortPulseTime > MIN_SPULSE_TIME)
                                         ? pData.shortPulseTime - 1 : MIN_SPULSE_TIME;
            } else {
                pData.shortPulseTime = (pData.shortPulseTime < MAX_SPULSE_TIME)
                                         ? pData.shortPulseTime + 1 : MAX_SPULSE_TIME;
            }
            markEEPROMDirty();
            displayMenuType2(FPSTR(LS_SHORTPLS),
                             valStr(str, pData.shortPulseTime, ValueFormat::SHORT_PULSE),
                             FPSTR(LS_PCOF));
        }
        atomicSetEvent(Event::NONE);
    }
}

void handleSubMenu2(char *str) {
    Event ev = atomicReadEvent();
    if (ev == Event::BTNDN) {
        // Exit back to MAIN_SCREEN and persist any edited value.
        mState = State::MAIN_SCREEN;
        atomicSetEvent(Event::NONE);
        menuState.selectedMenu = 0;
        markEEPROMDirty();

    } else if (ev == Event::ENCUP || ev == Event::ENCDN) {
        if (menuState.selectedMainMenu == 0) {
            if (menuState.selectedSubMenu == 0) {
                pData.pFlags.en_autoPulse ^= 1;
                displayMenuType2(FPSTR(LS_AUTOPLSON), nullptr,
                                 pData.pFlags.en_autoPulse ? FPSTR(LS_AUTO) : FPSTR(LS_MANUAL));
            } else if (menuState.selectedSubMenu == 1) {
                if (ev == Event::ENCDN) {
                    pData.autoPulseDelay = (pData.autoPulseDelay > MIN_APULSE_DELAY)
                                             ? pData.autoPulseDelay - 1 : MIN_APULSE_DELAY;
                } else {
                    pData.autoPulseDelay = (pData.autoPulseDelay < MAX_APULSE_DELAY)
                                             ? pData.autoPulseDelay + 1 : MAX_APULSE_DELAY;
                }
                displayMenuType2(FPSTR(LS_AUTOPLSDLY),
                                 valStr(str, pData.autoPulseDelay, ValueFormat::AUTO_DELAY),
                                 FPSTR(LS_SECONDS));
            } else if (menuState.selectedSubMenu == 2) {
                pData.pFlags.en_Sound ^= 1;
                displayMenuType2(FPSTR(LS_WELDSOUNDM), nullptr,
                                 pData.pFlags.en_Sound ? FPSTR(LS_SOUNDON) : FPSTR(LS_SOUNDOFF));
            } else if (menuState.selectedSubMenu == 3) {
                if (ev == Event::ENCDN) {
                    pData.nominalVoltage = (pData.nominalVoltage > MIN_NOMINAL_VOLTAGE_EE)
                                             ? pData.nominalVoltage - 1 : MIN_NOMINAL_VOLTAGE_EE;
                } else {
                    pData.nominalVoltage = (pData.nominalVoltage < MAX_NOMINAL_VOLTAGE_EE)
                                             ? pData.nominalVoltage + 1 : MAX_NOMINAL_VOLTAGE_EE;
                }
                displayMenuType2(FPSTR(LS_NOMVOLTMENU),
                                 valStr(str, pData.nominalVoltage, ValueFormat::BATTERY_ALARM),
                                 FPSTR(LS_VOLTAGE));
            }
        } else if (menuState.selectedMainMenu == 1) {
            // Battery alarm editors — keep lowAlarm < highAlarm by at least 100 mV.
            if (menuState.selectedSubMenu == 0) {
                uint16_t maxLowAlarm = (pData.batteryhighAlarm > 0)
                                         ? pData.batteryhighAlarm - 1
                                         : MIN_BATT_ALARM_MV / 100;
                if (ev == Event::ENCDN) {
                    pData.batteryAlarm = (pData.batteryAlarm > MIN_BATT_ALARM_MV / 100)
                                           ? pData.batteryAlarm - 1 : MIN_BATT_ALARM_MV / 100;
                } else {
                    pData.batteryAlarm = (pData.batteryAlarm < maxLowAlarm)
                                           ? pData.batteryAlarm + 1 : maxLowAlarm;
                }
                displayMenuType2(FPSTR(LS_LOWALRMMENU),
                                 valStr(str, pData.batteryAlarm, ValueFormat::BATTERY_ALARM),
                                 FPSTR(LS_VOLTAGE));
            } else if (menuState.selectedSubMenu == 1) {
                uint16_t minHighAlarm = pData.batteryAlarm + 1;
                if (ev == Event::ENCDN) {
                    pData.batteryhighAlarm = (pData.batteryhighAlarm > minHighAlarm)
                                               ? pData.batteryhighAlarm - 1 : minHighAlarm;
                } else {
                    pData.batteryhighAlarm = (pData.batteryhighAlarm < MAX_BATT_ALARM_MV / 100)
                                               ? pData.batteryhighAlarm + 1
                                               : MAX_BATT_ALARM_MV / 100;
                }
                displayMenuType2(FPSTR(LS_HIGHALRMMENU),
                                 valStr(str, pData.batteryhighAlarm, ValueFormat::BATTERY_ALARM),
                                 FPSTR(LS_VOLTAGE));
            }
        }
        markEEPROMDirty();
        atomicSetEvent(Event::NONE);
    }
}

/*==================================================================================================
*  State handlers — System menu tree (SYSTEM_SCREEN / SYSTEM_MENU / REBOOT_MENU / MAXWELD / INVERT)
*================================================================================================*/
void enterSystemScreen() {
    mState = State::SYSTEM_MENU;
    sysMenu = true;
    menuState.selectedMenu = 0;
    displayMenuType1(FPSTR(LS_SYSMENU), FPSTR(LS_MAXPULSE),
                     FPSTR(LS_DISPLAY), FPSTR(LS_BOOT), 0);
}

void handleSystemMenu(char *str) {
    Event ev = atomicReadEvent();
    if (ev == Event::BTNDN) {
        atomicSetEvent(Event::NONE);

        if (menuState.selectedMenu == 0) {
            displayMenuType2(FPSTR(LS_MAXPLSMENU),
                             valStr(str, pData.maxPulseTime, ValueFormat::PULSE_DELAY),
                             FPSTR(LS_MS));
            mState = State::MAXWELD_SCREEN;
            atomicSetEvent(Event::NONE);

        } else if (menuState.selectedMenu == 1) {
            menuState.selectedSubMenu = 0;
            mState = State::INVERT_SCREEN;
            displayMenuType2(FPSTR(LS_INVERTMENU), nullptr,
                             pData.pFlags.en_oledInvert ? FPSTR(LS_SCRINV) : FPSTR(LS_SCRNORM));

        } else if (menuState.selectedMenu == 2) {
            menuState.btn = false;
            mState = State::REBOOT_MENU;
            menuState.selectedMenu = 0;
            displayMenuType1(FPSTR(LS_BOOTMENU), FPSTR(LS_REBOOT),
                             FPSTR(LS_SAFERST), FPSTR(LS_FULLRST), 0);
        }

        menuState.selectedMenu = 0;

    } else if (ev == Event::ENCUP || ev == Event::ENCDN) {
        if (ev == Event::ENCDN) {
            menuState.selectedMenu = (menuState.selectedMenu == 0) ? 2 : menuState.selectedMenu - 1;
        } else {
            menuState.selectedMenu = (menuState.selectedMenu == 2) ? 0 : menuState.selectedMenu + 1;
        }
        atomicSetEvent(Event::NONE);
        displayMenuType1(FPSTR(LS_SYSMENU), FPSTR(LS_MAXPULSE),
                         FPSTR(LS_DISPLAY), FPSTR(LS_BOOT), menuState.selectedMenu);
    }
}

void handleRebootMenu(char * /*str*/) {
    Event ev = atomicReadEvent();
    if (ev == Event::BTNDN) {
        menuState.btn = true;  // Latch — wait for the release to fire the action.
    } else if ((ev == Event::BTNUP) && menuState.btn) {
        if (menuState.selectedMenu == 1)      resetEEPROM(false);
        else if (menuState.selectedMenu == 2) resetEEPROM(true);

        const __FlashStringHelper *actionMsg =
            (menuState.selectedMenu == 1) ? FPSTR(LS_REBOOTSR) :
            (menuState.selectedMenu == 2) ? FPSTR(LS_REBOOTFR) :
                                            FPSTR(LS_REBOOTNR);

        message(FPSTR(LS_REBOOT), actionMsg, FPSTR(LS_WAITMSG), 2);
        delay(MESSAGE_BLOCK_MS);  // Give the user time to read before the device vanishes.
        reboot();

    } else if (ev == Event::ENCUP || ev == Event::ENCDN) {
        if (ev == Event::ENCDN) {
            menuState.selectedMenu = (menuState.selectedMenu == 0) ? 2 : menuState.selectedMenu - 1;
        } else {
            menuState.selectedMenu = (menuState.selectedMenu == 2) ? 0 : menuState.selectedMenu + 1;
        }
        displayMenuType1(FPSTR(LS_BOOTMENU), FPSTR(LS_REBOOT),
                         FPSTR(LS_SAFERST), FPSTR(LS_FULLRST), menuState.selectedMenu);
    }
    atomicSetEvent(Event::NONE);
}

void handleMaxWeldScreen(char *str) {
    Event ev = atomicReadEvent();
    if (ev == Event::BTNDN) {
        message(FPSTR(LS_MAXPULSE), FPSTR(LS_MAXPMSG), FPSTR(LS_WAITMSG), 2);
        mState = State::SYSTEM_SCREEN;
        menuState.selectedMenu = 0;
        markEEPROMDirty();

    } else if (ev == Event::ENCUP || ev == Event::ENCDN) {
        if (ev == Event::ENCDN) {
            pData.maxPulseTime = (pData.maxPulseTime > MIN_PULSE_TIME)
                                   ? pData.maxPulseTime - 1 : MIN_PULSE_TIME;
        } else {
            pData.maxPulseTime = (pData.maxPulseTime < ABS_MAX_PULSE_TIME)
                                   ? pData.maxPulseTime + 1 : ABS_MAX_PULSE_TIME;
        }
        // Belt-and-suspenders clamp (in case MIN/MAX ever get misconfigured).
        if (pData.maxPulseTime < MIN_PULSE_TIME)     pData.maxPulseTime = MIN_PULSE_TIME;
        if (pData.maxPulseTime > ABS_MAX_PULSE_TIME) pData.maxPulseTime = ABS_MAX_PULSE_TIME;
        // If the new max is below the current pulseTime, clamp pulseTime down.
        if (pData.pulseTime > pData.maxPulseTime)    pData.pulseTime = pData.maxPulseTime;

        displayMenuType2(FPSTR(LS_MAXPLSMENU),
                         valStr(str, pData.maxPulseTime, ValueFormat::PULSE_DELAY),
                         FPSTR(LS_MS));
    }
    atomicSetEvent(Event::NONE);
}

void handleInvertScreen() {
    Event ev = atomicReadEvent();
    if (ev == Event::BTNDN) {
        mState = State::SYSTEM_SCREEN;
        atomicSetEvent(Event::NONE);
        menuState.selectedMenu = 0;
        markEEPROMDirty();

    } else if (ev == Event::ENCUP || ev == Event::ENCDN) {
        pData.pFlags.en_oledInvert = !pData.pFlags.en_oledInvert;
        display.setRotation(pData.pFlags.en_oledInvert ? OLED_INVERT_ROTATION : OLED_NORMAL_ROTATION);
        displayMenuType2(FPSTR(LS_INVERTMENU), nullptr,
                         pData.pFlags.en_oledInvert ? FPSTR(LS_SCRINV) : FPSTR(LS_SCRNORM));
    }
    atomicSetEvent(Event::NONE);
}

// EOF — a_state_machine.ino
