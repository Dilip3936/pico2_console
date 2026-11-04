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

# The profiler runs the current console and games, not a synthetic workload.
add_pico_test(cpu_profile cpu_profile.cpp hardware_spi hardware_adc hardware_dma)
target_compile_definitions(cpu_profile PRIVATE PICO_BUILD=1)
target_compile_features(cpu_profile PRIVATE cxx_std_14)
target_include_directories(cpu_profile PRIVATE ../include)
file(GLOB PROFILE_GAMES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../src/game_*.cpp")
list(SORT PROFILE_GAMES)
set(PROFILE_REGISTRY "#include \"console.h\"\n")
set(PROFILE_ENTRIES "")
foreach(source IN LISTS PROFILE_GAMES)
    get_filename_component(game_symbol "${source}" NAME_WE)
    if(NOT game_symbol MATCHES "^game_[A-Za-z0-9_]+$")
        message(FATAL_ERROR "Invalid game filename: ${source}; use game_<identifier>.cpp")
    endif()
    string(APPEND PROFILE_REGISTRY "extern const Game ${game_symbol};\n")
    string(APPEND PROFILE_ENTRIES "    &${game_symbol},\n")
endforeach()
string(APPEND PROFILE_REGISTRY
    "const Game* const registered_games[] = {\n${PROFILE_ENTRIES}    nullptr\n};\n"
    "const size_t registered_game_count = sizeof(registered_games) / sizeof(registered_games[0]) - 1;\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/profile_game_registry.cpp"
    CONTENT "${PROFILE_REGISTRY}")
target_sources(cpu_profile PRIVATE
    ../src/console.cpp ../src/multiplayer.cpp ../src/graphics.cpp
    ../src/display.cpp ../src/controls.cpp ../src/radio.cpp
    ${PROFILE_GAMES}
    "${CMAKE_CURRENT_BINARY_DIR}/generated/profile_game_registry.cpp"
)
