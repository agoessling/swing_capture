#ifndef SWING_CAPTURE_ANDROID_POSE_HIL_POSE_STANDBY_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_POSE_HIL_POSE_STANDBY_VALIDATION_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace swing_capture::android::pose_hil {

struct PoseStatusSample {
  bool valid = false;
  std::string diagnostic;
  std::uint64_t server_elapsed_realtime_ns = 0;
  std::string state;
  bool armed = false;
  std::string mode;
  std::string configured_delegate;
  std::string actual_delegate;
  std::string phase;
  std::uint64_t offered_images = 0;
  std::uint64_t scheduled_images = 0;
  std::uint64_t dropped_images = 0;
  std::uint64_t successful_inferences = 0;
  std::uint64_t failed_inferences = 0;
  std::uint64_t encoded_evidence_frames = 0;
  bool standby_audio_ready = false;
  int standby_audio_source = -1;
  std::uint64_t standby_audio_end_frame_position = 0;
  std::uint64_t standby_audio_dropped_events = 0;
  std::uint64_t standby_audio_timestamp_rejections = 0;
  std::uint64_t standby_audio_discontinuities = 0;
  std::string standby_audio_last_error;
};

// Strictly validates the public capture-status fields consumed by the physical HIL.
[[nodiscard]] PoseStatusSample InspectPoseStatus(std::string_view text) noexcept;

// Returns true only for a structurally valid status with the requested number of completed JPEG
// encodes. Inference completion alone is intentionally insufficient because encoding is async.
[[nodiscard]] bool HasMinimumEncodedPoseEvidence(const PoseStatusSample &sample,
                                                 std::uint64_t minimum_frames) noexcept;

struct PoseCadenceAcceptance {
  bool passed = false;
  std::string diagnostic;
  double interval_seconds = 0.0;
  double successful_inferences_per_second = 0.0;
  double dropped_fraction = 0.0;
  std::uint64_t offered_images = 0;
  std::uint64_t scheduled_images = 0;
  std::uint64_t dropped_images = 0;
  std::uint64_t successful_inferences = 0;
  std::uint64_t failed_inferences = 0;
};

// Applies the bounded standby acceptance policy to two phone-clock status samples.
[[nodiscard]] PoseCadenceAcceptance EvaluatePoseCadence(const PoseStatusSample &first,
                                                        const PoseStatusSample &last) noexcept;

struct DeviceTelemetry {
  bool valid = false;
  std::string diagnostic;
  int thermal_status = -1;
  int battery_level_percent = -1;
  double battery_temperature_celsius = -1.0;
  int battery_voltage_millivolts = -1;
};

// Parses stable fields from Pixel dumpsys battery and thermalservice output.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] DeviceTelemetry InspectDeviceTelemetry(std::string_view battery,
                                                     std::string_view thermal) noexcept;

}  // namespace swing_capture::android::pose_hil

#endif  // SWING_CAPTURE_ANDROID_POSE_HIL_POSE_STANDBY_VALIDATION_H_
