#include <stdio.h>
#include <pico/stdlib.h>
#include "game.h"
#include "graphics.h"
#include "display.h"

int main() {
    stdio_usb_init();
    controls_init();

    // Give the user a moment to open the serial monitor
    sleep_ms(2000);
    printf("Starting Flappy Pico Console...\n");

    display_init();
    display_set_madctl(0x60); // MX + MV: landscape, keeping RGB order.
    display_fill(COLOR_BLACK);

    game_init();
    game_draw();
    game_draw_ui("PRESS A/B/C/D");
    display_blit(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, framebuffer);
    
    uint64_t last_time = time_us_64();
    const uint64_t logic_delay = 1000000 / 60;
    const uint64_t frame_delay = 1000000 / 30;
    uint64_t last_frame_time = last_time;
    
    uint32_t frame_count = 0;
    uint64_t last_fps_time = time_us_64();
    uint32_t current_fps = 0;
    
    GameState last_state = get_state();
    uint8_t pending_pressed = 0;
    uint8_t pending_released = 0;

    while (true) {
        ControlsState controls = controls_poll();
        pending_pressed |= controls.pressed;
        pending_released |= controls.released;
        uint64_t current_time = time_us_64();
        
        // Render at 30 FPS; preserve 60 Hz fixed game-logic steps.
        if (current_time - last_frame_time >= frame_delay) {
            last_frame_time = current_time;
            // Non-blocking input read from USB CDC
            int c = getchar_timeout_us(0);
            InputState input = {0};
            input.controls = controls;
            input.controls.pressed = pending_pressed;
            input.controls.released = pending_released;
            pending_pressed = 0;
            pending_released = 0;
            
            if (c != PICO_ERROR_TIMEOUT) {
                // Map 'f' or spacebar to btn_a (flap)
                if (c == 'f' || c == ' ' || c == 'F') {
                    input.btn_a = true;
                }
            }

            // Advance elapsed logic steps after blocking SPI writes.
            // Press/release events apply once, even during catch-up steps.
            do {
                game_update(input);
                input.btn_a = false;
                input.controls.pressed = 0;
                input.controls.released = 0;
                last_time += logic_delay;
            } while (current_time - last_time >= logic_delay);

            game_draw();
            game_draw_ui("PRESS A/B/C/D");
            // The bottom ground strip is static and was sent with the first frame.
            display_blit(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT - GAME_GROUND_HEIGHT, framebuffer);
            uint64_t frame_time = time_us_64() - current_time;

            frame_count++;
            uint64_t now = time_us_64();
            uint64_t fps_elapsed = now - last_fps_time;
            if (fps_elapsed >= 1000000) {
                current_fps = (frame_count * 1000000ULL + fps_elapsed / 2) / fps_elapsed;
                printf("\n[DISPLAY] FPS:%u | frame time:%llu us\n",
                       (unsigned)current_fps, (unsigned long long)frame_time);
                frame_count = 0;
                last_fps_time = now;
            }

            // Print state transitions
            GameState state = get_state();
            if (state != last_state) {
                if (state == PLAYING) printf("\n[STATE] GAME STARTED!\n");
                else if (state == GAME_OVER) printf("\n[STATE] GAME OVER! Score: %d, Highscore: %d\n", get_score(), get_highscore());
                else if (state == TITLE) printf("\n[STATE] TITLE SCREEN - Press A/B/C/D or serial 'f'/SPACE to start\n");
                last_state = state;
            }

            // Print continuous telemetry (throttle it to prevent USB flood, maybe every 10 frames)
            if (frame_count % 10 == 0 && state == PLAYING) {
                float y, vy;
                get_telemetry(&y, &vy);
                printf("F:%04d | Y:%6.2f | VY:%6.2f | SCORE:%2d | FPS:%2d\r", 
                    frame_count, y, vy, get_score(), current_fps);
            }
        }
        sleep_ms(1);
    }

    return 0;
}
