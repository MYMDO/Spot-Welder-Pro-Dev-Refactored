# ⚡ Spot Welder Pro

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Arduino%20%28ATmega328P%29-00979D?logo=arduino)](https://www.arduino.cc/)
[![Firmware](https://img.shields.io/badge/Firmware-v4.1.0-green)](Spot-Welder-Pro.h)
[![Language](https://img.shields.io/badge/Language-C%2B%2B14-informational)](https://isocpp.org/)
[![CI](https://github.com/MYMDO/Spot-Welder-Pro/actions/workflows/ci.yml/badge.svg)](https://github.com/MYMDO/Spot-Welder-Pro/actions/workflows/ci.yml)

> 🇺🇦 Arduino-based spot welder controller for battery pack welding — featuring an OLED display, INA226 power monitoring, dual-pulse mode, foot switch support, watchdog-protected event-driven state machine, and persistent EEPROM settings with CRC-16 integrity.

---

## Table of Contents

- [Features](#features)
- [Hardware Requirements](#hardware-requirements)
- [Pin Mapping](#pin-mapping)
- [Firmware Architecture](#firmware-architecture)
- [Library Dependencies](#library-dependencies)
- [Getting Started](#getting-started)
- [Configuration](#configuration)
- [Operating Modes](#operating-modes)
- [Menu System](#menu-system)
- [Default Parameters](#default-parameters)
- [Project Structure](#project-structure)
- [Building with PlatformIO](#building-with-platformio)
- [Safety Notes](#safety-notes)
- [Contributing](#contributing)
- [Changelog](#changelog)
- [License](#license)

---

## Features

- **Dual-pulse welding** — configurable short pre-pulse + main pulse for better weld quality
- **Auto & Manual modes** — automatic pulse via touch sensor or manual trigger via foot switch / encoder button
- **Real-time monitoring** — battery voltage and weld current via INA226 over I2C
- **Voltage compensation** — pulse duration automatically adjusted for battery voltage variation (clamped to [0.7×, 1.4×] baseTime to prevent runaway)
- **OLED UI** — 128×64 SSD1306 display with menu navigation via rotary encoder
- **Protection system** — low/high battery voltage alarms, over-temperature alarm (NTC), watchdog timer (250 ms)
- **EEPROM persistence** — all settings saved automatically every 30 s + within 2 s of any change, with CRC-16/MODBUS integrity check
- **Weld counter** — cumulative weld count stored in EEPROM
- **Standby mode** — auto standby after configurable inactivity timeout (~10.7 minutes)
- **Sound feedback** — piezo buzzer on weld events and alarms
- **Screen invert** — 0°/180° OLED rotation option
- **Multi-language skeleton** — EN / DE / FR / ES / IT (English active by default)
- **Debug output** — Serial diagnostic mode (`_DEVELOPMENT_` build flag, 115200 baud)
- **Type-safe FSM** — `enum class State` / `enum class Event` prevent accidental cross-type bugs
- **Layered architecture** — pure FSM logic, isolated hardware abstraction, separate rendering & persistence layers

---

## Hardware Requirements

| Component | Specification |
|-----------|---------------|
| MCU | Arduino (ATmega328P), e.g. Arduino Nano / Uno |
| OLED display | 128×64 SSD1306, I2C |
| Current/voltage sensor | INA226, I2C (addr `0x40`) |
| Rotary encoder | Incremental, with push button |
| Weld pulse switch | MOSFET gate or SSR driven from digital pin 5 |
| Foot switch | Normally-open, pull-up |
| Auto-pulse sensor | Touch / proximity, active-high on pin 3 |
| Buzzer | Passive piezo |
| Temperature sensor | NTC thermistor on A3 |
| Current sensor (weld) | Hall effect sensor SS49E on A0 |
| Shunt resistor | 0.01 Ω / INA226 path |
| Power source | LiPo / Li-ion pack (supported range: 3.5 V – 15 V) |

---

## Pin Mapping

| Signal | Arduino Pin | Notes |
|---|---|---|
| Encoder CLK | **D2** | INT0 — hardware interrupt |
| Encoder DT | **D8** | Phase detection |
| Encoder SW | **D6** | Pull-up, active-low |
| Foot switch | **D7** | Pull-up, active-low |
| Auto-pulse sensor | **D3** | Active-high |
| Weld pulse output | **D5** | To MOSFET gate / SSR |
| Buzzer | **A1** | Passive piezo |
| Temperature (NTC) | **A3** | Voltage divider to GND |
| Hall sensor (weld current) | **A0** | SS49E — read during pulse |
| OLED + INA226 (I2C) | **A4 / A5** | SDA / SCL |

**I2C addresses:**
- SSD1306 OLED: `0x3C`
- INA226: `0x40`

---

## Firmware Architecture

The firmware is built around a **type-safe event-driven state machine** with strict separation of concerns:

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Spot-Welder-Pro.ino  —  Entry: setup(), loop(), ISR, atomic helpers    │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  a_state_machine.ino  —  Pure FSM: 13 states × 11 events                │
│    stateMachine() → handle*State() → menu navigation                    │
└─────────────────────────────────────────────────────────────────────────┘
              │                    │                    │
              ▼                    ▼                    ▼
┌──────────────────────────┐ ┌──────────────────┐ ┌─────────────────────┐
│  d_hardware.ino          │ │  b_display.ino   │ │  c_eeprom_sound.ino │
│  Hardware abstraction    │ │  OLED rendering  │ │  Persistence + audio│
│  - INA226 reads          │ │  - Status line   │ │  - CRC-16/MODBUS    │
│  - Hall sensor ADC       │ │  - Menus         │ │  - Dirty-flag saves │
│  - NTC thermistor        │ │  - Progress bar  │ │  - valStr()         │
│  - Weld pulse output     │ │  - Alarms        │ │  - Piezo sounds     │
│  - I2C clock mgmt        │ │  - Splash        │ │  - reboot()         │
│  - check*Event() ticks   │ │                  │ │                     │
└──────────────────────────┘ └──────────────────┘ └─────────────────────┘
```

**States (13):** `STANDBY`, `MAIN_SCREEN`, `MAIN_SCREEN_CNT`, `MENU_SCREEN`, `SUB_MENU_1/2`, `BATTERY_HIGH`, `TEMP_HIGH`, `SYSTEM_SCREEN`, `SYSTEM_MENU`, `REBOOT_MENU`, `MAXWELD_SCREEN`, `INVERT_SCREEN`.

**Events (11):** `NONE`, `BTNDN`, `BTNUP`, `ENCUP`, `ENCDN`, `BOOTDN`, `STBY_TIMEOUT`, `BATT_LV`, `BATT_HV`, `TEMP_HIGH`, `EEUPD`.

All access to the shared `mEvent` volatile is wrapped in `cli()/sei()` atomic helpers (`atomicReadEvent()`, `atomicReadAndClearEvent()`, `atomicSetEvent()`). The same applies to the 4-byte `lastActiveTime`.

### EEPROM Layout (frozen, do NOT reorder)

```
Address  Size  Field
------  ----  -----
  0      4    Magic ID (0x18fae9c8) — validates EEPROM was initialised by us
  4     26    progData struct (settings + statistics)
 30      2    CRC-16/MODBUS over progData bytes
```

The `sizeof(progData) == 26` invariant is enforced by `static_assert` at compile time. Reordering, removing, or resizing any field in `progData` will break EEPROM compatibility with previously-deployed devices.

---

## Library Dependencies

Install via **Arduino Library Manager** or **PlatformIO** (recommended):

| Library | Purpose | Install name |
|---------|---------|---|
| [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306) | OLED display driver | `Adafruit SSD1306` |
| [INA226](https://github.com/RobTillaart/INA226) | Voltage/current sensing | `INA226` by Rob Tillaart |
| EEPROM | Settings persistence | Built-in (AVR) |
| Wire | I2C bus | Built-in (Arduino) |
| avr/wdt.h | Watchdog timer | Built-in (AVR) |

---

## Getting Started

### Option A — Arduino IDE (beginner-friendly)

1. **Clone the repository**
   ```bash
   git clone https://github.com/MYMDO/Spot-Welder-Pro.git
   ```

2. **Open in Arduino IDE**
   Open `Spot-Welder-Pro.ino` — the IDE will automatically include all `.ino` tab files in alphabetical order.

3. **Install dependencies**
   Use **Sketch → Include Library → Manage Libraries** and install `Adafruit SSD1306` and `INA226`.

4. **Configure build flags (optional)**
   Edit the top of `Spot-Welder-Pro.h` before flashing:
   ```cpp
   #define _DEVELOPMENT_    // Enable Serial debug output (115200 baud)
   // #define _BOOTSYS_     // Force boot into system menu (for testing)
   // #define _TESTING_     // Ignore low-battery alarm
   #define _LANG_EN_        // Language: _LANG_EN_ / _LANG_DE_ / _LANG_FR_ / _LANG_ES_ / _LANG_IT_
   ```

5. **Select board and flash**
   - **Board:** Arduino Nano (ATmega328P) or compatible
   - **Programmer:** USB (standard bootloader)
   - Upload via **Sketch → Upload**

6. **First boot**
   On first boot with blank EEPROM, the firmware loads defaults automatically and validates them with CRC. Hold the encoder button during power-on to enter the system menu.

### Option B — PlatformIO (recommended for power users)

```bash
pip install platformio
git clone https://github.com/MYMDO/Spot-Welder-Pro.git
cd Spot-Welder-Pro
pio run                  # build
pio run -t upload        # build + flash (requires the board on USB)
pio device monitor       # serial monitor (115200 baud)
```

---

## Configuration

All parameters are adjustable at runtime via the on-device menu and persisted to EEPROM. Key compile-time limits defined in `Spot-Welder-Pro.h`:

| Define | Default | Description |
|--------|---------|-------------|
| `_SERIAL_BAUD_` | `115200` | Serial debug baud rate |
| `INA_MAX_CURRENT_A` | `8` | INA226 full-scale current (A) |
| `INA_SHUNT_RESISTANCE_OHM` | `0.01` | Shunt resistor value (Ω) |
| `INA_AVERAGING_MODE` | `4` | INA226 averaging samples |
| `STANDBY_TIMEOUT_MS` | `640000` | Idle-to-standby timeout (ms) |
| `EEPROM_AUTOSAVE_MS` | `30000` | EEPROM periodic auto-save interval (ms) |
| `EEPROM_DIRTY_SAVE_MS` | `2000` | Debounced save after a setting changes (ms) |
| `WP_RETRIGGER_DELAY_MS` | `500` | Min delay between welds (ms) |
| `FS_TRIGGER_DELAY_MS` | `200` | Foot switch debounce delay (ms) |
| `WDT_TIMEOUT_MS` | `250` | Watchdog timeout (ms) |
| `I2C_NORMAL_CLOCK_HZ` | `100000` | Standard I2C speed for UI / battery sampling |
| `I2C_FAST_CLOCK_HZ` | `800000` | Fast I2C speed for INA226 read during pulse |
| `PULSE_COMP_MIN_FACTOR` | `0.7` | Min voltage-compensation factor (× baseTime) |
| `PULSE_COMP_MAX_FACTOR` | `1.4` | Max voltage-compensation factor (× baseTime) |

---

## Operating Modes

### Auto-pulse mode
Weld triggered automatically when the auto-pulse sensor (D3) detects electrode contact. After the configurable delay (`autoPulseDelay`), the pulse fires without manual input. A progress bar is shown during the delay.

### Manual mode
Weld triggered by pressing the foot switch (D7) or the encoder button. Pulse starts immediately on activation.

### Dual-pulse sequence
When `shortPulseTime > 0`, each weld fires a short pre-pulse (% of main pulse time) followed by the full main pulse. This cleans the nickel strip surface and improves weld penetration.

### Voltage compensation
The main pulse duration is dynamically adjusted based on the live battery voltage:
```
compensatedTime = baseTime × (Vnom / Vact)²
```
The result is clamped to `[0.7×, 1.4×] × baseTime` to prevent runaway when the battery is unusually low (which would over-extend the pulse) or high (which would under-fire). All arithmetic is integer (no `float` on the AVR).

---

## Menu System

Navigate with the rotary encoder; press to confirm.

```
Main Screen
│
├── [Pulse Set]        — Weld pulse duration (1–100 ms)
├── [Shrt Pulse]       — Short pre-pulse (0–100% of pulse time)
├── [Batt Alarm]       — Low / High battery voltage alarms
│   ├── [Low Alarm]    — Low voltage threshold
│   ├── [High Alarm]   — High voltage threshold
│   └── [Exit]
│
└── [Mode] submenu
    ├── [Auto/Manual]  — Toggle auto-pulse mode
    ├── [Delay]        — Auto-pulse delay
    ├── [Sound]        — Weld sound on/off
    └── [Vnom]         — Nominal voltage for compensation

System Menu (hold button on boot)
├── [Max Pulse]        — Maximum allowed pulse time
├── [Display]
│   └── Screen Orientation — Normal / Inverted
└── [Boot]
    ├── Reboot
    ├── Safe Reset — Reload defaults, keep weld count
    └── Full Reset — Clear all EEPROM
```

---

## Default Parameters

| Parameter | Default | Range |
|-----------|---------|-------|
| Pulse time | 5 ms | 1–100 ms |
| Max pulse time | 100 ms | 1–100 ms |
| Short pulse | 0 % | 0–100 % |
| Auto-pulse delay | 2000 ms | 500–5000 ms |
| Low battery alarm | 7500 mV | 3500–15000 mV |
| High battery alarm | 15000 mV | 3500–15000 mV |
| Nominal voltage | 8000 mV | 3000–15000 mV |
| High-temp alarm | 65 °C | — |
| Auto-pulse | Enabled | — |
| Weld sound | Enabled | — |
| OLED orientation | Normal | Normal / Inverted |

---

## Project Structure

```
Spot-Welder-Pro/
├── Spot-Welder-Pro.ino      # Entry: setup(), loop(), ISR, atomic helpers
├── Spot-Welder-Pro.h        # Config, pin map, enums, structs, language strings
├── a_state_machine.ino      # Pure FSM logic (no hardware code)
├── b_display.ino            # OLED rendering + progress bar
├── c_eeprom_sound.ino       # EEPROM persistence + buzzer + valStr()
├── d_hardware.ino           # Hardware abstraction (sensors, pulse, I2C)
├── platformio.ini           # PlatformIO build config (3 environments)
├── library.properties       # Arduino Library Manager metadata
├── .clang-format            # Code style config
├── .github/workflows/ci.yml # GitHub Actions CI (build + syntax + lint)
├── implementation_plan.md   # Historical analysis & improvement roadmap
├── AGENTS.md                # Developer guide
├── README.md                # This file
└── LICENSE                  # GPL-3.0
```

---

## Building with PlatformIO

Three build environments are pre-configured:

| Environment | Build flags | Use case |
|---|---|---|
| `nano_atmega328` | `_DEVELOPMENT_` + `_LANG_EN_` | Default — development with Serial debug |
| `production` | `_LANG_EN_` only | Production — no Serial debug, smaller image, faster boot |
| `testing` | `_DEVELOPMENT_` + `_LANG_EN_` + `_TESTING_` | Bench testing — bypasses low-battery alarm |

```bash
pio run                           # build default env
pio run -e production             # build production env
pio run -e production -t upload   # flash production env
pio device monitor -e nano_atmega328  # serial monitor
```

---

## Safety Notes

> ⚠️ **This device controls high-current welding pulses. Incorrect wiring or software misconfiguration can cause fire, battery damage, or personal injury. Build and operate at your own risk.**

- Always verify MOSFET/SSR wiring before first power-on.
- The foot switch is checked at startup — if pressed during boot, the device halts and requires a clean restart.
- The watchdog timer (250 ms) will force a hardware reset if the main loop stalls. It is reset inside the `micros()` busy-wait loop during pulses, so a hang anywhere in the pulse path still triggers a reset.
- The weld pulse output is **defensively forced LOW** at both entry and exit of `sendWeldPulse()`, regardless of any earlier state. If the user holds the foot switch down indefinitely after a pulse, the MOSFET is held OFF on every iteration of the release-wait loop.
- Battery voltage is measured every 2 s; audible and visual alarms activate outside the configured thresholds.
- Temperature is sampled every 10 s; the welder disables pulse output above 65 °C (default) until cooling.
- The voltage-compensation factor is clamped to `[0.7×, 1.4×] × baseTime` so a faulty battery reading cannot cause a wildly over- or under-fired pulse.
- I2C clock is restored to 100 kHz (normal mode) after each weld pulse — the 800 kHz fast mode is only used inside the critical INA226 read.
- Remove power before modifying hardware connections.

---

## Contributing

Pull requests and issues are welcome.

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/my-improvement`
3. Run `clang-format -i *.ino *.h` before committing (style is enforced in CI)
4. Commit your changes: `git commit -m "Add: description"`
5. Push and open a Pull Request

Please:
- Keep `sizeof(progData)` unchanged — see the EEPROM compatibility contract in `Spot-Welder-Pro.h`.
- Add new persistent fields at the END of the struct, and bump `_VERSION_MINOR_`.
- Document any new `#define` constants in this README.
- Update `AGENTS.md` if you change the file architecture.

---

## Changelog

### v4.1.0 — Professional refactor

**Type safety & architecture**
- `enum class State` / `Event` / `ValueFormat` replace plain enums (prevents cross-type bugs)
- Type aliases: `millivolts_t`, `milliamps_t`, `celsius_t`, `milliseconds_t`, `adc_t`
- `static_assert` locks the EEPROM layout (26 bytes) and FSM enum counts
- New `d_hardware.ino` isolates all sensor / pulse / I2C access from the FSM
- Removed dead `printf.h` (was unused — no `#include` anywhere)

**Safety critical fixes**
- ISR now writes `lastActiveTime` atomically (was a non-atomic 4-byte write on 8-bit AVR)
- `sendWeldPulse()` forces `PIN_PULSE` LOW on entry AND exit (defense in depth)
- WDT is reset INSIDE the `micros()` busy-wait loop, not just before it
- Release-wait loop drives the MOSFET LOW on every iteration — holding the pedal cannot re-fire a pulse
- I2C clock side-effect fixed: normal 100 kHz is restored after each pulse
- All `delay()` calls in critical paths replaced with WDT-safe chunked waits

**EEPROM improvements**
- Dirty-flag pattern: settings changes save within 2 s instead of waiting up to 30 s
- `loadEEPROM()` range-clamps every persisted field, not just battery alarms
- Removed unconditional `EEPROM.write()` full-clear in `resetEEPROM()`

**Code quality**
- All magic numbers replaced with named constants
- Fixed `batteryAmphere` typo (now uses `readBatteryCurrent()` returning `milliamps_t`)
- `[[nodiscard]]` on read-only helpers
- ADC oversampling (+2 bits via 4-sample averaging) for the Hall sensor
- NTC table size now derived at compile time
- Voltage compensation clamped to `[0.7×, 1.4×] × baseTime`

**Tooling**
- `platformio.ini` with three build environments (default, production, testing)
- `library.properties` for Arduino Library Manager
- `.clang-format` for code style
- GitHub Actions CI: build matrix + syntax check + clang-format lint

### v4.0.3 — Baseline

Initial public release. See `implementation_plan.md` for the analysis that drove v4.1.

---

## License

This project is licensed under the **GNU General Public License v3.0** — see [LICENSE](LICENSE) for details.

---

*Made in Ukraine 🇺🇦 · [Sponsor](http://coindrop.to/mymdo)*
