#include "android/dual_hil/concurrent_hil_validation.h"

#include <cassert>
#include <exception>
#include <nlohmann/json.hpp>
#include <string>

#include "android/dual_coordination_hil/dual_coordination.h"

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
namespace coordination = swing_capture::android::dual_coordination_hil;
using swing_capture::android::dual_hil::ArmedStatusInspection;
using swing_capture::android::dual_hil::NodeApiIdentity;
using swing_capture::android::dual_hil::PoseConfiguredDescriptorInspection;
using swing_capture::android::dual_hil::ValidateArmedCaptureStatus;
using swing_capture::android::dual_hil::ValidateCanonicalCoordinationReplay;
using swing_capture::android::dual_hil::ValidateNodeDescriptor;
using swing_capture::android::dual_hil::ValidatePairedPoseHilReport;
using swing_capture::android::dual_hil::ValidatePoseConfiguredNodeDescriptor;
using swing_capture::android::dual_hil::ValidateTriggerReport;

template <typename Action>
void ExpectFailure(Action action) {
  try {
    action();
  } catch (const std::exception &) {
    return;
  }
  assert(false);
}

NodeApiIdentity Identity() {
  return ValidateNodeDescriptor(Json({
                                         {"schema_version", 1},
                                         {"node_id", "node-dtl"},
                                         {"role", "down_the_line"},
                                         {"capture_profile", "720p240"},
                                         {"service_urls", Json::array()},
                                         {"control_authentication", "bearer"},
                                     })
                                    .dump(),
                                coordination::CaptureRole::kDownTheLine, "720p240");
}

Json PairedPoseReportFixture() {
  constexpr std::string_view kSharedSessionId = "pose-hil-shared";
  const auto node = [](std::string_view role) {
    return Json{{"passed", true}, {"role", role}, {"shared_session_id", "pose-hil-shared"}};
  };
  const auto artifacts = [](std::string_view role) {
    const std::string prefix = std::string(role) + "/";
    return Json{
        {"initial_node_descriptor", prefix + "node-descriptor-initial.json"},
        {"pose_configured_node_descriptor", prefix + "node-descriptor-pose-configured.json"},
        {"pose_setup", prefix + "pose-setup.json"},
        {"pose_status_monitoring", prefix + "pose-status-monitoring.json"},
        {"pose_status_high_speed", prefix + "pose-status-high-speed.json"},
    };
  };
  return Json{
      {"schema_version", 1},
      {"report_type", "android_dual_phone_paired_pose_arm_hil"},
      {"passed", true},
      {"camera_jobs_concurrent", true},
      {"single_feather_swing_count", 1},
      {"shared_session_id", kSharedSessionId},
      {"pose_transition",
       {{"passed", true},
        {"leader_role", "down_the_line"},
        {"shadow_role", "face_on"},
        {"peer_dispatch", "production_pose_peer_arm_client"},
        {"high_speed_profile", "720p240"},
        {"endpoint_hil_launch_gated", true},
        {"persisted_peer_arm",
         {{"leader", {{"state", "accepted"}, {"shared_session_id", kSharedSessionId}}},
          {"shadow", {{"state", "inbound_accepted"}, {"shared_session_id", kSharedSessionId}}}}}}},
      {"configuration_restore",
       {{"passed", true},
        {"scope", "complete_private_node_configuration_generation"},
        {"temporary_peer_configuration_removed", true},
        {"secret_material_preserved_in_artifacts", false}}},
      {"artifacts",
       {{"down_the_line", artifacts("down_the_line")}, {"face_on", artifacts("face_on")}}},
      {"nodes", Json::array({node("down_the_line"), node("face_on")})},
  };
}

void ExactDescriptorAndArmedStatusPass() {
  const NodeApiIdentity identity = Identity();
  assert(identity.node_id == "node-dtl");
  ValidateArmedCaptureStatus(ArmedStatusInspection{
      .status_json = Json({
                              {"schema_version", 2},
                              {"state", "armed"},
                              {"armed", true},
                              {"shared_session_id", "shared-1"},
                              {"error", ""},
                          })
                         .dump(),
      .shared_session_id = "shared-1",
  });
}

