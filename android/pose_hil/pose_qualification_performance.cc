#include "android/pose_hil/pose_qualification_performance.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace swing_capture::android::pose_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

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

std::uint64_t DecimalString(const Json &value, std::string_view name) {
  if (!value.contains(name) || !value.at(name).is_string()) {
    Invalid(std::string(name) + " must be a decimal string");
  }
  const auto &text = value.at(name).get_ref<const std::string &>();
  std::uint64_t parsed = 0;
  // std::from_chars exposes a bounded pointer-pair interface.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char *const text_end = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), text_end, parsed);
  if (error != std::errc() || end != text_end || text.empty() ||
      (text.size() > 1 && text.front() == '0')) {
    Invalid(std::string(name) + " is not a canonical unsigned decimal integer");
  }
  return parsed;
}

double FiniteNumber(const Json &value, std::string_view name) {
  if (!value.contains(name) || !value.at(name).is_number()) {
    Invalid(std::string(name) + " must be numeric");
  }
  const double parsed = value.at(name).get<double>();
  if (!std::isfinite(parsed)) {
    Invalid(std::string(name) + " must be finite");
  }
  return parsed;
}

void CheckedAdd(std::uint64_t value, std::uint64_t *total, std::string_view name) {
  if (value > std::numeric_limits<std::uint64_t>::max() - *total) {
    Invalid(std::string(name) + " overflowed");
  }
  *total += value;
}

std::uint64_t NearestRankPercentile(std::vector<std::uint64_t> values, std::size_t percentile) {
  if (values.empty() || percentile == 0 || percentile > 100) {
    Invalid("qualification percentile input is invalid");
  }
  std::ranges::sort(values);
  const std::size_t rank = (values.size() * percentile + 99U) / 100U;
  return values.at(rank - 1U);
}

struct RoleAccumulator {
  std::size_t telemetry_intervals = 0;
  std::uint64_t total_offered = 0;
  std::uint64_t total_successful = 0;
  std::uint64_t total_failed = 0;
  std::uint64_t total_dropped = 0;
  std::uint64_t total_deadline_misses = 0;
  std::uint64_t total_outliers = 0;
  std::uint64_t total_decision_samples = 0;
  std::uint64_t total_rejected_decisions = 0;
  double worst_dropped_fraction = 0.0;
  std::uint64_t worst_inference_p95_ns = 0;
  std::uint64_t maximum_inference_ns = 0;
  std::uint64_t worst_decision_p95_ns = 0;
  std::uint64_t maximum_decision_ns = 0;
  double maximum_process_cpu_utilization_percent = 0.0;
  double maximum_cpu_temperature_celsius = -1.0;
  double maximum_gpu_temperature_celsius = -1.0;
  std::uint64_t maximum_cpu_cooling_device_value = 0;
  std::uint64_t maximum_gpu_cooling_device_value = 0;
  std::vector<std::uint64_t> arm_to_first_camera_frame_ns;
  std::vector<std::uint64_t> arm_to_first_usable_frame_ns;
  std::vector<std::uint64_t> arm_to_full_pre_roll_ns;
};

