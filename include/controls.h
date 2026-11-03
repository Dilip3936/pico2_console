#pragma once
#include <stdint.h>

enum ControlButton : uint8_t {
    CONTROL_A = 1 << 0,
    CONTROL_B = 1 << 1,
    CONTROL_C = 1 << 2,
    CONTROL_D = 1 << 3,
    CONTROL_E = 1 << 4,
    CONTROL_F = 1 << 5,
    CONTROL_K = 1 << 6,
};

struct ControlsState {
    uint8_t held;
    uint8_t pressed;
    uint8_t released;
    uint16_t x;
    uint16_t y;
};

// Menu actions are decoded once by the console; gameplay keeps the raw controls.
struct MenuInput {
    bool up = false;
    bool down = false;
    bool select = false;
    bool back = false;
};

void controls_init();
// Poll regularly. Event masks contain changes since the previous poll.
ControlsState controls_poll();
