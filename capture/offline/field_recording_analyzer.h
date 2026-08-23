#ifndef SWING_CAPTURE_CAPTURE_OFFLINE_FIELD_RECORDING_ANALYZER_H_
#define SWING_CAPTURE_CAPTURE_OFFLINE_FIELD_RECORDING_ANALYZER_H_

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

#include "capture/audio/pcm_wav.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture::offline {

struct FieldRecordingPair {
  nlohmann::json down_the_line_manifest;
  DecodedMonoPcmS16Wav down_the_line_audio;
  nlohmann::json face_on_manifest;
  DecodedMonoPcmS16Wav face_on_audio;
};

struct FieldRecordingAnalysisConfig {
  std::int64_t pre_roll_us = 3'000'000;
  std::int64_t post_roll_us = 2'000'000;
  std::int64_t pair_tolerance_us = 80'000;
  ImpactDetectorConfig down_the_line_detector = {
      .threshold_multiplier = 5.0F,
      .minimum_peak_amplitude = 0.006F,
      .initial_noise_floor = 0.001F,
      .noise_update_clip_multiplier = 4.0F,
      .noise_floor_time_constant_seconds = 0.25,
      .peak_confirmation_seconds = 0.0025,
      .refractory_period_seconds = 0.30,
  };
  ImpactDetectorConfig face_on_detector = down_the_line_detector;
};

// Produces a deterministic, high-recall impulse index. Cross-phone pairs remain
// candidates until video/pose confirms that they are swings; every unmatched
// local candidate is retained rather than silently discarded.
[[nodiscard]] nlohmann::json AnalyzeFieldRecordingPair(const FieldRecordingPair &recordings,
                                                       FieldRecordingAnalysisConfig config = {});
[[nodiscard]] std::string FieldRecordingPairedCandidatesCsv(const nlohmann::json &analysis);

}  // namespace swing_capture::offline

#endif  // SWING_CAPTURE_CAPTURE_OFFLINE_FIELD_RECORDING_ANALYZER_H_
