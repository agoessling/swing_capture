#include "android/dual_hil/paired_pose_qualification_policy.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "android/pose_hil/pose_qualification_performance.h"

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr PairedPoseQualificationPolicy kFiveMinutePolicy{
    .mode = "paired-pose-qualify-5m",
    .report_type = "android_dual_phone_paired_pose_qualification_5m",
    .duration_seconds = 300,
    .telemetry_interval_seconds = 30,
    .cycle_interval_seconds = 60,
    .minimum_cycle_count = 5,
    .minimum_telemetry_sample_count = 10,
};

constexpr PairedPoseQualificationPolicy kThirtyMinutePolicy{
    .mode = "paired-pose-soak-30m",
    .report_type = "android_dual_phone_paired_pose_soak_30m",
    .duration_seconds = 1800,
    .telemetry_interval_seconds = 30,
    .cycle_interval_seconds = 60,
    .minimum_cycle_count = 30,
    .minimum_telemetry_sample_count = 60,
};

constexpr int kMaximumThermalStatus = 2;
constexpr double kMinimumInferenceCadenceHz = 4.0;
constexpr double kMaximumDroppedFraction = 0.20;
constexpr std::uint64_t kMaximumInferenceP95Ns = 200'000'000;
constexpr std::uint64_t kMaximumInferenceDurationNs = 400'000'000;
constexpr std::uint64_t kLatencyHistogramBucketWidthNs = 1'000'000;

[[noreturn]] void Invalid(const std::string &message) { throw std::invalid_argument(message); }

std::uint64_t NonnegativeInteger(const Json &value, std::string_view name) {
  if (!value.contains(name) || !value.at(name).is_number_integer()) {
    Invalid(std::string(name) + " must be an integer");
  }
  const std::int64_t parsed = value.at(name).get<std::int64_t>();
  if (parsed < 0) {
    Invalid(std::string(name) + " must be nonnegative");
  }
  return static_cast<std::uint64_t>(parsed);
}

