#include "game.h"
#include "graphics.h"
#include "sprites.h"
#include <stdlib.h>
#include <stdio.h>

#define MAX_PIPES 3
#define PIPE_SPACING 80
#define PIPE_WIDTH 16
#define BIRD_X 40
#define BIRD_SIZE 14 // slightly smaller than 16 for generous collision
#define GROUND_Y 112

struct Pipe {
    float x;
    float gap_y;
    bool passed;
};

// Game state
static GameState current_state = TITLE;
static float bird_y = 0;
static float bird_velocity = 0;
static const float GRAVITY = 0.25f;       
static const float JUMP_STRENGTH = -3.5f;
static int score = 0;
static int highscore = 0;
static Pipe pipes[MAX_PIPES];
static int anim_frame = 0;
static int anim_timer = 0;

static struct InputState prev_input = {0};

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
    pipes[index].gap_y = 30 + (rand() % (GROUND_Y - 70)); // Random gap y between 30 and GROUND_Y-40
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

void game_init() {
    load_highscore();
    reset_game();
    current_state = TITLE;
}

int get_score() { return score; }
int get_highscore() { return highscore; }
GameState get_state() { return current_state; }

void get_telemetry(float* y, float* vy) {
    if (y) *y = bird_y;
    if (vy) *vy = bird_velocity;
}

static bool check_collision() {
    // Ground collision
    if (bird_y + BIRD_SIZE >= GROUND_Y) return true;
    if (bird_y <= 0) return true; // Ceiling collision

    // Pipe collision
    for (int i = 0; i < MAX_PIPES; i++) {
        float px = pipes[i].x;
        float py = pipes[i].gap_y;

        // Bounding box for bird
        float bx1 = BIRD_X + 2; // Offset for generous hitbox
        float bx2 = BIRD_X + BIRD_SIZE - 2;
        float by1 = bird_y + 2;
        float by2 = bird_y + BIRD_SIZE - 2;

        // Upper pipe
        float upx1 = px;
        float upx2 = px + PIPE_WIDTH;
        float upy1 = 0;
        float upy2 = py - 20; // 40px gap total (20 up, 20 down)

        // Lower pipe
        float lpx1 = px;
        float lpx2 = px + PIPE_WIDTH;
        float lpy1 = py + 20;
        float lpy2 = GROUND_Y;

        // Check rect intersection
        if (bx1 < upx2 && bx2 > upx1 && by1 < upy2 && by2 > upy1) return true;
        if (bx1 < lpx2 && bx2 > lpx1 && by1 < lpy2 && by2 > lpy1) return true;
    }

    return false;
}

void game_update(struct InputState input) {
    bool btn_a_pressed = input.btn_a && !prev_input.btn_a;

    if (current_state == TITLE) {
        if (btn_a_pressed) {
            reset_game();
            current_state = PLAYING;
        }
    } else if (current_state == PLAYING) {
        if (btn_a_pressed) {
            bird_velocity = JUMP_STRENGTH;
        }

        bird_velocity += GRAVITY;
        bird_y += bird_velocity;

        // Update pipes
        for (int i = 0; i < MAX_PIPES; i++) {
            pipes[i].x -= 1.5f; // Pipe speed
            
            // Score tracking
            if (!pipes[i].passed && pipes[i].x + PIPE_WIDTH < BIRD_X) {
                pipes[i].passed = true;
                score++;
            }

            // Respawn pipe
            if (pipes[i].x < -PIPE_WIDTH) {
                float max_x = 0;
                for (int j = 0; j < MAX_PIPES; j++) {
                    if (pipes[j].x > max_x) max_x = pipes[j].x;
                }
                spawn_pipe(i, max_x + PIPE_SPACING);
            }
        }

        // Animation
        anim_timer++;
        if (anim_timer > 5) {
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
        if (bird_y < GROUND_Y - 16) {
            bird_velocity += GRAVITY;
            bird_y += bird_velocity;
        }
        if (btn_a_pressed) {
            current_state = TITLE;
        }
    }

    prev_input = input;
}

void game_draw() {
    clear_screen(0x5CBF); // Sky blue background

    // Draw pipes
    if (current_state == PLAYING || current_state == GAME_OVER) {
        for (int i = 0; i < MAX_PIPES; i++) {
            int px = (int)pipes[i].x;
            int py = (int)pipes[i].gap_y;

            // Lower pipe
            draw_sprite(px, py + 20, pipe_top, 16, 16);
            for (int y = py + 20 + 16; y < GROUND_Y; y += 16) {
                draw_sprite(px, y, pipe_middle, 16, 16);
            }
            
            // Upper pipe
            draw_sprite(px, py - 20 - 16, pipe_bottom, 16, 16);
            for (int y = py - 20 - 32; y >= -16; y -= 16) {
                draw_sprite(px, y, pipe_middle, 16, 16);
            }
        }
    }

    // Draw ground
    draw_filled_rect(0, GROUND_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT - GROUND_Y, 0xDE8B); // Greenish brown ground
    draw_rect(0, GROUND_Y, DISPLAY_WIDTH, 1, 0x0000); // Outline

    // Draw bird
    if (current_state == TITLE) {
        draw_sprite(BIRD_X, DISPLAY_HEIGHT / 2, bird2, 16, 16);
    } else {// slightly smaller than 16 for generous collision
        const uint16_t* bird_sprites[] = {bird1, bird2, bird3};
        int frame = (bird_velocity < 0) ? anim_frame : 1;
        draw_sprite(BIRD_X, (int)bird_y, bird_sprites[frame], 16, 16);
    }
}
