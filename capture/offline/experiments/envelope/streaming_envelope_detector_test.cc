#include "capture/offline/experiments/envelope/streaming_envelope_detector.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/offline/experiments/envelope/envelope_experiment.h"

namespace {

using swing_capture::DecodedMonoPcmS16Wav;
using swing_capture::offline::envelope::Candidate;
using swing_capture::offline::envelope::DetectCandidates;
using swing_capture::offline::envelope::DetectorConfig;
using swing_capture::offline::envelope::ExperimentConfigs;
using swing_capture::offline::envelope::RobustHp120X12Config;
using swing_capture::offline::envelope::StreamingEnvelopeDetector;

constexpr std::uint32_t kRate = 48'000;

DecodedMonoPcmS16Wav SyntheticAudio(std::size_t seconds) {
  DecodedMonoPcmS16Wav audio{.sample_rate_hz = kRate,
                             .samples = std::vector<std::int16_t>(kRate * seconds)};
  for (std::size_t index = 0; index < audio.samples.size(); ++index) {
    const double time = static_cast<double>(index) / kRate;
    const double background = 45.0 * std::sin(2.0 * std::numbers::pi * 997.0 * time) +
                              21.0 * std::sin(2.0 * std::numbers::pi * 233.0 * time);
    audio.samples[index] = static_cast<std::int16_t>(std::lround(background));
  }
  for (const std::size_t impulse :
       std::array{static_cast<std::size_t>(kRate), static_cast<std::size_t>(kRate * 2U + 137U)}) {
    if (impulse + 3U < audio.samples.size()) {
      audio.samples[impulse] = 16'000;
      audio.samples[impulse + 1U] = -11'000;
      audio.samples[impulse + 2U] = 6'000;
      audio.samples[impulse + 3U] = -2'000;
    }
  }
  return audio;
}

std::vector<Candidate> StreamingCandidates(const DecodedMonoPcmS16Wav &audio,
                                           std::span<const std::size_t> chunks,
                                           std::uint64_t first_frame_position = 0) {
  StreamingEnvelopeDetector detector(audio.sample_rate_hz, RobustHp120X12Config(),
                                     first_frame_position);
  std::vector<Candidate> candidates;
  const auto emit = [&](const Candidate &candidate) { candidates.push_back(candidate); };
  std::size_t cursor = 0;
  std::size_t chunk_index = 0;
  while (cursor < audio.samples.size()) {
    const std::size_t count =
        std::min(chunks[chunk_index % chunks.size()], audio.samples.size() - cursor);
    detector.Process(std::span(audio.samples).subspan(cursor, count), emit);
    cursor += count;
    ++chunk_index;
  }
  detector.Finish(emit);
  return candidates;
}

void AssertCandidateEqual(const Candidate &expected, const Candidate &actual) {
  assert(expected.strike_us == actual.strike_us);
  assert(expected.decision_us == actual.decision_us);
  assert(expected.peak == actual.peak);
  assert(expected.fast_rms == actual.fast_rms);
  assert(expected.background_rms == actual.background_rms);
  assert(expected.background_ratio == actual.background_ratio);
  assert(expected.rise_ratio == actual.rise_ratio);
  assert(expected.high_frequency_ratio == actual.high_frequency_ratio);
  assert(expected.crest_factor == actual.crest_factor);
  assert(expected.cluster_duration_us == actual.cluster_duration_us);
}

void MatchesFrozenOfflineCandidateForArbitraryChunks() {
  const DecodedMonoPcmS16Wav audio = SyntheticAudio(4);
  const std::vector<Candidate> expected = DetectCandidates(audio, RobustHp120X12Config());
  assert(expected.size() == 2U);
  for (const std::vector<std::size_t> &chunks :
       std::array{std::vector<std::size_t>{1U}, std::vector<std::size_t>{4'096U},
                  std::vector<std::size_t>{7U, 1'013U, 2U, 8'191U, 31U}}) {
    const std::vector<Candidate> actual = StreamingCandidates(audio, chunks);
    assert(actual.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
      AssertCandidateEqual(expected[index], actual[index]);
    }
  }
}

void SelectedConfigExactlyMatchesExperimentSweep() {
  const DetectorConfig selected = RobustHp120X12Config();
  const std::vector<DetectorConfig> configs = ExperimentConfigs();
  const auto found = std::ranges::find(configs, selected.name, &DetectorConfig::name);
  assert(found != configs.end());
  assert(found->high_pass_hz == selected.high_pass_hz);
  assert(found->hop_us == selected.hop_us);
  assert(found->fast_window_us == selected.fast_window_us);
  assert(found->background_window_us == selected.background_window_us);
  assert(found->background_guard_us == selected.background_guard_us);
  assert(found->background_block_us == selected.background_block_us);
  assert(found->background_multiplier == selected.background_multiplier);
  assert(found->rise_ratio == selected.rise_ratio);
  assert(found->minimum_peak == selected.minimum_peak);
  assert(found->minimum_high_frequency_ratio == selected.minimum_high_frequency_ratio);
  assert(found->minimum_crest_factor == selected.minimum_crest_factor);
  assert(found->cluster_gap_us == selected.cluster_gap_us);
  assert(found->refractory_us == selected.refractory_us);
}

void StateCapacityIsIndependentOfStreamDuration() {
  StreamingEnvelopeDetector detector(kRate, RobustHp120X12Config());
  const std::size_t capacity = detector.buffered_frame_capacity();
  assert(capacity < kRate);
  std::array<std::int16_t, 4'096> silence{};
  const auto discard = [](const Candidate &) {};
  for (std::size_t block = 0; block < 800U; ++block) {
    detector.Process(silence, discard);
    assert(detector.buffered_frame_capacity() == capacity);
  }
  detector.Finish(discard);
  assert(detector.processed_frames() == silence.size() * 800U);
}

void ResetReplayUsesAbsoluteTimeline() {
  const DecodedMonoPcmS16Wav audio = SyntheticAudio(2);
  constexpr std::uint64_t kOffsetFrames = 10U * kRate;
  const std::vector<Candidate> local = StreamingCandidates(audio, std::array<std::size_t, 1>{333U});
  const std::vector<Candidate> shifted =
      StreamingCandidates(audio, std::array<std::size_t, 1>{777U}, kOffsetFrames);
  assert(local.size() == shifted.size());
  for (std::size_t index = 0; index < local.size(); ++index) {
    assert(shifted[index].strike_us == local[index].strike_us + 10'000'000);
    assert(shifted[index].decision_us == local[index].decision_us + 10'000'000);
  }
}

}  // namespace

int main() {
  MatchesFrozenOfflineCandidateForArbitraryChunks();
  SelectedConfigExactlyMatchesExperimentSweep();
  StateCapacityIsIndependentOfStreamDuration();
  ResetReplayUsesAbsoluteTimeline();
}
