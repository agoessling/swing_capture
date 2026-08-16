#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_DUAL_SESSION_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_DUAL_SESSION_VALIDATION_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::android::dual_hil {

struct NodeEvidence {
  std::string node_id;
  std::string role;
  std::string local_session_id;
  std::string shared_session_id;
  std::string manifest_path;
  std::string media_path;
  std::size_t media_bytes = 0;
  std::size_t frame_count = 0;
  double measured_sensor_fps = 0.0;
  std::int64_t actual_pre_roll_us = 0;
  std::int64_t actual_post_roll_us = 0;
  std::int64_t local_nearest_frame_residual_us = 0;
  int width = 0;
  int height = 0;
  int bitrate_bits_per_second = 0;
  std::int64_t trigger_timestamp_uncertainty_ns = 0;
  std::int64_t trigger_time_ns = 0;
  std::int64_t confirmation_time_ns = 0;
  int audio_sample_rate_hz = 0;
  double audio_peak_amplitude = 0.0;
  double audio_noise_floor = 0.0;
  double audio_threshold = 0.0;
  std::vector<std::int64_t> media_times_us;
  std::vector<std::int64_t> times_from_impact_us;
};

struct CaptureProfileExpectation {
  int width = 0;
  int height = 0;
  int bitrate_bits_per_second = 0;
};

struct NodeEvidenceInspection {
  std::string_view report;
  std::string_view manifest;
  std::string_view media;
  std::string_view expected_role;
  std::string_view expected_shared_session_id;
  CaptureProfileExpectation expected_profile;
};

struct TimingCorrelationInspection {
  std::int64_t optical_onset_lower_bound_us = 0;
  std::int64_t optical_onset_upper_bound_us = 0;
  std::int64_t audio_trigger_uncertainty_ns = 0;
  std::int64_t media_pts_residual_us = 0;
};

struct TimingCorrelationEvidence {
  std::int64_t acceptance_limit_us = 20000;
  std::int64_t optical_onset_lower_bound_us = 0;
  std::int64_t optical_onset_upper_bound_us = 0;
  std::int64_t optical_interval_width_us = 0;
  std::int64_t audio_trigger_uncertainty_us = 0;
  std::int64_t media_pts_residual_us = 0;
  std::int64_t accounted_uncertainty_us = 0;
  std::int64_t minimum_residual_us = 0;
  std::int64_t maximum_residual_us = 0;
  std::int64_t total_bound_us = 0;
  bool passed = false;
};

struct AprilTagFrameEvidence {
  std::size_t frame_index = 0;
  std::string family;
  int id = -1;
  int hamming = 0;
  double decision_margin = 0.0;
};

[[nodiscard]] NodeEvidence ValidateNodeEvidence(const NodeEvidenceInspection &inspection);

[[nodiscard]] std::int64_t ValidateFfprobeTimeline(const NodeEvidence &evidence,
                                                   std::string_view ffprobe_json);

[[nodiscard]] TimingCorrelationEvidence EvaluateTimingCorrelation(
    const TimingCorrelationInspection &inspection);

void ValidateRequiredAprilTagPersistence(std::span<const AprilTagFrameEvidence> frames);

void ValidateDualSession(const NodeEvidence &down_the_line, const NodeEvidence &face_on);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_DUAL_SESSION_VALIDATION_H_
