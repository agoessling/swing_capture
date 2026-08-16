#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_CONCURRENT_HIL_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_CONCURRENT_HIL_VALIDATION_H_

#include <string>
#include <string_view>

#include "android/dual_coordination_hil/dual_coordination.h"

namespace swing_capture::android::dual_hil {

struct NodeApiIdentity {
  std::string node_id;
  dual_coordination_hil::CaptureRole role = dual_coordination_hil::CaptureRole::kDownTheLine;
  std::string capture_profile;
};

struct ArmedStatusInspection {
  std::string_view status_json;
  std::string_view shared_session_id;
};

[[nodiscard]] NodeApiIdentity ValidateNodeDescriptor(
    std::string_view descriptor_json, dual_coordination_hil::CaptureRole expected_role,
    std::string_view expected_profile);

void ValidateArmedCaptureStatus(const ArmedStatusInspection &inspection);

[[nodiscard]] dual_coordination_hil::TriggerReport ValidateTriggerReport(
    std::string_view trigger_report_json, const NodeApiIdentity &identity,
    std::string_view shared_session_id, std::string_view local_session_id);

// Node HTTP responses contain the canonical JSON plus the server's single
// framing newline. Replay must otherwise be byte-for-byte identical.
void ValidateCanonicalCoordinationReplay(std::string_view response_body,
                                         std::string_view canonical_json);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_CONCURRENT_HIL_VALIDATION_H_
