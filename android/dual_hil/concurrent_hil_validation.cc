#include "android/dual_hil/concurrent_hil_validation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

constexpr std::int64_t kMaximumPeerClockUncertaintyNs = 25'000'000L;

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

std::int64_t SignedInteger(const Json &object, std::string_view field) {
  const auto value = object.find(field);
  if (value == object.end() || !value->is_number_integer()) {
    Invalid(std::string(field) + " must be an integer");
  }
  return value->get<std::int64_t>();
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

void ValidatePairedCaptureArtifactPaths(
    const Json &artifacts,  // NOLINT(bugprone-easily-swappable-parameters)
    const Json &node) {
  const std::string role = node.value("role", "");
  if (role != "face_on" && role != "down_the_line") {
    Invalid("paired pose HIL capture artifact has an unknown role");
  }
  const std::string prefix = role + "/";
  const std::string audio_filename =
      role == "face_on" ? "audio_evidence.wav" : "diagnostic_audio.wav";
  const Json &capture = artifacts.at(role).at("capture");
  const Json &diagnostics = capture.at("diagnostic_frames");
  const Json &node_diagnostics = node.at("diagnostic_frames");
  if (capture.value("capture_report", "") != prefix + "report.json" ||
      capture.value("manifest", "") != prefix + "manifest.json" ||
      capture.value("media", "") != prefix + role + ".mp4" ||
      capture.value("audio", "") != prefix + audio_filename ||
      capture.value("ffprobe", "") != prefix + "ffprobe.json" ||
      diagnostics.value("pre", "") != prefix + "diagnostic-01.png" ||
      diagnostics.value("marker", "") != prefix + "diagnostic-02.png" ||
      diagnostics.value("post", "") != prefix + "diagnostic-03.png" ||
      node.value("report", "") != capture.value("capture_report", "") ||
      node.value("manifest", "") != capture.value("manifest", "") ||
      node.value("media", "") != capture.value("media", "") ||
      node.value("audio_evidence", "") != capture.value("audio", "") ||
      node.at("audio").value("retained_wav", "") != capture.value("audio", "") ||
      node.value("ffprobe", "") != capture.value("ffprobe", "") ||
      node_diagnostics.value("pre", "") != diagnostics.value("pre", "") ||
      node_diagnostics.value("marker", "") != diagnostics.value("marker", "") ||
      node_diagnostics.value("post", "") != diagnostics.value("post", "")) {
    Invalid("paired pose HIL retained capture artifact paths are incomplete or inconsistent");
  }
}

void ValidateStartupTimingReportEvidence(const Json &node) {
  const Json &startup = node.at("startup_timing");
  const Json &milestones = startup.at("milestones");
  const Json &durations = startup.at("durations");
  const std::int64_t arm =
      DecimalString(milestones.at("arm_requested_elapsed_realtime_ns"), "startup arm");
  const std::int64_t engine =
      DecimalString(milestones.at("engine_started_elapsed_realtime_ns"), "startup engine");
  const std::int64_t camera =
      DecimalString(milestones.at("first_camera_frame_elapsed_realtime_ns"), "startup camera");
  const std::int64_t encoded = DecimalString(
      milestones.at("first_usable_encoded_frame_elapsed_realtime_ns"), "startup encoder");
  const std::int64_t ready =
      DecimalString(milestones.at("full_pre_roll_ready_elapsed_realtime_ns"), "startup pre-roll");
  if (startup.value("schema_version", 0) != 1 || startup.value("clock", "") != "CLOCK_BOOTTIME" ||
      !startup.value("continuity_clean", false) ||
      DecimalString(startup.at("startup_continuity_reset_count"),
                    "startup continuity reset count") != 0 ||
      DecimalString(startup.at("maximum_startup_continuity_gap_ns"),
                    "startup maximum continuity gap") != 0 ||
      arm < 0 || engine < arm || camera < engine || encoded < camera || ready < encoded ||
      DecimalString(durations.at("arm_to_engine_start_ns"), "arm to engine") != engine - arm ||
      DecimalString(durations.at("engine_start_to_first_camera_frame_ns"), "engine to camera") !=
          camera - engine ||
      DecimalString(durations.at("arm_to_first_camera_frame_ns"), "arm to camera") !=
          camera - arm ||
      DecimalString(durations.at("first_camera_frame_to_first_usable_encoded_frame_ns"),
                    "camera to encoder") != encoded - camera ||
      DecimalString(durations.at("arm_to_first_usable_encoded_frame_ns"), "arm to encoder") !=
          encoded - arm ||
      DecimalString(durations.at("first_usable_encoded_frame_to_full_pre_roll_ready_ns"),
                    "encoder to pre-roll") != ready - encoded ||
      DecimalString(durations.at("arm_to_full_pre_roll_ready_ns"), "arm to pre-roll") !=
          ready - arm) {
    Invalid("paired pose HIL node startup timing is incomplete or inconsistent");
  }
}

void ValidateLanPairedPoseReportEvidence(const Json &report) {
  const Json &lan = report.at("lan_endpoint_validation");
  const Json &artifacts = report.at("artifacts");
  for (const std::string_view role : {"down_the_line", "face_on"}) {
    const Json &endpoint = lan.at(role);
    if (!endpoint.value("host_direct_request", false) ||
        endpoint.value("unauthenticated_setup_status", 0) != 401 ||
        endpoint.value("authenticated_setup_status", 0) != 200 ||
        endpoint.value("descriptor_schema_version", 0) != 1 ||
        endpoint.value("status_schema_version", 0) != 2 ||
        endpoint.value("clock_schema_version", 0) != 1 ||
        endpoint.value("control_authentication", "") != "bearer" ||
        endpoint.value("secret_material_preserved", true)) {
      Invalid("LAN paired pose report lacks direct endpoint readiness evidence");
    }
    RequireArtifactPath(
        artifacts,
        {.role = role, .field = "lan_node_descriptor", .filename = "lan-node-descriptor.json"});
    RequireArtifactPath(artifacts, {.role = role,
                                    .field = "lan_setup_authenticated",
                                    .filename = "lan-setup-authenticated.json"});
    RequireArtifactPath(artifacts, {.role = role,
                                    .field = "lan_setup_unauthenticated",
                                    .filename = "lan-setup-unauthenticated.json"});
    RequireArtifactPath(
        artifacts,
        {.role = role, .field = "lan_capture_status", .filename = "lan-capture-status.json"});
    RequireArtifactPath(artifacts,
                        {.role = role, .field = "lan_clock", .filename = "lan-clock.json"});
  }
  const Json &leader_endpoint = lan.at("face_on");
  const Json &shadow_endpoint = lan.at("down_the_line");
  if (leader_endpoint.value("role", "") != "face_on" ||
      leader_endpoint.value("pose_mode", "") != "leader" ||
      shadow_endpoint.value("role", "") != "down_the_line" ||
      shadow_endpoint.value("pose_mode", "") != "shadow" ||
      leader_endpoint.value("peer_origin", "") != shadow_endpoint.value("origin", "") ||
      !shadow_endpoint.at("peer_origin").is_null() ||
      leader_endpoint.value("origin", "") == shadow_endpoint.value("origin", "")) {
    Invalid("LAN paired pose report does not prove the exact leader-to-shadow topology");
  }
}

void ValidateSetupPreviewReportEvidence(const Json &report) {
  const Json &previews = report.at("setup_preview_validation");
  const Json &artifacts = report.at("artifacts");
  for (const std::string_view role : {"down_the_line", "face_on"}) {
    const Json &monitoring = previews.at(role).at("monitoring");
    const Json &high_speed = previews.at(role).at("high_speed");
    const int rotation = monitoring.value("image_rotation_degrees", -1);
    if (!monitoring.value("passed", false) ||
        monitoring.value("unauthenticated_status", 0) != 401 ||
        monitoring.value("authenticated_status", 0) != 200 ||
        monitoring.value("content_type", "") != "image/jpeg" ||
        monitoring.value("cache_control", "") != "no-store, max-age=0" ||
        monitoring.value("byte_count", 0U) < 4U || monitoring.value("state", "") != "available" ||
        monitoring.value("reason", "") != "none" || monitoring.value("generation", 0LL) < 1 ||
        monitoring.value("frame_age_ms", -1LL) < 0 ||
        monitoring.value("frame_age_ms", -1LL) > 3'000 ||
        (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) ||
        !high_speed.value("passed", false) || high_speed.value("authenticated_status", 0) != 503 ||
        high_speed.value("cache_control", "") != "no-store, max-age=0" ||
        high_speed.value("state", "") != "unavailable" ||
        high_speed.value("reason", "") != "high_speed_capture") {
      Invalid("paired pose HIL report lacks the live-to-high-speed setup preview contract");
    }
    RequireArtifactPath(artifacts,
                        {.role = role, .field = "setup_preview", .filename = "setup-preview.jpg"});
    RequireArtifactPath(
        artifacts,
        {.role = role, .field = "setup_preview_metadata", .filename = "setup-preview.json"});
    RequireArtifactPath(artifacts, {.role = role,
                                    .field = "setup_preview_high_speed",
                                    .filename = "setup-preview-high-speed.json"});
  }
}

void ValidateStandbyInferenceReportEvidence(const Json &report) {
  const Json &inference = report.at("standby_inference_validation");
  for (const std::string_view role : {"down_the_line", "face_on"}) {
    const Json &node = inference.at(role);
    if (!node.value("passed", false) || node.value("actual_delegate", "") != "gpu" ||
        node.value("successful_inferences", 0ULL) < 2ULL ||
        node.value("inference_duration_p95_ns", 200'000'001ULL) > 200'000'000ULL ||
        node.value("maximum_inference_duration_ns", 400'000'001ULL) > 400'000'000ULL ||
        node.value("inference_outliers", 1ULL) != 0ULL ||
        node.value("rejected_decision_timestamps", 1ULL) != 0ULL ||
        node.value("decision_age_samples", 0ULL) != node.value("successful_inferences", 1ULL) ||
        node.value("scheduled_images", 1ULL) > node.value("offered_images", 0ULL)) {
      Invalid("paired pose HIL report lacks bounded short GPU inference evidence");
    }
  }
}

void ValidateUncalibratedTimingClaim(const Json &report) {
  const Json expected_components =
      Json::array({"camera_exposure_timestamp_semantics", "microphone_input_path_latency",
                   "speaker_amplifier_and_fixture_acoustic_latency",
                   "field_ball_to_microphone_acoustic_latency"});
  const Json &claim = report.at("timing_claim");
  if (!claim.is_object() || claim.size() != 5U ||
      claim.value("scope", "") != "fixture_optical_marker_to_audio_trigger" ||
      claim.value("qualification", "") != "operational_correlation_only" ||
      claim.value("review_marker_semantics", "") != "audio_trigger_estimate" ||
      claim.value("absolute_ball_impact_calibrated", true) ||
      claim.at("unmeasured_latency_components") != expected_components) {
    Invalid("paired pose HIL report overstates or omits its uncalibrated timing claim");
  }
}

void ValidatePcmTimingCorrelation(const Json &node) {
  constexpr std::int64_t kAcceptanceLimitUs = 25'000L;
  constexpr std::int64_t kMaximumInputUs = 1'000'000L;
  const Json &decoded = node.at("decoded_video");
  const Json &optical = node.at("optical");
  const Json &timing = optical.at("timing_correlation");
  const std::int64_t acceptance_limit = SignedInteger(timing, "acceptance_limit_us");
  const std::int64_t lower = SignedInteger(timing, "optical_onset_lower_bound_us");
  const std::int64_t upper = SignedInteger(timing, "optical_onset_upper_bound_us");
  const std::int64_t trigger_uncertainty_ns =
      SignedInteger(node, "trigger_timestamp_uncertainty_ns");
  const std::int64_t audio_uncertainty_us = SignedInteger(timing, "audio_trigger_uncertainty_us");
  const std::int64_t media_residual_us = SignedInteger(timing, "media_pts_residual_us");
  const std::int64_t decoded_media_residual_us =
      SignedInteger(decoded, "maximum_media_time_residual_us");
  const std::int64_t point_residual_us = SignedInteger(optical, "optical_to_audio_offset_us");
  if (acceptance_limit != kAcceptanceLimitUs || lower < -kMaximumInputUs ||
      upper > kMaximumInputUs || lower > upper || trigger_uncertainty_ns < 0 ||
      trigger_uncertainty_ns > kMaximumInputUs * 1000L || media_residual_us < 0 ||
      media_residual_us > kMaximumInputUs || point_residual_us < lower ||
      point_residual_us > upper || media_residual_us != decoded_media_residual_us) {
    Invalid("paired pose HIL optical/audio timing inputs are invalid or inconsistent");
  }
  const std::int64_t expected_audio_uncertainty_us = (trigger_uncertainty_ns + 999L) / 1000L;
  const std::int64_t expected_accounted_uncertainty_us =
      expected_audio_uncertainty_us + media_residual_us;
  const std::int64_t expected_minimum_residual_us = lower - expected_accounted_uncertainty_us;
  const std::int64_t expected_maximum_residual_us = upper + expected_accounted_uncertainty_us;
  const std::int64_t expected_total_bound_us =
      std::max(std::abs(expected_minimum_residual_us), std::abs(expected_maximum_residual_us));
  const bool expected_passed = expected_minimum_residual_us >= -acceptance_limit &&
                               expected_maximum_residual_us <= acceptance_limit;
  if (!decoded.value("passed", false) || !decoded.value("exact_frame_count", false) ||
      !optical.value("passed", false) || !timing.value("passed", false) || !expected_passed ||
      timing.value("claim_scope", "") != "fixture_optical_marker_to_audio_trigger" ||
      timing.value("absolute_ball_impact_calibrated", true) ||
      SignedInteger(timing, "optical_interval_width_us") != upper - lower ||
      audio_uncertainty_us != expected_audio_uncertainty_us ||
      SignedInteger(timing, "accounted_uncertainty_us") != expected_accounted_uncertainty_us ||
      SignedInteger(timing, "minimum_residual_us") != expected_minimum_residual_us ||
      SignedInteger(timing, "maximum_residual_us") != expected_maximum_residual_us ||
      SignedInteger(timing, "total_bound_us") != expected_total_bound_us) {
    Invalid("paired pose HIL timing result is missing or not derived from retained evidence");
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
                                                  std::string_view local_session_id,
                                                  std::string_view expected_source) {
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
        trigger.trigger_uncertainty_ns < 0 || trigger.source != expected_source) {
      Invalid("Android trigger report identity or source evidence is invalid");
    }
    return trigger;
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse Android trigger report: ") + failure.what());
  }
}

