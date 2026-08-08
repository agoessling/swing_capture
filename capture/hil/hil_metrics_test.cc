#include "capture/hil/hil_metrics.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

using swing_capture::hil::CameraMetrics;
using swing_capture::hil::Check;
using swing_capture::hil::EvaluateCamera;
using swing_capture::hil::Evaluation;
using swing_capture::hil::Thresholds;

int failures = 0;

void Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const Check *FindCheck(const Evaluation &evaluation, const std::string &id) {
  for (const Check &check : evaluation.checks) {
    if (check.id == id) {
      return &check;
    }
  }
  return nullptr;
}

void ExpectFailedCheck(const Evaluation &evaluation, const std::string &id) {
  const Check *check = FindCheck(evaluation, id);
  Expect(check != nullptr, "missing check: " + id);
  if (check != nullptr) {
    Expect(!check->passed, "expected check to fail: " + id);
  }
}

CameraMetrics PassingMetrics() {
  constexpr std::uint64_t kFrames = 3403;
  constexpr std::uint64_t kPayload = 1555200;
  return {
      .requested_frames_per_second = 227.0,
      .resulting_frames_per_second = 226.86,
      .host_frames_per_second = 226.88,
      .device_frames_per_second = 226.87,
      .maximum_host_frame_interval_seconds = 0.006,
      .maximum_device_frame_interval_seconds = 0.005,
      .requested_duration_seconds = 15.0,
      .measured_duration_seconds = 14.99,
      .complete_frames = kFrames,
      .payload_bytes = kFrames * kPayload,
      .expected_payload_bytes_per_frame = kPayload,
  };
}

void PassingRunPassesAllChecks() {
  const Evaluation evaluation = EvaluateCamera(PassingMetrics(), Thresholds{});
  Expect(evaluation.passed, "passing run should pass");
  for (const Check &check : evaluation.checks) {
    Expect(check.passed, "passing run failed check: " + check.id);
  }
}

void FrameIdGapFails() {
  CameraMetrics metrics = PassingMetrics();
  metrics.missing_frame_ids = 1;
  const Evaluation evaluation = EvaluateCamera(metrics, Thresholds{});
  Expect(!evaluation.passed, "frame ID gap should fail evaluation");
  ExpectFailedCheck(evaluation, "frames.missing_ids_zero");
}

void TimeoutFails() {
  CameraMetrics metrics = PassingMetrics();
  metrics.timeouts = 1;
  const Evaluation evaluation = EvaluateCamera(metrics, Thresholds{});
  Expect(!evaluation.passed, "capture timeout should fail evaluation");
  ExpectFailedCheck(evaluation, "frames.timeouts_zero");
}

void LowFrameRateFails() {
  CameraMetrics metrics = PassingMetrics();
  metrics.host_frames_per_second = 200.0;
  const Evaluation evaluation = EvaluateCamera(metrics, Thresholds{});
  Expect(!evaluation.passed, "low host frame rate should fail evaluation");
  ExpectFailedCheck(evaluation, "timing.host_fps_minimum");
}

void HighFrameRateFails() {
  CameraMetrics metrics = PassingMetrics();
  metrics.device_frames_per_second = 240.0;
  const Evaluation evaluation = EvaluateCamera(metrics, Thresholds{});
  Expect(!evaluation.passed, "high device frame rate should fail evaluation");
  ExpectFailedCheck(evaluation, "timing.device_fps_maximum");
}

void TimestampResetFails() {
  CameraMetrics metrics = PassingMetrics();
  metrics.timestamp_non_monotonic = 1;
  const Evaluation evaluation = EvaluateCamera(metrics, Thresholds{});
  Expect(!evaluation.passed, "device timestamp reset/nonmonotonic value should fail evaluation");
  ExpectFailedCheck(evaluation, "timing.timestamps_monotonic");
}

void HostStallFails() {
  CameraMetrics metrics = PassingMetrics();
  metrics.maximum_host_frame_interval_seconds = 0.050;
  const Evaluation evaluation =
      EvaluateCamera(metrics, Thresholds{.maximum_host_frame_interval_multiple = 10.0});
  Expect(!evaluation.passed, "host receive stall should fail evaluation");
  ExpectFailedCheck(evaluation, "timing.host_interval_maximum");
}

void DeviceStallFails() {
  CameraMetrics metrics = PassingMetrics();
  metrics.maximum_device_frame_interval_seconds = 0.018;
  const Evaluation evaluation =
      EvaluateCamera(metrics, Thresholds{.maximum_device_frame_interval_multiple = 4.0});
  Expect(!evaluation.passed, "device timestamp stall should fail evaluation");
  ExpectFailedCheck(evaluation, "timing.device_interval_maximum");
}

void PayloadMismatchFails() {
  CameraMetrics metrics = PassingMetrics();
  --metrics.payload_bytes;
  const Evaluation evaluation = EvaluateCamera(metrics, Thresholds{});
  Expect(!evaluation.passed, "payload mismatch should fail evaluation");
  ExpectFailedCheck(evaluation, "frames.payload_size_exact");
}

}  // namespace

int main() {
  PassingRunPassesAllChecks();
  FrameIdGapFails();
  TimeoutFails();
  LowFrameRateFails();
  HighFrameRateFails();
  TimestampResetFails();
  HostStallFails();
  DeviceStallFails();
  PayloadMismatchFails();
  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }
  return 0;
}
