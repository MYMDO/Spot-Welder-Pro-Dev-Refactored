# AGENTS.md

## Project

Arduino spot welder controller firmware (v4.1.0) for ATmega328P (Nano/Uno). Type-safe event-driven state machine with OLED UI, INA226 power monitoring, EEPROM persistence, watchdog-protected dual-pulse welding.

## Build

### Option A — Arduino IDE

Open `Spot-Welder-Pro.ino` (IDE auto-includes all `.ino` tabs in alphabetical order).

**Board:** Arduino Nano (ATmega328P), standard USB bootloader.

**Dependencies** (install via Library Manager):
- `Adafruit SSD1306`
- `INA226` by Rob Tillaart

**Build flags** in `Spot-Welder-Pro.h` (lines 36–46):
```cpp
#define _DEVELOPMENT_   // Serial debug at 115200 baud
// #define _BOOTSYS_    // Force boot into system menu
// #define _TESTING_    // Ignore low-battery alarm
#define _LANG_EN_       // Language: EN/DE/FR/ES/IT
```

### Option B — PlatformIO (recommended for CI / headless)

```bash
pio run                    # build default env
pio run -e production      # build production env (no Serial debug)
pio run -e testing         # build testing env (low-battery alarm bypassed)
pio run -t upload          # flash
```

### Option C — Syntax check without an AVR toolchain

A g++ stub harness lives at `/syntax-check/`. Build it with:
```bash
g++ -std=c++14 -fsyntax-only -Wall -Wextra -Isyntax-check -Isyntax-check/arduino syntax-check/check.cpp
```
This catches type errors, missing prototypes, and enum-mismatch bugs without needing avr-gcc.

## File structure

Alphabetical naming controls Arduino IDE tab order:

| File | Responsibility |
|---|---|
| `Spot-Welder-Pro.ino` | Entry: `setup()`, `loop()`, `isr()`, atomic helpers, globals, pin/init |
| `Spot-Welder-Pro.h` | All `#define` constants, pin mapping, `enum class`, structs, prototypes, language strings |
| `a_state_machine.ino` | Pure FSM: `stateMachine()` + all `handle*State()` + `markEEPROMDirty()` |
| `b_display.ino` | All OLED routines + `progress`/`drawProgress` |
| `c_eeprom_sound.ino` | `valStr()`, EEPROM load/save/reset, CRC-16, buzzer, `reboot()` |
| `d_hardware.ino` | Hardware abstraction: INA226, Hall sensor, NTC, weld pulse, I2C clock, `check*Event()` |

## Architecture

- **13 states**, **11 events** routed through `mEvent` volatile (typed `enum class`)
- **Strict layering:** FSM logic (a_state_machine) → hardware (d_hardware) + rendering (b_display) + persistence (c_eeprom_sound)
- **No I2C / analogRead in the FSM or display layers** — those calls live exclusively in `d_hardware.ino`
- All `mEvent` access via atomic helpers: `atomicReadEvent()`, `atomicReadAndClearEvent()`, `atomicSetEvent()` — use these, never touch `mEvent` directly
- `mEvent` is set by: ISR (encoder), `checkForBtnEvent()`, `checkForLowVoltageEvent()`, `checkTemp()`, `checkForSleepEvent()`
- `stateMachine()` called every `loop()` iteration
- Watchdog (250 ms) enabled at end of `setup()`, reset every `loop()` and inside every long-running loop (`micros()` busy-waits, `delay()` chunked waits)

## Type-safe enums

v4.1 uses `enum class` instead of plain enums. This prevents accidental mixing of states and events (a real bug source in v4.0.x where both enums leaked into the same scope).

- `enum class State : uint8_t { STANDBY, MAIN_SCREEN, ... }` — use `State::MAIN_SCREEN`
- `enum class Event : uint8_t { NONE, BTNDN, ... }` — use `Event::BTNDN`
- `enum class ValueFormat : uint8_t { BATTERY_ALARM, ... }` — use `ValueFormat::BATTERY_VOLTS`

Backwards-compat aliases (`ST_*`, `EV_*`, `VF_*`) are provided for the `valStr()` call sites, but new code should use the `enum class` form.

## Key gotchas

- **`sendWeldPulse()`** (`d_hardware.ino`) uses busy-wait `micros()` for pulse timing, not `delay()`. WDT is reset inside the loop. The MOSFET is forced LOW at entry AND exit, and held LOW on every iteration of the release-wait loop.
- **`batteryVoltage`** is updated in `checkForLowVoltageEvent()` every 2 s. Display routines only read the cached global — v4.0.x had a hidden `INA.getCurrent_mA()` call inside `drawStatusLine()` that has been removed.
- **EEPROM** auto-saves every 30 s via `updateEEPROM()`, AND within 2 s of `markEEPROMDirty()` being called. Settings editors in `a_state_machine.ino` call `markEEPROMDirty()` after every value change.
- **I2C clock** is set to 800 kHz only inside `sendWeldPulse()` (via `enterCriticalPulseMode()`) and restored to 100 kHz afterward (via `exitCriticalPulseMode()`).
- **Voltage units** are unified to millivolts (mV) everywhere except the EEPROM boundary, where `batteryAlarm` / `batteryhighAlarm` / `nominalVoltage` are stored as centivolts (units of 100 mV) for backwards compatibility with v4.0.x devices. Type aliases (`millivolts_t`, etc.) document intent.
- **Strings** are stored in PROGMEM (`FPSTR()` macro). Use `PSTR()` for `sprintf_P` format strings.
- **EEPROM layout is frozen** — see `static_assert(sizeof(progData) == 26, ...)` in the header. Adding new persistent fields requires appending to the END of the struct and bumping `_VERSION_MINOR_`.

## Compile-time safety nets

The header includes several `static_assert`s that catch common mistakes at compile time:

```cpp
static_assert(sizeof(progData) == 26, "EEPROM layout drift");           // AVR-only
static_assert(static_cast<uint8_t>(State::_COUNT) == 13, "State enum drift");
static_assert(static_cast<uint8_t>(Event::_COUNT) == 11, "Event enum drift");
```

If you add or remove a state/event/struct field, the corresponding assertion will fire and tell you to audit the affected switch statements / EEPROM migration code.

## Known issues

`implementation_plan.md` documents 15 issues from v4.0.x (4 critical, 4 significant, 7 quality). All critical and significant issues are resolved in v4.1; see the v4.1.0 changelog in README.md for the full mapping.

## How to add a new persistent setting

1. Add the field to the END of `progData` in `Spot-Welder-Pro.h` (do NOT reorder existing fields).
2. Update the `static_assert(sizeof(progData) == N, ...)` constant with the new size.
3. Initialise the field in `resetEEPROM()`.
4. Range-clamp it in `loadEEPROM()`.
5. Bump `_VERSION_MINOR_` and add a changelog entry.
6. Add a menu editor in `a_state_machine.ino` if user-adjustable.
7. Call `markEEPROMDirty()` after every change.
