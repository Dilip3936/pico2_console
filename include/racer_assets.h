#pragma once
#include "speaker.h"

namespace racer_sounds {
constexpr SpeakerTone countdown_notes[] = {{660, 70}};
constexpr SpeakerTone start_notes[] = {{1320, 160}};
constexpr SpeakerTone crash_notes[] = {{160, 65}, {100, 100}};
constexpr SpeakerTone boost_notes[] = {{660, 45}, {990, 45}, {1320, 90}};
constexpr SpeakerTone result_notes[] = {{784, 80}, {988, 80}, {1175, 160}};

constexpr auto countdown_pcm = speaker_make_pcm<speaker_sample_count(countdown_notes)>(countdown_notes);
constexpr auto start_pcm = speaker_make_pcm<speaker_sample_count(start_notes)>(start_notes);
constexpr auto crash_pcm = speaker_make_pcm<speaker_sample_count(crash_notes)>(crash_notes);
constexpr auto boost_pcm = speaker_make_pcm<speaker_sample_count(boost_notes)>(boost_notes);
constexpr auto result_pcm = speaker_make_pcm<speaker_sample_count(result_notes)>(result_notes);
constexpr SpeakerSound countdown = {countdown_pcm.samples, speaker_sample_count(countdown_notes), 1};
constexpr SpeakerSound start = {start_pcm.samples, speaker_sample_count(start_notes), 2};
constexpr SpeakerSound crash = {crash_pcm.samples, speaker_sample_count(crash_notes), 3};
constexpr SpeakerSound boost = {boost_pcm.samples, speaker_sample_count(boost_notes), 2};
constexpr SpeakerSound result = {result_pcm.samples, speaker_sample_count(result_notes), 4};
} // namespace racer_sounds