void ValidatePairedPoseClockStatus(const PairedPoseClockStatusInspection &inspection) {
  try {
    const Json leader = Json::parse(inspection.leader_status_json);
    const Json shadow = Json::parse(inspection.shadow_status_json);
    const Json &leader_pose = leader.at("pose");
    const Json &shadow_pose = shadow.at("pose");
    const Json &clock = leader_pose.at("peer_clock");
    const std::int64_t age = DecimalString(clock.at("age_ns"), "peer_clock.age_ns");
    const std::int64_t uncertainty =
        DecimalString(clock.at("uncertainty_ns"), "peer_clock.uncertainty_ns");
    const std::int64_t minimum_round_trip =
        DecimalString(clock.at("minimum_round_trip_ns"), "peer_clock.minimum_round_trip_ns");
    const std::int64_t maximum_round_trip =
        DecimalString(clock.at("maximum_round_trip_ns"), "peer_clock.maximum_round_trip_ns");
    if (leader_pose.value("mode", "") != "leader" || !leader_pose.value("peer_configured", false) ||
        shadow_pose.value("mode", "") != "shadow" || shadow_pose.value("peer_configured", true) ||
        !shadow_pose.at("peer_clock").is_null() ||
        clock.value("peer_node_id", "") != inspection.shadow_identity.node_id || age < 0 ||
        age > 10'000'000'000L || uncertainty < 0 || uncertainty > kMaximumPeerClockUncertaintyNs ||
        minimum_round_trip < 0 || maximum_round_trip < minimum_round_trip ||
        clock.value("sample_count", 0) < 3) {
      Invalid("paired pose leader does not expose a usable shadow-clock estimate");
    }
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse paired pose clock status: ") +
                             failure.what());
  }
}