void AccumulateTelemetry(const Json &role, bool initial, RoleAccumulator *summary) {
  if (!role.is_object() || !role.value("processor_thermal_complete", false)) {
    Invalid("qualification CPU/GPU thermal telemetry is incomplete");
  }
  const double cpu_temperature = FiniteNumber(role, "maximum_cpu_temperature_celsius");
  const double gpu_temperature = FiniteNumber(role, "maximum_gpu_temperature_celsius");
  if (cpu_temperature < -40.0 || cpu_temperature > 200.0 || gpu_temperature < -40.0 ||
      gpu_temperature > 200.0) {
    Invalid("qualification CPU/GPU temperature is outside the physical evidence range");
  }
  summary->maximum_cpu_temperature_celsius =
      std::max(summary->maximum_cpu_temperature_celsius, cpu_temperature);
  summary->maximum_gpu_temperature_celsius =
      std::max(summary->maximum_gpu_temperature_celsius, gpu_temperature);
  summary->maximum_cpu_cooling_device_value =
      std::max(summary->maximum_cpu_cooling_device_value,
               NonnegativeInteger(role, "maximum_cpu_cooling_device_value"));
  summary->maximum_gpu_cooling_device_value =
      std::max(summary->maximum_gpu_cooling_device_value,
               NonnegativeInteger(role, "maximum_gpu_cooling_device_value"));

  const std::uint64_t cpu_delta = NonnegativeInteger(role, "process_cpu_time_delta_ms");
  const double cpu_utilization = FiniteNumber(role, "process_cpu_utilization_percent");
  const double interval_seconds = FiniteNumber(role, "interval_seconds");
  const double expected_cpu_utilization =
      interval_seconds == 0.0
          ? 0.0
          : (static_cast<double>(cpu_delta) / 1000.0) / interval_seconds * 100.0;
  const double cpu_tolerance = std::max(1e-9, expected_cpu_utilization * 1e-9);
  if (cpu_utilization < 0.0 || (initial && (cpu_delta != 0 || cpu_utilization != 0.0)) ||
      (initial && interval_seconds != 0.0) ||
      (!initial && (interval_seconds <= 0.0 || cpu_delta == 0 || cpu_utilization == 0.0)) ||
      std::abs(cpu_utilization - expected_cpu_utilization) > cpu_tolerance) {
    Invalid("qualification process CPU interval telemetry is invalid");
  }
  summary->maximum_process_cpu_utilization_percent =
      std::max(summary->maximum_process_cpu_utilization_percent, cpu_utilization);
  if (initial) {
    return;
  }

  ++summary->telemetry_intervals;
  const std::uint64_t offered = NonnegativeInteger(role, "offered_images_delta");
  const std::uint64_t successful = NonnegativeInteger(role, "successful_inferences_delta");
  const std::uint64_t failed = NonnegativeInteger(role, "failed_inferences_delta");
  const std::uint64_t dropped = NonnegativeInteger(role, "dropped_images_delta");
  const std::uint64_t deadline_misses = NonnegativeInteger(role, "inference_deadline_misses_delta");
  const std::uint64_t outliers = NonnegativeInteger(role, "inference_outliers_delta");
  const std::uint64_t decision_samples = NonnegativeInteger(role, "decision_age_samples_delta");
  const std::uint64_t rejected_decisions =
      NonnegativeInteger(role, "rejected_decision_timestamps_delta");
  const double dropped_fraction = FiniteNumber(role, "dropped_fraction");
  const double expected_dropped_fraction =
      offered == 0 ? 0.0 : static_cast<double>(dropped) / static_cast<double>(offered);
  if (dropped > offered || dropped_fraction < 0.0 || dropped_fraction > 1.0 ||
      std::abs(dropped_fraction - expected_dropped_fraction) > 1e-9) {
    Invalid("qualification latest-frame drop telemetry is inconsistent");
  }
  const std::uint64_t inference_p95 = NonnegativeInteger(role, "inference_duration_p95_ns");
  const std::uint64_t inference_maximum = NonnegativeInteger(role, "maximum_inference_duration_ns");
  const std::uint64_t decision_p95 = NonnegativeInteger(role, "decision_age_p95_ns");
  const std::uint64_t decision_maximum = NonnegativeInteger(role, "maximum_decision_age_ns");
  constexpr std::uint64_t kHistogramBucketWidthNs = 1'000'000;
  const auto bucket_upper_bound = [](std::uint64_t maximum) {
    return maximum == 0 ? 0
                        : ((maximum - 1) / kHistogramBucketWidthNs + 1) * kHistogramBucketWidthNs;
  };
  if (inference_p95 > bucket_upper_bound(inference_maximum) ||
      decision_p95 > bucket_upper_bound(decision_maximum)) {
    Invalid("qualification inference or decision-age percentile exceeds its maximum");
  }
  CheckedAdd(offered, &summary->total_offered, "offered image total");
  CheckedAdd(successful, &summary->total_successful, "successful inference total");
  CheckedAdd(failed, &summary->total_failed, "failed inference total");
  CheckedAdd(dropped, &summary->total_dropped, "dropped image total");
  CheckedAdd(deadline_misses, &summary->total_deadline_misses, "deadline miss total");
  CheckedAdd(outliers, &summary->total_outliers, "outlier total");
  CheckedAdd(decision_samples, &summary->total_decision_samples, "decision sample total");
  CheckedAdd(rejected_decisions, &summary->total_rejected_decisions, "rejected decision total");
  summary->worst_dropped_fraction = std::max(summary->worst_dropped_fraction, dropped_fraction);
  summary->worst_inference_p95_ns = std::max(summary->worst_inference_p95_ns, inference_p95);
  summary->maximum_inference_ns = std::max(summary->maximum_inference_ns, inference_maximum);
  summary->worst_decision_p95_ns = std::max(summary->worst_decision_p95_ns, decision_p95);
  summary->maximum_decision_ns = std::max(summary->maximum_decision_ns, decision_maximum);
}

