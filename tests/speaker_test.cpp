#include <stdio.h>
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/pwm.h>

namespace {
constexpr uint SPEAKER_PIN = 22;
constexpr float PWM_DIVIDER = 64.0f;
struct Tone { uint frequency; uint duration_ms; };
constexpr Tone SEQUENCE[] = {{440, 150}, {0, 100}, {880, 150}, {0, 100}, {1320, 200}};
uint slice;
uint channel;
int sequence_index = -1;
uint64_t deadline = 0;

void silence() {
    pwm_set_chan_level(slice, channel, 0);
    pwm_set_enabled(slice, false);
}

void play(const Tone& tone) {
    silence();
    if (tone.frequency) {
        const uint32_t period = clock_get_hz(clk_sys) / (64u * tone.frequency);
        pwm_set_wrap(slice, uint16_t(period - 1));
        pwm_set_counter(slice, 0);
        pwm_set_chan_level(slice, channel, period / 2);
        pwm_set_enabled(slice, true);
    }
    deadline = time_us_64() + uint64_t(tone.duration_ms) * 1000;
}

void stop() {
    silence();
    sequence_index = -1;
    deadline = 0;
}

void help() {
    printf("Speaker test: VIN/SIGNAL=GP22, VCC=3V3, GND=GND. Start with low volume.\n");
    printf("1/2/3: 440/880/1320 Hz for 200 ms; T: three-tone sequence; S: stop; H: help.\n");
}
}

int main() {
    stdio_init_all();
    // Set a low output before handing the pin to PWM; nothing plays at boot.
    gpio_init(SPEAKER_PIN);
    gpio_put(SPEAKER_PIN, false);
    gpio_set_dir(SPEAKER_PIN, GPIO_OUT);
    slice = pwm_gpio_to_slice_num(SPEAKER_PIN);
    channel = pwm_gpio_to_channel(SPEAKER_PIN);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, PWM_DIVIDER);
    pwm_init(slice, &config, false);
    pwm_set_chan_level(slice, channel, 0);
    gpio_set_function(SPEAKER_PIN, GPIO_FUNC_PWM);

    bool usb_connected = false;
    while (true) {
        const bool connected = stdio_usb_connected();
        if (connected && !usb_connected) help();
        if (!connected && usb_connected) stop();
        usb_connected = connected;

        const int command = getchar_timeout_us(0);
        if (command == 's' || command == 'S') {
            stop();
            printf("Stopped.\n");
        } else if (command >= '1' && command <= '3') {
            stop();
            const uint frequency = 440u * uint(command - '0');
            play({frequency, 200});
            printf("Tone: %u Hz, 200 ms.\n", frequency);
        } else if (command == 't' || command == 'T') {
            stop();
            sequence_index = 0;
            play(SEQUENCE[0]);
            printf("Playing three-tone sequence.\n");
        } else if (command == 'h' || command == 'H') {
            help();
        }

        // Polling advances only the envelope; hardware PWM generates every cycle.
        if (deadline && time_us_64() >= deadline) {
            if (sequence_index >= 0 && ++sequence_index < int(sizeof(SEQUENCE) / sizeof(SEQUENCE[0]))) {
                play(SEQUENCE[sequence_index]);
            } else {
                stop();
            }
        }
        sleep_ms(1);
    }
}
