#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_OS_REBOOT_CEREMONY_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_OS_REBOOT_CEREMONY_VALIDATION_H_

#include <cstdint>
#include <string>

namespace swing_capture::android::dual_hil {

struct OsRebootNodeInspection {
  std::string role;
  std::string before_boot_id;
  std::string after_boot_id;
  std::string before_setup_json;
  std::string before_status_json;
  std::string after_setup_json;
  std::string after_launch_status_json;
  std::string armed_status_json;
  std::string cleanup_status_json;
  std::string cleanup_setup_json;
  bool exact_apk_verified = false;
  bool reboot_command_accepted = false;
  bool user_unlocked_before_launch = false;
  bool capture_service_absent_before_launch = false;
  bool node_api_unavailable_before_launch = false;
  bool foreground_activity_observed = false;
  bool adb_foreground_launch_sent = false;
  bool disarm_acknowledged = false;
};

struct OsRebootCeremonyInspection {
  OsRebootNodeInspection face_on;
  OsRebootNodeInspection down_the_line;
};

struct OsRebootNodeEvidence {
  std::string role;
  std::string node_id;
  std::string device_model;
  std::int64_t boot_observation_count_before = 0;
  std::int64_t boot_observation_count_after = 0;
};

struct OsRebootCeremonyEvidence {
  OsRebootNodeEvidence face_on;
  OsRebootNodeEvidence down_the_line;
};

/**
 * Validates retained evidence from a real two-phone OS reboot.  This function does not infer that
 * a reboot occurred from static policy: boot IDs, post-unlock pre-launch absence, the boot marker,
 * durable setup, live peer recovery with a fresh bounded shadow-clock mapping, autonomous
 * monitoring, real inference/audio monitoring, and terminal disarm are all required.
 */
[[nodiscard]] OsRebootCeremonyEvidence ValidateOsRebootCeremony(
    const OsRebootCeremonyInspection &inspection);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_OS_REBOOT_CEREMONY_VALIDATION_H_
