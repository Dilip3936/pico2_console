cmake_minimum_required(VERSION 3.10)
project(Pico2Console)

# Simulator executable
add_executable(sim 
    src/main_sim.cpp
    src/game.cpp
    src/graphics.cpp
)

target_include_directories(sim PRIVATE include)
find_package(SDL2 REQUIRED)
target_include_directories(sim PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(sim PRIVATE ${SDL2_LIBRARIES})
