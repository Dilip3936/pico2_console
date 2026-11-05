#pragma once
#include <stddef.h>
#include <stdint.h>

// Samples are unsigned 8-bit PWM levels stored in 16-bit words: 128 is silence.
// Assets must remain alive and unchanged until playback finishes or is stopped.
constexpr uint32_t SPEAKER_SAMPLE_RATE = 24000;
struct SpeakerSound {
    const uint16_t* samples;
    size_t sample_count;
    uint8_t priority;
};

struct SpeakerTone { uint16_t frequency_hz; uint16_t duration_ms; };
template<size_t N> struct SpeakerPcm { uint16_t samples[N] = {}; };

template<size_t N>
constexpr size_t speaker_sample_count(const SpeakerTone (&notes)[N]) {
    size_t count = 1;
    for (const SpeakerTone& note : notes) count += note.duration_ms * SPEAKER_SAMPLE_RATE / 1000;
    return count;
}

// Bake square-wave notes into flash at compile time, restarting phase per note.
// The last neutral sample avoids holding a nonzero audio level between DMA
// completion and the next service. Levels preserve the original preview volume.
template<size_t Samples, size_t Notes>
constexpr SpeakerPcm<Samples> speaker_make_pcm(const SpeakerTone (&notes)[Notes]) {
    SpeakerPcm<Samples> pcm;
    size_t offset = 0;
    for (const SpeakerTone& note : notes) {
        const size_t count = note.duration_ms * SPEAKER_SAMPLE_RATE / 1000;
        for (size_t i = 0; i < count; ++i) {
            pcm.samples[offset++] = !note.frequency_hz ? 128 :
                ((i * note.frequency_hz) % SPEAKER_SAMPLE_RATE < SPEAKER_SAMPLE_RATE / 2 ? 151 : 104);
        }
    }
    pcm.samples[offset] = 128;
    return pcm;
}

#ifdef PICO_BUILD
void speaker_init();
// Call frequently to silence the output after DMA finishes; never waits for audio.
void speaker_service();
// One voice, no queue. Equal/higher priority replaces the current sound.
void speaker_play(const SpeakerSound& sound);
void speaker_stop();
#else
// The simulator shares game code but does not initialize or produce audio.
inline void speaker_init() {}
inline void speaker_service() {}
inline void speaker_play(const SpeakerSound&) {}
inline void speaker_stop() {}
#endif

namespace speaker_detail {
// The inline function's local static is shared across translation units in C++14.
inline bool& enabled() { static bool value = true; return value; }
}

// Local to this console and kept across games, but not stored across reboots.
inline bool speaker_is_enabled() { return speaker_detail::enabled(); }
inline void speaker_set_enabled(bool enabled) {
    speaker_detail::enabled() = enabled;
    if (!enabled) speaker_stop();
}
