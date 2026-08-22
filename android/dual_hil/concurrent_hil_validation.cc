#include "android/dual_hil/concurrent_hil_validation.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "android/dual_coordination_hil/dual_coordination.h"

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
namespace coordination = swing_capture::android::dual_coordination_hil;

[[noreturn]] void Invalid(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

std::int64_t DecimalString(const Json &value, std::string_view field) {
  if (!value.is_string()) {
    Invalid(std::string(field) + " must be a decimal string");
  }
  try {
    std::size_t consumed = 0;
    const std::string text = value.get<std::string>();
    const std::int64_t parsed = std::stoll(text, &consumed);
    if (consumed != text.size()) {
      Invalid(std::string(field) + " contains trailing characters");
    }
    return parsed;
  } catch (const std::exception &) {
    Invalid(std::string(field) + " is outside the signed 64-bit range");
  }
}

coordination::CaptureRole ParseRole(std::string_view role) {
  if (role == "down_the_line") {
    return coordination::CaptureRole::kDownTheLine;
  }
  if (role == "face_on") {
    return coordination::CaptureRole::kFaceOn;
  }
  Invalid("Android node returned an unknown role");
}

struct ArtifactPathContract {
  std::string_view role;
  std::string_view field;
  std::string_view filename;
};

void RequireArtifactPath(const Json &artifacts, const ArtifactPathContract &contract) {
  const std::string expected = std::string(contract.role) + "/" + std::string(contract.filename);
  if (artifacts.at(contract.role).value(contract.field, "") != expected) {
    Invalid("paired pose HIL report lacks a stage-accurate artifact path");
  }
}

}  // namespace

NodeApiIdentity ValidateNodeDescriptor(std::string_view descriptor_json,
                                       coordination::CaptureRole expected_role,
                                       std::string_view expected_profile) {
  try {
    const Json descriptor = Json::parse(descriptor_json);
    NodeApiIdentity identity{
        .node_id = descriptor.value("node_id", ""),
        .role = ParseRole(descriptor.value("role", "")),
        .capture_profile = descriptor.value("capture_profile", ""),
    };
    if (descriptor.value("schema_version", 0) != 1 || identity.node_id.empty() ||
        identity.role != expected_role || identity.capture_profile != expected_profile ||
        descriptor.value("control_authentication", "") != "bearer") {
      Invalid("Android node descriptor does not match its fixed HIL identity/profile");
    }
    return identity;
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse Android node descriptor: ") +
                             failure.what());
  }
}

void ValidateArmedCaptureStatus(const ArmedStatusInspection &inspection) {
  try {
    const Json status = Json::parse(inspection.status_json);
    const std::string error = status.value("error", "not empty");
    if (status.value("schema_version", 0) != 2 || status.value("state", "") != "armed" ||
        !status.value("armed", false) ||
        status.value("shared_session_id", "") != inspection.shared_session_id || !error.empty()) {
      Invalid("Android node did not reach ARMED for the requested shared session");
    }
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse Android capture status: ") + failure.what());
  }
}

void ValidatePoseConfiguredNodeDescriptor(const PoseConfiguredDescriptorInspection &inspection) {
  try {
    const Json descriptor = Json::parse(inspection.descriptor_json);
    const Json &pose = descriptor.at("pose");
    if (descriptor.value("schema_version", 0) != 1 ||
        descriptor.value("node_id", "") != inspection.identity.node_id ||
        ParseRole(descriptor.value("role", "")) != inspection.identity.role ||
        descriptor.value("capture_profile", "") != inspection.identity.capture_profile ||
        pose.value("mode", "") != inspection.expected_mode ||
        pose.value("delegate", "") != "gpu_preferred" ||
        !pose.value("debug_evidence_enabled", false) ||
        pose.value("peer_configured", false) != inspection.expected_peer_configured) {
      Invalid("Android node descriptor does not reflect paired pose configuration");
    }
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse configured Android node descriptor: ") +
                             failure.what());
  }
}

