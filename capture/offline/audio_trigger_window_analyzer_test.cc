#include "capture/offline/audio_trigger_window_analyzer.h"

#include <cassert>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/trigger/impact_detector.h"

namespace {

constexpr std::uint32_t kRate = 48'000;

swing_capture::DecodedMonoPcmS16Wav Audio() {
  std::vector<std::int16_t> samples(kRate * 6, 10);
  samples[kRate] = 12'000;
  samples[kRate * 2] = 25'000;
  samples[kRate * 5] = 25'000;
  return {.sample_rate_hz = kRate, .samples = std::move(samples)};
}

swing_capture::ImpactDetectorConfig Config() {
  return {.threshold_multiplier = 2.0F,
          .minimum_peak_amplitude = 0.05F,
          .initial_noise_floor = 0.005F,
          .noise_update_clip_multiplier = 4.0F,
          .noise_floor_time_constant_seconds = 0.5,
          .peak_confirmation_seconds = 0.001,
          .refractory_period_seconds = 0.25};
}

void ScoresTheFirstCandidateOfEachFreshArm() {
  const nlohmann::json result = swing_capture::offline::AnalyzeAudioTriggerWindows(
      Audio(), Config(),
      {{.id = "S01", .start_us = 500'000, .target_us = 2'000'000, .end_us = 2'200'000},
       {.id = "S02", .start_us = 4'000'000, .target_us = 5'000'000, .end_us = 5'200'000}});

  assert(result.at("window_count") == 2);
  assert(result.at("target_first_count") == 1);
  assert(result.at("false_early_terminal_count") == 1);
  assert(result.at("windows").at(0).at("outcome") == "false_early_terminal");
  assert(result.at("windows").at(1).at("outcome") == "target_first");
}

}  // namespace

int main() {
  ScoresTheFirstCandidateOfEachFreshArm();
  return 0;
}
