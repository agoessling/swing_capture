#include "capture/audio/commanded_click_analyzer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>

#include "capture/audio/audio_hil_metrics.h"

namespace swing_capture {
namespace {

constexpr double kPcmScale = 32768.0;
constexpr std::uint32_t kClippedMagnitude = 32767;

std::string Number(double value) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(6) << value;
  return output.str();
}

void AddCheck(CommandedClickEvaluation *evaluation, std::string name, bool passed,
              std::string message) {
  evaluation->checks.push_back({
      .name = std::move(name),
      .passed = passed,
      .message = std::move(message),
  });
}

std::uint32_t Magnitude(std::int16_t sample) {
  const std::int32_t widened = sample;
  return static_cast<std::uint32_t>(widened < 0 ? -widened : widened);
}

double RootMeanSquare(std::span<const std::int16_t> samples) {
  if (samples.empty()) {
    return 0.0;
  }
  double squared_sum = 0.0;
  for (const std::int16_t sample : samples) {
    const double magnitude = Magnitude(sample);
    squared_sum += magnitude * magnitude;
  }
  return std::sqrt(squared_sum / static_cast<double>(samples.size())) / kPcmScale;
}

struct Peak {
  std::size_t offset = 0;
  std::uint32_t magnitude = 0;
};

Peak FindPeak(std::span<const std::int16_t> samples, std::size_t base_offset) {
  Peak peak = {.offset = base_offset, .magnitude = 0};
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const std::uint32_t magnitude = Magnitude(samples[index]);
    if (magnitude > peak.magnitude) {
      peak = {.offset = base_offset + index, .magnitude = magnitude};
    }
  }
  return peak;
}

bool IsFiniteThreshold(double value) { return std::isfinite(value) && value >= 0.0; }

bool ThresholdsAreValid(const CommandedClickThresholds &thresholds, std::string *message) {
  if (thresholds.minimum_peak_delay < std::chrono::milliseconds::zero() ||
      thresholds.maximum_peak_delay <= thresholds.minimum_peak_delay) {
    *message = "peak-delay window must be nonnegative and have positive duration";
    return false;
  }
  if (thresholds.background_duration <= std::chrono::milliseconds::zero() ||
      thresholds.background_guard < std::chrono::milliseconds::zero()) {
    *message = "background duration must be positive and its guard must be nonnegative";
    return false;
  }
  if (thresholds.event_radius <= std::chrono::milliseconds::zero()) {
    *message = "event RMS radius must be positive";
    return false;
  }
  if (!IsFiniteThreshold(thresholds.minimum_peak_normalized_amplitude) ||
      thresholds.minimum_peak_normalized_amplitude > 1.0 ||
      !IsFiniteThreshold(thresholds.minimum_event_rms_normalized_amplitude) ||
      thresholds.minimum_event_rms_normalized_amplitude > 1.0 ||
      !IsFiniteThreshold(thresholds.minimum_signal_to_noise_decibels) ||
      !IsFiniteThreshold(thresholds.maximum_event_clipped_fraction) ||
      thresholds.maximum_event_clipped_fraction > 1.0) {
    *message =
        "amplitude and clipping thresholds must be finite values in [0, 1], and the "
        "minimum SNR must be finite and nonnegative";
    return false;
  }
  return true;
}

std::optional<std::uint64_t> DurationSamples(std::chrono::milliseconds duration,
                                             std::uint32_t sample_rate_hz) {
  const auto duration_count = duration.count();
  if (duration_count < 0) {
    return std::nullopt;
  }
  const auto unsigned_duration = static_cast<std::uint64_t>(duration_count);
  constexpr std::uint64_t kMillisecondsPerSecond = 1000;
  if (sample_rate_hz != 0 && unsigned_duration > (std::numeric_limits<std::uint64_t>::max() -
                                                  (kMillisecondsPerSecond - 1)) /
                                                     sample_rate_hz) {
    return std::nullopt;
  }
  const std::uint64_t product = unsigned_duration * sample_rate_hz;
  return (product + (kMillisecondsPerSecond - 1)) / kMillisecondsPerSecond;
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t *result) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

std::string PeakFailureMessage(const CommandedClickMeasurements &measurements,
                               const CommandedClickThresholds &thresholds) {
  std::string message = "commanded-window peak " + Number(measurements.peak_normalized_amplitude) +
                        " is below minimum " +
                        Number(thresholds.minimum_peak_normalized_amplitude) + ". ";
  const bool capture_peak_outside =
      measurements.capture_peak_sample_offset.has_value() &&
      (*measurements.capture_peak_sample_offset < measurements.search_start_sample_offset ||
       *measurements.capture_peak_sample_offset >= measurements.search_end_sample_offset) &&
      measurements.capture_peak_normalized_amplitude >=
          thresholds.minimum_peak_normalized_amplitude;
  if (capture_peak_outside) {
    message += "A stronger transient was found outside the commanded window at sample " +
               std::to_string(*measurements.capture_peak_sample_offset) +
               "; check the command-to-PCM timestamp mapping or widen the latency window.";
  } else {
    message += "Verify speaker power and volume, microphone selection/channel, and placement.";
  }
  return message;
}

}  // namespace

