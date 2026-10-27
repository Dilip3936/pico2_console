#include "console.h"
#include "graphics.h"
#include <stdio.h>
#include <string.h>

namespace {
const Game* active_game = nullptr;
size_t selected_game = 0;
bool paused = false;
bool full_refresh = true;
bool previous_up = false;
bool previous_down = false;
const char* action_prompt = nullptr;

void centered_text(int y, const char* text, uint16_t color) {
    draw_text((DISPLAY_WIDTH - (int)strlen(text) * 6) / 2, y, text, color);
}

void draw_menu() {
    clear_screen(0x1082);
    centered_text(20, "SELECT GAME", 0xFFFF);

    const size_t page_size = 6;
    const size_t first = selected_game / page_size * page_size;
    for (size_t i = first; i < registered_game_count && i < first + page_size; ++i) {
        int y = 50 + (int)(i - first) * 22;
        if (i == selected_game) {
            draw_filled_rect(16, y - 4, DISPLAY_WIDTH - 32, 18, 0xFFFF);
        }
        char title[45];
        snprintf(title, sizeof(title), "%.44s", registered_games[i]->name);
        centered_text(y, title, i == selected_game ? 0x0000 : 0xFFFF);
    }

    if (registered_game_count == 0) centered_text(90, "NO GAMES INSTALLED", 0xFFFF);
    centered_text(194, "C/D OR UP/DOWN: CHOOSE", 0xFFFF);
    centered_text(210, "A/B OR SPACE/X: OPEN", 0xFFFF);
    centered_text(226, "E/P: PAUSE   F/ESC: MENU", 0xFFFF);
}
}

void console_init(const char* prompt) {
    active_game = nullptr;
    selected_game = 0;
    paused = false;
    full_refresh = true;
    previous_up = false;
    previous_down = false;
    action_prompt = prompt;
}

void console_update(const InputState& input) {
    bool up_pressed = input.up && !previous_up;
    bool down_pressed = input.down && !previous_down;
    previous_up = input.up;
    previous_down = input.down;
    uint8_t pressed = input.controls.pressed;

    // We handle system actions first so the same press cannot also trigger gameplay.
    if (pressed & CONSOLE_MENU_BUTTON) {
        active_game = nullptr;
        paused = false;
        full_refresh = true;
        return;
    }

    if (!active_game) {
        if (registered_game_count == 0) return;
        if (up_pressed || (pressed & CONTROL_C)) {
            selected_game = selected_game == 0 ? registered_game_count - 1 : selected_game - 1;
        } else if (down_pressed || (pressed & CONTROL_D)) {
            selected_game = (selected_game + 1) % registered_game_count;
        } else if (pressed & (CONTROL_A | CONTROL_B)) {
            active_game = registered_games[selected_game];
            active_game->init();
            full_refresh = true;
        }
        return;
    }

    if (pressed & CONSOLE_PAUSE_BUTTON) {
        paused = !paused;
        full_refresh = true; // We need a full frame to erase the pause overlay on resume.
        return;
    }
    if (paused) return;

    InputState game_input = input;
    const uint8_t system_buttons = CONSOLE_PAUSE_BUTTON | CONSOLE_MENU_BUTTON;
    game_input.controls.held &= ~system_buttons;
    game_input.controls.pressed &= ~system_buttons;
    game_input.controls.released &= ~system_buttons;
    active_game->update(game_input);
}

int console_draw() {
    if (!active_game) {
        draw_menu();
        full_refresh = false;
        return DISPLAY_HEIGHT;
    }

    active_game->draw(action_prompt);
    if (paused) {
        draw_filled_rect(60, 80, 200, 72, 0xFFFF);
        draw_rect(60, 80, 200, 72, 0x0000);
        centered_text(92, "PAUSED", 0x0000);
        centered_text(112, "E / P: RESUME", 0x0000);
        centered_text(132, "F / ESC: SELECT GAME", 0x0000);
    }

    int rows = active_game->redraw_rows;
    if (full_refresh || paused || rows <= 0 || rows > DISPLAY_HEIGHT) rows = DISPLAY_HEIGHT;
    full_refresh = false;
    return rows;
}

void console_debug() {
    if (active_game && !paused && active_game->debug) active_game->debug();
}
