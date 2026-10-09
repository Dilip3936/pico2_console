#include <stdio.h>
#include <pico/stdlib.h>
#include "game.h"
#include "graphics.h"

int main() {
    stdio_usb_init();

    // Give the user a moment to open the serial monitor
    sleep_ms(2000);
    printf("Starting Flappy Pico Console...\n");

    game_init();
    
    uint64_t last_time = time_us_64();
    const uint64_t frame_delay = 1000000 / 60; // 60 FPS
    
    uint32_t frame_count = 0;
    uint32_t last_fps_time = time_us_32();
    uint32_t current_fps = 0;
    
    GameState last_state = get_state();

    while (true) {
        uint64_t current_time = time_us_64();
        
        // Fixed timestep 60Hz loop
        if (current_time - last_time >= frame_delay) {
            last_time = current_time;
            
            // Non-blocking input read from USB CDC
            int c = getchar_timeout_us(0);
            InputState input = {0};
            
            if (c != PICO_ERROR_TIMEOUT) {
                // Map 'f' or spacebar to btn_a (flap)
                if (c == 'f' || c == ' ' || c == 'F') {
                    input.btn_a = true;
                }
            }

            // Update game logic
            game_update(input);
            // Draw to RAM framebuffer (for telemetry validation, not sent to SPI yet)
            game_draw();

            frame_count++;
            uint32_t now_ms = time_us_32() / 1000;
            if (now_ms - (last_fps_time / 1000) >= 1000) {
                current_fps = frame_count;
                frame_count = 0;
                last_fps_time = time_us_32();
            }

            // Print state transitions
            GameState state = get_state();
            if (state != last_state) {
                if (state == PLAYING) printf("\n[STATE] GAME STARTED!\n");
                else if (state == GAME_OVER) printf("\n[STATE] GAME OVER! Score: %d, Highscore: %d\n", get_score(), get_highscore());
                else if (state == TITLE) printf("\n[STATE] TITLE SCREEN - Press 'f' or SPACE to start\n");
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
    }

    return 0;
}
