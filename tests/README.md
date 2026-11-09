# Pico hardware tests

Tests have their own CMake project and use the Pico SDK through `PICO_SDK_PATH`.
Run these commands from the repository root; `PICO_BUILD` is not needed.

Configure:

```sh
cmake -S tests -B build_test
```

Configuration also generates `build_test/compile_commands.json` for the editor's
C++ IntelliSense, including the exact SDK headers and compiler settings.

Build one test:

```sh
cmake --build build_test --target button_test -j2
```

Build all tests:

```sh
cmake --build build_test --target all_tests -j2
```

Each test produces its own `build_test/<target>.uf2`. Building all tests compiles
separate firmware images; it does not combine them into one running program.
The default build also builds all registered tests.

To add a test, put its source in this directory and register it in
`tests/CMakeLists.txt`, listing any hardware libraries after the source:

```cmake
add_pico_test(example_test example_test.cpp hardware_spi)
```

`button_test` reports A-F and K presses/releases and X/Y analog joystick values
over USB serial using the same `src/controls.cpp` driver as the game firmware.
Hardware validation is still pending.

`nrf_link_test` is the standalone two-console SPI1 radio diagnostic. Build it with
`cmake --build build_test --target nrf_link_test -j2` and load
`build_test/nrf_link_test.uf2` on both Picos. Over USB serial, send `R` to one
console, then `I` to the other for 1,000 probes at 30 Hz, or `F` for a continuous
request/reply run. `S` stops and prints a summary. Start the responder first;
use `L` instead of `R` to compare a 5 ms reply delay with the default 0.5 ms.
Swap roles to test the other direction. Displays and controls are not initialized.
Configuration, metric definitions, hardware results and limitations are in
[docs.md](../docs.md#nrf-link-diagnostic). Confirm radio power/wiring before use.

`nrf_delivery_test` measures one-way delivery without application replies. Build
and flash its own UF2 on both consoles. For automatic ACKs/three retries, send
`B` to the receiver then `A` to the sender. Without ACKs/retries, send `M` to the
receiver then `N` to the sender. These letters select modes, not console names.
Each sender submits 1,000 numbered packets at 30 Hz and stops after local TX
completion. Wait for its `RESULT`, allow 200 ms for receiver draining, then send
`S` to the receiver for unique delivery, missing percentage and longest missing
sequence streak. Its expected count includes leading/trailing losses and assumes
the sender completed all 1,000 submissions. No-ACK `sent_unconfirmed` is local
completion only. USB reports replace RF echoes; no one-way latency is measured.

## Speaker diagnostic

Build with `cmake --build build_test --target speaker_test -j4` to produce
`build_test/speaker_test.uf2`. This image does not initialize games, display or radio.
With power disconnected, connect the SmartElex module VCC to 3V3, GND to GND,
and VIN/SIGNAL to GP22 (physical pin 29). Start with low module volume.
It is silent at boot. Over USB serial, send `1`, `2` or `3` for a 200 ms
440/880/1320 Hz tone, `T` for a three-tone sequence, `S` to stop or `H` for help.
Hardware PWM generates tones; the main loop advances durations without blocking
for each tone. USB disconnect also stops playback. Actual sound output and power
stability remain unverified. Restore normal game firmware after testing.

## Reusable task profiler

```sh
cmake --build build_test --target cpu_profile -j4
```

`build_test/cpu_profile.uf2` runs the current console/game main loop with timing
wrappers from the single source `tests/cpu_profile.cpp`. Game/driver sources are
referenced, not copied. Normal `pico_game` builds do not contain the profiler.
Building does not flash either board. Use a Release configuration for comparisons
(`cmake -S tests -B build_test -DCMAKE_BUILD_TYPE=Release`).

Flash the profiler manually to the desired console(s), open USB serial and use
normal controls to exercise menus, games, pause or wireless multiplayer. It emits
`[PROFILE]`, nine `[PROFILE_TASK]` lines, `[PROFILE_DISPLAY]` and `[PROFILE_END]` roughly once per
second. Task lines show total microseconds, elapsed-time percentage, call count,
integer mean and maximum. Divide header polls/updates/frames by
`elapsed_us / 1000000` for rates. `accounted_us` should equal `elapsed_us` exactly;
percentage rounding can prevent displayed percentages summing to 100.
`audio_service` measures the main loop's speaker maintenance call. Sound triggering
inside games remains in `game_update`. `[PROFILE_DISPLAY]` reports completed DMA
transfers' observed total/maximum duration, submitted row range and active game.
These durations overlap CPU tasks and are not added to the accounting totals.
`[PROFILE_INFO]` reports system/peripheral clocks and SDK-selected SPI0 baud rate.

These are core-0 **elapsed wall times, not active CPU cycles/utilization**.
Blocking waits and interrupts stay in their enclosing task. `sleep` is not CPU
work; display DMA duration is not charged as CPU work. `game_update` times each
`console_update`, not input preparation or an entire catch-up batch; `other`
contains remaining loop work and instrumentation. Counts/maxima for `other` are
residual gap spans. Radio work called inside a game update stays in that bucket.
Startup is excluded. Report printing belongs to the next interval's logging;
extra logging and timing perturb the workload. Session/role are boundary values,
so discard windows spanning scenario transitions. No packet loss/airtime, actual
button latency or runtime RAM/stack headroom is measured.

The wrappers follow the actual main loop via an include; review the aliases if
main-loop call names change or new tasks need separate attribution. Keep this
CMake target's source/dependency/definition list aligned with the normal Pico
target when adding subsystems; game discovery is automatic. Restore the
normal `build_pico/pico_game.uf2` after testing. Serial ACLs may need renewing
after each reboot; see `docs.md` for the established device-access procedure.
