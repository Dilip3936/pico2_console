#include <stdio.h>
#include <pico/stdlib.h>
#include "console.h"
#include "graphics.h"
#include "display.h"
#include "multiplayer.h"

int main() {
    stdio_usb_init();
    controls_init();

    // We wait for the USB serial monitor before printing startup messages.
    sleep_ms(2000);
    printf("Starting Pico Console...\n");

    display_init();
    display_set_madctl(0x60); // MX + MV: landscape, keeping RGB order.
    display_fill(COLOR_BLACK);

    console_init("PRESS A/B/C/D");
    console_draw();
    display_blit(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, framebuffer);
    
    uint64_t last_time = time_us_64();
    const uint64_t logic_delay = 1000000 / GAME_FPS;
    const uint64_t frame_delay = logic_delay;
    uint64_t last_frame_time = last_time;
    
    uint32_t frame_count = 0;
    uint64_t last_fps_time = time_us_64();
    uint32_t current_fps = 0;
    int ready_rows = 0;
    bool frame_in_flight = false;
    uint64_t transfer_start = 0;
    uint64_t transfer_time = 0;
    uint64_t draw_time = 0;
    
    uint8_t pending_pressed = 0;
    uint8_t pending_released = 0;

    while (true) {
        ControlsState controls = controls_poll();
        pending_pressed |= controls.pressed;
        pending_released |= controls.released;
        multiplayer_service(time_us_64());
        uint64_t current_time = time_us_64();
        
        bool busy = display_busy();
        if (frame_in_flight && !busy) {
            transfer_time = current_time - transfer_start;
            frame_in_flight = false;
            ++frame_count;
        }

        if (current_time - last_time >= logic_delay) {
            int c = getchar_timeout_us(0);
            InputState input = {0};
            input.controls = controls;
            input.controls.pressed = pending_pressed;
            input.controls.released = pending_released;
            pending_pressed = 0;
            pending_released = 0;
            
            if (c != PICO_ERROR_TIMEOUT) {
                // We treat each serial character as a new press, even on consecutive frames.
                if (c == 'f' || c == ' ' || c == 'F') {
                    input.controls.pressed |= CONTROL_A;
                } else if (c == 'p' || c == 'P') {
                    input.controls.pressed |= CONSOLE_PAUSE_BUTTON;
                } else if (c == 'm' || c == 'M') {
                    input.controls.pressed |= CONSOLE_MENU_BUTTON;
                } else if (c == 'w' || c == 'W') {
                    input.controls.pressed |= CONTROL_C;
                } else if (c == 's' || c == 'S') {
                    input.controls.pressed |= CONTROL_D;
                }
            }

            // Events belong to the first elapsed step; catch-up steps keep held input.
            do {
                console_update(input);
                input.controls.pressed = 0;
                input.controls.released = 0;
                last_time += logic_delay;
            } while (current_time - last_time >= logic_delay);
        }

        // One completed frame can wait while DMA sends the other buffer.
        // We never redraw or discard a waiting frame: it may contain a full refresh.
        if (!ready_rows && current_time - last_frame_time >= frame_delay) {
            last_frame_time = current_time;
            uint64_t draw_start = time_us_64();
            ready_rows = console_draw();
            draw_time = time_us_64() - draw_start;
        }
        if (ready_rows && !busy) {
            transfer_start = time_us_64();
            if (display_blit_async(0, 0, DISPLAY_WIDTH, ready_rows, framebuffer)) {
                graphics_swap_buffers();
                ready_rows = 0;
                frame_in_flight = true;
                console_debug();
            }
        }

        uint64_t now = time_us_64();
        uint64_t fps_elapsed = now - last_fps_time;
        if (fps_elapsed >= 1000000) {
            current_fps = (frame_count * 1000000ULL + fps_elapsed / 2) / fps_elapsed;
            printf("\n[DISPLAY] FPS:%u | draw:%llu us | transfer observed:%llu us\n",
                   (unsigned)current_fps, (unsigned long long)draw_time,
                   (unsigned long long)transfer_time);
            frame_count = 0;
            last_fps_time = now;
        }
        sleep_ms(1);
    }

    return 0;
}
