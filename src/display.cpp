#include "display.h"
#include <hardware/spi.h>
#include <hardware/dma.h>
#include <pico/stdlib.h>

static const uint PIN_CS = 17;
static const uint PIN_SCK = 18;
static const uint PIN_SDI = 19;   // MOSI
static const uint PIN_DC = 20;
static const uint PIN_RESET = 21;
static const uint SPI_BAUD = 40000000; // SDK selects 37.5 MHz with the default Pico 2 clock.
static const uint SPI_INIT_BAUD = 4000000;
static int display_width = TFT_WIDTH;
static int display_height = TFT_HEIGHT;
static int dma_channel = -1;
static bool transfer_active = false;

bool display_busy() {
    if (!transfer_active) return false;
    // DMA finishing only means the last word reached the FIFO, not the panel.
    if (dma_channel_is_busy(dma_channel) || spi_is_busy(spi0)) return true;

    // Like the SDK's write-only SPI path, we discard RX and clear overrun.
    while (spi_is_readable(spi0)) (void)spi_get_hw(spi0)->dr;
    spi_get_hw(spi0)->icr = SPI_SSPICR_RORIC_BITS;
    gpio_put(PIN_CS, 1);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    transfer_active = false;
    return false;
}

void display_wait() {
    while (display_busy()) tight_loop_contents();
}

static void write_command(uint8_t command, const uint8_t* data = nullptr,
                          size_t length = 0) {
    display_wait();
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0);
    spi_write_blocking(spi0, &command, 1);
    if (length) {
        gpio_put(PIN_DC, 1);
        spi_write_blocking(spi0, data, length);
    }
    gpio_put(PIN_CS, 1);
}

#define CMD(cmd, ...)                                         \
    do {                                                      \
        const uint8_t d_[] = {__VA_ARGS__};                   \
        write_command(cmd, d_, sizeof(d_));                   \
    } while (0)

void display_set_invert(bool on) { write_command(on ? 0x21 : 0x20); }

void display_set_madctl(uint8_t value) {
    write_command(0x36, &value, 1);
    display_width = (value & 0x20) ? TFT_HEIGHT : TFT_WIDTH;
    display_height = (value & 0x20) ? TFT_WIDTH : TFT_HEIGHT;
}

static void init_st7789() {
    write_command(0x01); sleep_ms(150);          // SWRESET
    write_command(0x11); sleep_ms(120);          // SLPOUT
    CMD(0x3A, 0x55);                             // COLMOD RGB565
    CMD(0x36, 0x00);                             // MADCTL portrait, RGB
    CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);     // porch
    CMD(0xB7, 0x35);                             // gate control
    CMD(0xBB, 0x19);                             // VCOM
    CMD(0xC0, 0x2C);
    CMD(0xC2, 0x01);
    CMD(0xC3, 0x12);                             // VRH
    CMD(0xC4, 0x20);                             // VDV
    CMD(0xC6, 0x0F);                             // 60 Hz
    CMD(0xD0, 0xA4, 0xA1);                       // power control
    CMD(0xE0, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
              0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23);
    CMD(0xE1, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
              0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23);
    write_command(0x20);                         // INVOFF: disable color inversion.
    write_command(0x13);                         // NORON
    sleep_ms(10);
}

void display_init() {
    display_wait();
    if (dma_channel < 0) dma_channel = dma_claim_unused_channel(true);
    display_width = TFT_WIDTH;
    display_height = TFT_HEIGHT;
    gpio_init(PIN_CS);
    gpio_put(PIN_CS, 1);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_init(PIN_DC);
    gpio_put(PIN_DC, 0);
    gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_init(PIN_RESET);
    gpio_put(PIN_RESET, 1);
    gpio_set_dir(PIN_RESET, GPIO_OUT);

    spi_init(spi0, SPI_INIT_BAUD);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SDI, GPIO_FUNC_SPI);

    sleep_ms(150);
    gpio_put(PIN_RESET, 0);
    sleep_ms(20);
    gpio_put(PIN_RESET, 1);
    sleep_ms(150);

    init_st7789();

    display_fill(COLOR_BLACK);   // clear GRAM before turning output on
    write_command(0x29);         // DISPON
    sleep_ms(100);
    spi_set_baudrate(spi0, SPI_BAUD);
}

static void set_window(int x, int y, int w, int h) {
    int x_end = x + w - 1;
    int y_end = y + h - 1;
    const uint8_t columns[] = {(uint8_t)(x >> 8), (uint8_t)x,
                               (uint8_t)(x_end >> 8), (uint8_t)x_end};
    const uint8_t rows[] = {(uint8_t)(y >> 8), (uint8_t)y,
                            (uint8_t)(y_end >> 8), (uint8_t)y_end};
    write_command(0x2A, columns, sizeof(columns));   // CASET
    write_command(0x2B, rows, sizeof(rows));         // RASET
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0 || x >= display_width || y >= display_height) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (w > display_width - x) w = display_width - x;
    if (h > display_height - y) h = display_height - y;
    set_window(x, y, w, h);

    uint8_t pixels[256];
    for (size_t i = 0; i < sizeof(pixels); i += 2) {
        pixels[i] = (uint8_t)(color >> 8);
        pixels[i + 1] = (uint8_t)color;
    }

    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0);
    const uint8_t ramwr = 0x2C;
    spi_write_blocking(spi0, &ramwr, 1);
    gpio_put(PIN_DC, 1);
    int remaining = w * h;
    while (remaining > 0) {
        int count = remaining > 128 ? 128 : remaining;
        spi_write_blocking(spi0, pixels, count * 2);
        remaining -= count;
    }
    gpio_put(PIN_CS, 1);
}

void display_fill(uint16_t color) {
    display_fill_rect(0, 0, display_width, display_height, color);
}

void display_blit(int x, int y, int w, int h, const uint16_t* pixels) {
    display_wait();
    if (display_blit_async(x, y, w, h, pixels)) display_wait();
}

bool display_blit_async(int x, int y, int w, int h, const uint16_t* pixels) {
    if (!pixels || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x >= display_width || y >= display_height ||
        w > display_width - x || h > display_height - y ||
        dma_channel < 0 || display_busy()) return false;
    set_window(x, y, w, h);

    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0);
    const uint8_t ramwr = 0x2C;
    spi_write_blocking(spi0, &ramwr, 1);
    gpio_put(PIN_DC, 1);

    // Hardware sends each RGB565 word MSB first, without a conversion buffer.
    spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    dma_channel_config config = dma_channel_get_default_config(dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_16);
    channel_config_set_read_increment(&config, true);
    channel_config_set_write_increment(&config, false);
    channel_config_set_dreq(&config, spi_get_dreq(spi0, true));
    transfer_active = true;
    dma_channel_configure(dma_channel, &config, &spi_get_hw(spi0)->dr,
                          pixels, w * h, true);
    return true;
}
