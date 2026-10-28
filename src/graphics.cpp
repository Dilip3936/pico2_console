#include "graphics.h"
#include "font.h"

static uint16_t framebuffers[2][DISPLAY_WIDTH * DISPLAY_HEIGHT];

uint16_t* framebuffer = framebuffers[0];

void graphics_swap_buffers() {
    framebuffer = framebuffer == framebuffers[0] ? framebuffers[1] : framebuffers[0];
}

void clear_screen(uint16_t color) {
    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
        framebuffer[i] = color;
    }
}
void draw_sprite(int x, int y, const uint16_t* sprite_data, int w, int h, int scale) {
    if (scale <= 0) return;
    for (int sy = 0; sy < h; sy++) {
        for (int sx = 0; sx < w; sx++) {
            uint16_t pixel = sprite_data[sy * w + sx];
            if (pixel == 0xF81F) continue;
            for (int dy = 0; dy < scale; dy++) {
                int screen_y = y + sy * scale + dy;
                if (screen_y < 0 || screen_y >= DISPLAY_HEIGHT) continue;
                for (int dx = 0; dx < scale; dx++) {
                    int screen_x = x + sx * scale + dx;
                    if (screen_x >= 0 && screen_x < DISPLAY_WIDTH) {
                        framebuffer[screen_y * DISPLAY_WIDTH + screen_x] = pixel;
                    }
                }
            }
        }
    }
}

void draw_pixel(int x, int y, uint16_t color) {
    if (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT) {
        framebuffer[y * DISPLAY_WIDTH + x] = color;
    }
}

void draw_rect(int x, int y, int w, int h, uint16_t color) {
    for (int i = 0; i < w; i++) {
        draw_pixel(x + i, y, color);
        draw_pixel(x + i, y + h - 1, color);
    }
    for (int i = 0; i < h; i++) {
        draw_pixel(x, y + i, color);
        draw_pixel(x + w - 1, y + i, color);
    }
}

void draw_filled_rect(int x, int y, int w, int h, uint16_t color) {
    for (int sy = 0; sy < h; sy++) {
        for (int sx = 0; sx < w; sx++) {
            draw_pixel(x + sx, y + sy, color);
        }
    }
}

void draw_char(int x, int y, char c, uint16_t color) {
    if (c < 32 || c > 127) return;
    int index = c - 32;
    for (int col = 0; col < 5; col++) {
        uint8_t col_data = font5x7[index][col];
        for (int row = 0; row < 8; row++) {
            if (col_data & (1 << row)) {
                draw_pixel(x + col, y + row, color);
            }
        }
    }
}

void draw_text(int x, int y, const char* str, uint16_t color) {
    while (*str) {
        draw_char(x, y, *str, color);
        x += 6;
        str++;
    }
}