void ValidateMappedPeerImpactStatus(const MappedPeerImpactStatusInspection &inspection) {
  try {
    const Json status = Json::parse(inspection.shadow_status_json);
    const Json &mapping = status.at("pose").at("peer_impact_mapping");
    const std::int64_t mapped_timestamp =
        DecimalString(mapping.at("mapped_peer_trigger_elapsed_realtime_ns"),
                      "peer_impact_mapping.mapped_peer_trigger_elapsed_realtime_ns");
    const std::int64_t uncertainty = DecimalString(mapping.at("mapping_uncertainty_ns"),
                                                   "peer_impact_mapping.mapping_uncertainty_ns");
    const std::int64_t age = DecimalString(mapping.at("mapping_age_at_send_ns"),
                                           "peer_impact_mapping.mapping_age_at_send_ns");
    const std::int64_t minimum_round_trip = DecimalString(
        mapping.at("minimum_round_trip_ns"), "peer_impact_mapping.minimum_round_trip_ns");
    const std::int64_t maximum_round_trip = DecimalString(
        mapping.at("maximum_round_trip_ns"), "peer_impact_mapping.maximum_round_trip_ns");
    if (mapping.value("schema_version", 0) != 2 ||
        mapping.value("leader_node_id", "") != inspection.leader_identity.node_id ||
        mapping.value("target_peer_node_id", "") != inspection.shadow_identity.node_id ||
        inspection.expected_shared_session_id.empty() ||
        mapping.value("shared_session_id", "") != inspection.expected_shared_session_id ||
        !mapping.value("mapping_within_policy", false) || mapped_timestamp <= 0 || age < 0 ||
        age > 10'000'000'000L || uncertainty < 0 || uncertainty > kMaximumPeerClockUncertaintyNs ||
        minimum_round_trip < 0 || maximum_round_trip < minimum_round_trip ||
        mapping.value("sample_count", 0) < 3 ||
        mapping.value("selected_source", "") != "peer_audio_clock_candidate" ||
        mapping.value("fallback_semantics", "") != "mapped_candidate_else_arrival") {
      Invalid("shadow did not retain its accepted schema-2 mapped-impact evidence");
    }
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse mapped peer-impact status: ") +
                             failure.what());
  }
}

