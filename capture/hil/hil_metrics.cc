#include "capture/hil/hil_metrics.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

namespace swing_capture::hil {
namespace {

std::string ValueMessage(const char *name, double actual, double expected, const char *comparison) {
  std::ostringstream message;
  message << name << "=" << actual << " expected " << comparison << ' ' << expected;
  return message.str();
}

void AddCheck(Evaluation &evaluation, std::string id, bool passed, std::string message) {
  evaluation.checks.push_back({
      .id = std::move(id),
      .passed = passed,
      .message = std::move(message),
  });
}

}  // namespace

Evaluation EvaluateCamera(const CameraMetrics &metrics, const Thresholds &thresholds) {
  Evaluation evaluation;
  AddCheck(evaluation, "capture.no_internal_error", !metrics.internal_error,
           metrics.internal_error ? "capture raised an internal error" : "no internal error");
  AddCheck(evaluation, "frames.complete_nonzero", metrics.complete_frames > 0,
           "complete_frames=" + std::to_string(metrics.complete_frames));
  AddCheck(evaluation, "frames.incomplete_zero", metrics.incomplete_frames == 0,
           "incomplete_frames=" + std::to_string(metrics.incomplete_frames));
  AddCheck(evaluation, "frames.timeouts_zero", metrics.timeouts == 0,
           "timeouts=" + std::to_string(metrics.timeouts));
  AddCheck(evaluation, "frames.missing_ids_zero", metrics.missing_frame_ids == 0,
           "missing_frame_ids=" + std::to_string(metrics.missing_frame_ids));
  AddCheck(evaluation, "timing.timestamps_monotonic", metrics.timestamp_non_monotonic == 0,
           "timestamp_non_monotonic=" + std::to_string(metrics.timestamp_non_monotonic));

  const double minimum_fps = metrics.requested_frames_per_second * thresholds.minimum_fps_ratio;
  const double maximum_fps = metrics.requested_frames_per_second * thresholds.maximum_fps_ratio;
  AddCheck(evaluation, "timing.host_fps_minimum", metrics.host_frames_per_second >= minimum_fps,
           ValueMessage("host_fps", metrics.host_frames_per_second, minimum_fps, ">="));
  AddCheck(evaluation, "timing.host_fps_maximum", metrics.host_frames_per_second <= maximum_fps,
           ValueMessage("host_fps", metrics.host_frames_per_second, maximum_fps, "<="));
  AddCheck(evaluation, "timing.device_fps_minimum", metrics.device_frames_per_second >= minimum_fps,
           ValueMessage("device_fps", metrics.device_frames_per_second, minimum_fps, ">="));
  AddCheck(evaluation, "timing.device_fps_maximum", metrics.device_frames_per_second <= maximum_fps,
           ValueMessage("device_fps", metrics.device_frames_per_second, maximum_fps, "<="));

  const double expected_frames_per_second = metrics.resulting_frames_per_second > 0.0
                                                ? metrics.resulting_frames_per_second
                                                : metrics.requested_frames_per_second;
  const double maximum_host_frame_interval_seconds =
      expected_frames_per_second > 0.0
          ? thresholds.maximum_host_frame_interval_multiple / expected_frames_per_second
          : 0.0;
  const double maximum_device_frame_interval_seconds =
      expected_frames_per_second > 0.0
          ? thresholds.maximum_device_frame_interval_multiple / expected_frames_per_second
          : 0.0;
  AddCheck(evaluation, "timing.host_interval_maximum",
           maximum_host_frame_interval_seconds > 0.0 &&
               metrics.maximum_host_frame_interval_seconds <= maximum_host_frame_interval_seconds,
           ValueMessage("maximum_host_frame_interval_seconds",
                        metrics.maximum_host_frame_interval_seconds,
                        maximum_host_frame_interval_seconds, "<="));
  AddCheck(
      evaluation, "timing.device_interval_maximum",
      maximum_device_frame_interval_seconds > 0.0 &&
          metrics.maximum_device_frame_interval_seconds <= maximum_device_frame_interval_seconds,
      ValueMessage("maximum_device_frame_interval_seconds",
                   metrics.maximum_device_frame_interval_seconds,
                   maximum_device_frame_interval_seconds, "<="));

  const double minimum_duration = metrics.requested_duration_seconds * 0.98;
  AddCheck(evaluation, "capture.measured_duration",
           metrics.measured_duration_seconds >= minimum_duration,
           ValueMessage("measured_duration_seconds", metrics.measured_duration_seconds,
                        minimum_duration, ">="));

  const std::uint64_t expected_payload_bytes =
      metrics.complete_frames * metrics.expected_payload_bytes_per_frame;
  AddCheck(evaluation, "frames.payload_size_exact", metrics.payload_bytes == expected_payload_bytes,
           "payload_bytes=" + std::to_string(metrics.payload_bytes) +
               " expected=" + std::to_string(expected_payload_bytes));

  evaluation.passed = true;
  for (const Check &check : evaluation.checks) {
    evaluation.passed = evaluation.passed && check.passed;
  }
  return evaluation;
}

}  // namespace swing_capture::hil
