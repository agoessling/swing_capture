#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_AUTONOMOUS_RECOVERY_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_AUTONOMOUS_RECOVERY_VALIDATION_H_

#include <string_view>

namespace swing_capture::android::dual_hil {

struct AutonomousLocalOnlyRecoveryInspection {
  std::string_view leader_status_json;
  std::string_view expected_shared_session_id;
};

struct AutonomousRecoveredStationInspection {
  std::string_view leader_status_json;
  std::string_view shadow_status_json;
  std::string_view expected_shared_session_id;
};

// Validates the durable state after peer unavailability forced local-only completion.
void ValidateAutonomousLocalOnlyRecovery(const AutonomousLocalOnlyRecoveryInspection &inspection);

// Validates backlog replication and automatic low-rate monitoring on both phones.
void ValidateAutonomousRecoveredStation(const AutonomousRecoveredStationInspection &inspection);

// Validates that the leader has detected a stopped/unreachable peer without losing station state.
void ValidateAutonomousPeerUnavailable(std::string_view leader_status_json);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_AUTONOMOUS_RECOVERY_VALIDATION_H_