bool HasPairedAutomaticImpactEvidence(std::string_view leader_status_json,
                                      std::string_view shadow_status_json) {
  try {
    const Json leader = Json::parse(leader_status_json);
    const Json shadow = Json::parse(shadow_status_json);
    return !leader.at("last_trigger_elapsed_realtime_ns").is_null() &&
           !shadow.at("last_trigger_elapsed_realtime_ns").is_null() &&
           shadow.at("pose").at("peer_impact_mapping").is_object();
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse paired automatic-impact status: ") +
                             failure.what());
  }
}

void ValidatePairedPoseSessionState(const PairedPoseSessionStateInspection &inspection) {
  try {
    if (inspection.expected_phase != "monitoring" && inspection.expected_phase != "high_speed") {
      Invalid("paired pose session-state inspection has an unsupported phase");
    }
    const Json leader = Json::parse(inspection.leader_status_json);
    const Json shadow = Json::parse(inspection.shadow_status_json);
    const Json &leader_pose = leader.at("pose");
    const Json &shadow_pose = shadow.at("pose");
    const Json &leader_peer_arm = leader_pose.at("peer_arm");
    const Json &shadow_peer_arm = shadow_pose.at("peer_arm");
    const auto common_ready = [&](const Json &status, const Json &pose,
                                  std::string_view expected_mode, bool peer_configured) {
      return status.value("schema_version", 0) == 2 && status.value("state", "") == "armed" &&
             status.value("armed", false) && status.value("error", "not empty").empty() &&
             status.at("last_trigger_elapsed_realtime_ns").is_null() &&
             pose.value("phase", "") == inspection.expected_phase &&
             pose.value("mode", "") == expected_mode &&
             pose.value("peer_configured", false) == peer_configured;
    };
    if (!common_ready(leader, leader_pose, "leader", true) ||
        !common_ready(shadow, shadow_pose, "shadow", false)) {
      Invalid("paired pose nodes violate the common pre-trigger state model");
    }
    if (inspection.expected_phase == "monitoring") {
      if (!inspection.expected_shared_session_id.empty() ||
          !leader.at("shared_session_id").is_null() || !shadow.at("shared_session_id").is_null() ||
          leader_pose.value("transition_requested", true) ||
          shadow_pose.value("transition_requested", true) ||
          leader_peer_arm.value("state", "") != "not_requested" ||
          shadow_peer_arm.value("state", "") != "not_requested" ||
          !leader_peer_arm.at("shared_session_id").is_null() ||
          !shadow_peer_arm.at("shared_session_id").is_null() ||
          !leader_pose.at("standby_audio").value("ready", false) ||
          !shadow_pose.at("standby_audio").value("ready", false)) {
        Invalid("paired pose monitoring state is not an unclaimed standby session");
      }
      return;
    }
    if (inspection.expected_shared_session_id.empty() ||
        leader.value("shared_session_id", "") != inspection.expected_shared_session_id ||
        shadow.value("shared_session_id", "") != inspection.expected_shared_session_id ||
        !leader_pose.value("transition_requested", false) ||
        !shadow_pose.value("transition_requested", false) ||
        leader_peer_arm.value("state", "") != "accepted" ||
        shadow_peer_arm.value("state", "") != "inbound_accepted" ||
        leader_peer_arm.value("shared_session_id", "") != inspection.expected_shared_session_id ||
        shadow_peer_arm.value("shared_session_id", "") != inspection.expected_shared_session_id ||
        leader_peer_arm.value("http_status", 0) != 202 ||
        !shadow_peer_arm.at("http_status").is_null() ||
        !leader_pose.at("high_speed_audio").value("ready", false) ||
        !shadow_pose.at("high_speed_audio").value("ready", false) ||
        leader.value("ring_duration_us", 0L) < 1'300'000L ||
        shadow.value("ring_duration_us", 0L) < 1'300'000L) {
      Invalid("paired pose high-speed state does not share one accepted arm session");
    }
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse paired pose session state: ") +
                             failure.what());
  }
}

