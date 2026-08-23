#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_TIMING_CALIBRATION_EVIDENCE_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_TIMING_CALIBRATION_EVIDENCE_H_

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace swing_capture::android::dual_hil {

// Parses, validates, and independently recomputes a timing-evidence document. The returned report
// deliberately distinguishes complete arithmetic/provenance from a physical calibration claim.
[[nodiscard]] nlohmann::json EvaluateTimingCalibrationEvidence(
    std::string_view evidence_json, const std::filesystem::path &artifact_root);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_TIMING_CALIBRATION_EVIDENCE_H_
