#pragma once
#include <stddef.h>
#include "game.h"

// CMake generates the menu's game list from src/game_*.cpp, sorted by filename.
extern const Game* const registered_games[];
extern const size_t registered_game_count;

// We keep the temporary pause/menu bindings here for both platforms.
const uint8_t CONSOLE_PAUSE_BUTTON = CONTROL_E;
const uint8_t CONSOLE_MENU_BUTTON = CONTROL_F;

void console_init(const char* action_prompt);
// Leave the current game/session and return to the existing game selection.
void console_return_to_selection();
void console_update(const InputState& input);
// The return value tells the Pico how many top rows need transferring.
int console_draw();
void console_debug();
