#include "capture/offline/experiments/envelope/envelope_experiment.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <numbers>
#include <string>
#include <vector>

#include "capture/audio/pcm_wav.h"

namespace {

using swing_capture::DecodedMonoPcmS16Wav;
using swing_capture::offline::envelope::Analyze;
using swing_capture::offline::envelope::ArmWindow;
using swing_capture::offline::envelope::DetectCandidates;
using swing_capture::offline::envelope::DetectorConfig;
using swing_capture::offline::envelope::ParseArmWindows;

DecodedMonoPcmS16Wav SyntheticAudio() {
  constexpr std::uint32_t kRate = 48'000;
  DecodedMonoPcmS16Wav audio{.sample_rate_hz = kRate,
                             .samples = std::vector<std::int16_t>(kRate * 2, 0)};
  for (std::size_t index = 0; index < audio.samples.size(); ++index) {
    const double time = static_cast<double>(index) / kRate;
    audio.samples[index] =
        static_cast<std::int16_t>(40.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * time));
  }
  const std::size_t impulse = kRate;
  audio.samples[impulse] = 12'000;
  audio.samples[impulse + 1] = -8'000;
  audio.samples[impulse + 2] = 4'000;
  return audio;
}

DetectorConfig TestConfig() {
  DetectorConfig config;
  config.name = "test";
  config.background_multiplier = 5.0;
  config.rise_ratio = 2.0;
  return config;
}

void DetectsAndReportsBoundedImpulse() {
  const auto candidates = DetectCandidates(SyntheticAudio(), TestConfig());
  assert(candidates.size() == 1);
  assert(std::abs(candidates.front().strike_us - 1'000'000) < 1'000);
  assert(candidates.front().decision_us >= candidates.front().strike_us);
  assert(candidates.front().decision_us - candidates.front().strike_us < 30'000);

  const auto report = Analyze(
      SyntheticAudio(),
      {ArmWindow{.id = "S01", .arm_us = 600'000, .target_us = 1'000'000, .end_us = 1'100'000}},
      {TestConfig()}, 200'000);
  const auto &result = report.at("reports").at(0);
  assert(result.at("candidate_generation").at("target_recalled") == 1);
  assert(result.at("armed_terminal").at("target_first_count") == 1);
}

void ReadinessRejectsEarlierImpulse() {
  auto audio = SyntheticAudio();
  audio.samples[static_cast<std::size_t>(0.7 * audio.sample_rate_hz)] = 14'000;
  const auto report = Analyze(
      audio,
      {ArmWindow{.id = "S01", .arm_us = 600'000, .target_us = 1'000'000, .end_us = 1'100'000}},
      {TestConfig()}, 200'000);
  assert(report.at("reports").at(0).at("armed_terminal").at("target_first_count") == 1);
}

void ParsesFrozenWindowShape() {
  const nlohmann::json input = {
      {"windows", {{{"id", "S01"}, {"start_us", "10"}, {"target_us", "20"}, {"end_us", "30"}}}}};
  const auto windows = ParseArmWindows(input);
  assert(windows.size() == 1);
  assert(windows.front().arm_us == 10);
  assert(windows.front().target_us == 20);
}

}  // namespace

int main() {
  DetectsAndReportsBoundedImpulse();
  ReadinessRejectsEarlierImpulse();
  ParsesFrozenWindowShape();
}
