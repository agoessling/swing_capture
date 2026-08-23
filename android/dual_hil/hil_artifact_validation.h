#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_ARTIFACT_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_ARTIFACT_VALIDATION_H_

#include <filesystem>
#include <optional>
#include <string_view>

namespace swing_capture::android::dual_hil {

// Validates that every string leaf below report.artifacts names a regular file
// contained by output_root. Null leaves represent intentionally absent optional
// artifacts. allowed_missing_self_report may name the report being validated so
// the contract can run immediately before that report's first write.
void ValidateHilReportArtifacts(
    std::string_view report_json, const std::filesystem::path &output_root,
    std::optional<std::filesystem::path> allowed_missing_self_report = std::nullopt);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_ARTIFACT_VALIDATION_H_
