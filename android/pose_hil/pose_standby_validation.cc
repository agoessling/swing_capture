#include "android/pose_hil/pose_standby_validation.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace swing_capture::android::pose_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

std::uint64_t RequiredCounter(const Json &value, std::string_view name) {
  if (!value.contains(name) || !value.at(name).is_number_integer()) {
    throw std::invalid_argument(std::string(name) + " must be an integer");
  }
  const std::int64_t signed_value = value.at(name).get<std::int64_t>();
  if (signed_value < 0) {
    throw std::invalid_argument(std::string(name) + " must be nonnegative");
  }
  return static_cast<std::uint64_t>(signed_value);
}

std::uint64_t RequiredDecimalString(const Json &value, std::string_view name) {
  if (!value.contains(name) || !value.at(name).is_string()) {
    throw std::invalid_argument(std::string(name) + " must be a decimal string");
  }
  const auto &text = value.at(name).get_ref<const std::string &>();
  std::uint64_t parsed = 0;
  // std::from_chars exposes a bounded pointer-pair interface.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char *const text_end = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), text_end, parsed);
  if (error != std::errc() || end != text_end) {
    throw std::invalid_argument(std::string(name) + " is not an unsigned decimal integer");
  }
  return parsed;
}

std::optional<int> ParseLabeledInteger(std::string_view text, std::string_view label) {
  std::size_t line_start = 0;
  while (line_start < text.size()) {
    const std::size_t line_end = text.find('\n', line_start);
    std::string_view line =
        text.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos
                                                                   : line_end - line_start);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }
    if (line.starts_with(label)) {
      line.remove_prefix(label.size());
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
      }
      int parsed = 0;
      const auto [end, error] = std::from_chars(line.begin(), line.end(), parsed);
      if (error == std::errc()) {
        return parsed;
      }
      return std::nullopt;
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + 1;
  }
  return std::nullopt;
}

