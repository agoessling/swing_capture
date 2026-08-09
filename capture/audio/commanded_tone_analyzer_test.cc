#include "capture/audio/commanded_tone_analyzer.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

#include "capture/audio/audio_hil_metrics.h"

namespace {

using swing_capture::AnalyzeCommandedTone;
using swing_capture::AudioHilCheck;
using swing_capture::CommandedToneEvaluation;
using swing_capture::CommandedToneInput;
using swing_capture::CommandedToneStimulus;
using swing_capture::CommandedToneThresholds;

constexpr std::uint32_t kSampleRateHz = 32000;
constexpr std::size_t kCommandOffset = 12000;
constexpr std::size_t kNominalToneOffset = kCommandOffset + 3200;
constexpr std::size_t kCaptureSamples = 40000;

const AudioHilCheck &FindCheck(const CommandedToneEvaluation &evaluation, std::string_view name) {
  const auto check = std::ranges::find(evaluation.checks, name, &AudioHilCheck::name);
  assert(check != evaluation.checks.end());
  return *check;
}

std::vector<std::int16_t> QuietCapture() {
  std::vector<std::int16_t> samples(kCaptureSamples);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    samples[index] = index % 2 == 0 ? 100 : -100;
  }
  return samples;
}

void AddSquareTone(std::vector<std::int16_t> *samples, std::size_t start,
                   std::chrono::microseconds duration, std::uint32_t frequency_hz,
                   std::int16_t amplitude = 8000) {
  const std::size_t sample_count =
      static_cast<std::size_t>(std::chrono::duration<double>(duration).count() * kSampleRateHz);
  for (std::size_t index = 0; index < sample_count; ++index) {
    const double phase = 2.0 * std::numbers::pi_v<double> * frequency_hz *
                         static_cast<double>(index) / kSampleRateHz;
    (*samples)[start + index] = std::sin(phase) >= 0.0 ? amplitude : -amplitude;
  }
}

CommandedToneEvaluation Analyze(std::span<const std::int16_t> samples) {
  return AnalyzeCommandedTone({
      .mono_samples = samples,
      .sample_rate_hz = kSampleRateHz,
      .commanded_sample_offset = kCommandOffset,
  });
}

void RequestedSquareTonePassesWithFrequencyDurationAndTimingEvidence() {
  auto samples = QuietCapture();
  constexpr std::size_t kSerialAndAcousticDelaySamples = 256;
  constexpr std::size_t kToneStart = kNominalToneOffset + kSerialAndAcousticDelaySamples;
  AddSquareTone(&samples, kToneStart, std::chrono::milliseconds(20), 2000);

  const auto evaluation = Analyze(samples);

  assert(evaluation.passed);
  assert(FindCheck(evaluation, "tone_energy").passed);
  assert(FindCheck(evaluation, "tone_frequency").passed);
  assert(FindCheck(evaluation, "tone_duration").passed);
  assert(evaluation.measurements.detected_onset_sample_offset.has_value());
  assert(*evaluation.measurements.detected_onset_sample_offset <= kToneStart);
  assert(kToneStart - *evaluation.measurements.detected_onset_sample_offset <= 96);
  assert(evaluation.measurements.estimated_frequency_hz >= 1970.0);
  assert(evaluation.measurements.estimated_frequency_hz <= 2030.0);
  assert(evaluation.measurements.fundamental_power_fraction > 0.70);
  assert(evaluation.measurements.measured_active_duration_seconds >= 0.019);
  assert(evaluation.measurements.measured_active_duration_seconds <= 0.024);
  assert(evaluation.measurements.signal_to_noise_decibels > 30.0);
}

void AmbientImpulseIsNotAcceptedAsTheCommandedTone() {
  auto samples = QuietCapture();
  const std::size_t impulse = kNominalToneOffset + 200;
  samples[impulse - 1] = 12000;
  samples[impulse] = 28000;
  samples[impulse + 1] = -16000;

  const auto evaluation = Analyze(samples);

  assert(!evaluation.passed);
  assert(!FindCheck(evaluation, "tone_frequency").passed);
  assert(!FindCheck(evaluation, "tone_duration").passed);
  assert(FindCheck(evaluation, "tone_frequency").message.find("broadband click") !=
         std::string_view::npos);
}

