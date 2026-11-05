#pragma once
#include "speaker.h"

namespace pong_sounds {
constexpr SpeakerTone wall_notes[] = {{440, 18}};
constexpr SpeakerTone paddle_notes[] = {{880, 28}};
constexpr SpeakerTone point_notes[] = {{660, 60}, {0, 20}, {990, 90}};
constexpr SpeakerTone match_end_notes[] = {{784, 80}, {988, 80}, {1175, 160}};

constexpr auto wall_pcm = speaker_make_pcm<speaker_sample_count(wall_notes)>(wall_notes);
constexpr auto paddle_pcm = speaker_make_pcm<speaker_sample_count(paddle_notes)>(paddle_notes);
constexpr auto point_pcm = speaker_make_pcm<speaker_sample_count(point_notes)>(point_notes);
constexpr auto match_end_pcm = speaker_make_pcm<speaker_sample_count(match_end_notes)>(match_end_notes);
// Points and results take priority over collision sounds in multiball play.
constexpr SpeakerSound wall = {wall_pcm.samples, speaker_sample_count(wall_notes), 1};
constexpr SpeakerSound paddle = {paddle_pcm.samples, speaker_sample_count(paddle_notes), 2};
constexpr SpeakerSound point = {point_pcm.samples, speaker_sample_count(point_notes), 3};
constexpr SpeakerSound match_end = {match_end_pcm.samples, speaker_sample_count(match_end_notes), 4};
} // namespace pong_sounds