void ValidateLanEndpoint(const LanEndpointInspection &inspection) {
  try {
    const Json descriptor = Json::parse(inspection.descriptor_json);
    const Json setup = Json::parse(inspection.setup_json);
    const Json status = Json::parse(inspection.status_json);
    const Json clock = Json::parse(inspection.clock_json);
    const auto contains_origin = [&](const Json &urls) {
      return urls.is_array() && std::ranges::any_of(urls, [&](const Json &url) {
               return url.is_string() && url.get<std::string>() == inspection.expected_origin;
             });
    };
    const Json &configuration = setup.at("configuration");
    const Json &pose = configuration.at("pose");
    const Json &peer = pose.at("peer");
    const bool peer_matches =
        inspection.expected_peer_origin.empty()
            ? peer.is_null()
            : peer.is_object() && peer.value("origin", "") == inspection.expected_peer_origin;
    if (inspection.expected_origin.empty() || inspection.expected_device_model.empty() ||
        inspection.expected_pose_mode.empty() || inspection.unauthenticated_setup_status != 401 ||
        inspection.authenticated_setup_status != 200 ||
        descriptor.value("schema_version", 0) != 1 ||
        descriptor.value("node_id", "") != inspection.identity.node_id ||
        ParseRole(descriptor.value("role", "")) != inspection.identity.role ||
        descriptor.value("capture_profile", "") != inspection.identity.capture_profile ||
        descriptor.value("control_authentication", "") != "bearer" ||
        !contains_origin(descriptor.at("service_urls")) || setup.value("schema_version", 0) != 1 ||
        setup.at("node").value("node_id", "") != inspection.identity.node_id ||
        setup.at("node").value("device_model", "") != inspection.expected_device_model ||
        !contains_origin(setup.at("node").at("service_urls")) ||
        ParseRole(configuration.value("role", "")) != inspection.identity.role ||
        configuration.value("capture_profile", "") != inspection.identity.capture_profile ||
        pose.value("mode", "") != inspection.expected_pose_mode || !peer_matches ||
        !setup.at("readiness").value("editable", false) ||
        !setup.at("readiness").at("issues").is_array() ||
        !setup.at("readiness").at("issues").empty() || status.value("schema_version", 0) != 2 ||
        clock.value("schema_version", 0) != 1 ||
        clock.value("node_id", "") != inspection.identity.node_id ||
        DecimalString(clock.at("request_received_elapsed_realtime_ns"),
                      "request_received_elapsed_realtime_ns") <= 0 ||
        DecimalString(clock.at("response_prepared_elapsed_realtime_ns"),
                      "response_prepared_elapsed_realtime_ns") <= 0 ||
        inspection.setup_json.contains("control_token")) {
      Invalid("LAN endpoint identity/version/network/authentication contract is invalid");
    }
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse LAN endpoint evidence: ") + failure.what());
  }
}

