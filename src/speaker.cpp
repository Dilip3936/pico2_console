#include "speaker.h"
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/pwm.h>
#include <pico/stdlib.h>

namespace {
constexpr uint SPEAKER_PIN = 22;
int audio_dma = -1;
uint speaker_slice;
bool playing = false;
uint8_t current_priority = 0;
}

void speaker_stop() {
    if (audio_dma < 0) return;
    if (dma_channel_is_busy(audio_dma)) dma_channel_abort(audio_dma);
    pwm_set_enabled(speaker_slice, false);
    // GPIO overrides PWM so stopping cannot leave the last sample as DC/high.
    gpio_set_function(SPEAKER_PIN, GPIO_FUNC_SIO);
    gpio_put(SPEAKER_PIN, false);
    playing = false;
    current_priority = 0;
}

void speaker_init() {
    if (audio_dma >= 0) return;
    gpio_init(SPEAKER_PIN);
    gpio_put(SPEAKER_PIN, false);
    gpio_set_dir(SPEAKER_PIN, GPIO_OUT);
    speaker_slice = pwm_gpio_to_slice_num(SPEAKER_PIN);
    // Reserve this PWM slice for audio. Peripheral halfword writes replicate
    // into both CC halves, so channel B must not be used independently.
    hard_assert(pwm_gpio_to_channel(SPEAKER_PIN) == PWM_CHAN_A);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, 255);
    pwm_config_set_clkdiv(&config, 1.0f);
    pwm_init(speaker_slice, &config, false);

    audio_dma = dma_claim_unused_channel(true);
    const uint timer = dma_claim_unused_timer(true);
    uint32_t numerator = SPEAKER_SAMPLE_RATE;
    uint32_t denominator = clock_get_hz(clk_sys);
    uint32_t a = numerator, b = denominator;
    while (b) {
        const uint32_t remainder = a % b;
        a = b;
        b = remainder;
    }
    numerator /= a;
    denominator /= a;
    hard_assert(numerator <= 65535 && denominator <= 65535);
    dma_timer_set_fraction(timer, numerator, denominator);
    dma_channel_config dma_config = dma_channel_get_default_config(audio_dma);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_16);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_dreq(&dma_config, dma_get_timer_dreq(timer));
    dma_channel_configure(audio_dma, &dma_config, &pwm_hw->slice[speaker_slice].cc,
                          nullptr, 0, false);
}

void speaker_service() {
    if (playing && !dma_channel_is_busy(audio_dma)) speaker_stop();
}

void speaker_play(const SpeakerSound& sound) {
    // RP2350 uses the upper four transfer-count bits for DMA modes, not length.
    if (audio_dma < 0 || !sound.samples || !sound.sample_count ||
        sound.sample_count > 0x0fffffffu) return;
    speaker_service();
    if (playing && sound.priority < current_priority) return;
    speaker_stop();
    dma_channel_set_read_addr(audio_dma, sound.samples, false);
    dma_channel_set_trans_count(audio_dma, sound.sample_count, false);
    pwm_set_counter(speaker_slice, 0);
    pwm_set_gpio_level(SPEAKER_PIN, 128);
    gpio_set_function(SPEAKER_PIN, GPIO_FUNC_PWM);
    pwm_set_enabled(speaker_slice, true);
    current_priority = sound.priority;
    playing = true;
    dma_start_channel_mask(1u << audio_dma);
}