void ValidateRoleTelemetry(const Json &role, bool initial_sample) {
  if (!role.is_object() || !role.value("screen_off", false) ||
      role.value("phase", "") != "monitoring" || role.value("state", "") != "armed" ||
      role.value("actual_delegate", "") != "gpu" || role.value("thermal_status", -1) < 0 ||
      role.value("thermal_status", -1) > kMaximumThermalStatus ||
      role.value("battery_level_percent", -1) < 0 ||
      role.value("battery_level_percent", -1) > 100) {
    Invalid("qualification telemetry did not retain nominal screen, pose, and thermal state");
  }
  const std::uint64_t offered = NonnegativeInteger(role, "offered_images_delta");
  const std::uint64_t successful = NonnegativeInteger(role, "successful_inferences_delta");
  const std::uint64_t failed = NonnegativeInteger(role, "failed_inferences_delta");
  const std::uint64_t dropped = NonnegativeInteger(role, "dropped_images_delta");
  const std::uint64_t deadline_misses = NonnegativeInteger(role, "inference_deadline_misses_delta");
  const std::uint64_t outliers = NonnegativeInteger(role, "inference_outliers_delta");
  const std::uint64_t decision_samples = NonnegativeInteger(role, "decision_age_samples_delta");
  const std::uint64_t rejected_decisions =
      NonnegativeInteger(role, "rejected_decision_timestamps_delta");
  const std::uint64_t p95 = NonnegativeInteger(role, "inference_duration_p95_ns");
  const std::uint64_t maximum = NonnegativeInteger(role, "maximum_inference_duration_ns");
  const std::uint64_t decision_p95 = NonnegativeInteger(role, "decision_age_p95_ns");
  const std::uint64_t decision_maximum = NonnegativeInteger(role, "maximum_decision_age_ns");
  const std::uint64_t audio_drops = NonnegativeInteger(role, "standby_audio_dropped_events");
  const std::uint64_t audio_timestamp_rejections =
      NonnegativeInteger(role, "standby_audio_timestamp_rejections");
  const std::uint64_t audio_discontinuities =
      NonnegativeInteger(role, "standby_audio_discontinuities");
  if (!role.contains("cadence_eligible") || !role.at("cadence_eligible").is_boolean() ||
      !role.contains("metrics_generation_reset") ||
      !role.at("metrics_generation_reset").is_boolean()) {
    Invalid("qualification telemetry is missing cadence-generation state");
  }
  const bool cadence_eligible = role.at("cadence_eligible").get<bool>();
  const bool metrics_generation_reset = role.at("metrics_generation_reset").get<bool>();
  const std::uint64_t decision_maximum_bucket_upper_bound =
      decision_maximum == 0 ? 0
                            : ((decision_maximum - 1) / kLatencyHistogramBucketWidthNs + 1) *
                                  kLatencyHistogramBucketWidthNs;
  if (failed != 0 || outliers != 0 || deadline_misses < outliers || dropped > offered ||
      p95 > kMaximumInferenceP95Ns || maximum > kMaximumInferenceDurationNs) {
    Invalid("qualification pose inference exceeded its failure, latency, or accounting bound");
  }
  if (rejected_decisions != 0 || decision_samples != successful ||
      decision_p95 > decision_maximum_bucket_upper_bound) {
    Invalid("qualification pose decision-age evidence is incomplete or clock-invalid");
  }
  if (audio_drops != 0 || audio_timestamp_rejections > 2 || audio_discontinuities != 0) {
    Invalid("qualification standby audio continuity counters are outside policy");
  }
  if (initial_sample) {
    if (offered != 0 || successful != 0 || failed != 0 || dropped != 0 || deadline_misses != 0 ||
        outliers != 0 || decision_samples != 0 || rejected_decisions != 0 || cadence_eligible ||
        metrics_generation_reset) {
      Invalid("initial qualification telemetry deltas must be zero");
    }
    return;
  }
  if (metrics_generation_reset == cadence_eligible) {
    Invalid("qualification metric generation resets must occur exactly after capture intervals");
  }
  const double cadence = role.value("successful_inferences_per_second", -1.0);
  const double dropped_fraction = role.value("dropped_fraction", -1.0);
  if (offered == 0 || successful == 0 || cadence < 0.0 || dropped_fraction < 0.0 ||
      dropped_fraction > kMaximumDroppedFraction ||
      (cadence_eligible && cadence < kMinimumInferenceCadenceHz)) {
    Invalid("qualification pose cadence or drop fraction is outside policy");
  }
}

