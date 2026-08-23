#ifndef SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_SPECTRAL_SPECTRAL_IMPACT_EXPERIMENT_H_
#define SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_SPECTRAL_SPECTRAL_IMPACT_EXPERIMENT_H_

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "capture/audio/pcm_wav.h"

namespace swing_capture::offline::spectral {

struct ImpactFeatures {
  std::vector<double> engineered;
  std::vector<double> normalized_log_spectrum;
};

struct RecordingInput {
  std::string view;
  std::string device;
  DecodedMonoPcmS16Wav audio;
  nlohmann::json production_windows;
};

// Extracts causal-at-deadline features around a production detector candidate.
// The longest feature observes 30 ms after the detector timestamp; callers must
// include that bounded latency in any online terminal policy.
[[nodiscard]] ImpactFeatures ExtractImpactFeatures(const DecodedMonoPcmS16Wav &audio,
                                                   std::int64_t candidate_time_us);

// Runs nested leave-one-swing-out scoring. Both phone observations for the
// evaluated swing are excluded from model fitting, reference templates, and
// operating-threshold selection.
[[nodiscard]] nlohmann::json EvaluateSpectralImpactExperiment(
    const std::vector<RecordingInput> &recordings);

[[nodiscard]] std::string RenderSpectralImpactExperimentMarkdown(const nlohmann::json &report);

}  // namespace swing_capture::offline::spectral

#endif  // SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_SPECTRAL_SPECTRAL_IMPACT_EXPERIMENT_H_