void AccumulateStartup(const Json &role, RoleAccumulator *summary) {
  const Json &startup = role.at("startup_timing");
  if (!startup.is_object() || startup.value("schema_version", 0) != 1 ||
      startup.value("clock", "") != "CLOCK_BOOTTIME" || !startup.value("continuity_clean", false) ||
      DecimalString(startup, "startup_continuity_reset_count") != 0 ||
      DecimalString(startup, "maximum_startup_continuity_gap_ns") != 0) {
    Invalid("qualification startup timing is incomplete or discontinuous");
  }
  const Json &durations = startup.at("durations");
  const std::uint64_t first_camera = DecimalString(durations, "arm_to_first_camera_frame_ns");
  const std::uint64_t first_usable =
      DecimalString(durations, "arm_to_first_usable_encoded_frame_ns");
  const std::uint64_t full_pre_roll = DecimalString(durations, "arm_to_full_pre_roll_ready_ns");
  if (first_camera == 0 || first_usable < first_camera || full_pre_roll < first_usable) {
    Invalid("qualification startup duration milestones are not monotonic");
  }
  summary->arm_to_first_camera_frame_ns.push_back(first_camera);
  summary->arm_to_first_usable_frame_ns.push_back(first_usable);
  summary->arm_to_full_pre_roll_ns.push_back(full_pre_roll);
}

Json RoleSummary(const RoleAccumulator &role) {
  if (role.telemetry_intervals == 0 || role.arm_to_first_usable_frame_ns.empty()) {
    Invalid("qualification has no steady telemetry interval or concurrent startup sample");
  }
  return {
      {"telemetry_interval_count", role.telemetry_intervals},
      {"inference",
       {{"total_offered_images", role.total_offered},
        {"total_successful_inferences", role.total_successful},
        {"total_failed_inferences", role.total_failed},
        {"total_dropped_images", role.total_dropped},
        {"total_deadline_misses", role.total_deadline_misses},
        {"total_outliers", role.total_outliers},
        {"worst_interval_dropped_fraction", role.worst_dropped_fraction},
        {"worst_inference_duration_p95_ns", std::to_string(role.worst_inference_p95_ns)},
        {"maximum_inference_duration_ns", std::to_string(role.maximum_inference_ns)},
        {"total_decision_age_samples", role.total_decision_samples},
        {"total_rejected_decision_timestamps", role.total_rejected_decisions},
        {"worst_decision_age_p95_ns", std::to_string(role.worst_decision_p95_ns)},
        {"maximum_decision_age_ns", std::to_string(role.maximum_decision_ns)}}},
      {"workload",
       {{"maximum_process_cpu_utilization_percent", role.maximum_process_cpu_utilization_percent},
        {"maximum_cpu_temperature_celsius", role.maximum_cpu_temperature_celsius},
        {"maximum_gpu_temperature_celsius", role.maximum_gpu_temperature_celsius},
        {"maximum_cpu_cooling_device_value", role.maximum_cpu_cooling_device_value},
        {"maximum_gpu_cooling_device_value", role.maximum_gpu_cooling_device_value}}},
      {"startup_under_contention",
       {{"sample_count", role.arm_to_first_usable_frame_ns.size()},
        {"arm_to_first_camera_frame_p95_ns",
         std::to_string(NearestRankPercentile(role.arm_to_first_camera_frame_ns, 95))},
        {"arm_to_first_camera_frame_max_ns",
         std::to_string(*std::ranges::max_element(role.arm_to_first_camera_frame_ns))},
        {"arm_to_first_usable_encoded_frame_p95_ns",
         std::to_string(NearestRankPercentile(role.arm_to_first_usable_frame_ns, 95))},
        {"arm_to_first_usable_encoded_frame_max_ns",
         std::to_string(*std::ranges::max_element(role.arm_to_first_usable_frame_ns))},
        {"arm_to_full_pre_roll_ready_p95_ns",
         std::to_string(NearestRankPercentile(role.arm_to_full_pre_roll_ns, 95))},
        {"arm_to_full_pre_roll_ready_max_ns",
         std::to_string(*std::ranges::max_element(role.arm_to_full_pre_roll_ns))}}},
  };
}

