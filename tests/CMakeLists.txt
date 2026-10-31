cmake_minimum_required(VERSION 3.13)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_BOARD pico2)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(Pico2Console_Tests C CXX ASM)
pico_sdk_init()

# Build all test firmware images, or select an individual executable target.
add_custom_target(all_tests)

function(add_pico_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE pico_stdlib ${ARGN})
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
    add_dependencies(all_tests ${name})
endfunction()

# Register each test with only the hardware libraries it needs.
add_pico_test(button_test button_test.cpp hardware_adc)
target_sources(button_test PRIVATE ../src/controls.cpp)
target_include_directories(button_test PRIVATE ../include)

add_pico_test(nrf_link_test nrf_link_test.cpp hardware_spi pico_rand)
target_sources(nrf_link_test PRIVATE ../src/radio.cpp)
target_include_directories(nrf_link_test PRIVATE ../include)
target_compile_features(nrf_link_test PRIVATE cxx_std_14)

add_pico_test(nrf_delivery_test nrf_delivery_test.cpp hardware_spi pico_rand)
target_sources(nrf_delivery_test PRIVATE ../src/radio.cpp)
target_include_directories(nrf_delivery_test PRIVATE ../include)
target_compile_features(nrf_delivery_test PRIVATE cxx_std_14)
