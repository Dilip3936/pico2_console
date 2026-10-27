#pragma once
#include <stdint.h>

#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240

extern uint16_t framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];

void clear_screen(uint16_t color);
void draw_pixel(int x, int y, uint16_t color);
void draw_sprite(int x, int y, const uint16_t* sprite_data, int w, int h, int scale = 1);
void draw_rect(int x, int y, int w, int h, uint16_t color);
void draw_filled_rect(int x, int y, int w, int h, uint16_t color);
void draw_char(int x, int y, char c, uint16_t color);
void draw_text(int x, int y, const char* str, uint16_t color);
