#pragma once
#include "speaker.h"

namespace grid_blasters_sounds {
constexpr SpeakerTone shot_notes[] = {{1600, 12}, {1100, 18}};
constexpr SpeakerTone dash_notes[] = {{400, 20}, {800, 20}, {1200, 25}};
constexpr SpeakerTone hit_notes[] = {{180, 45}, {90, 65}};
constexpr SpeakerTone countdown_notes[] = {{660, 40}};
constexpr SpeakerTone start_notes[] = {{880, 40}, {1320, 70}};
constexpr SpeakerTone result_notes[] = {{660, 70}, {440, 70}, {220, 140}};

constexpr auto shot_pcm = speaker_make_pcm<speaker_sample_count(shot_notes)>(shot_notes);
constexpr auto dash_pcm = speaker_make_pcm<speaker_sample_count(dash_notes)>(dash_notes);
constexpr auto hit_pcm = speaker_make_pcm<speaker_sample_count(hit_notes)>(hit_notes);
constexpr auto countdown_pcm = speaker_make_pcm<speaker_sample_count(countdown_notes)>(countdown_notes);
constexpr auto start_pcm = speaker_make_pcm<speaker_sample_count(start_notes)>(start_notes);
constexpr auto result_pcm = speaker_make_pcm<speaker_sample_count(result_notes)>(result_notes);

// Hits and results remain audible when both players are firing continuously.
constexpr SpeakerSound shot = {shot_pcm.samples, speaker_sample_count(shot_notes), 1};
constexpr SpeakerSound dash = {dash_pcm.samples, speaker_sample_count(dash_notes), 2};
constexpr SpeakerSound hit = {hit_pcm.samples, speaker_sample_count(hit_notes), 3};
constexpr SpeakerSound countdown = {countdown_pcm.samples, speaker_sample_count(countdown_notes), 2};
constexpr SpeakerSound start = {start_pcm.samples, speaker_sample_count(start_notes), 3};
constexpr SpeakerSound result = {result_pcm.samples, speaker_sample_count(result_notes), 4};
} // namespace grid_blasters_sounds
