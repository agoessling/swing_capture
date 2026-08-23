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

struct PoseConfiguredDescriptorInspection {
  std::string_view descriptor_json;
  NodeApiIdentity identity;
  std::string_view expected_mode;
  bool expected_peer_configured = false;
};

struct PairedPoseClockStatusInspection {
  std::string_view leader_status_json;
  std::string_view shadow_status_json;
  NodeApiIdentity leader_identity;
  NodeApiIdentity shadow_identity;
};

struct MappedPeerImpactStatusInspection {
  std::string_view shadow_status_json;
  NodeApiIdentity leader_identity;
  NodeApiIdentity shadow_identity;
  std::string_view expected_shared_session_id;
};

struct PairedPoseSessionStateInspection {
  std::string_view leader_status_json;
  std::string_view shadow_status_json;
  std::string_view expected_phase;
  std::string_view expected_shared_session_id;
};

struct LanEndpointInspection {
  std::string_view descriptor_json;
  std::string_view setup_json;
  std::string_view status_json;
  std::string_view clock_json;
  NodeApiIdentity identity;
  std::string_view expected_origin;
  std::string_view expected_device_model;
  std::string_view expected_pose_mode;
  std::string_view expected_peer_origin;
  int unauthenticated_setup_status = 0;
  int authenticated_setup_status = 0;
};

enum class DurablePairingState {
  kAbsent,
  kActive,
  kRevoked,
};

// Inspection used to make discovery/pairing HIL setup independent of durable
// field state preserved by `adb install -r`.
struct DiscoveryPairingFixtureInspection {
  std::string pose_mode;
  bool peer_configured = false;
  DurablePairingState pairing_state = DurablePairingState::kAbsent;
  std::string peer_node_id;

  [[nodiscard]] bool IsUnpaired() const {
    return pose_mode == "disabled" && !peer_configured &&
           pairing_state == DurablePairingState::kAbsent;
  }
};

struct ShortPoseLatencyEvidence {
  std::string actual_delegate;
  std::uint64_t successful_inferences = 0;
  std::uint64_t inference_duration_p95_ns = 0;
  std::uint64_t maximum_inference_duration_ns = 0;
  std::uint64_t inference_deadline_misses = 0;
  std::uint64_t inference_outliers = 0;
  std::uint64_t decision_age_samples = 0;
  std::uint64_t rejected_decision_timestamps = 0;
  std::uint64_t decision_age_p95_ns = 0;
  std::uint64_t maximum_decision_age_ns = 0;
  std::uint64_t offered_images = 0;
  std::uint64_t scheduled_images = 0;
  std::uint64_t dropped_images = 0;
  std::uint64_t maximum_warmup_duration_ns = 0;
};

[[nodiscard]] NodeApiIdentity ValidateNodeDescriptor(
    std::string_view descriptor_json, dual_coordination_hil::CaptureRole expected_role,
    std::string_view expected_profile);

void ValidateArmedCaptureStatus(const ArmedStatusInspection &inspection);

void ValidatePoseConfiguredNodeDescriptor(const PoseConfiguredDescriptorInspection &inspection);

[[nodiscard]] dual_coordination_hil::TriggerReport ValidateTriggerReport(
    std::string_view trigger_report_json, const NodeApiIdentity &identity,
    std::string_view shared_session_id, std::string_view local_session_id,
    std::string_view expected_source);

void ValidatePairedPoseClockStatus(const PairedPoseClockStatusInspection &inspection);

void ValidateMappedPeerImpactStatus(const MappedPeerImpactStatusInspection &inspection);

[[nodiscard]] bool HasPairedAutomaticImpactEvidence(std::string_view leader_status_json,
                                                    std::string_view shadow_status_json);

void ValidatePairedPoseSessionState(const PairedPoseSessionStateInspection &inspection);

void ValidateLanEndpoint(const LanEndpointInspection &inspection);

[[nodiscard]] DiscoveryPairingFixtureInspection InspectDiscoveryPairingFixture(
    std::string_view setup_json);

// Applies the short paired-HIL regression bound. This is not a substitute for steady-state
// motion/thermal qualification.
[[nodiscard]] ShortPoseLatencyEvidence ValidateShortPoseLatency(std::string_view status_json);

// Node HTTP responses contain the canonical JSON plus the server's single
// framing newline. Replay must otherwise be byte-for-byte identical.
void ValidateCanonicalCoordinationReplay(std::string_view response_body,
                                         std::string_view canonical_json);

// Validates the transportable evidence contract emitted by the paired pose-arm HIL. The physical
// runner calls this immediately before publishing its successful aggregate report.
void ValidatePairedPoseHilReport(std::string_view report_json);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_CONCURRENT_HIL_VALIDATION_H_