void ValidateCycleArtifactSchema(const Json &cycle, const Json &artifacts, std::size_t index) {
  const std::string prefix = "cycles/cycle-" + std::to_string(index) + "/";
  if (cycle.value("artifact_path_scope", "") != "undeclared_output_root" ||
      !artifacts.is_object() || artifacts.value("cycle", "") != prefix + "cycle.json" ||
      artifacts.value("coordination", "") != prefix + "coordination.json" ||
      cycle.at("coordination").value("canonical_record", "") !=
          artifacts.value("coordination", "") ||
      !cycle.at("nodes").is_object() || !cycle.at("clock").is_object()) {
    Invalid("qualification cycle artifact scope or root paths are invalid");
  }
  for (const std::string_view role : {"face_on", "down_the_line"}) {
    const Json &role_artifacts = artifacts.at(role);
    const Json &node = cycle.at("nodes").at(role);
    const Json &decoded = node.at("decoded_video");
    const Json &diagnostic_frames = role_artifacts.at("diagnostic_frames");
    const std::string expected_audio =
        prefix + std::string(role) +
        (role == "face_on" ? "/audio_evidence.wav" : "/diagnostic_audio.wav");
    const std::string expected_node_prefix = prefix + std::string(role) + "/";
    const std::uint64_t frame_count = NonnegativeInteger(node, "frame_count");
    if (!role_artifacts.is_object() ||
        role_artifacts.value("clock", "") != expected_node_prefix + "clock.json" ||
        role_artifacts.value("report", "") != expected_node_prefix + "report.json" ||
        role_artifacts.value("manifest", "") != expected_node_prefix + "manifest.json" ||
        role_artifacts.value("media", "") != expected_node_prefix + std::string(role) + ".mp4" ||
        role_artifacts.value("audio", "") != expected_audio ||
        role_artifacts.value("ffprobe", "") != expected_node_prefix + "ffprobe.json" ||
        !diagnostic_frames.is_object() ||
        diagnostic_frames.value("pre", "") != expected_node_prefix + "diagnostic-01.png" ||
        diagnostic_frames.value("marker", "") != expected_node_prefix + "diagnostic-02.png" ||
        diagnostic_frames.value("post", "") != expected_node_prefix + "diagnostic-03.png" ||
        node.value("report", "") != role_artifacts.value("report", "") ||
        node.value("manifest", "") != role_artifacts.value("manifest", "") ||
        node.value("media", "") != role_artifacts.value("media", "") ||
        node.value("audio", "") != role_artifacts.value("audio", "") ||
        node.value("ffprobe", "") != role_artifacts.value("ffprobe", "") ||
        node.at("diagnostic_frames") != diagnostic_frames || !node.value("passed", false) ||
        frame_count == 0 || !decoded.is_object() || !decoded.value("passed", false) ||
        !decoded.value("exact_frame_count", false) ||
        NonnegativeInteger(decoded, "decoded_frame_count") != frame_count ||
        NonnegativeInteger(decoded, "manifest_frame_count") != frame_count ||
        NonnegativeInteger(decoded, "analysis_width") != 640 ||
        NonnegativeInteger(decoded, "analysis_height") != 360 ||
        !node.at("optical").value("passed", false) ||
        !node.at("optical").value("timing_correlation_passed", false) ||
        !node.at("april_tag").value("passed", false) ||
        !node.at("april_tag").value("persistent_pre_marker_post", false) ||
        cycle.at("clock").value(std::string(role), "") != role_artifacts.value("clock", "")) {
      Invalid("qualification cycle node media evidence or artifact paths are incomplete");
    }
  }
}

void ValidateCycles(const Json &cycles, const Json &cycle_artifacts,
                    const PairedPoseQualificationPolicy &policy) {
  if (!cycles.is_array() || cycles.size() < policy.minimum_cycle_count ||
      !cycle_artifacts.is_array() || cycle_artifacts.size() != cycles.size()) {
    Invalid("qualification report has too few production capture cycles");
  }
  std::set<std::string, std::less<>> shared_session_ids;
  for (std::size_t index = 0; index < cycles.size(); ++index) {
    const Json &cycle = cycles.at(index);
    const std::string shared_session_id = cycle.value("shared_session_id", "");
    const Json &publication = cycle.at("publication");
    if (cycle.value("cycle_index", cycles.size()) != index || !cycle.value("passed", false) ||
        shared_session_id.empty() || !shared_session_ids.insert(shared_session_id).second ||
        !cycle.value("pose_to_high_speed", false) || !cycle.value("feather_audio_trigger", false) ||
        !cycle.value("rearmed", false) || !publication.value("face_on", false) ||
        !publication.value("down_the_line", false)) {
      Invalid("qualification cycle is incomplete, duplicated, or out of order");
    }
    if (cycle.at("artifacts") != cycle_artifacts.at(index)) {
      Invalid("qualification cycle artifact index disagrees with the cycle report");
    }
    ValidateCycleArtifactSchema(cycle, cycle_artifacts.at(index), index);
  }
}

