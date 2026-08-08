#include "capture/audio/audio_hil_metrics.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>
#include <utility>

namespace swing_capture {
namespace {

std::string Number(double value) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(6) << value;
  return output.str();
}

void AddCheck(AudioHilEvaluation *evaluation, std::string name, bool passed, std::string message) {
  evaluation->checks.push_back({
      .name = std::move(name),
      .passed = passed,
      .message = std::move(message),
  });
}

}  // namespace

AudioHilEvaluation EvaluateAudioHil(const AudioHilMeasurements &measurements,
                                    const AudioHilThresholds &thresholds) {
  AudioHilEvaluation evaluation;

  AddCheck(&evaluation, "source_capture", measurements.source_error.empty(),
           measurements.source_error.empty() ? "PCM source completed without an error"
                                             : "PCM source error: " + measurements.source_error);

  const bool enough_samples = measurements.received_frames >= measurements.target_frames;
  AddCheck(&evaluation, "sample_count", enough_samples,
           "received " + std::to_string(measurements.received_frames) +
               " samples; required at least " + std::to_string(measurements.target_frames));

  const bool has_duration = measurements.sample_rate_hz > 0 && measurements.received_frames > 0;
  if (has_duration) {
    evaluation.captured_audio_seconds = static_cast<double>(measurements.received_frames) /
                                        static_cast<double>(measurements.sample_rate_hz);
    evaluation.cadence_ratio = measurements.elapsed_seconds / evaluation.captured_audio_seconds;
  }
  const bool cadence_passed = has_duration && std::isfinite(measurements.elapsed_seconds) &&
                              measurements.elapsed_seconds >= 0.0 &&
                              std::isfinite(evaluation.cadence_ratio) &&
                              evaluation.cadence_ratio >= thresholds.minimum_cadence_ratio &&
                              evaluation.cadence_ratio <= thresholds.maximum_cadence_ratio;
  if (has_duration) {
    AddCheck(&evaluation, "real_time_cadence", cadence_passed,
             "elapsed/audio ratio " + Number(evaluation.cadence_ratio) + " (" +
                 Number(measurements.elapsed_seconds) + " s / " +
                 Number(evaluation.captured_audio_seconds) + " s); required range [" +
                 Number(thresholds.minimum_cadence_ratio) + ", " +
                 Number(thresholds.maximum_cadence_ratio) + "]");
  } else {
    AddCheck(&evaluation, "real_time_cadence", false,
             "cadence unavailable because sample rate or received sample "
             "count is zero");
  }

  const bool signal_values_valid =
      std::isfinite(measurements.rms_amplitude) && std::isfinite(measurements.peak_amplitude);
  const bool non_silence_passed = measurements.received_frames > 0 &&
                                  measurements.nonzero_samples > 0 && signal_values_valid &&
                                  measurements.rms_amplitude >= thresholds.minimum_rms_amplitude &&
                                  measurements.peak_amplitude >= thresholds.minimum_peak_amplitude;
  AddCheck(&evaluation, "non_silence", non_silence_passed,
           "RMS " + Number(measurements.rms_amplitude) + " (minimum " +
               Number(thresholds.minimum_rms_amplitude) + "), peak " +
               Number(measurements.peak_amplitude) + " (minimum " +
               Number(thresholds.minimum_peak_amplitude) + "), nonzero samples " +
               std::to_string(measurements.nonzero_samples));

  if (measurements.received_frames > 0) {
    evaluation.clipped_fraction = static_cast<double>(measurements.clipped_samples) /
                                  static_cast<double>(measurements.received_frames);
  }
  const bool clipping_passed = measurements.received_frames > 0 &&
                               std::isfinite(evaluation.clipped_fraction) &&
                               evaluation.clipped_fraction <= thresholds.maximum_clipped_fraction;
  AddCheck(&evaluation, "clipping", clipping_passed,
           "clipped fraction " + Number(evaluation.clipped_fraction) + " (" +
               std::to_string(measurements.clipped_samples) + "/" +
               std::to_string(measurements.received_frames) + "); maximum " +
               Number(thresholds.maximum_clipped_fraction));

  evaluation.passed = std::ranges::all_of(evaluation.checks,
                                          [](const AudioHilCheck &check) { return check.passed; });
  return evaluation;
}

}  // namespace swing_capture
