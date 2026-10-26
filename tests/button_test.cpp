#include <stdio.h>
#include <pico/stdlib.h>
#include "controls.h"

struct Button {
    char name;
    unsigned int pin;
    uint8_t mask;
};

static Button buttons[] = {
    {'A', 5, CONTROL_A},
    {'B', 8, CONTROL_B},
    {'C', 4, CONTROL_C},
    {'D', 7, CONTROL_D},
    {'E', 3, CONTROL_E},
    {'F', 6, CONTROL_F},
    {'K', 2, CONTROL_K},
};

int main() {
    stdio_init_all();

    controls_init();

    sleep_ms(2000);
    printf("Button test: A=GP5 B=GP8 C=GP4 D=GP7 E=GP3 F=GP6 K=GP2\n");
    printf("Press each button; K is the joystick press.\n");
    printf("Joystick: X=GP27/ADC1 Y=GP26/ADC0, raw range 0-4095.\n");

    uint64_t last_status = time_us_64();
    uint64_t last_joystick = last_status;
    while (true) {
        uint64_t now = time_us_64();
        ControlsState controls = controls_poll();
        for (const Button& button : buttons) {
            if ((controls.pressed | controls.released) & button.mask) {
                printf("%c (GP%u): %s\n", button.name, button.pin,
                       (controls.held & button.mask) ? "PRESSED" : "RELEASED");
            }
        }

        if (now - last_joystick >= 100000) {
            printf("Joystick: X=%u Y=%u\n", (unsigned)controls.x, (unsigned)controls.y);
            last_joystick = now;
        }

        // Repeat the full state so a late-opened serial monitor is useful.
        if (now - last_status >= 1000000) {
            printf("Buttons (1=pressed):");
            for (const Button& button : buttons) {
                printf(" %c=%d", button.name, (controls.held & button.mask) ? 1 : 0);
            }
            printf("\n");
            last_status = now;
        }
        sleep_ms(5);
    }
}
