#pragma once
#include <stdint.h>
#include <stdbool.h>

enum GameState {
    TITLE,
    PLAYING,
    GAME_OVER
};

struct InputState {
    bool up, down, left, right, btn_a, btn_b;
};

void game_init();
void game_update(struct InputState input);
void game_draw();

int get_score();
int get_highscore();
enum GameState get_state();
