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
