cmake_minimum_required(VERSION 3.13)

# Define a toggle for the build type (Defaults to OFF / Simulator)
option(PICO_BUILD "Build for Raspberry Pi Pico" OFF)

if (NOT PICO_BUILD)
    # -----------------------------------------------------
    # SIMULATOR TARGET
    # -----------------------------------------------------
    project(Pico2Console_Sim)

    add_executable(sim 
        src/main_sim.cpp
        src/game.cpp
        src/graphics.cpp
    )

    target_include_directories(sim PRIVATE include)
    find_package(SDL2 REQUIRED)
    target_include_directories(sim PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(sim PRIVATE ${SDL2_LIBRARIES})

else()
    # -----------------------------------------------------
    # RASPBERRY PI PICO 2 TARGET
    # -----------------------------------------------------
    # Hardcode the board to pico 2 so you don't need to pass it in the terminal
    set(PICO_BOARD pico2)
    
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    project(Pico2Console_Pico C CXX ASM)
    
    pico_sdk_init()

    add_executable(pico_game 
        src/main_pico.cpp
        src/game.cpp
        src/graphics.cpp
        src/display.cpp
    )

    target_include_directories(pico_game PRIVATE include)
    
    # Define PICO_BUILD so game.cpp knows to disable disk I/O
    target_compile_definitions(pico_game PRIVATE PICO_BUILD=1)

    target_link_libraries(pico_game PRIVATE
        pico_stdlib
        hardware_spi
    )

    # Enable USB serial, disable UART serial
    pico_enable_stdio_usb(pico_game 1)
    pico_enable_stdio_uart(pico_game 0)

    # Generate .uf2 file
    pico_add_extra_outputs(pico_game)
endif()
