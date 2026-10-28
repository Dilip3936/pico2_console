#pragma once
#include <stdint.h>
#include <stddef.h>

// Native panel dimensions (portrait).
#define TFT_WIDTH 240
#define TFT_HEIGHT 320

// RGB888 -> RGB565
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xFF) >> 3)))

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     RGB565(255, 0, 0)
#define COLOR_GREEN   RGB565(0, 255, 0)
#define COLOR_BLUE    RGB565(0, 0, 255)
#define COLOR_YELLOW  RGB565(255, 255, 0)
#define COLOR_CYAN    RGB565(0, 255, 255)
#define COLOR_MAGENTA RGB565(255, 0, 255)
#define COLOR_ORANGE  RGB565(255, 128, 0)
#define COLOR_GRAY    RGB565(128, 128, 128)

void display_init();
void display_set_invert(bool on);         // INVON / INVOFF
void display_set_madctl(uint8_t value);   // orientation / RGB-BGR order
void display_fill(uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
// Send an unscaled RGB565 image that fits entirely inside the current orientation.
void display_blit(int x, int y, int w, int h, const uint16_t* pixels);

// False means busy or invalid bounds. On success, pixels must stay unchanged
// until display_busy() returns false, including the last SPI bits on the wire.
bool display_blit_async(int x, int y, int w, int h, const uint16_t* pixels);
bool display_busy();
void display_wait();
