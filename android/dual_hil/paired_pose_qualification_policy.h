#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_PAIRED_POSE_QUALIFICATION_POLICY_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_PAIRED_POSE_QUALIFICATION_POLICY_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace swing_capture::android::dual_hil {

struct PairedPoseQualificationPolicy {
  std::string_view mode;
  std::string_view report_type;
  int duration_seconds = 0;
  int telemetry_interval_seconds = 0;
  int cycle_interval_seconds = 0;
  std::size_t minimum_cycle_count = 0;
  std::size_t minimum_telemetry_sample_count = 0;
};

// Returns one of the two deliberately closed long-run policies. Arbitrary durations are not
// accepted because a shortened run must never be mistaken for qualification evidence.
[[nodiscard]] const PairedPoseQualificationPolicy &PairedPoseQualificationPolicyForMode(
    std::string_view mode);

[[nodiscard]] bool IsPairedPoseQualificationMode(std::string_view mode) noexcept;

struct PairedPoseQualificationReportValidation {
  bool passed = false;
  std::string diagnostic;
};

// Validates the durable aggregate independently of physical hardware. The expected mode is
// mandatory so evidence from the five-minute gate cannot satisfy the thirty-minute soak.
[[nodiscard]] PairedPoseQualificationReportValidation ValidatePairedPoseQualificationReport(
    std::string_view report_json, std::string_view expected_mode) noexcept;

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_PAIRED_POSE_QUALIFICATION_POLICY_H_
