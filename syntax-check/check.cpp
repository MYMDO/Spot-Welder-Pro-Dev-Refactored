// Syntax-check harness for Spot-Welder-Pro firmware.
// Compiles all .ino + .h files into a single TU with Arduino API stubs.
// Used by .github/workflows/ci.yml to catch type errors without avr-gcc.
//
// Local invocation:
//   g++ -std=c++14 -fsyntax-only -Wall -Wextra -Isyntax-check -Isyntax-check/arduino \
//       syntax-check/check.cpp
#include "arduino/Arduino.h"

#define _DEVELOPMENT_
#define _LANG_EN_

// Project files live at the repo root, so we include them via relative path.
#include "../Spot-Welder-Pro.h"
#include "../Spot-Welder-Pro.ino"
#include "../a_state_machine.ino"
#include "../b_display.ino"
#include "../c_eeprom_sound.ino"
#include "../d_hardware.ino"

int main() {
    setup();
    for (int i = 0; i < 10; ++i) loop();
    return 0;
}
