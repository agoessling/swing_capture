#ifndef SWING_CAPTURE_ANDROID_HIL_POSE_REPLAY_REPORT_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_HIL_POSE_REPLAY_REPORT_VALIDATION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace swing_capture::android::hil {

inline constexpr int kPoseReplayReportSchemaVersion = 1;
inline constexpr std::string_view kPoseReplayReportType = "pose_replay_hil";
inline constexpr std::size_t kPoseReplayMaximumTraceFrames = 1200U;
inline constexpr std::size_t kPoseReplayMaximumReportBytes = 1500000U;

struct PoseReplayReportInspection {
  bool valid = false;
  bool passed = false;
  std::string outcome;
  std::string failure_code;
  std::string role;
  std::string projection;
  std::optional<std::string> actual_delegate;
  std::size_t frame_count = 0;
  std::size_t arm_request_count = 0;
  std::optional<std::uint64_t> first_source_timestamp_ns;
  std::optional<std::uint64_t> last_source_timestamp_ns;
  std::optional<std::uint64_t> first_arm_request_ns;
  std::string diagnostic;
};

// Validates the complete schema-v1 Java replay report. Malformed input is
// represented by valid == false so a HIL caller can publish the diagnostic.
[[nodiscard]] PoseReplayReportInspection InspectPoseReplayReport(std::string_view report) noexcept;

}  // namespace swing_capture::android::hil

#endif  // SWING_CAPTURE_ANDROID_HIL_POSE_REPLAY_REPORT_VALIDATION_H_