void AmplifierPowerOnPopIsNotAcceptedAsTheCommandedTone() {
  auto samples = QuietCapture();
  constexpr std::size_t kPopStart = kNominalToneOffset - 400;
  constexpr std::size_t kPopSamples = 480;
  for (std::size_t index = 0; index < kPopSamples; ++index) {
    const double decay = std::exp(-static_cast<double>(index) / 75.0);
    samples[kPopStart + index] = static_cast<std::int16_t>(24000.0 * decay);
  }

  const auto evaluation = Analyze(samples);

  assert(!evaluation.passed);
  assert(!FindCheck(evaluation, "tone_frequency").passed);
  assert(!FindCheck(evaluation, "tone_duration").passed);
  assert(evaluation.measurements.fundamental_power_fraction < 0.30);
}

void WrongFrequencyToneReportsMeasuredMismatch() {
  auto samples = QuietCapture();
  AddSquareTone(&samples, kNominalToneOffset, std::chrono::milliseconds(20), 3000);

  const auto evaluation = Analyze(samples);

  assert(!evaluation.passed);
  const auto &frequency = FindCheck(evaluation, "tone_frequency");
  assert(!frequency.passed);
  assert(evaluation.measurements.frequency_error_hz > 800.0);
  assert(frequency.message.find("expected 2000 Hz") != std::string_view::npos);
}

void TooShortAndOverlongTonesFailDuration() {
  {
    auto samples = QuietCapture();
    AddSquareTone(&samples, kNominalToneOffset, std::chrono::milliseconds(4), 2000);
    const auto evaluation = Analyze(samples);
    assert(!evaluation.passed);
    assert(!FindCheck(evaluation, "tone_duration").passed);
    assert(evaluation.measurements.measured_active_duration_seconds < 0.012);
  }

  {
    auto samples = QuietCapture();
    AddSquareTone(&samples, kNominalToneOffset, std::chrono::milliseconds(80), 2000);
    const auto evaluation = Analyze(samples);
    assert(!evaluation.passed);
    assert(FindCheck(evaluation, "tone_frequency").passed);
    assert(!FindCheck(evaluation, "tone_duration").passed);
    assert(evaluation.measurements.measured_active_duration_seconds > 0.035);
  }
}

void MatchingToneOutsideBoundedOnsetWindowFails() {
  auto samples = QuietCapture();
  constexpr std::size_t kLateTone = kCommandOffset + 12800;
  AddSquareTone(&samples, kLateTone, std::chrono::milliseconds(20), 2000);

  const auto evaluation = Analyze(samples);

  assert(!evaluation.passed);
  assert(!FindCheck(evaluation, "tone_energy").passed);
  assert(!FindCheck(evaluation, "tone_duration").passed);
}

void ClippedToneFailsWithoutLosingIdentityEvidence() {
  auto samples = QuietCapture();
  AddSquareTone(&samples, kNominalToneOffset, std::chrono::milliseconds(20), 2000,
                std::numeric_limits<std::int16_t>::max());

  const auto evaluation = Analyze(samples);

  assert(!evaluation.passed);
  assert(FindCheck(evaluation, "tone_frequency").passed);
  assert(FindCheck(evaluation, "tone_duration").passed);
  assert(!FindCheck(evaluation, "event_clipping").passed);
  assert(evaluation.measurements.event_clipped_fraction > 0.95);
}

void InvalidOrIncompleteCaptureFailsBeforeSignalAnalysis() {
  const std::vector<std::int16_t> short_capture(1000);
  const auto incomplete = AnalyzeCommandedTone({
      .mono_samples = short_capture,
      .sample_rate_hz = kSampleRateHz,
      .commanded_sample_offset = 100,
  });
  assert(!incomplete.passed);
  assert(!FindCheck(incomplete, "analysis_window").passed);

  CommandedToneThresholds invalid_thresholds;
  invalid_thresholds.minimum_fundamental_power_fraction = 1.5;
  const auto invalid = AnalyzeCommandedTone(
      CommandedToneInput{
          .mono_samples = short_capture,
          .sample_rate_hz = kSampleRateHz,
          .commanded_sample_offset = 100,
      },
      CommandedToneStimulus{}, invalid_thresholds);
  assert(!invalid.passed);
  assert(!FindCheck(invalid, "configuration").passed);
}

}  // namespace

int main() {
  RequestedSquareTonePassesWithFrequencyDurationAndTimingEvidence();
  AmbientImpulseIsNotAcceptedAsTheCommandedTone();
  AmplifierPowerOnPopIsNotAcceptedAsTheCommandedTone();
  WrongFrequencyToneReportsMeasuredMismatch();
  TooShortAndOverlongTonesFailDuration();
  MatchingToneOutsideBoundedOnsetWindowFails();
  ClippedToneFailsWithoutLosingIdentityEvidence();
  InvalidOrIncompleteCaptureFailsBeforeSignalAnalysis();
  return 0;
}