void PoseConfiguredDescriptorIsStageExact() {
  const NodeApiIdentity identity = Identity();
  Json descriptor = {
      {"schema_version", 1},
      {"node_id", "node-dtl"},
      {"role", "down_the_line"},
      {"capture_profile", "720p240"},
      {"pose",
       {{"mode", "leader"},
        {"delegate", "gpu_preferred"},
        {"debug_evidence_enabled", true},
        {"peer_configured", true}}},
  };
  const auto inspection = [&] {
    ValidatePoseConfiguredNodeDescriptor(PoseConfiguredDescriptorInspection{
        .descriptor_json = descriptor.dump(),
        .identity = identity,
        .expected_mode = "leader",
        .expected_peer_configured = true,
    });
  };
  inspection();
  descriptor["pose"]["mode"] = "shadow";
  ExpectFailure(inspection);
}

void PairedPoseReportRequiresStageExactEvidence() {
  Json report = PairedPoseReportFixture();
  ValidatePairedPoseHilReport(report.dump());

  Json stale_descriptor = report;
  stale_descriptor["artifacts"]["down_the_line"].erase("initial_node_descriptor");
  stale_descriptor["artifacts"]["down_the_line"]["node_descriptor"] =
      "down_the_line/node-descriptor.json";
  ExpectFailure([&] { ValidatePairedPoseHilReport(stale_descriptor.dump()); });

  Json missing_monitoring = report;
  missing_monitoring["artifacts"]["face_on"].erase("pose_status_monitoring");
  ExpectFailure([&] { ValidatePairedPoseHilReport(missing_monitoring.dump()); });

  Json mismatched_session = report;
  mismatched_session["pose_transition"]["persisted_peer_arm"]["shadow"]["shared_session_id"] =
      "other-session";
  ExpectFailure([&] { ValidatePairedPoseHilReport(mismatched_session.dump()); });
}

void IdentityAndSessionMismatchFail() {
  Json descriptor = {
      {"schema_version", 1},
      {"node_id", "node-dtl"},
      {"role", "face_on"},
      {"capture_profile", "720p240"},
      {"control_authentication", "bearer"},
  };
  ExpectFailure([&] {
    static_cast<void>(ValidateNodeDescriptor(descriptor.dump(),
                                             coordination::CaptureRole::kDownTheLine, "720p240"));
  });
  const Json status = {
      {"schema_version", 2},          {"state", "armed"}, {"armed", true},
      {"shared_session_id", "other"}, {"error", ""},
  };
  ExpectFailure([&] {
    ValidateArmedCaptureStatus(
        ArmedStatusInspection{.status_json = status.dump(), .shared_session_id = "shared-1"});
  });
}

void ExactTriggerReportPassesAndCorruptionFails() {
  const NodeApiIdentity identity = Identity();
  Json report = {
      {"schema_version", 1},
      {"role", "down_the_line"},
      {"node_id", "node-dtl"},
      {"shared_session_id", "shared-1"},
      {"local_session_id", "local-dtl"},
      {"trigger_elapsed_realtime_ns", "123456789"},
      {"timestamp_uncertainty_ns", 400000},
      {"source", "local_audio"},
  };
  const auto trigger = ValidateTriggerReport(report.dump(), identity, "shared-1", "local-dtl");
  assert(trigger.trigger_timestamp_ns == 123456789L);
  report["local_session_id"] = "foreign";
  ExpectFailure([&] {
    static_cast<void>(ValidateTriggerReport(report.dump(), identity, "shared-1", "local-dtl"));
  });
  report["local_session_id"] = "local-dtl";
  report["source"] = "manual";
  ExpectFailure([&] {
    static_cast<void>(ValidateTriggerReport(report.dump(), identity, "shared-1", "local-dtl"));
  });
}

void CanonicalReplayIsByteExactExceptForFramingNewline() {
  constexpr std::string_view kCanonical = R"({"schema_version":1,"status":"paired"})";
  ValidateCanonicalCoordinationReplay(std::string(kCanonical) + "\n", kCanonical);
  ExpectFailure([&] {
    ValidateCanonicalCoordinationReplay(R"({"status":"paired","schema_version":1})", kCanonical);
  });
  ExpectFailure(
      [&] { ValidateCanonicalCoordinationReplay(std::string(kCanonical) + "\n\n", kCanonical); });
}

}  // namespace

int main() {
  ExactDescriptorAndArmedStatusPass();
  PoseConfiguredDescriptorIsStageExact();
  PairedPoseReportRequiresStageExactEvidence();
  IdentityAndSessionMismatchFail();
  ExactTriggerReportPassesAndCorruptionFails();
  CanonicalReplayIsByteExactExceptForFramingNewline();
  return 0;
}
