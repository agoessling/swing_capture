#ifndef SWING_CAPTURE_ANDROID_POSE_HIL_POSE_QUALIFICATION_PERFORMANCE_H_
#define SWING_CAPTURE_ANDROID_POSE_HIL_POSE_QUALIFICATION_PERFORMANCE_H_

#include <string>
#include <string_view>

namespace swing_capture::android::pose_hil {

struct PoseQualificationPerformanceInspection {
  bool valid = false;
  std::string diagnostic;
  std::string summary_json;
};

// Derives a compact, deterministic summary from paired long-qualification telemetry and capture
// cycles. No threshold is selected for process cost or startup time: the summary makes the
// per-role tails reviewable without coupling the Pixel 6 path to the Pixel 5a result.
[[nodiscard]] PoseQualificationPerformanceInspection BuildPoseQualificationPerformanceSummary(
    std::string_view report_json) noexcept;

// Requires the report's embedded performance_summary to exactly match a fresh derivation.
[[nodiscard]] PoseQualificationPerformanceInspection ValidatePoseQualificationPerformanceSummary(
    std::string_view report_json) noexcept;

}  // namespace swing_capture::android::pose_hil

#endif  // SWING_CAPTURE_ANDROID_POSE_HIL_POSE_QUALIFICATION_PERFORMANCE_H_
