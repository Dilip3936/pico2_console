# Pico 2 Wireless Handheld Console

A team project for **EE4500 — Embedded Programming**, developed in collaboration with [@Dhyana2904](https://github.com/Dhyana2904).

## Overview

Two Raspberry Pi Pico 2 handheld consoles communicate directly over an nRF24L01+ radio link. Each console handles its own graphics, controls and sound; the radio carries compact game state and inputs rather than video or audio.

Wireless gameplay is **host-authoritative**: the host runs the game simulation, while the joiner sends controls and renders the received state. Either console can host. The project includes Flappy, Pong, Grid Blasters and Race Duel, plus an SDL2 desktop simulator for local development.

Highlights:

- 320×240 RGB565 graphics with two framebuffers and asynchronous display DMA.
- Fixed 30 Hz game logic, joystick input and debounced buttons.
- Local 24 kHz sampled sound using PWM and DMA.
- Shared pairing and multiplayer transport with game-defined packet bodies.
- Fresh snapshots replace outdated state; commands/events repeat until acknowledged by the game.

See [the project report](report.md) for the protocol, design decisions and measured results.

## Components

Each console uses:

| Component | Purpose |
| --- | --- |
| Raspberry Pi Pico 2 (RP2350, non-W) | Game logic and peripheral control |
| 2.8-inch ST7789 SPI display | 320×240 landscape graphics |
| nRF24L01+ PA+LNA module | Direct 2.4 GHz wireless link |
| Joystick/button shield | Two analog axes and seven buttons, including joystick click |
| SmartElex Digital Speaker Module | Local sound effects |
| USB power, jumper wires and headers | Power and interconnections |

Two sets are needed for physical wireless play. Display touch input is not used; microphone experiments are not part of the gameplay build.

## Pin layout

Both consoles use the same assignments. **GP numbers below are GPIO numbers, not physical header-pin numbers.**

| Peripheral | Signal | Pico GPIO |
| --- | --- | --- |
| Display — SPI0 | CS | GP17 |
| Display — SPI0 | SCK | GP18 |
| Display — SPI0 | SDI / MOSI | GP19 |
| Display | DC | GP20 |
| Display | RESET | GP21 |
| Display | SDO / MISO, connected but unused by normal transfers | GP16 |
| Radio — SPI1 | CE | GP11 |
| Radio — SPI1 | MISO | GP12 |
| Radio — SPI1 | CSN | GP13 |
| Radio — SPI1 | SCK | GP14 |
| Radio — SPI1 | MOSI | GP15 |
| Buttons | A / B / C / D | GP5 / GP8 / GP4 / GP7 |
| Buttons | E / F / K (joystick click) | GP3 / GP6 / GP2 |
| Joystick | X / Y | GP27 (ADC1) / GP26 (ADC0) |
| Speaker | VIN / signal | GP22 |

- Buttons are active-low with internal pull-ups and 20 ms software debounce.
- Radio IRQ is unused; radio status is polled through SPI1.
- The recorded setup powers the display VCC/LED from VBUS, and the radio and speaker from 3V3 OUT, with common ground. This is module-specific, not a general wiring recommendation: check voltage/current requirements and backlight circuitry before connecting another module.
- PA+LNA radios need a stable 3.3 V supply and suitable local decoupling. Joystick supply wiring and overall power design must be checked for the hardware used.

## Build and run

Run the following commands from the repository root.

### Desktop simulator

Requires CMake, a native C/C++ compiler, a build tool such as Make or Ninja, and **SDL2 development libraries**.

```sh
cmake -S . -B build_sim -DPICO_BUILD=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build_sim --target sim -j4
./build_sim/sim
```

The simulator shares the console, rendering and game code. It supports local gameplay development, but does not emulate the physical nRF link or display-panel scanning.

### Pico 2 firmware

Requires CMake, a build tool, the **Arm GNU embedded toolchain** (`arm-none-eabi-gcc` / `arm-none-eabi-g++`) and a **Raspberry Pi Pico SDK with RP2350 support** (2.x or later). Install the SDK and its required submodules, then replace the SDK path below with its location.

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build_pico -DPICO_BUILD=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build_pico --target pico_game -j4
```

CMake selects the `pico2` board. The firmware output is:

```text
build_pico/pico_game.uf2
```

To flash, hold **BOOTSEL** while connecting the Pico 2 by USB, then copy the UF2 onto its USB mass-storage drive. Install the same firmware on both consoles for wireless play; choose Host or Join from the connection screen.

Separate hardware diagnostics and profiling firmware are documented in [`tests/README.md`](tests/README.md).

## Code structure

| Path | Responsibility |
| --- | --- |
| `CMakeLists.txt` | Simulator/firmware builds and automatic game registration |
| `src/main_pico.cpp` | Hardware startup and main controls/game/radio/audio/display loop |
| `src/main_sim.cpp` | SDL2 window, keyboard input and desktop loop |
| `src/console.cpp` | Game selection, menus, pause and shared sound setting |
| `include/game.h` | Game callbacks and common input contract |
| `src/controls.cpp` | GPIO buttons, debounce and joystick ADC reads |
| `src/graphics.cpp` | Framebuffers, text, shapes and sprites |
| `src/display.cpp` | ST7789 initialization and SPI0/DMA transfers |
| `src/radio.cpp` | nRF register configuration and packet transmission/reception |
| `src/multiplayer.cpp` | Pairing, sessions, shared gameplay transport and two-packet state assembly |
| `src/speaker.cpp` | PWM/DMA sound playback |
| `src/game_*.cpp` | Individual game rules, rendering and game-specific wireless encoding |
| `include/*_assets.h` | Compiled artwork and sound samples |
| `scripts/` | Artwork conversion utilities |
| `tests/` | Standalone hardware diagnostics and profiling |

CMake discovers `src/game_*.cpp` and generates the game registry. A new game implements the `Game` interface; wireless support also requires its own state/input encoding and multiplayer handling. Shared drivers and pairing do not need to be reimplemented.