DiscoveryPairingFixtureInspection InspectDiscoveryPairingFixture(std::string_view setup_json) {
  try {
    const Json setup = Json::parse(setup_json);
    if (setup.value("schema_version", 0) != 1 || !setup.contains("revision") ||
        !setup.at("revision").is_number_integer() || setup.at("revision").get<std::int64_t>() < 0) {
      Invalid("discovery/pairing fixture setup metadata is invalid");
    }

    const Json &pose = setup.at("configuration").at("pose");
    const std::string pose_mode = pose.value("mode", "");
    if (pose_mode != "disabled" && pose_mode != "leader" && pose_mode != "shadow") {
      Invalid("discovery/pairing fixture pose mode is invalid");
    }
    const Json &peer = pose.at("peer");
    bool peer_configured = false;
    if (peer.is_object()) {
      peer_configured = !peer.value("origin", "").empty();
      if (!peer_configured) {
        Invalid("discovery/pairing fixture peer origin is empty");
      }
    } else if (!peer.is_null()) {
      Invalid("discovery/pairing fixture peer must be an object or null");
    }

    const Json &pairing = setup.at("pairing");
    if (pairing.is_null()) {
      return DiscoveryPairingFixtureInspection{
          .pose_mode = pose_mode,
          .peer_configured = peer_configured,
          .pairing_state = DurablePairingState::kAbsent,
          .peer_node_id = {},
      };
    }
    if (!pairing.is_object()) {
      Invalid("discovery/pairing fixture binding must be an object or null");
    }
    const std::string peer_node_id = pairing.value("peer_node_id", "");
    if (peer_node_id.empty()) {
      Invalid("discovery/pairing fixture binding lacks a peer node ID");
    }
    const std::string state = pairing.value("state", "");
    DurablePairingState pairing_state = DurablePairingState::kAbsent;
    if (state == "active") {
      pairing_state = DurablePairingState::kActive;
    } else if (state == "revoked") {
      pairing_state = DurablePairingState::kRevoked;
    } else {
      Invalid("discovery/pairing fixture binding has an unknown state");
    }
    return DiscoveryPairingFixtureInspection{
        .pose_mode = pose_mode,
        .peer_configured = peer_configured,
        .pairing_state = pairing_state,
        .peer_node_id = peer_node_id,
    };
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse discovery/pairing fixture setup: ") +
                             failure.what());
  }
}