Json Derive(const Json &report) {
  if (!report.is_object() || !report.contains("telemetry_samples") ||
      !report.at("telemetry_samples").is_array() || report.at("telemetry_samples").size() < 2 ||
      !report.contains("cycles") || !report.at("cycles").is_array() ||
      report.at("cycles").empty()) {
    Invalid("qualification report lacks telemetry or production capture cycles");
  }
  RoleAccumulator face;
  RoleAccumulator down;
  for (std::size_t index = 0; index < report.at("telemetry_samples").size(); ++index) {
    const Json &roles = report.at("telemetry_samples").at(index).at("roles");
    AccumulateTelemetry(roles.at("face_on"), index == 0, &face);
    AccumulateTelemetry(roles.at("down_the_line"), index == 0, &down);
  }
  for (const Json &cycle : report.at("cycles")) {
    const Json &nodes = cycle.at("nodes");
    AccumulateStartup(nodes.at("face_on"), &face);
    AccumulateStartup(nodes.at("down_the_line"), &down);
  }
  return {
      {"schema_version", 1},
      {"scope", "simultaneous_pair_production_workload"},
      {"startup_bound_policy", "measurement_only"},
      {"roles", {{"face_on", RoleSummary(face)}, {"down_the_line", RoleSummary(down)}}},
  };
}

}  // namespace

PoseQualificationPerformanceInspection BuildPoseQualificationPerformanceSummary(
    std::string_view report_json) noexcept {
  PoseQualificationPerformanceInspection result;
  try {
    constexpr std::size_t kMaximumReportBytes = 16ULL * 1024ULL * 1024ULL;
    if (report_json.size() > kMaximumReportBytes) {
      Invalid("qualification report exceeds 16 MiB");
    }
    const Json report = Json::parse(report_json);
    result.summary_json = Derive(report).dump();
    result.valid = true;
    result.diagnostic = "paired pose performance summary is complete";
  } catch (const std::exception &failure) {
    result.diagnostic = failure.what();
  }
  return result;
}

PoseQualificationPerformanceInspection ValidatePoseQualificationPerformanceSummary(
    std::string_view report_json) noexcept {
  PoseQualificationPerformanceInspection result =
      BuildPoseQualificationPerformanceSummary(report_json);
  if (!result.valid) {
    return result;
  }
  try {
    const Json report = Json::parse(report_json);
    const Json expected = Json::parse(result.summary_json);
    if (!report.contains("performance_summary") || report.at("performance_summary") != expected) {
      Invalid("embedded paired pose performance summary does not match source evidence");
    }
    result.diagnostic = "embedded paired pose performance summary matches source evidence";
  } catch (const std::exception &failure) {
    result.valid = false;
    result.diagnostic = failure.what();
  }
  return result;
}

}  // namespace swing_capture::android::pose_hil
