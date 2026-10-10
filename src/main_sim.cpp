#include <SDL2/SDL.h>
#include "graphics.h"
#include "game.h"
#include <stdio.h>

int main() {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Pico Handheld Sim", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * 4, DISPLAY_HEIGHT * 4, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    
    game_init();
    InputState input = {0};
    bool running = true;
    SDL_Event event;

    Uint32 fps_timer = SDL_GetTicks();
    int frame_count = 0;
    int current_fps = 0;

    const int FPS_CAP = 60;
    const int FRAME_DELAY = 1000 / FPS_CAP;

    while (running) {
        Uint32 frame_start = SDL_GetTicks();

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            
            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                bool is_pressed = (event.type == SDL_KEYDOWN);
                switch (event.key.keysym.sym) {
                    case SDLK_UP: input.up = is_pressed; break;
                    case SDLK_DOWN: input.down = is_pressed; break;
                    case SDLK_LEFT: input.left = is_pressed; break;
                    case SDLK_RIGHT: input.right = is_pressed; break;
                    case SDLK_SPACE: input.btn_a = is_pressed; break;       
                    case SDLK_x: input.btn_b = is_pressed; break;
                }
            }
        }

        // Run one frame of the game
        game_update(input);
        game_draw();

        // UI rendering
        char buffer[32];
        sprintf(buffer, "FPS:%d", current_fps);
        draw_text(2, 2, buffer, 0xFFFF);

        game_draw_ui("PRESS SPACE");

        // Blit framebuffer to SDL window
        SDL_UpdateTexture(texture, NULL, framebuffer, DISPLAY_WIDTH * sizeof(uint16_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        
        frame_count++;
        if (SDL_GetTicks() - fps_timer >= 1000) {
            current_fps = frame_count;
            frame_count = 0;
            fps_timer = SDL_GetTicks();
        }

        Uint32 frame_time = SDL_GetTicks() - frame_start;
        if (FRAME_DELAY > frame_time) {
            SDL_Delay(FRAME_DELAY - frame_time);
        }
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
