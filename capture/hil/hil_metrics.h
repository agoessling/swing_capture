#ifndef SWING_CAPTURE_CAPTURE_HIL_HIL_METRICS_H_
#define SWING_CAPTURE_CAPTURE_HIL_HIL_METRICS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace swing_capture::hil {

struct CameraMetrics {
  double requested_frames_per_second = 0.0;
  double resulting_frames_per_second = 0.0;
  double host_frames_per_second = 0.0;
  double device_frames_per_second = 0.0;
  double maximum_host_frame_interval_seconds = 0.0;
  double maximum_device_frame_interval_seconds = 0.0;
  double requested_duration_seconds = 0.0;
  double measured_duration_seconds = 0.0;
  std::uint64_t complete_frames = 0;
  std::uint64_t incomplete_frames = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t missing_frame_ids = 0;
  std::uint64_t timestamp_non_monotonic = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t expected_payload_bytes_per_frame = 0;
  bool internal_error = false;
};

struct Thresholds {
  double minimum_fps_ratio = 0.98;
  double maximum_fps_ratio = 1.02;
  // Host delivery can be delayed briefly by Linux scheduling while the
  // camera/SDK queue preserves every frame. Device timing remains stricter.
  double maximum_host_frame_interval_multiple = 10.0;
  double maximum_device_frame_interval_multiple = 4.0;
};

struct Check {
  std::string id;
  bool passed = false;
  std::string message;
};

struct Evaluation {
  bool passed = false;
  std::vector<Check> checks;
};

[[nodiscard]] Evaluation EvaluateCamera(const CameraMetrics &metrics, const Thresholds &thresholds);

}  // namespace swing_capture::hil

#endif  // SWING_CAPTURE_CAPTURE_HIL_HIL_METRICS_H_
