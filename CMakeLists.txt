cmake_minimum_required(VERSION 3.13)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PICO_BUILD "Build for Raspberry Pi Pico" OFF)

if (NOT PICO_BUILD)
    project(Pico2Console_Sim)

    add_executable(sim 
        src/main_sim.cpp
    )
    set(CONSOLE_TARGET sim)

    target_include_directories(sim PRIVATE include)
    find_package(SDL2 REQUIRED)
    target_include_directories(sim PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(sim PRIVATE ${SDL2_LIBRARIES})

else()
    set(PICO_BOARD pico2)
    
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    project(Pico2Console_Pico C CXX ASM)
    
    pico_sdk_init()

    add_executable(pico_game 
        src/main_pico.cpp
        src/display.cpp
        src/controls.cpp
    )
    set(CONSOLE_TARGET pico_game)

    target_include_directories(pico_game PRIVATE include)
    
    # We disable filesystem access in games when building firmware.
    target_compile_definitions(pico_game PRIVATE PICO_BUILD=1)

    target_link_libraries(pico_game PRIVATE
        pico_stdlib
        hardware_spi
        hardware_adc
        hardware_dma
    )

    pico_enable_stdio_usb(pico_game 1)
    pico_enable_stdio_uart(pico_game 0)

    pico_add_extra_outputs(pico_game)

endif()

# We use each game filename as its descriptor name in the generated list.
# CONFIGURE_DEPENDS picks up added or removed games on the next build.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/game_*.cpp")
list(SORT GAME_SOURCES)
set(GAME_DECLARATIONS "")
set(GAME_ENTRIES "")
foreach(source IN LISTS GAME_SOURCES)
    get_filename_component(game_symbol "${source}" NAME_WE)
    if(NOT game_symbol MATCHES "^game_[A-Za-z0-9_]+$")
        message(FATAL_ERROR "Invalid game filename: ${source}; use game_<identifier>.cpp")
    endif()
    string(APPEND GAME_DECLARATIONS "extern const Game ${game_symbol};\n")
    string(APPEND GAME_ENTRIES "    &${game_symbol},\n")
endforeach()
set(GAME_REGISTRY_TEMPLATE [=[
// CMake generates this list from src/game_*.cpp.
#include "console.h"

@GAME_DECLARATIONS@
const Game* const registered_games[] = {
@GAME_ENTRIES@    nullptr
};

// We keep a sentinel so an empty game list still compiles.
const size_t registered_game_count =
    sizeof(registered_games) / sizeof(registered_games[0]) - 1;
]=])
string(CONFIGURE "${GAME_REGISTRY_TEMPLATE}" GAME_REGISTRY_SOURCE @ONLY)
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/game_registry.cpp"
    CONTENT "${GAME_REGISTRY_SOURCE}"
)

target_compile_features(${CONSOLE_TARGET} PRIVATE cxx_std_14)
target_sources(${CONSOLE_TARGET} PRIVATE
    src/console.cpp
    src/graphics.cpp
    ${GAME_SOURCES}
    "${CMAKE_CURRENT_BINARY_DIR}/generated/game_registry.cpp"
)
