#include "controls.h"
#include <pico/stdlib.h>
#include <hardware/adc.h>

// Low Y means Up and low X means Left. Flip either axis independently if the
// joystick is mounted in the opposite direction; button mappings do not change.
static constexpr bool JOYSTICK_INVERT_X = false;
static constexpr bool JOYSTICK_INVERT_Y = true;

struct Button {
    uint8_t mask;
    uint pin;
    bool raw_pressed;
    bool pressed;
    uint64_t changed_at;
};

static Button buttons[] = {
    {CONTROL_A, 5, false, false, 0},
    {CONTROL_B, 8, false, false, 0},
    {CONTROL_C, 4, false, false, 0},
    {CONTROL_D, 7, false, false, 0},
    {CONTROL_E, 3, false, false, 0},
    {CONTROL_F, 6, false, false, 0},
    {CONTROL_K, 2, false, false, 0},
};

void controls_init() {
    adc_init();
    adc_gpio_init(27); // X: ADC1
    adc_gpio_init(26); // Y: ADC0

    for (Button& button : buttons) {
        gpio_init(button.pin);
        gpio_set_dir(button.pin, GPIO_IN);
        gpio_pull_up(button.pin);
        button.raw_pressed = false;
        button.pressed = false;
        button.changed_at = time_us_64();
    }
}

ControlsState controls_poll() {
    ControlsState state = {0};
    uint64_t now = time_us_64();
    for (Button& button : buttons) {
        bool pressed = !gpio_get(button.pin);
        if (pressed != button.raw_pressed) {
            button.raw_pressed = pressed;
            button.changed_at = now;
        }

        // Accept a change after the input has been stable for 20 ms.
        if (button.raw_pressed != button.pressed &&
            now - button.changed_at >= 20000) {
            button.pressed = button.raw_pressed;
            if (button.pressed) state.pressed |= button.mask;
            else state.released |= button.mask;
        }
        if (button.pressed) state.held |= button.mask;
    }

    adc_select_input(1);
    state.x = adc_read();
    adc_select_input(0);
    state.y = adc_read();
    if (JOYSTICK_INVERT_X) state.x = 4095 - state.x;
    if (JOYSTICK_INVERT_Y) state.y = 4095 - state.y;
    return state;
}