CommandedClickEvaluation AnalyzeCommandedClick(CommandedClickInput input,
                                               const CommandedClickThresholds &thresholds) {
  CommandedClickEvaluation evaluation;
  evaluation.measurements.sample_rate_hz = input.sample_rate_hz;
  evaluation.measurements.captured_samples = input.mono_samples.size();
  evaluation.measurements.commanded_sample_offset = input.commanded_sample_offset;

  std::string configuration_error;
  const bool configuration_valid = ThresholdsAreValid(thresholds, &configuration_error);
  AddCheck(&evaluation, "configuration", configuration_valid,
           configuration_valid ? "commanded click thresholds are valid"
                               : "invalid commanded click thresholds: " + configuration_error);
  if (!configuration_valid) {
    return evaluation;
  }

  if (input.sample_rate_hz == 0 || input.mono_samples.empty()) {
    AddCheck(&evaluation, "analysis_window", false,
             "PCM sample rate and mono sample capture must both be nonzero");
    return evaluation;
  }

  const auto minimum_delay_samples =
      DurationSamples(thresholds.minimum_peak_delay, input.sample_rate_hz);
  const auto maximum_delay_samples =
      DurationSamples(thresholds.maximum_peak_delay, input.sample_rate_hz);
  const auto background_samples =
      DurationSamples(thresholds.background_duration, input.sample_rate_hz);
  const auto background_guard_samples =
      DurationSamples(thresholds.background_guard, input.sample_rate_hz);
  const auto event_radius_samples = DurationSamples(thresholds.event_radius, input.sample_rate_hz);
  if (!minimum_delay_samples || !maximum_delay_samples || !background_samples ||
      !background_guard_samples || !event_radius_samples) {
    AddCheck(&evaluation, "analysis_window", false,
             "configured durations overflow sample-offset arithmetic at the requested sample rate");
    return evaluation;
  }

  std::uint64_t search_start = 0;
  std::uint64_t search_end = 0;
  const bool search_offsets_valid =
      CheckedAdd(input.commanded_sample_offset, *minimum_delay_samples, &search_start) &&
      CheckedAdd(input.commanded_sample_offset, *maximum_delay_samples, &search_end);
  std::uint64_t required_background_samples = 0;
  const bool background_size_valid =
      CheckedAdd(*background_guard_samples, *background_samples, &required_background_samples);
  const bool background_offset_valid =
      background_size_valid && input.commanded_sample_offset >= required_background_samples;
  const std::uint64_t background_end =
      input.commanded_sample_offset >= *background_guard_samples
          ? input.commanded_sample_offset - *background_guard_samples
          : 0;
  const std::uint64_t background_start =
      background_end >= *background_samples ? background_end - *background_samples : 0;
  evaluation.measurements.search_start_sample_offset = search_start;
  evaluation.measurements.search_end_sample_offset = search_end;
  evaluation.measurements.background_start_sample_offset = background_start;
  evaluation.measurements.background_end_sample_offset = background_end;

  const bool complete_window = search_offsets_valid && background_offset_valid &&
                               search_start < search_end && search_end <= input.mono_samples.size();
  std::string window_message;
  if (complete_window) {
    window_message = "background [" + std::to_string(background_start) + ", " +
                     std::to_string(background_end) + "), commanded search [" +
                     std::to_string(search_start) + ", " + std::to_string(search_end) + ")";
  } else {
    window_message =
        "capture does not contain the complete guarded background and commanded "
        "event windows; capture at least " +
        std::to_string(required_background_samples) + " samples before and through sample " +
        std::to_string(search_end) + " after mapping the command time";
  }
  AddCheck(&evaluation, "analysis_window", complete_window, std::move(window_message));
  if (!complete_window) {
    return evaluation;
  }

  const Peak capture_peak = FindPeak(input.mono_samples, 0);
  evaluation.measurements.capture_peak_sample_offset = capture_peak.offset;
  evaluation.measurements.capture_peak_normalized_amplitude =
      static_cast<double>(capture_peak.magnitude) / kPcmScale;

  const auto search = input.mono_samples.subspan(
      static_cast<std::size_t>(search_start), static_cast<std::size_t>(search_end - search_start));
  const Peak event_peak = FindPeak(search, static_cast<std::size_t>(search_start));
  evaluation.measurements.detected_peak_sample_offset = event_peak.offset;
  evaluation.measurements.detected_delay_samples =
      event_peak.offset - input.commanded_sample_offset;
  evaluation.measurements.detected_delay_seconds =
      static_cast<double>(evaluation.measurements.detected_delay_samples) / input.sample_rate_hz;
  evaluation.measurements.peak_normalized_amplitude =
      static_cast<double>(event_peak.magnitude) / kPcmScale;

  const std::uint64_t peak_offset = event_peak.offset;
  const std::uint64_t event_start = peak_offset - search_start < *event_radius_samples
                                        ? search_start
                                        : peak_offset - *event_radius_samples;
  const std::uint64_t right_event_samples =
      std::min(*event_radius_samples, search_end - peak_offset - 1);
  const std::uint64_t event_end = peak_offset + right_event_samples + 1;
  evaluation.measurements.event_start_sample_offset = event_start;
  evaluation.measurements.event_end_sample_offset = event_end;

  const auto background =
      input.mono_samples.subspan(static_cast<std::size_t>(background_start),
                                 static_cast<std::size_t>(background_end - background_start));
  const auto event = input.mono_samples.subspan(static_cast<std::size_t>(event_start),
                                                static_cast<std::size_t>(event_end - event_start));
  evaluation.measurements.background_rms_normalized_amplitude = RootMeanSquare(background);
  evaluation.measurements.event_rms_normalized_amplitude = RootMeanSquare(event);
  const double background_power = evaluation.measurements.background_rms_normalized_amplitude *
                                  evaluation.measurements.background_rms_normalized_amplitude;
  const double event_power = evaluation.measurements.event_rms_normalized_amplitude *
                             evaluation.measurements.event_rms_normalized_amplitude;
  evaluation.measurements.signal_rms_normalized_amplitude =
      std::sqrt(std::max(0.0, event_power - background_power));
  constexpr double kQuantizationFloor = 1.0 / kPcmScale;
  evaluation.measurements.signal_to_noise_decibels =
      20.0 * std::log10(std::max(evaluation.measurements.signal_rms_normalized_amplitude,
                                 kQuantizationFloor) /
                        std::max(evaluation.measurements.background_rms_normalized_amplitude,
                                 kQuantizationFloor));

  evaluation.measurements.event_clipped_samples = std::ranges::count_if(
      event, [](std::int16_t sample) { return Magnitude(sample) >= kClippedMagnitude; });
  evaluation.measurements.event_clipped_fraction =
      static_cast<double>(evaluation.measurements.event_clipped_samples) /
      static_cast<double>(event.size());

  const bool peak_passed = evaluation.measurements.peak_normalized_amplitude >=
                           thresholds.minimum_peak_normalized_amplitude;
  AddCheck(&evaluation, "commanded_peak", peak_passed,
           peak_passed
               ? "peak " + Number(evaluation.measurements.peak_normalized_amplitude) +
                     " at sample " + std::to_string(event_peak.offset) + ", delay " +
                     std::to_string(evaluation.measurements.detected_delay_samples) + " samples"
               : PeakFailureMessage(evaluation.measurements, thresholds));

  const bool event_rms_passed = evaluation.measurements.event_rms_normalized_amplitude >=
                                thresholds.minimum_event_rms_normalized_amplitude;
  AddCheck(&evaluation, "event_energy", event_rms_passed,
           "event RMS " + Number(evaluation.measurements.event_rms_normalized_amplitude) +
               " over samples [" + std::to_string(event_start) + ", " + std::to_string(event_end) +
               "); minimum " + Number(thresholds.minimum_event_rms_normalized_amplitude) +
               (event_rms_passed ? "" : "; verify speaker output and microphone input gain"));

  const bool snr_passed = evaluation.measurements.signal_to_noise_decibels >=
                          thresholds.minimum_signal_to_noise_decibels;
  AddCheck(&evaluation, "signal_to_noise", snr_passed,
           "SNR " + Number(evaluation.measurements.signal_to_noise_decibels) + " dB, event RMS " +
               Number(evaluation.measurements.event_rms_normalized_amplitude) +
               ", background RMS " +
               Number(evaluation.measurements.background_rms_normalized_amplitude) + "; minimum " +
               Number(thresholds.minimum_signal_to_noise_decibels) + " dB" +
               (snr_passed ? ""
                           : "; reduce ambient noise, raise click level, or move the microphone "
                             "closer to the speaker"));

  const bool clipping_passed =
      evaluation.measurements.event_clipped_fraction <= thresholds.maximum_event_clipped_fraction;
  AddCheck(&evaluation, "event_clipping", clipping_passed,
           "event clipped fraction " + Number(evaluation.measurements.event_clipped_fraction) +
               " (" + std::to_string(evaluation.measurements.event_clipped_samples) + "/" +
               std::to_string(event.size()) + "); maximum " +
               Number(thresholds.maximum_event_clipped_fraction) +
               (clipping_passed ? "" : "; lower speaker volume or microphone input gain"));

  evaluation.passed = std::ranges::all_of(evaluation.checks,
                                          [](const AudioHilCheck &check) { return check.passed; });
  return evaluation;
}

}  // namespace swing_capture