coordination::TriggerReport ValidateTriggerReport(std::string_view trigger_report_json,
                                                  const NodeApiIdentity &identity,
                                                  std::string_view shared_session_id,
                                                  std::string_view local_session_id) {
  try {
    const Json report = Json::parse(trigger_report_json);
    coordination::TriggerReport trigger{
        .role = ParseRole(report.value("role", "")),
        .node_id = report.value("node_id", ""),
        .shared_session_id = report.value("shared_session_id", ""),
        .local_session_id = report.value("local_session_id", ""),
        .trigger_timestamp_ns =
            DecimalString(report.at("trigger_elapsed_realtime_ns"), "trigger_elapsed_realtime_ns"),
        .trigger_uncertainty_ns = report.value("timestamp_uncertainty_ns", -1L),
        .source = report.value("source", ""),
    };
    if (report.value("schema_version", 0) != 1 || trigger.role != identity.role ||
        trigger.node_id != identity.node_id || trigger.shared_session_id != shared_session_id ||
        trigger.local_session_id != local_session_id || trigger.trigger_timestamp_ns <= 0 ||
        trigger.trigger_uncertainty_ns < 0 || trigger.source != "local_audio") {
      Invalid("Android trigger report identity or local-audio evidence is invalid");
    }
    return trigger;
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse Android trigger report: ") + failure.what());
  }
}

void ValidateCanonicalCoordinationReplay(std::string_view response_body,
                                         std::string_view canonical_json) {
  if (response_body.ends_with('\n')) {
    response_body.remove_suffix(1U);
  }
  if (response_body != canonical_json) {
    Invalid("Android node coordination replay differs from the canonical paired record");
  }
}

void ValidatePairedPoseHilReport(std::string_view report_json) {
  try {
    const Json report = Json::parse(report_json);
    const std::string shared_session_id = report.value("shared_session_id", "");
    const Json &transition = report.at("pose_transition");
    const Json &peer_arm = transition.at("persisted_peer_arm");
    const Json &restore = report.at("configuration_restore");
    if (report.value("schema_version", 0) != 1 ||
        report.value("report_type", "") != "android_dual_phone_paired_pose_arm_hil" ||
        !report.value("passed", false) || !report.value("camera_jobs_concurrent", false) ||
        report.value("single_feather_swing_count", 0) != 1 || shared_session_id.empty() ||
        !transition.value("passed", false) ||
        transition.value("leader_role", "") != "down_the_line" ||
        transition.value("shadow_role", "") != "face_on" ||
        transition.value("peer_dispatch", "") != "production_pose_peer_arm_client" ||
        transition.value("high_speed_profile", "") != "720p240" ||
        !transition.value("endpoint_hil_launch_gated", false) ||
        peer_arm.at("leader").value("state", "") != "accepted" ||
        peer_arm.at("leader").value("shared_session_id", "") != shared_session_id ||
        peer_arm.at("shadow").value("state", "") != "inbound_accepted" ||
        peer_arm.at("shadow").value("shared_session_id", "") != shared_session_id ||
        !restore.value("passed", false) ||
        restore.value("scope", "") != "complete_private_node_configuration_generation" ||
        !restore.value("temporary_peer_configuration_removed", false) ||
        restore.value("secret_material_preserved_in_artifacts", true)) {
      Invalid("paired pose HIL aggregate report contract is invalid");
    }

    const Json &artifacts = report.at("artifacts");
    for (const std::string_view role : {"down_the_line", "face_on"}) {
      RequireArtifactPath(artifacts, {.role = role,
                                      .field = "initial_node_descriptor",
                                      .filename = "node-descriptor-initial.json"});
      RequireArtifactPath(artifacts, {.role = role,
                                      .field = "pose_configured_node_descriptor",
                                      .filename = "node-descriptor-pose-configured.json"});
      RequireArtifactPath(artifacts,
                          {.role = role, .field = "pose_setup", .filename = "pose-setup.json"});
      RequireArtifactPath(artifacts, {.role = role,
                                      .field = "pose_status_monitoring",
                                      .filename = "pose-status-monitoring.json"});
      RequireArtifactPath(artifacts, {.role = role,
                                      .field = "pose_status_high_speed",
                                      .filename = "pose-status-high-speed.json"});
    }

    const Json &nodes = report.at("nodes");
    if (!nodes.is_array() || nodes.size() != 2U) {
      Invalid("paired pose HIL report must contain exactly two node results");
    }
    std::set<std::string, std::less<>> roles;
    for (const Json &node : nodes) {
      if (!node.value("passed", false) ||
          node.value("shared_session_id", "") != shared_session_id ||
          !roles.insert(node.value("role", "")).second) {
        Invalid("paired pose HIL node result is invalid or duplicated");
      }
    }
    if (roles != std::set<std::string, std::less<>>{"down_the_line", "face_on"}) {
      Invalid("paired pose HIL node roles are incomplete");
    }
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse paired pose HIL report: ") + failure.what());
  }
}

}  // namespace swing_capture::android::dual_hil
