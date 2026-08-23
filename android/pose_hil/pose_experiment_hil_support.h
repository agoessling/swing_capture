#ifndef SWING_CAPTURE_ANDROID_POSE_HIL_POSE_EXPERIMENT_HIL_SUPPORT_H_
#define SWING_CAPTURE_ANDROID_POSE_HIL_POSE_EXPERIMENT_HIL_SUPPORT_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::android::pose_hil {

struct PoseExperimentHilInputs {
  std::string model_variant;
  std::uint32_t standby_width = 0;
  std::uint32_t standby_height = 0;
};

// Resolves the production-equivalent experiment defaults and validates the bounded manual-HIL
// contract. Runtime camera support and packaged model-asset availability remain device checks.
[[nodiscard]] PoseExperimentHilInputs ResolvePoseExperimentHilInputs(std::string_view model_variant,
                                                                     std::string_view standby_size);

// Returns only the activity extras consumed by the manual pose standby experiment path.
[[nodiscard]] std::vector<std::string> PoseExperimentActivityExtras(
    const PoseExperimentHilInputs &inputs);

[[nodiscard]] bool IsProductionEquivalent(const PoseExperimentHilInputs &inputs);

}  // namespace swing_capture::android::pose_hil

#endif  // SWING_CAPTURE_ANDROID_POSE_HIL_POSE_EXPERIMENT_HIL_SUPPORT_H_
