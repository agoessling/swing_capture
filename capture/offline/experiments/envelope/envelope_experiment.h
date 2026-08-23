#ifndef SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_ENVELOPE_ENVELOPE_EXPERIMENT_H_
#define SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_ENVELOPE_ENVELOPE_EXPERIMENT_H_

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "capture/audio/pcm_wav.h"

namespace swing_capture::offline::envelope {

struct DetectorConfig {
  std::string name;
  double high_pass_hz = 120.0;
  std::int64_t hop_us = 500;
  std::int64_t fast_window_us = 1'500;
  std::int64_t background_window_us = 240'000;
  std::int64_t background_guard_us = 10'000;
  std::int64_t background_block_us = 10'000;
  double background_multiplier = 8.0;
  double rise_ratio = 1.4;
  double minimum_peak = 0.004;
  double minimum_high_frequency_ratio = 0.0;
  double minimum_crest_factor = 0.0;
  std::int64_t cluster_gap_us = 20'000;
  std::int64_t refractory_us = 30'000;
};

struct ArmWindow {
  std::string id;
  std::int64_t arm_us = 0;
  std::int64_t target_us = 0;
  std::int64_t end_us = 0;
};

struct Candidate {
  std::int64_t strike_us = 0;
  std::int64_t decision_us = 0;
  double peak = 0.0;
  double fast_rms = 0.0;
  double background_rms = 0.0;
  double background_ratio = 0.0;
  double rise_ratio = 0.0;
  double high_frequency_ratio = 0.0;
  double crest_factor = 0.0;
  std::int64_t cluster_duration_us = 0;
};

[[nodiscard]] std::vector<Candidate> DetectCandidates(const DecodedMonoPcmS16Wav &audio,
                                                      const DetectorConfig &config);

// Scores candidate generation over the entire recording and terminal behavior
// after each arm's bounded trigger-readiness delay.
[[nodiscard]] nlohmann::json Analyze(const DecodedMonoPcmS16Wav &audio,
                                     const std::vector<ArmWindow> &windows,
                                     const std::vector<DetectorConfig> &configs,
                                     std::int64_t readiness_delay_us = 2'450'000,
                                     std::int64_t target_tolerance_us = 100'000);

[[nodiscard]] std::vector<ArmWindow> ParseArmWindows(const nlohmann::json &window_report);
[[nodiscard]] std::vector<DetectorConfig> ExperimentConfigs();

}  // namespace swing_capture::offline::envelope

#endif  // SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_ENVELOPE_ENVELOPE_EXPERIMENT_H_