void ValidateTelemetry(const Json &samples, const PairedPoseQualificationPolicy &policy) {
  if (!samples.is_array() || samples.size() < policy.minimum_telemetry_sample_count) {
    Invalid("qualification report has too few periodic telemetry samples");
  }
  double previous_elapsed = -1.0;
  std::size_t cadence_eligible_samples = 0;
  const auto maximum_gap = static_cast<double>(policy.telemetry_interval_seconds + 15);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const Json &sample = samples.at(index);
    const double elapsed = sample.value("elapsed_seconds", -1.0);
    if (sample.value("sample_index", samples.size()) != index || elapsed < 0.0 ||
        (index == 0 && elapsed > policy.telemetry_interval_seconds) ||
        (index > 0 && (elapsed <= previous_elapsed || elapsed - previous_elapsed > maximum_gap))) {
      Invalid("qualification telemetry cadence is incomplete or nonmonotonic");
    }
    const Json &roles = sample.at("roles");
    ValidateRoleTelemetry(roles.at("face_on"), index == 0);
    ValidateRoleTelemetry(roles.at("down_the_line"), index == 0);
    const bool face_cadence_eligible = roles.at("face_on").value("cadence_eligible", false);
    const bool down_cadence_eligible = roles.at("down_the_line").value("cadence_eligible", false);
    if (face_cadence_eligible != down_cadence_eligible) {
      Invalid("qualification roles disagree about cadence eligibility");
    }
    cadence_eligible_samples += face_cadence_eligible ? 1U : 0U;
    previous_elapsed = elapsed;
  }
  if (previous_elapsed < policy.duration_seconds) {
    Invalid("qualification telemetry does not cover the requested duration");
  }
  if (cadence_eligible_samples < policy.minimum_cycle_count) {
    Invalid("qualification has too few capture-free intervals for the 5 Hz cadence gate");
  }
}

}  // namespace

const PairedPoseQualificationPolicy &PairedPoseQualificationPolicyForMode(std::string_view mode) {
  if (mode == kFiveMinutePolicy.mode) {
    return kFiveMinutePolicy;
  }
  if (mode == kThirtyMinutePolicy.mode) {
    return kThirtyMinutePolicy;
  }
  Invalid(
      "paired pose qualification mode must be paired-pose-qualify-5m or "
      "paired-pose-soak-30m");
}

bool IsPairedPoseQualificationMode(std::string_view mode) noexcept {
  return mode == kFiveMinutePolicy.mode || mode == kThirtyMinutePolicy.mode;
}

// Both strings are deliberately adjacent because this is the public validation boundary.
PairedPoseQualificationReportValidation ValidatePairedPoseQualificationReport(
    std::string_view report_json,  // NOLINT(bugprone-easily-swappable-parameters)
    std::string_view expected_mode) noexcept {
  PairedPoseQualificationReportValidation result;
  try {
    const auto &policy = PairedPoseQualificationPolicyForMode(expected_mode);
    const Json report = Json::parse(report_json);
    const double observed_duration = report.value("observed_duration_seconds", 0.0);
    if (!report.is_object() || report.value("schema_version", 0) != 1 ||
        report.value("report_type", "") != policy.report_type ||
        report.value("mode", "") != policy.mode || !report.value("passed", false) ||
        !report.value("qualification_eligible", false) ||
        report.value("requested_duration_seconds", 0) != policy.duration_seconds ||
        report.value("telemetry_interval_seconds", 0) != policy.telemetry_interval_seconds ||
        report.value("cycle_interval_seconds", 0) != policy.cycle_interval_seconds ||
        observed_duration < policy.duration_seconds ||
        observed_duration > policy.duration_seconds + policy.telemetry_interval_seconds + 15 ||
        !report.at("screen_off").value("passed", false) ||
        !report.at("screen_off").value("face_on", false) ||
        !report.at("screen_off").value("down_the_line", false) ||
        report.value("high_speed_profile", "") != "720p240" ||
        report.value("standby_inference", "") != "real_5hz_on_device" ||
        report.value("peer_transport", "") != "wifi_lan_direct") {
      Invalid("qualification report identity, duration, or workload contract is invalid");
    }
    if (report.value("artifact_path_scope", "") != "undeclared_output_root") {
      Invalid("qualification report artifact path scope is invalid");
    }
    ValidateCycles(report.at("cycles"), report.at("artifacts").at("cycles"), policy);
    ValidateTelemetry(report.at("telemetry_samples"), policy);
    const auto performance = pose_hil::ValidatePoseQualificationPerformanceSummary(report_json);
    if (!performance.valid) {
      Invalid("qualification performance summary is invalid: " + performance.diagnostic);
    }
    result.passed = true;
    result.diagnostic = "paired pose long qualification report passed";
  } catch (const std::exception &failure) {
    result.diagnostic = failure.what();
  }
  return result;
}

}  // namespace swing_capture::android::dual_hil
