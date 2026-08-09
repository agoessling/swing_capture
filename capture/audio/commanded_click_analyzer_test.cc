#include "capture/audio/commanded_click_analyzer.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "capture/audio/audio_hil_metrics.h"

namespace {

using swing_capture::AnalyzeCommandedClick;
using swing_capture::AudioHilCheck;
using swing_capture::CommandedClickEvaluation;
using swing_capture::CommandedClickInput;
using swing_capture::CommandedClickThresholds;

constexpr std::uint32_t kSampleRateHz = 32000;
constexpr std::size_t kCommandOffset = kSampleRateHz / 2;

const AudioHilCheck &FindCheck(const CommandedClickEvaluation &evaluation, std::string_view name) {
  const auto check = std::ranges::find(evaluation.checks, name, &AudioHilCheck::name);
  assert(check != evaluation.checks.end());
  return *check;
}

std::vector<std::int16_t> QuietCapture() {
  std::vector<std::int16_t> samples(kSampleRateHz * 2);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    samples[index] = index % 2 == 0 ? 100 : -100;
  }
  return samples;
}

CommandedClickEvaluation Analyze(std::span<const std::int16_t> samples,
                                 const CommandedClickThresholds &thresholds = {}) {
  return AnalyzeCommandedClick(
      CommandedClickInput{
          .mono_samples = samples,
          .sample_rate_hz = kSampleRateHz,
          .commanded_sample_offset = kCommandOffset,
      },
      thresholds);
}

void AddClick(std::vector<std::int16_t> *samples, std::size_t peak_offset,
              std::int16_t peak = 20000) {
  constexpr std::int16_t kShoulder = 8000;
  (*samples)[peak_offset - 2] = 2000;
  (*samples)[peak_offset - 1] = kShoulder;
  (*samples)[peak_offset] = peak;
  (*samples)[peak_offset + 1] = -kShoulder;
  (*samples)[peak_offset + 2] = -2000;
}

void CommandedClickPassesWithMeasuredDelayAndFiniteMetrics() {
  auto samples = QuietCapture();
  constexpr std::size_t kDelaySamples = 2560;
  AddClick(&samples, kCommandOffset + kDelaySamples);

  const auto evaluation = Analyze(samples);

  assert(evaluation.passed);
  assert(evaluation.measurements.detected_peak_sample_offset == kCommandOffset + kDelaySamples);
  assert(evaluation.measurements.detected_delay_samples == kDelaySamples);
  assert(evaluation.measurements.detected_delay_seconds == 0.08);
  assert(evaluation.measurements.peak_normalized_amplitude > 0.61);
  assert(evaluation.measurements.event_rms_normalized_amplitude > 0.039);
  assert(evaluation.measurements.background_rms_normalized_amplitude > 0.003);
  assert(evaluation.measurements.signal_to_noise_decibels > 20.0);
  assert(evaluation.measurements.event_clipped_samples == 0);
  assert(FindCheck(evaluation, "commanded_peak").passed);
  assert(FindCheck(evaluation, "signal_to_noise").passed);
}

void MissingClickReportsSpeakerAndMicrophoneActions() {
  const auto evaluation = Analyze(QuietCapture());

  assert(!evaluation.passed);
  assert(!FindCheck(evaluation, "commanded_peak").passed);
  assert(FindCheck(evaluation, "commanded_peak").message.find("speaker power") !=
         std::string_view::npos);
  assert(!FindCheck(evaluation, "event_energy").passed);
  assert(!FindCheck(evaluation, "signal_to_noise").passed);
}

void StrongTransientOutsideCommandWindowReportsTimingAction() {
  auto samples = QuietCapture();
  AddClick(&samples, kCommandOffset + 20000, 28000);

  const auto evaluation = Analyze(samples);

  assert(!evaluation.passed);
  const auto &peak_check = FindCheck(evaluation, "commanded_peak");
  assert(!peak_check.passed);
  assert(peak_check.message.find("outside the commanded window") != std::string_view::npos);
  assert(peak_check.message.find("timestamp mapping") != std::string_view::npos);
}

