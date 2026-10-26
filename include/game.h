#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "controls.h"

enum GameState {
    TITLE,
    PLAYING,
    GAME_OVER
};

struct InputState {
    bool up, down, left, right, btn_a, btn_b;
    ControlsState controls;
};

const int GAME_FPS = 30;

// Static ground strip; the Pico only needs to send it once.
const int GAME_GROUND_HEIGHT = 24;

void game_init();
void game_update(struct InputState input);
void game_draw();
void game_draw_ui(const char* start_prompt);

int get_score();
int get_highscore();
enum GameState get_state();
void get_telemetry(float* y, float* vy);
