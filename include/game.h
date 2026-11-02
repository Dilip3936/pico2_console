#pragma once
#include "controls.h"

struct InputState {
    // We keep keyboard directions separate from the raw joystick readings in controls.x/y.
    bool up, down, left, right;
    ControlsState controls;
    // Local race testing supplies player two separately; hardware input remains player one.
    ControlsState second_player_controls;
};

const int GAME_FPS = 30;

struct Game {
    const char* name;
    void (*init)();                          // We reset transient state each time a game opens.
    void (*update)(const InputState& input); // One 1/30-second step, including catch-up updates.
    void (*draw)(const char* action_prompt); // We redraw the full framebuffer and UI without advancing gameplay.

    // Zero sends the whole screen. To skip bottom rows, we guarantee they stay
    // unchanged until the game exits; entry and pause/resume always send a full frame.
    int redraw_rows = 0;
    void (*debug)() = nullptr;               // Optional Pico USB diagnostics; no gameplay changes here.
    // Connected games own shared pause/readiness; also called after a disconnect
    // so they can abort before the connection screen takes over.
    void (*multiplayer_update)(const InputState& input) = nullptr;
};