void HighBackgroundFailsSnrDespiteAnInWindowPeak() {
  auto samples = QuietCapture();
  std::fill(samples.begin(), samples.begin() + kCommandOffset, 5000);
  constexpr std::size_t kPeakOffset = kCommandOffset + 2000;
  AddClick(&samples, kPeakOffset, 20000);

  const auto evaluation = Analyze(samples);

  assert(!evaluation.passed);
  assert(FindCheck(evaluation, "commanded_peak").passed);
  const auto &snr_check = FindCheck(evaluation, "signal_to_noise");
  assert(!snr_check.passed);
  assert(snr_check.message.find("ambient noise") != std::string_view::npos);
}

void ClippedClickReportsLevelAdjustment() {
  auto samples = QuietCapture();
  constexpr std::size_t kPeakOffset = kCommandOffset + 2000;
  constexpr std::size_t kClippedSamples = 40;
  std::fill_n(samples.begin() + kPeakOffset - (kClippedSamples / 2), kClippedSamples,
              std::numeric_limits<std::int16_t>::max());

  const auto evaluation = Analyze(samples);

  assert(!evaluation.passed);
  assert(FindCheck(evaluation, "commanded_peak").passed);
  const auto &clipping_check = FindCheck(evaluation, "event_clipping");
  assert(!clipping_check.passed);
  assert(clipping_check.message.find("lower speaker volume") != std::string_view::npos);
  assert(evaluation.measurements.event_clipped_samples == kClippedSamples);
}

void IncompleteOrInvalidWindowsFailBeforeAnalysis() {
  const std::vector<std::int16_t> short_capture(1000);
  const auto incomplete = AnalyzeCommandedClick({
      .mono_samples = short_capture,
      .sample_rate_hz = kSampleRateHz,
      .commanded_sample_offset = 100,
  });
  assert(!incomplete.passed);
  assert(!FindCheck(incomplete, "analysis_window").passed);
  assert(FindCheck(incomplete, "analysis_window").message.find("capture at least") !=
         std::string_view::npos);

  auto invalid_thresholds = CommandedClickThresholds{};
  invalid_thresholds.maximum_peak_delay = std::chrono::milliseconds(0);
  const auto invalid = AnalyzeCommandedClick(
      CommandedClickInput{
          .mono_samples = short_capture,
          .sample_rate_hz = kSampleRateHz,
          .commanded_sample_offset = 100,
      },
      invalid_thresholds);
  assert(!invalid.passed);
  assert(!FindCheck(invalid, "configuration").passed);
}

void NegativeFullScaleSampleDoesNotOverflow() {
  auto samples = QuietCapture();
  constexpr std::size_t kPeakOffset = kCommandOffset + 2000;
  samples[kPeakOffset] = std::numeric_limits<std::int16_t>::min();
  auto thresholds = CommandedClickThresholds{};
  thresholds.maximum_event_clipped_fraction = 0.0;

  const auto evaluation = Analyze(samples, thresholds);

  assert(evaluation.measurements.peak_normalized_amplitude == 1.0);
  assert(evaluation.measurements.detected_peak_sample_offset == kPeakOffset);
  assert(!FindCheck(evaluation, "event_clipping").passed);
}

void DigitalSilenceBackgroundUsesFiniteQuantizationFloor() {
  std::vector<std::int16_t> samples(kSampleRateHz * 2, 0);
  constexpr std::size_t kPeakOffset = kCommandOffset + 2000;
  AddClick(&samples, kPeakOffset);

  const auto evaluation = Analyze(samples);

  assert(evaluation.passed);
  assert(evaluation.measurements.background_rms_normalized_amplitude == 0.0);
  assert(std::isfinite(evaluation.measurements.signal_to_noise_decibels));
  assert(evaluation.measurements.signal_to_noise_decibels > 60.0);
}

}  // namespace

int main() {
  CommandedClickPassesWithMeasuredDelayAndFiniteMetrics();
  MissingClickReportsSpeakerAndMicrophoneActions();
  StrongTransientOutsideCommandWindowReportsTimingAction();
  HighBackgroundFailsSnrDespiteAnInWindowPeak();
  ClippedClickReportsLevelAdjustment();
  IncompleteOrInvalidWindowsFailBeforeAnalysis();
  NegativeFullScaleSampleDoesNotOverflow();
  DigitalSilenceBackgroundUsesFiniteQuantizationFloor();
  return 0;
}
