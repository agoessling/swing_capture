#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_RETAINED_MEDIA_ANALYSIS_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_RETAINED_MEDIA_ANALYSIS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json_fwd.hpp>

#include "android/dual_hil/dual_session_validation.h"
#include "android/dual_hil/rgb_swing_analysis.h"
#include "capture/optical/april_tag.h"

namespace swing_capture::android::dual_hil {

struct MediaAnalysisGeometry {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

[[nodiscard]] MediaAnalysisGeometry SelectMediaAnalysisGeometry(const NodeEvidence &evidence,
                                                                std::uint32_t maximum_width = 640U);

struct RetainedMediaAnalysisRequest {
  std::filesystem::path ffmpeg;
  std::filesystem::path ffprobe;
  std::filesystem::path media_path;
  std::filesystem::path node_output;
  std::reference_wrapper<const NodeEvidence> evidence;
};

struct RetainedMediaAnalysis {
  std::int64_t maximum_media_time_residual_us = 0;
  RgbSwingAnalysis optical;
  std::array<swing_capture::optical::AprilTagDetection, 3> april_tags;
  std::array<std::size_t, 3> diagnostic_frame_indices = {};
  std::int64_t ffprobe_stage_milliseconds = 0;
  std::int64_t decode_stage_milliseconds = 0;
  std::int64_t diagnostic_stage_milliseconds = 0;
  std::uint32_t analysis_width = 0;
  std::uint32_t analysis_height = 0;
};

[[nodiscard]] RetainedMediaAnalysis AnalyzeRetainedMedia(
    const RetainedMediaAnalysisRequest &request);

// Serializes every completed decode/optical/tag analysis, including a rejected
// optical candidate. AnalyzeRetainedMedia writes this evidence before
// enforcing the optical acceptance result so a physical failure remains
// independently inspectable.
[[nodiscard]] nlohmann::json RetainedMediaAnalysisEvidenceJson(
    const RetainedMediaAnalysis &analysis, const NodeEvidence &evidence);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_RETAINED_MEDIA_ANALYSIS_H_
