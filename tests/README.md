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
