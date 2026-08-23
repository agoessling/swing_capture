#include "capture/offline/experiments/spectral/spectral_impact_experiment.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "capture/audio/pcm_wav.h"

namespace {

constexpr std::uint32_t kSampleRateHz = 48'000;

swing_capture::DecodedMonoPcmS16Wav SyntheticAudio(int frequency_offset) {
  swing_capture::DecodedMonoPcmS16Wav audio;
  audio.sample_rate_hz = kSampleRateHz;
  audio.samples.assign(kSampleRateHz * 12U, 0);
  for (int shot = 0; shot < 4; ++shot) {
    const std::size_t false_frame = static_cast<std::size_t>((shot * 2 + 3) * kSampleRateHz);
    const std::size_t target_frame = false_frame + kSampleRateHz / 2U;
    for (std::size_t index = 0; index < 1'000; ++index) {
      const double false_wave =
          2'000.0 * std::sin(2.0 * std::numbers::pi * static_cast<double>(200 + frequency_offset) *
                             static_cast<double>(index) / kSampleRateHz);
      audio.samples[false_frame + index] = static_cast<std::int16_t>(std::llround(false_wave));
    }
    for (std::size_t index = 0; index < 400; ++index) {
      const double envelope = std::exp(-static_cast<double>(index) / 90.0);
      const double target_wave =
          12'000.0 * envelope *
          (std::sin(2.0 * std::numbers::pi * static_cast<double>(4'000 + frequency_offset) *
                    static_cast<double>(index) / kSampleRateHz) +
           0.5 * std::sin(2.0 * std::numbers::pi * static_cast<double>(9'000 + frequency_offset) *
                          static_cast<double>(index) / kSampleRateHz));
      audio.samples[target_frame + index] = static_cast<std::int16_t>(std::llround(target_wave));
    }
  }
  return audio;
}

nlohmann::json SyntheticWindows(int target_offset_us) {
  nlohmann::json windows = nlohmann::json::array();
  for (int shot = 0; shot < 4; ++shot) {
    const std::int64_t false_us = static_cast<std::int64_t>(shot * 2 + 3) * 1'000'000;
    const std::int64_t target_us = false_us + 500'000 + target_offset_us;
    const std::string id = "S0" + std::to_string(shot + 1);
    windows.push_back(
        {{"id", id},
         {"start_us", std::to_string(false_us - 2'500'000)},
         {"target_us", std::to_string(target_us)},
         {"end_us", std::to_string(target_us + 150'000)},
         {"candidates",
          {{{"time_us", std::to_string(false_us)}}, {{"time_us", std::to_string(target_us)}}}}});
  }
  return {{"schema_version", 1},
          {"target_tolerance_us", "100000"},
          {"window_count", 4},
          {"windows", std::move(windows)}};
}

void ExtractsFiniteFeatures() {
  const auto audio = SyntheticAudio(0);
  const auto features = swing_capture::offline::spectral::ExtractImpactFeatures(audio, 3'500'000);
  assert(features.engineered.size() == 18);
  assert(features.normalized_log_spectrum.size() == 32);
  for (double feature : features.engineered) {
    assert(std::isfinite(feature));
  }
  double norm = 0.0;
  for (double value : features.normalized_log_spectrum) {
    norm += value * value;
  }
  assert(std::abs(norm - 1.0) < 1.0e-9);
}

void EvaluatesAllMethodsWithoutLeakingHeldOutSwing() {
  const std::vector<swing_capture::offline::spectral::RecordingInput> recordings = {
      {.view = "down_the_line",
       .device = "synthetic-a",
       .audio = SyntheticAudio(0),
       .production_windows = SyntheticWindows(0)},
      {.view = "face_on",
       .device = "synthetic-b",
       .audio = SyntheticAudio(50),
       .production_windows = SyntheticWindows(0)},
  };
  const nlohmann::json report =
      swing_capture::offline::spectral::EvaluateSpectralImpactExperiment(recordings);
  assert(report.at("methods").size() == 7);
  assert(report.at("candidate_generation").at("face_on").at("target_recall") == 4);
  for (const auto &method : report.at("methods")) {
    assert(method.at("folds").size() == 4);
    for (const auto &fold : method.at("folds")) {
      assert(fold.at("training_shot_count") == 3);
    }
  }
  const std::string markdown =
      swing_capture::offline::spectral::RenderSpectralImpactExperimentMarkdown(report);
  assert(markdown.find("Nested leave-one-swing-out") != std::string::npos);
}

void RejectsInsufficientContext() {
  bool rejected = false;
  try {
    static_cast<void>(
        swing_capture::offline::spectral::ExtractImpactFeatures(SyntheticAudio(0), 1'000));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

}  // namespace

int main() {
  ExtractsFiniteFeatures();
  EvaluatesAllMethodsWithoutLeakingHeldOutSwing();
  RejectsInsufficientContext();
  return 0;
}
