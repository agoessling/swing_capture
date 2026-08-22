#ifndef SWING_CAPTURE_ANDROID_POSE_HIL_WARM_RETAINED_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_POSE_HIL_WARM_RETAINED_VALIDATION_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace swing_capture::android::pose_hil {

struct WarmRetainedInspection {
  std::string_view manifest;
  std::string_view media;
  std::string_view preview_mjpeg;
  std::string_view pose_trace_ndjson;
  std::string_view expected_session_id;
  std::string_view expected_shared_session_id;
  std::string_view expected_role;
};

struct WarmRetainedEvidence {
  bool valid = false;
  std::string diagnostic;
  std::string media_path;
  std::size_t media_bytes = 0;
  std::size_t frame_count = 0;
  std::size_t preview_frame_count = 0;
  std::int64_t camera_to_encoder_ordinal_shift = 0;
  std::int64_t encoder_to_sensor_offset_ns = 0;
  std::int64_t expected_encoder_to_sensor_offset_ns = 0;
  std::uint64_t encoder_to_sensor_offset_residual_ns = 0;
  std::int64_t timestamp_offset_span_ns = 0;
  std::size_t timestamp_pair_count = 0;
  std::int64_t actual_pre_roll_us = 0;
  std::int64_t actual_post_roll_us = 0;
};

// Validates the complete retained warm-transition session and its pose preview sidecar.
[[nodiscard]] WarmRetainedEvidence ValidateWarmRetainedEvidence(
    const WarmRetainedInspection &inspection) noexcept;

}  // namespace swing_capture::android::pose_hil

#endif  // SWING_CAPTURE_ANDROID_POSE_HIL_WARM_RETAINED_VALIDATION_H_