ShortPoseLatencyEvidence ValidateShortPoseLatency(std::string_view status_json) {
  try {
    const Json status = Json::parse(status_json);
    const Json &metrics = status.at("pose").at("metrics");
    const auto counter = [&](std::string_view name) {
      const Json &value = metrics.at(name);
      if (!value.is_number_unsigned() && !value.is_number_integer()) {
        Invalid(std::string("pose latency counter is not an integer: ") + std::string(name));
      }
      const std::int64_t signed_value = value.get<std::int64_t>();
      if (signed_value < 0) {
        Invalid(std::string("pose latency counter is negative: ") + std::string(name));
      }
      return static_cast<std::uint64_t>(signed_value);
    };
    ShortPoseLatencyEvidence evidence{
        .actual_delegate = metrics.value("delegate", ""),
        .successful_inferences = counter("successful_inferences"),
        .inference_duration_p95_ns = counter("inference_duration_p95_ns"),
        .maximum_inference_duration_ns = counter("maximum_inference_duration_ns"),
        .inference_deadline_misses = counter("inference_deadline_misses"),
        .inference_outliers = counter("inference_outliers"),
        .decision_age_samples = counter("decision_age_samples"),
        .rejected_decision_timestamps = counter("rejected_decision_timestamps"),
        .decision_age_p95_ns = counter("decision_age_p95_ns"),
        .maximum_decision_age_ns = counter("maximum_decision_age_ns"),
        .offered_images = counter("offered_images"),
        .scheduled_images = counter("scheduled_images"),
        .dropped_images = counter("dropped_images"),
        .maximum_warmup_duration_ns = counter("maximum_warmup_duration_ns"),
    };
    if (metrics.value("successful_warmup_inferences", 0LL) != 1 ||
        metrics.value("failed_warmup_inferences", -1LL) != 0 || evidence.actual_delegate != "gpu" ||
        evidence.successful_inferences < 2 || metrics.value("failed_inferences", -1LL) != 0 ||
        evidence.inference_duration_p95_ns > 200'000'000ULL ||
        evidence.maximum_inference_duration_ns > 400'000'000ULL ||
        evidence.inference_outliers != 0 || evidence.rejected_decision_timestamps != 0 ||
        evidence.decision_age_samples != evidence.successful_inferences ||
        evidence.scheduled_images > evidence.offered_images) {
      Invalid("short paired pose inference violates its GPU latency/accounting bound");
    }
    return evidence;
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse short pose latency status: ") +
                             failure.what());
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
    const std::string report_type = report.value("report_type", "");
    const bool lan_only = report_type == "android_dual_phone_paired_pose_arm_lan_hil";
    const Json &transition = report.at("pose_transition");
    const Json &peer_arm = transition.at("persisted_peer_arm");
    const Json &restore = report.at("configuration_restore");
    if (report.value("schema_version", 0) != 1 ||
        (report_type != "android_dual_phone_paired_pose_arm_hil" && !lan_only) ||
        !report.value("passed", false) || !report.value("camera_jobs_concurrent", false) ||
        report.value("single_pcm_replay_count", 0) != 1 || shared_session_id.empty() ||
        !transition.value("passed", false) || transition.value("leader_role", "") != "face_on" ||
        transition.value("shadow_role", "") != "down_the_line" ||
        transition.value("peer_dispatch", "") != "production_pose_peer_arm_client" ||
        transition.value("peer_transport", "") !=
            (lan_only ? "wifi_lan_direct" : "adb_reverse_to_shadow_http_api") ||
        transition.value("adb_reverse_used", lan_only) != !lan_only ||
        transition.value("host_control_transport", "") != "adb_forward" ||
        transition.value("high_speed_profile", "") != "720p240" ||
        transition.value("leader_trigger_source", "") != "local_audio" ||
        transition.value("shadow_trigger_source", "") != "peer_audio_clock_candidate" ||
        transition.value("peer_impact_schema_version", 0) != 2 ||
        !transition.value("target_peer_node_id_verified", false) ||
        transition.value("mapped_fallback_semantics", "") != "mapped_candidate_else_arrival" ||
        transition.at("mapped_impact_evidence").value("shared_session_id", "") !=
            shared_session_id ||
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
    ValidateUncalibratedTimingClaim(report);
    ValidateStandbyInferenceReportEvidence(report);
    ValidateSetupPreviewReportEvidence(report);
    if (lan_only) {
      ValidateLanPairedPoseReportEvidence(report);
    } else if (!report.at("lan_endpoint_validation").is_null()) {
      Invalid("ADB-reverse paired pose report unexpectedly claims LAN endpoint evidence");
    }
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
                                      .filename = "pose-status-high_speed.json"});
      if (role == "down_the_line") {
        RequireArtifactPath(artifacts, {.role = role,
                                        .field = "pose_status_triggered",
                                        .filename = "pose-status-triggered.json"});
      }
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
      ValidateStartupTimingReportEvidence(node);
      ValidatePcmTimingCorrelation(node);
      ValidatePairedCaptureArtifactPaths(artifacts, node);
    }
    if (roles != std::set<std::string, std::less<>>{"down_the_line", "face_on"}) {
      Invalid("paired pose HIL node roles are incomplete");
    }
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse paired pose HIL report: ") + failure.what());
  }
}

}  // namespace swing_capture::android::dual_hil
