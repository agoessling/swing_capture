#ifndef SWING_CAPTURE_CAPTURE_OFFLINE_AUDIO_TRIGGER_WINDOW_ANALYZER_H_
#define SWING_CAPTURE_CAPTURE_OFFLINE_AUDIO_TRIGGER_WINDOW_ANALYZER_H_

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture::offline {

struct AudioTriggerWindow {
  std::string id;
  std::int64_t start_us = 0;
  std::int64_t target_us = 0;
  std::int64_t end_us = 0;
};

// Recreates the production behavior in which each high-speed arm starts a fresh
// detector and the first accepted transient is terminal for that attempt.
[[nodiscard]] nlohmann::json AnalyzeAudioTriggerWindows(
    const DecodedMonoPcmS16Wav &audio, ImpactDetectorConfig detector_config,
    const std::vector<AudioTriggerWindow> &windows, std::int64_t target_tolerance_us = 100'000);

}  // namespace swing_capture::offline

#endif  // SWING_CAPTURE_CAPTURE_OFFLINE_AUDIO_TRIGGER_WINDOW_ANALYZER_H_