std::optional<double> ParseMaximumTemperature(std::string_view text, int thermal_type) {
  const std::size_t section_start = text.find("Current temperatures from HAL:");
  const std::size_t section_end = text.find("Current cooling devices from HAL:", section_start);
  if (section_start == std::string_view::npos || section_end == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view section = text.substr(section_start, section_end - section_start);
  const std::string type_field = "mType=" + std::to_string(thermal_type) + ",";
  std::optional<double> maximum;
  std::size_t line_start = 0;
  while (line_start < section.size()) {
    const std::size_t line_end = section.find('\n', line_start);
    const std::string_view line =
        section.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos
                                                                      : line_end - line_start);
    constexpr std::string_view kValuePrefix = "Temperature{mValue=";
    const std::size_t value_start = line.find(kValuePrefix);
    if (value_start != std::string_view::npos && line.contains(type_field)) {
      const std::size_t number_start = value_start + kValuePrefix.size();
      const std::size_t number_end = line.find(',', number_start);
      if (number_end == std::string_view::npos) {
        return std::nullopt;
      }
      const std::string_view number = line.substr(number_start, number_end - number_start);
      double parsed = 0.0;
      const auto [end, error] = std::from_chars(number.begin(), number.end(), parsed);
      if (error != std::errc() || end != number.end() || !std::isfinite(parsed)) {
        return std::nullopt;
      }
      maximum = maximum.has_value() ? std::max(*maximum, parsed) : parsed;
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + 1;
  }
  return maximum;
}

std::optional<std::uint64_t> ParseMaximumCoolingState(std::string_view text, int cooling_type) {
  const std::size_t section_start = text.find("Current cooling devices from HAL:");
  const std::size_t section_end =
      text.find("Temperature static thresholds from HAL:", section_start);
  if (section_start == std::string_view::npos || section_end == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view section = text.substr(section_start, section_end - section_start);
  const std::string type_field = "mType=" + std::to_string(cooling_type) + ",";
  std::optional<std::uint64_t> maximum;
  std::size_t line_start = 0;
  while (line_start < section.size()) {
    const std::size_t line_end = section.find('\n', line_start);
    const std::string_view line =
        section.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos
                                                                      : line_end - line_start);
    constexpr std::string_view kValuePrefix = "CoolingDevice{mValue=";
    const std::size_t value_start = line.find(kValuePrefix);
    if (value_start != std::string_view::npos && line.contains(type_field)) {
      const std::size_t number_start = value_start + kValuePrefix.size();
      const std::size_t number_end = line.find(',', number_start);
      if (number_end == std::string_view::npos) {
        return std::nullopt;
      }
      const std::string_view number = line.substr(number_start, number_end - number_start);
      std::uint64_t parsed = 0;
      const auto [end, error] = std::from_chars(number.begin(), number.end(), parsed);
      if (error != std::errc() || end != number.end()) {
        return std::nullopt;
      }
      maximum = maximum.has_value() ? std::max(*maximum, parsed) : parsed;
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + 1;
  }
  return maximum;
}

bool CountersMonotonic(const PoseStatusSample &first, const PoseStatusSample &last) {
  return last.offered_images >= first.offered_images &&
         last.scheduled_images >= first.scheduled_images &&
         last.dropped_images >= first.dropped_images &&
         last.successful_warmup_inferences >= first.successful_warmup_inferences &&
         last.failed_warmup_inferences >= first.failed_warmup_inferences &&
         last.total_warmup_duration_ns >= first.total_warmup_duration_ns &&
         last.maximum_warmup_duration_ns >= first.maximum_warmup_duration_ns &&
         last.successful_inferences >= first.successful_inferences &&
         last.failed_inferences >= first.failed_inferences &&
         last.maximum_inference_duration_ns >= first.maximum_inference_duration_ns &&
         last.inference_deadline_misses >= first.inference_deadline_misses &&
         last.inference_outliers >= first.inference_outliers &&
         last.decision_age_samples >= first.decision_age_samples &&
         last.rejected_decision_timestamps >= first.rejected_decision_timestamps &&
         last.maximum_decision_age_ns >= first.maximum_decision_age_ns &&
         last.encoded_evidence_frames >= first.encoded_evidence_frames &&
         last.process_cpu_time_ms >= first.process_cpu_time_ms;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool OrderedPercentiles(std::uint64_t p50, std::uint64_t p90, std::uint64_t p95, std::uint64_t p99,
                        std::uint64_t maximum) {
  constexpr std::uint64_t kBucketWidthNs = 1'000'000;
  const std::uint64_t maximum_bucket_upper_bound =
      maximum > std::numeric_limits<std::uint64_t>::max() - (kBucketWidthNs - 1)
          ? std::numeric_limits<std::uint64_t>::max()
          : ((maximum + kBucketWidthNs - 1) / kBucketWidthNs) * kBucketWidthNs;
  return p50 <= p90 && p90 <= p95 && p95 <= p99 && p99 <= maximum_bucket_upper_bound;
}

bool DelegateMatchesPolicy(std::string_view policy, std::string_view actual) {
  if (policy == "cpu_only") {
    return actual == "cpu";
  }
  if (policy == "gpu_required") {
    return actual == "gpu";
  }
  if (policy == "npu_required") {
    return actual == "npu";
  }
  if (policy == "gpu_preferred") {
    return actual == "cpu" || actual == "gpu";
  }
  return policy == "npu_preferred" && (actual == "cpu" || actual == "gpu" || actual == "npu");
}

constexpr std::uint64_t kMaximumStartupAudioTimestampRejections = 2;
constexpr std::uint64_t kInferenceDeadlineNs = 200'000'000;
constexpr std::uint64_t kInferenceOutlierBoundNs = 400'000'000;

}  // namespace

PoseStatusSample InspectPoseStatus(std::string_view text) noexcept {
  PoseStatusSample sample;
  try {
    constexpr std::size_t kMaximumStatusBytes = 128ULL * 1024ULL;
    if (text.size() > kMaximumStatusBytes) {
      throw std::invalid_argument("capture status exceeds 128 KiB");
    }
    const Json root = Json::parse(text);
    if (!root.is_object() || root.value("schema_version", 0) != 2) {
      throw std::invalid_argument("capture status must use schema version 2");
    }
    sample.server_elapsed_realtime_ns = RequiredDecimalString(root, "server_elapsed_realtime_ns");
    sample.state = root.at("state").get<std::string>();
    sample.armed = root.at("armed").get<bool>();

    const Json &pose = root.at("pose");
    if (!pose.is_object()) {
      throw std::invalid_argument("pose status must be an object");
    }
    sample.mode = pose.at("mode").get<std::string>();
    sample.configured_delegate = pose.at("configured_delegate").get<std::string>();
    sample.phase = pose.at("phase").get<std::string>();
    const Json &metrics = pose.at("metrics");
    if (!metrics.is_object()) {
      throw std::invalid_argument("pose metrics are not available");
    }
    sample.actual_delegate = metrics.at("delegate").get<std::string>();
    sample.offered_images = RequiredCounter(metrics, "offered_images");
    sample.scheduled_images = RequiredCounter(metrics, "scheduled_images");
    sample.dropped_images = RequiredCounter(metrics, "dropped_images");
    sample.successful_warmup_inferences = RequiredCounter(metrics, "successful_warmup_inferences");
    sample.failed_warmup_inferences = RequiredCounter(metrics, "failed_warmup_inferences");
    sample.total_warmup_duration_ns = RequiredCounter(metrics, "total_warmup_duration_ns");
    sample.maximum_warmup_duration_ns = RequiredCounter(metrics, "maximum_warmup_duration_ns");
    sample.successful_inferences = RequiredCounter(metrics, "successful_inferences");
    sample.failed_inferences = RequiredCounter(metrics, "failed_inferences");
    sample.maximum_inference_duration_ns =
        RequiredCounter(metrics, "maximum_inference_duration_ns");
    sample.inference_duration_p50_ns = RequiredCounter(metrics, "inference_duration_p50_ns");
    sample.inference_duration_p90_ns = RequiredCounter(metrics, "inference_duration_p90_ns");
    sample.inference_duration_p95_ns = RequiredCounter(metrics, "inference_duration_p95_ns");
    sample.inference_duration_p99_ns = RequiredCounter(metrics, "inference_duration_p99_ns");
    if (RequiredCounter(metrics, "inference_deadline_ns") != kInferenceDeadlineNs ||
        RequiredCounter(metrics, "inference_outlier_bound_ns") != kInferenceOutlierBoundNs) {
      throw std::invalid_argument("pose latency policy constants do not match the HIL contract");
    }
    sample.inference_deadline_misses = RequiredCounter(metrics, "inference_deadline_misses");
    sample.inference_outliers = RequiredCounter(metrics, "inference_outliers");
    sample.decision_age_samples = RequiredCounter(metrics, "decision_age_samples");
    sample.rejected_decision_timestamps = RequiredCounter(metrics, "rejected_decision_timestamps");
    sample.maximum_decision_age_ns = RequiredCounter(metrics, "maximum_decision_age_ns");
    sample.decision_age_p50_ns = RequiredCounter(metrics, "decision_age_p50_ns");
    sample.decision_age_p90_ns = RequiredCounter(metrics, "decision_age_p90_ns");
    sample.decision_age_p95_ns = RequiredCounter(metrics, "decision_age_p95_ns");
    sample.decision_age_p99_ns = RequiredCounter(metrics, "decision_age_p99_ns");
    sample.encoded_evidence_frames = RequiredCounter(metrics, "encoded_evidence_frames");
    sample.process_cpu_time_ms = RequiredCounter(pose, "process_cpu_time_ms");
    if (sample.server_elapsed_realtime_ns == 0 ||
        !DelegateMatchesPolicy(sample.configured_delegate, sample.actual_delegate)) {
      throw std::invalid_argument("capture status has an invalid clock or actual delegate");
    }
    const std::uint64_t warmup_inferences =
        sample.successful_warmup_inferences + sample.failed_warmup_inferences;
    if (sample.scheduled_images > sample.offered_images ||
        sample.dropped_images > sample.offered_images || warmup_inferences > 1 ||
        (warmup_inferences == 0 &&
         (sample.total_warmup_duration_ns != 0 || sample.maximum_warmup_duration_ns != 0)) ||
        (warmup_inferences == 1 &&
         (sample.total_warmup_duration_ns == 0 ||
          sample.maximum_warmup_duration_ns != sample.total_warmup_duration_ns)) ||
        (sample.phase == "warming_up" && warmup_inferences != 0) ||
        (sample.phase == "monitoring" &&
         (sample.successful_warmup_inferences != 1 || sample.failed_warmup_inferences != 0)) ||
        warmup_inferences + sample.successful_inferences + sample.failed_inferences >
            sample.scheduled_images ||
        sample.encoded_evidence_frames > sample.successful_inferences ||
        sample.inference_deadline_misses >
            sample.successful_inferences + sample.failed_inferences ||
        sample.inference_outliers > sample.inference_deadline_misses ||
        sample.decision_age_samples > sample.successful_inferences ||
        sample.rejected_decision_timestamps >
            sample.successful_inferences - sample.decision_age_samples ||
        sample.decision_age_samples + sample.rejected_decision_timestamps !=
            sample.successful_inferences ||
        !OrderedPercentiles(sample.inference_duration_p50_ns, sample.inference_duration_p90_ns,
                            sample.inference_duration_p95_ns, sample.inference_duration_p99_ns,
                            sample.maximum_inference_duration_ns) ||
        !OrderedPercentiles(sample.decision_age_p50_ns, sample.decision_age_p90_ns,
                            sample.decision_age_p95_ns, sample.decision_age_p99_ns,
                            sample.maximum_decision_age_ns)) {
      throw std::invalid_argument("pose metrics violate cumulative pipeline bounds");
    }
    const Json &standby_audio = pose.at("standby_audio");
    if (!standby_audio.is_object() || !standby_audio.at("ready").is_boolean() ||
        !standby_audio.at("audio_source").is_number_integer() ||
        !standby_audio.at("last_error").is_string()) {
      throw std::invalid_argument("standby audio status is structurally invalid");
    }
    sample.standby_audio_ready = standby_audio.at("ready").get<bool>();
    sample.standby_audio_source = standby_audio.at("audio_source").get<int>();
    sample.standby_audio_end_frame_position =
        RequiredDecimalString(standby_audio, "end_frame_position");
    sample.standby_audio_dropped_events = RequiredCounter(standby_audio, "dropped_events");
    sample.standby_audio_timestamp_rejections =
        RequiredCounter(standby_audio, "timestamp_rejections");
    sample.standby_audio_discontinuities = RequiredCounter(standby_audio, "discontinuities");
    sample.standby_audio_last_error = standby_audio.at("last_error").get<std::string>();
    if (sample.standby_audio_source < 0) {
      throw std::invalid_argument("standby audio source is invalid");
    }
    sample.valid = true;
    sample.diagnostic = "pose status is structurally valid";
  } catch (const std::exception &failure) {
    sample.diagnostic = failure.what();
  }
  return sample;
}

bool HasMinimumEncodedPoseEvidence(const PoseStatusSample &sample,
                                   std::uint64_t minimum_frames) noexcept {
  return sample.valid && sample.encoded_evidence_frames >= minimum_frames;
}

PoseCadenceAcceptance EvaluatePoseCadence(const PoseStatusSample &first,
                                          const PoseStatusSample &last) noexcept {
  PoseCadenceAcceptance result;
  try {
    if (!first.valid || !last.valid) {
      throw std::invalid_argument("cadence endpoints must be valid pose status samples");
    }
    if (first.phase != "monitoring" || last.phase != "monitoring" || first.mode != "shadow" ||
        last.mode != "shadow" || first.configured_delegate != last.configured_delegate ||
        first.state != "armed" || last.state != "armed" || !first.armed || !last.armed) {
      throw std::invalid_argument("pose standby left armed shadow monitoring");
    }
    if (first.actual_delegate != last.actual_delegate) {
      throw std::invalid_argument("actual inference delegate changed during the sample");
    }
    if (!first.standby_audio_ready || !last.standby_audio_ready ||
        first.standby_audio_source != last.standby_audio_source ||
        first.standby_audio_end_frame_position == 0 ||
        last.standby_audio_end_frame_position <= first.standby_audio_end_frame_position ||
        first.standby_audio_dropped_events != 0 || last.standby_audio_dropped_events != 0 ||
        first.standby_audio_discontinuities != 0 || last.standby_audio_discontinuities != 0 ||
        !first.standby_audio_last_error.empty() || !last.standby_audio_last_error.empty()) {
      throw std::invalid_argument("standby audio did not remain ready and continuously advancing");
    }
    if (last.standby_audio_timestamp_rejections < first.standby_audio_timestamp_rejections ||
        last.standby_audio_timestamp_rejections > kMaximumStartupAudioTimestampRejections) {
      throw std::invalid_argument("standby audio timestamp rejections exceed startup allowance");
    }
    if (last.server_elapsed_realtime_ns <= first.server_elapsed_realtime_ns ||
        !CountersMonotonic(first, last)) {
      throw std::invalid_argument("pose clock or counters moved backwards");
    }

    result.offered_images = last.offered_images - first.offered_images;
    result.scheduled_images = last.scheduled_images - first.scheduled_images;
    result.dropped_images = last.dropped_images - first.dropped_images;
    result.successful_inferences = last.successful_inferences - first.successful_inferences;
    result.failed_inferences = last.failed_inferences - first.failed_inferences;
    result.interval_seconds =
        static_cast<double>(last.server_elapsed_realtime_ns - first.server_elapsed_realtime_ns) /
        1'000'000'000.0;
    result.successful_inferences_per_second =
        static_cast<double>(result.successful_inferences) / result.interval_seconds;
    result.dropped_fraction = result.offered_images == 0
                                  ? 0.0
                                  : static_cast<double>(result.dropped_images) /
                                        static_cast<double>(result.offered_images);

    if (result.interval_seconds < 2.0) {
      throw std::invalid_argument("pose cadence interval is shorter than two seconds");
    }
    if (result.successful_inferences_per_second < 4.0) {
      throw std::invalid_argument("pose inference cadence is below 4 Hz");
    }
    if (result.failed_inferences != 0) {
      throw std::invalid_argument("pose inference failed during the cadence interval");
    }
    const std::uint64_t completed_or_dropped =
        result.successful_inferences + result.failed_inferences + result.dropped_images;
    const std::uint64_t pipeline_difference = completed_or_dropped > result.scheduled_images
                                                  ? completed_or_dropped - result.scheduled_images
                                                  : result.scheduled_images - completed_or_dropped;
    if (pipeline_difference > 2) {
      throw std::invalid_argument("pose pipeline accounting exceeds its two-image capacity");
    }
    if (result.dropped_images > 2 && result.dropped_fraction > 0.20) {
      throw std::invalid_argument("pose standby dropped more than the bounded allowance");
    }
    result.passed = true;
    result.diagnostic = "pose standby sustained at least 4 Hz without inference failures";
  } catch (const std::exception &failure) {
    result.diagnostic = failure.what();
  }
  return result;
}

PoseLatencyAcceptance EvaluatePoseLatency(const PoseStatusSample &sample) noexcept {
  PoseLatencyAcceptance result;
  try {
    if (!sample.valid || sample.successful_warmup_inferences != 1 ||
        sample.failed_warmup_inferences != 0 || sample.successful_inferences == 0) {
      throw std::invalid_argument("latency qualification requires successful pose inference");
    }
    if (sample.inference_duration_p95_ns > kInferenceDeadlineNs) {
      throw std::invalid_argument("pose inference p95 exceeds the 200 ms deadline");
    }
    if (sample.inference_outliers != 0 ||
        sample.maximum_inference_duration_ns > kInferenceOutlierBoundNs) {
      throw std::invalid_argument("pose inference has an outlier above 400 ms");
    }
    if (sample.rejected_decision_timestamps != 0) {
      throw std::invalid_argument("pose decision timestamp domain was rejected");
    }
    result.passed = true;
    result.diagnostic = "pose inference and decision timestamp bounds passed";
  } catch (const std::exception &failure) {
    result.diagnostic = failure.what();
  }
  return result;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
DeviceTelemetry InspectDeviceTelemetry(std::string_view battery,
                                       std::string_view thermal) noexcept {
  DeviceTelemetry result;
  try {
    const std::optional<int> level = ParseLabeledInteger(battery, "level:");
    const std::optional<int> temperature = ParseLabeledInteger(battery, "temperature:");
    const std::optional<int> voltage = ParseLabeledInteger(battery, "voltage:");
    std::optional<int> thermal_status = ParseLabeledInteger(thermal, "Thermal Status:");
    if (!thermal_status.has_value()) {
      thermal_status = ParseLabeledInteger(thermal, "Thermal status:");
    }
    if (!level.has_value() || !temperature.has_value() || !voltage.has_value() ||
        !thermal_status.has_value()) {
      throw std::invalid_argument("required battery or thermalservice fields are absent");
    }
    result.battery_level_percent = *level;
    result.battery_temperature_celsius = static_cast<double>(*temperature) / 10.0;
    result.battery_voltage_millivolts = *voltage;
    result.thermal_status = *thermal_status;
    const std::optional<double> cpu_temperature = ParseMaximumTemperature(thermal, 0);
    const std::optional<double> gpu_temperature = ParseMaximumTemperature(thermal, 1);
    const std::optional<std::uint64_t> cpu_cooling = ParseMaximumCoolingState(thermal, 2);
    const std::optional<std::uint64_t> gpu_cooling = ParseMaximumCoolingState(thermal, 3);
    result.processor_thermal_complete = cpu_temperature.has_value() &&
                                        gpu_temperature.has_value() && cpu_cooling.has_value() &&
                                        gpu_cooling.has_value();
    if (cpu_temperature.has_value()) {
      result.maximum_cpu_temperature_celsius = *cpu_temperature;
    }
    if (gpu_temperature.has_value()) {
      result.maximum_gpu_temperature_celsius = *gpu_temperature;
    }
    if (cpu_cooling.has_value()) {
      result.maximum_cpu_cooling_device_value = *cpu_cooling;
    }
    if (gpu_cooling.has_value()) {
      result.maximum_gpu_cooling_device_value = *gpu_cooling;
    }
    if (result.battery_level_percent < 0 || result.battery_level_percent > 100 ||
        !std::isfinite(result.battery_temperature_celsius) ||
        result.battery_temperature_celsius < -20.0 || result.battery_temperature_celsius > 100.0 ||
        result.battery_voltage_millivolts <= 0 || result.thermal_status < 0 ||
        result.thermal_status > 6) {
      throw std::invalid_argument("battery or thermalservice fields are outside valid bounds");
    }
    result.valid = true;
    result.diagnostic = result.processor_thermal_complete
                            ? "device battery, CPU, GPU, and thermal telemetry is valid"
                            : "device battery and thermal status are valid but CPU/GPU telemetry "
                              "is incomplete";
  } catch (const std::exception &failure) {
    result.diagnostic = failure.what();
  }
  return result;
}

}  // namespace swing_capture::android::pose_hil
