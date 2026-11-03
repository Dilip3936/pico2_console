#include "console.h"
#include "multiplayer.h"
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
bool joystick_menu_armed = true;
const char* action_prompt = nullptr;

MenuInput menu_input(const InputState& input) {
    MenuInput menu;
    const uint8_t pressed = input.controls.pressed;
    menu.up = (pressed & CONTROL_A) || (input.up && !previous_up);
    menu.down = (pressed & CONTROL_C) || (input.down && !previous_down);
    menu.select = pressed & (CONTROL_B | CONTROL_K);
    menu.back = pressed & CONTROL_D;
    previous_up = input.up;
    previous_down = input.down;

    // A wider activation band and a narrower neutral band avoid ADC chatter.
    // Returning to center rearms one movement, rather than repeating while held.
    const uint16_t y = input.controls.y;
    if (y >= 1536 && y <= 2560) {
        joystick_menu_armed = true;
    } else if (joystick_menu_armed && (y < 1024 || y > 3072)) {
        if (y < 1024) menu.up = true;
        else menu.down = true;
        joystick_menu_armed = false;
    }
    return menu;
}

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
    centered_text(194, "A/C OR STICK/ARROWS: CHOOSE", 0xFFFF);
    centered_text(210, "B/K OR X/ENTER: SELECT", 0xFFFF);
    centered_text(226, "E/P: PAUSE   F/ESC: MENU", 0xFFFF);
}
}

void console_init(const char* prompt) {
    multiplayer_close();
    active_game = nullptr;
    selected_game = 0;
    paused = false;
    full_refresh = true;
    previous_up = false;
    previous_down = false;
    joystick_menu_armed = true;
    action_prompt = prompt;
}

void console_return_to_selection() {
    multiplayer_close();
    active_game = nullptr;
    paused = false;
    full_refresh = true;
}

void console_update(const InputState& input) {
    InputState routed_input = input;
    routed_input.menu = menu_input(input);
    const MenuInput& menu = routed_input.menu;
    uint8_t pressed = input.controls.pressed;

    // We handle system actions first so the same press cannot also trigger gameplay.
    if (pressed & CONSOLE_MENU_BUTTON) {
        console_return_to_selection();
        return;
    }

    if (!active_game) {
        if (registered_game_count == 0) return;
        if (menu.up) {
            selected_game = selected_game == 0 ? registered_game_count - 1 : selected_game - 1;
        } else if (menu.down) {
            selected_game = (selected_game + 1) % registered_game_count;
        } else if (menu.select) {
            active_game = registered_games[selected_game];
            active_game->init();
            full_refresh = true;
        }
        return;
    }

    if (active_game->multiplayer_update &&
        (multiplayer_state() == MultiplayerState::connected || multiplayer_game_active())) {
        InputState network_input = routed_input;
        if (!multiplayer_game_active()) {
            multiplayer_enter_game();
            // Connection-screen actions must not also mark ready or flap.
            network_input.controls.pressed = 0;
            network_input.controls.released = 0;
            network_input.menu = {};
            full_refresh = true;
        }
        active_game->multiplayer_update(network_input);
        if (!active_game || multiplayer_state() == MultiplayerState::connected) return;
    }

    if (multiplayer_is_open()) {
        multiplayer_update(routed_input);
        full_refresh = true;
        return;
    }

    if (pressed & CONSOLE_PAUSE_BUTTON) {
        paused = !paused;
        full_refresh = true; // We need a full frame to erase the pause overlay on resume.
        return;
    }
    if (paused) {
        if (menu.back) console_return_to_selection();
        else if (menu.select) {
            paused = false;
            full_refresh = true;
        }
        return;
    }

    InputState game_input = routed_input;
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

    if (multiplayer_is_open()) {
        multiplayer_draw();
        // The shared screen uses the whole panel, including Flappy's static ground.
        full_refresh = true;
        return DISPLAY_HEIGHT;
    }

    active_game->draw(action_prompt);
    if (paused) {
        draw_filled_rect(60, 80, 200, 72, 0xFFFF);
        draw_rect(60, 80, 200, 72, 0x0000);
        centered_text(92, "PAUSED", 0x0000);
        centered_text(112, "B/K OR E/P: RESUME", 0x0000);
        centered_text(132, "D OR F/ESC: SELECT GAME", 0x0000);
    }

    int rows = active_game->redraw_rows;
    if (full_refresh || paused || rows <= 0 || rows > DISPLAY_HEIGHT) rows = DISPLAY_HEIGHT;
    full_refresh = false;
    return rows;
}

void console_debug() {
    if (active_game && !paused && !multiplayer_is_open() && active_game->debug) {
        active_game->debug();
    }
}
