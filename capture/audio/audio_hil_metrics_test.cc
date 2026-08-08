#include "capture/audio/audio_hil_metrics.h"

#include <cassert>
#include <string_view>

namespace {

using swing_capture::AudioHilCheck;
using swing_capture::AudioHilEvaluation;
using swing_capture::AudioHilMeasurements;
using swing_capture::EvaluateAudioHil;

AudioHilMeasurements PassingMeasurements() {
  return {
      .sample_rate_hz = 32000,
      .target_frames = 96000,
      .received_frames = 96256,
      .nonzero_samples = 95000,
      .clipped_samples = 0,
      .peak_amplitude = 0.045,
      .rms_amplitude = 0.00368,
      .elapsed_seconds = 3.38,
      .source_error = {},
  };
}

const AudioHilCheck &FindCheck(const AudioHilEvaluation &evaluation, std::string_view name) {
  for (const auto &check : evaluation.checks) {
    if (check.name == name) {
      return check;
    }
  }
  assert(false);
  return evaluation.checks.front();
}

void BaselinePassesEveryExplicitCheck() {
  const auto evaluation = EvaluateAudioHil(PassingMeasurements());

  assert(evaluation.passed);
  assert(evaluation.checks.size() == 5);
  for (const auto &check : evaluation.checks) {
    assert(check.passed);
    assert(!check.message.empty());
  }
  assert(evaluation.cadence_ratio > 1.12);
  assert(evaluation.cadence_ratio < 1.13);
}

void SilentInputFailsSignalGate() {
  auto measurements = PassingMeasurements();
  measurements.nonzero_samples = 0;
  measurements.peak_amplitude = 0.0;
  measurements.rms_amplitude = 0.0;

  const auto evaluation = EvaluateAudioHil(measurements);

  assert(!evaluation.passed);
  assert(!FindCheck(evaluation, "non_silence").passed);
  assert(FindCheck(evaluation, "real_time_cadence").passed);
}

void ImplausiblyFastOrSlowInputFailsCadenceGate() {
  auto fast = PassingMeasurements();
  fast.elapsed_seconds = 2.0;
  const auto fast_evaluation = EvaluateAudioHil(fast);
  assert(!fast_evaluation.passed);
  assert(!FindCheck(fast_evaluation, "real_time_cadence").passed);

  auto slow = PassingMeasurements();
  slow.elapsed_seconds = 6.0;
  const auto slow_evaluation = EvaluateAudioHil(slow);
  assert(!slow_evaluation.passed);
  assert(!FindCheck(slow_evaluation, "real_time_cadence").passed);
}

void MaterialClippingFailsButBoundaryPasses() {
  auto boundary = PassingMeasurements();
  boundary.received_frames = 96000;
  boundary.clipped_samples = 96;
  boundary.elapsed_seconds = 3.1;
  const auto boundary_evaluation = EvaluateAudioHil(boundary);
  assert(boundary_evaluation.passed);
  assert(FindCheck(boundary_evaluation, "clipping").passed);

  auto clipped = boundary;
  clipped.clipped_samples = 97;
  const auto clipped_evaluation = EvaluateAudioHil(clipped);
  assert(!clipped_evaluation.passed);
  assert(!FindCheck(clipped_evaluation, "clipping").passed);
}

void CaptureAndCountFailuresAreExplicit() {
  auto measurements = PassingMeasurements();
  measurements.received_frames = 100;
  measurements.source_error = "test read error";

  const auto evaluation = EvaluateAudioHil(measurements);

  assert(!evaluation.passed);
  assert(!FindCheck(evaluation, "source_capture").passed);
  assert(!FindCheck(evaluation, "sample_count").passed);
}

}  // namespace

int main() {
  BaselinePassesEveryExplicitCheck();
  SilentInputFailsSignalGate();
  ImplausiblyFastOrSlowInputFailsCadenceGate();
  MaterialClippingFailsButBoundaryPasses();
  CaptureAndCountFailuresAreExplicit();
  return 0;
}
