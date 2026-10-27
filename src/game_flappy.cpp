#include "game.h"
#include "graphics.h"
#include "flappy_assets.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MAX_PIPES 3
#define SPRITE_SCALE 2
#define SPRITE_SIZE (16 * SPRITE_SCALE)
#define PIPE_SPACING 160
#define PIPE_WIDTH 32
#define PIPE_GAP 72
#define BIRD_X 72
#define BIRD_SIZE (14 * SPRITE_SCALE)
// We keep the bottom 24 rows static so Flappy can skip transferring them after entry.
#define GROUND_Y (DISPLAY_HEIGHT - 24)
#define BIRD_GROUND_Y (GROUND_Y - SPRITE_SIZE)

namespace {

enum GameState { TITLE, PLAYING, GAME_OVER };

struct Pipe {
    float x;
    float gap_y;
    bool passed;
};

static GameState current_state = TITLE;
static float bird_y = 0;
static float bird_velocity = 0;
static const float GRAVITY = 1.6f;
static const float JUMP_STRENGTH = -10.8f;
static int score = 0;
static int highscore = 0;
static Pipe pipes[MAX_PIPES];
static int anim_frame = 0;
static int anim_timer = 0;

static GameState last_debug_state = TITLE;
static unsigned debug_frames = 0;

static void load_highscore() {
#ifndef PICO_BUILD
    FILE* f = fopen("highscore.txt", "r");
    if (f) {
        fscanf(f, "%d", &highscore);
        fclose(f);
    }
#endif
}

static void save_highscore() {
#ifndef PICO_BUILD
    FILE* f = fopen("highscore.txt", "w");
    if (f) {
        fprintf(f, "%d", highscore);
        fclose(f);
    }
#endif
}

static void spawn_pipe(int index, float x_pos) {
    pipes[index].x = x_pos;
    const int margin = PIPE_GAP / 2 + 20;
    pipes[index].gap_y = margin + (rand() % (GROUND_Y - 2 * margin + 1));
    pipes[index].passed = false;
}

static void reset_game() {
    bird_y = DISPLAY_HEIGHT / 2.0f;
    bird_velocity = 0;
    score = 0;
    for (int i = 0; i < MAX_PIPES; i++) {
        spawn_pipe(i, DISPLAY_WIDTH + i * PIPE_SPACING);
    }
}

void flappy_init() {
    anim_frame = 0;
    anim_timer = 0;
    last_debug_state = TITLE;
    debug_frames = 0;
    load_highscore();
    reset_game();
    current_state = TITLE;
}

static bool check_collision() {
    if (bird_y >= BIRD_GROUND_Y) return true;
    if (bird_y <= 0) return true;

    for (int i = 0; i < MAX_PIPES; i++) {
        float px = pipes[i].x;
        float py = pipes[i].gap_y;

        // We inset the hitbox so touching the sprite edges is a little more forgiving.
        float bx1 = BIRD_X + 2 * SPRITE_SCALE;
        float bx2 = BIRD_X + BIRD_SIZE - 2 * SPRITE_SCALE;
        float by1 = bird_y + 2 * SPRITE_SCALE;
        float by2 = bird_y + BIRD_SIZE - 2 * SPRITE_SCALE;

        float upx1 = px;
        float upx2 = px + PIPE_WIDTH;
        float upy1 = 0;
        float upy2 = py - PIPE_GAP / 2;

        float lpx1 = px;
        float lpx2 = px + PIPE_WIDTH;
        float lpy1 = py + PIPE_GAP / 2;
        float lpy2 = GROUND_Y;

        if (bx1 < upx2 && bx2 > upx1 && by1 < upy2 && by2 > upy1) return true;
        if (bx1 < lpx2 && bx2 > lpx1 && by1 < lpy2 && by2 > lpy1) return true;
    }

    return false;
}

static void update_bird() {
    bird_velocity += GRAVITY;
    bird_y += bird_velocity;
    if (bird_y >= BIRD_GROUND_Y) {
        bird_y = BIRD_GROUND_Y;
        bird_velocity = 0;
    }
}

void flappy_update(const InputState& input) {
    bool btn_a_pressed = input.controls.pressed &
        (CONTROL_A | CONTROL_B | CONTROL_C | CONTROL_D);

    if (current_state == TITLE) {
        if (btn_a_pressed) {
            reset_game();
            current_state = PLAYING;
        }
    } else if (current_state == PLAYING) {
        if (btn_a_pressed) {
            bird_velocity = JUMP_STRENGTH;
        }

        update_bird();

        for (int i = 0; i < MAX_PIPES; i++) {
            pipes[i].x -= 5.0f;
            
            if (!pipes[i].passed && pipes[i].x + PIPE_WIDTH < BIRD_X) {
                pipes[i].passed = true;
                score++;
            }

            if (pipes[i].x < -PIPE_WIDTH) {
                float max_x = 0;
                for (int j = 0; j < MAX_PIPES; j++) {
                    if (pipes[j].x > max_x) max_x = pipes[j].x;
                }
                spawn_pipe(i, max_x + PIPE_SPACING);
            }
        }

        anim_timer++;
        if (anim_timer >= 3) {
            anim_timer = 0;
            anim_frame = (anim_frame + 1) % 3;
        }

        if (check_collision()) {
            if (score > highscore) {
                highscore = score;
                save_highscore();
            }
            current_state = GAME_OVER;
        }
    } else if (current_state == GAME_OVER) {
        if (bird_y < BIRD_GROUND_Y) {
            update_bird();
        }
        if (btn_a_pressed) {
            current_state = TITLE;
        }
    }
}

void draw_playfield() {
    clear_screen(0x5CBF); // Sky blue background

    if (current_state == PLAYING || current_state == GAME_OVER) {
        for (int i = 0; i < MAX_PIPES; i++) {
            int px = (int)pipes[i].x;
            int py = (int)pipes[i].gap_y;

            int lower_y = py + PIPE_GAP / 2;
            draw_sprite(px, lower_y, pipe_top, 16, 16, SPRITE_SCALE);
            for (int y = lower_y + SPRITE_SIZE; y < GROUND_Y; y += SPRITE_SIZE) {
                draw_sprite(px, y, pipe_middle, 16, 16, SPRITE_SCALE);
            }
            
            int upper_y = py - PIPE_GAP / 2;
            draw_sprite(px, upper_y - SPRITE_SIZE, pipe_bottom, 16, 16, SPRITE_SCALE);
            for (int y = upper_y - 2 * SPRITE_SIZE; y >= -SPRITE_SIZE; y -= SPRITE_SIZE) {
                draw_sprite(px, y, pipe_middle, 16, 16, SPRITE_SCALE);
            }
        }
    }

    draw_filled_rect(0, GROUND_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - GROUND_Y, 0xDE8B); // Ground
    draw_rect(0, GROUND_Y, DISPLAY_WIDTH, 1, 0x0000);

    if (current_state == TITLE) {
        draw_sprite(BIRD_X, DISPLAY_HEIGHT / 2, bird2, 16, 16, SPRITE_SCALE);
    } else {
        const uint16_t* bird_sprites[] = {bird1, bird2, bird3};
        int frame = (bird_velocity < 0) ? anim_frame : 1;
        draw_sprite(BIRD_X, (int)bird_y, bird_sprites[frame], 16, 16, SPRITE_SCALE);
    }
}

static void draw_centered_text(int y, const char* text, uint16_t color) {
    draw_text((DISPLAY_WIDTH - (int)strlen(text) * 6) / 2, y, text, color);
}

void flappy_draw(const char* start_prompt) {
    draw_playfield();
    char buffer[32];
    if (current_state == TITLE) {
        draw_filled_rect(80, 32, 160, 72, 0xFFFF);
        draw_rect(80, 32, 160, 72, 0x0000);
        draw_centered_text(48, "FLAPPY PICO", 0x0000);
        draw_centered_text(80, start_prompt, 0x0000);
    } else if (current_state == GAME_OVER) {
        draw_filled_rect(90, 48, 140, 88, 0xFFFF);
        draw_rect(90, 48, 140, 88, 0x0000);
        draw_centered_text(64, "GAME OVER", 0x0000);
        snprintf(buffer, sizeof(buffer), "SCORE:%d", score);
        draw_centered_text(88, buffer, 0x0000);
        snprintf(buffer, sizeof(buffer), "BEST:%d", highscore);
        draw_centered_text(112, buffer, 0x0000);
    } else {
        snprintf(buffer, sizeof(buffer), "%d", score);
        draw_centered_text(12, buffer, 0xFFFF);
    }
}

void flappy_debug() {
    if (current_state != last_debug_state) {
        if (current_state == PLAYING) printf("[STATE] GAME STARTED!\n");
        else if (current_state == GAME_OVER) {
            printf("[STATE] GAME OVER! Score: %d, Highscore: %d\n", score, highscore);
        } else printf("[STATE] TITLE SCREEN\n");
        last_debug_state = current_state;
    }
    if (++debug_frames % 10 == 0 && current_state == PLAYING) {
        printf("[FLAPPY] Y:%.2f VY:%.2f SCORE:%d\n", bird_y, bird_velocity, score);
    }
}
}

extern const Game game_flappy = {
    "Flappy Bird", flappy_init, flappy_update, flappy_draw,
    GROUND_Y, flappy_debug
};
