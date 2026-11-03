#include <SDL2/SDL.h>
#include "graphics.h"
#include "console.h"
#include "multiplayer.h"
#include <stdio.h>

namespace {
uint8_t button_for_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_SPACE: return CONTROL_A;
        case SDLK_x: return CONTROL_B;
        case SDLK_v: return CONTROL_A;
        case SDLK_c: return CONTROL_C;
        case SDLK_d: return CONTROL_D;
        case SDLK_p: return CONSOLE_PAUSE_BUTTON;
        case SDLK_ESCAPE: return CONSOLE_MENU_BUTTON;
        case SDLK_RETURN: return CONTROL_K;
        default: return 0;
    }
}

void handle_key(InputState& input, const SDL_KeyboardEvent& key) {
    if (key.repeat) return;
    bool pressed = key.type == SDL_KEYDOWN;
    uint8_t button = button_for_key(key.keysym.sym);
    ControlsState& controls = key.keysym.sym == SDLK_v ? input.second_player_controls : input.controls;
    if (pressed) {
        controls.pressed |= button & ~controls.held;
        controls.held |= button;
    } else {
        controls.released |= button & controls.held;
        controls.held &= ~button;
    }
    switch (key.keysym.sym) {
        case SDLK_UP: input.up = pressed; break;
        case SDLK_DOWN: input.down = pressed; break;
        case SDLK_LEFT: input.left = pressed; break;
        case SDLK_RIGHT: input.right = pressed; break;
        default: break;
    }
}
}

int main() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Pico Handheld Sim", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * 4, DISPLAY_HEIGHT * 4, 0);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : nullptr;
    SDL_Texture* texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH, DISPLAY_HEIGHT) : nullptr;
    if (!texture) {
        fprintf(stderr, "SDL display setup failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    console_init("X / ENTER: SELECT");
    InputState input = {};
    input.controls.x = input.controls.y = 2048; // We leave the virtual joystick centered.
    input.second_player_controls.x = input.second_player_controls.y = 2048;
    bool running = true;
    int exit_code = 0;
    SDL_Event event;

    Uint32 fps_timer = SDL_GetTicks();
    unsigned frame_count = 0;
    unsigned current_fps = 0;
    const Uint64 timer_frequency = SDL_GetPerformanceFrequency();
    const Uint64 frame_delay = timer_frequency / GAME_FPS;
    Uint64 last_logic_time = SDL_GetPerformanceCounter() - frame_delay;

    while (running) {
        Uint64 frame_start = SDL_GetPerformanceCounter();
        multiplayer_service((frame_start / timer_frequency) * 1000000ULL +
                            (frame_start % timer_frequency) * 1000000ULL / timer_frequency);
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                handle_key(input, event.key);
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                uint8_t held = input.controls.held;
                uint8_t second_held = input.second_player_controls.held;
                input = {};
                input.controls.released = held;
                input.second_player_controls.released = second_held;
                input.controls.x = input.controls.y = 2048;
                input.second_player_controls.x = input.second_player_controls.y = 2048;
            }
        }
        if (!running) break;

        while (frame_start - last_logic_time >= frame_delay) {
            console_update(input);
            input.controls.pressed = 0;
            input.controls.released = 0;
            input.second_player_controls.pressed = 0;
            input.second_player_controls.released = 0;
            last_logic_time += frame_delay;
        }
        console_draw();

        char buffer[32];
        snprintf(buffer, sizeof(buffer), "FPS:%u", current_fps);
        draw_text(2, 2, buffer, 0xFFFF);
        if (SDL_UpdateTexture(texture, nullptr, framebuffer, DISPLAY_WIDTH * sizeof(uint16_t)) != 0) {
            fprintf(stderr, "SDL texture update failed: %s\n", SDL_GetError());
            exit_code = 1;
            break;
        }
        graphics_swap_buffers();
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        ++frame_count;
        Uint32 now = SDL_GetTicks();
        Uint32 elapsed = now - fps_timer;
        if (elapsed >= 1000) {
            current_fps = (frame_count * 1000ULL + elapsed / 2) / elapsed;
            frame_count = 0;
            fps_timer = now;
        }

        Uint64 frame_time = SDL_GetPerformanceCounter() - frame_start;
        if (frame_delay > frame_time) {
            Uint64 remaining = frame_delay - frame_time;
            SDL_Delay((remaining * 1000 + timer_frequency - 1) / timer_frequency);
        }
    }

    multiplayer_close();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
