#include "capture/audio/commanded_tone_analyzer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <limits>
#include <numbers>
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
constexpr double kQuantizationFloor = 1.0 / kPcmScale;

struct SampleWindows {
  std::uint64_t background_start = 0;
  std::uint64_t background_end = 0;
  std::uint64_t onset_start = 0;
  std::uint64_t onset_end = 0;
  std::uint64_t required_end = 0;
  std::uint64_t stimulus_samples = 0;
  std::uint64_t frame_samples = 0;
  std::uint64_t hop_samples = 0;
};

struct SpectralEvidence {
  double total_power = 0.0;
  double tone_power = 0.0;
  double tone_power_fraction = 0.0;
};

struct SignalEvidence {
  double peak = 0.0;
  double event_rms = 0.0;
  double background_rms = 0.0;
  double signal_rms = 0.0;
  double snr_decibels = 0.0;
};

struct ActiveRun {
  bool found = false;
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  std::uint64_t frames = 0;
  double summed_tone_power = 0.0;
};

struct Candidate {
  std::uint64_t start = 0;
  SpectralEvidence spectral;
};

struct DetectionEvidence {
  Candidate candidate;
  ActiveRun active_run;
  double background_rms = 0.0;
  std::uint64_t active_frames = 0;
};

std::string Number(double value) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(6) << value;
  return output.str();
}

void AddCheck(CommandedToneEvaluation *evaluation, std::string name, bool passed,
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

double PeakAmplitude(std::span<const std::int16_t> samples) {
  std::uint32_t peak = 0;
  for (const std::int16_t sample : samples) {
    peak = std::max(peak, Magnitude(sample));
  }
  return static_cast<double>(peak) / kPcmScale;
}

SignalEvidence MeasureSignal(std::span<const std::int16_t> event, double background_rms) {
  SignalEvidence evidence;
  evidence.peak = PeakAmplitude(event);
  evidence.event_rms = RootMeanSquare(event);
  evidence.background_rms = background_rms;
  const double event_power = evidence.event_rms * evidence.event_rms;
  const double background_power = background_rms * background_rms;
  evidence.signal_rms = std::sqrt(std::max(0.0, event_power - background_power));
  evidence.snr_decibels = 20.0 * std::log10(std::max(evidence.signal_rms, kQuantizationFloor) /
                                            std::max(background_rms, kQuantizationFloor));
  return evidence;
}

SpectralEvidence MeasureFrequency(std::span<const std::int16_t> samples,
                                  std::uint32_t sample_rate_hz, double frequency_hz) {
  SpectralEvidence evidence;
  if (samples.empty()) {
    return evidence;
  }

  double mean = 0.0;
  for (const std::int16_t sample : samples) {
    mean += static_cast<double>(sample);
  }
  mean /= static_cast<double>(samples.size());

  double squared_sum = 0.0;
  double real = 0.0;
  double imaginary = 0.0;
  const double radians_per_sample =
      2.0 * std::numbers::pi_v<double> * frequency_hz / sample_rate_hz;
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const double centered = static_cast<double>(samples[index]) - mean;
    const double phase = radians_per_sample * static_cast<double>(index);
    squared_sum += centered * centered;
    real += centered * std::cos(phase);
    imaginary += centered * std::sin(phase);
  }

  const auto sample_count = static_cast<double>(samples.size());
  evidence.total_power = squared_sum / (sample_count * kPcmScale * kPcmScale);
  evidence.tone_power = 2.0 * ((real * real) + (imaginary * imaginary)) /
                        (sample_count * sample_count * kPcmScale * kPcmScale);
  if (evidence.total_power > 0.0) {
    evidence.tone_power_fraction = std::min(1.0, evidence.tone_power / evidence.total_power);
  }
  return evidence;
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t *result) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

std::optional<std::uint64_t> DurationSamples(std::chrono::nanoseconds duration,
                                             std::uint32_t sample_rate_hz) {
  if (duration < std::chrono::nanoseconds::zero()) {
    return std::nullopt;
  }
  constexpr std::uint64_t kNanosecondsPerSecond = 1000000000;
  const auto duration_count = static_cast<std::uint64_t>(duration.count());
  if (sample_rate_hz != 0 &&
      duration_count > (std::numeric_limits<std::uint64_t>::max() - (kNanosecondsPerSecond - 1)) /
                           sample_rate_hz) {
    return std::nullopt;
  }
  const std::uint64_t product = duration_count * sample_rate_hz;
  return (product + (kNanosecondsPerSecond - 1)) / kNanosecondsPerSecond;
}

bool IsUnitInterval(double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; }

bool ValidateConfiguration(const CommandedToneStimulus &stimulus,
                           const CommandedToneThresholds &thresholds, std::string *error) {
  if (stimulus.lead < std::chrono::microseconds::zero() ||
      stimulus.duration <= std::chrono::microseconds::zero() || stimulus.frequency_hz == 0) {
    *error = "tone lead must be nonnegative, and duration and frequency must be positive";
    return false;
  }
  if (thresholds.onset_early_tolerance < std::chrono::milliseconds::zero() ||
      thresholds.maximum_additional_latency < std::chrono::milliseconds::zero() ||
      thresholds.onset_early_tolerance > stimulus.lead) {
    *error = "onset tolerances must be nonnegative and early tolerance cannot exceed tone lead";
    return false;
  }
  if (thresholds.background_duration <= std::chrono::milliseconds::zero() ||
      thresholds.background_guard < std::chrono::milliseconds::zero() ||
      thresholds.spectral_frame_duration <= std::chrono::milliseconds::zero() ||
      thresholds.spectral_hop_duration <= std::chrono::milliseconds::zero() ||
      thresholds.spectral_hop_duration > thresholds.spectral_frame_duration) {
    *error =
        "background and spectral frame durations must be positive, with hop no larger than "
        "the frame";
    return false;
  }
  if (thresholds.minimum_active_duration <= std::chrono::milliseconds::zero() ||
      thresholds.maximum_active_duration < thresholds.minimum_active_duration) {
    *error = "active-duration limits must be positive and ordered";
    return false;
  }
  if (!IsUnitInterval(thresholds.minimum_event_rms_normalized_amplitude) ||
      !std::isfinite(thresholds.minimum_signal_to_noise_decibels) ||
      thresholds.minimum_signal_to_noise_decibels < 0.0 ||
      !IsUnitInterval(thresholds.minimum_fundamental_power_fraction) ||
      !std::isfinite(thresholds.maximum_frequency_error_hz) ||
      thresholds.maximum_frequency_error_hz < 0.0 ||
      !IsUnitInterval(thresholds.maximum_event_clipped_fraction)) {
    *error =
        "amplitude, spectral, SNR, frequency, and clipping thresholds must be finite and "
        "within their documented nonnegative ranges";
    return false;
  }
  return true;
}

std::optional<SampleWindows> BuildWindows(const CommandedToneInput &input,
                                          const CommandedToneStimulus &stimulus,
                                          const CommandedToneThresholds &thresholds,
                                          std::string *error) {
  const auto lead = DurationSamples(stimulus.lead, input.sample_rate_hz);
  const auto duration = DurationSamples(stimulus.duration, input.sample_rate_hz);
  const auto early = DurationSamples(thresholds.onset_early_tolerance, input.sample_rate_hz);
  const auto latency = DurationSamples(thresholds.maximum_additional_latency, input.sample_rate_hz);
  const auto background = DurationSamples(thresholds.background_duration, input.sample_rate_hz);
  const auto guard = DurationSamples(thresholds.background_guard, input.sample_rate_hz);
  const auto frame = DurationSamples(thresholds.spectral_frame_duration, input.sample_rate_hz);
  const auto hop = DurationSamples(thresholds.spectral_hop_duration, input.sample_rate_hz);
  const auto maximum_active =
      DurationSamples(thresholds.maximum_active_duration, input.sample_rate_hz);
  if (!lead || !duration || !early || !latency || !background || !guard || !frame || !hop ||
      !maximum_active || *duration == 0 || *frame == 0 || *hop == 0) {
    *error = "configured durations overflow or round to zero at the requested sample rate";
    return std::nullopt;
  }

  std::uint64_t background_needed = 0;
  std::uint64_t nominal_onset = 0;
  std::uint64_t onset_end = 0;
  std::uint64_t scan_tail = 0;
  SampleWindows windows;
  const bool arithmetic_valid =
      CheckedAdd(*background, *guard, &background_needed) &&
      CheckedAdd(input.commanded_sample_offset, *lead, &nominal_onset) &&
      CheckedAdd(nominal_onset, *latency, &onset_end) &&
      CheckedAdd(*maximum_active, *frame, &scan_tail) &&
      CheckedAdd(onset_end, std::max(*duration, scan_tail), &windows.required_end);
  if (!arithmetic_valid || input.commanded_sample_offset < background_needed ||
      nominal_onset < *early) {
    *error =
        "sample-offset arithmetic overflowed or the capture lacks guarded pre-command "
        "background";
    return std::nullopt;
  }

  windows.background_end = input.commanded_sample_offset - *guard;
  windows.background_start = windows.background_end - *background;
  windows.onset_start = nominal_onset - *early;
  windows.onset_end = onset_end;
  windows.stimulus_samples = *duration;
  windows.frame_samples = *frame;
  windows.hop_samples = *hop;
  if (windows.required_end > input.mono_samples.size()) {
    *error = "capture ends at sample " + std::to_string(input.mono_samples.size()) +
             ", before required sample " + std::to_string(windows.required_end) +
             "; retain the complete command-relative tone and duration window";
    return std::nullopt;
  }
  return windows;
}

std::span<const std::int16_t> Slice(std::span<const std::int16_t> samples, std::uint64_t start,
                                    std::uint64_t size) {
  return samples.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(size));
}

Candidate FindBestCandidate(const CommandedToneInput &input, const CommandedToneStimulus &stimulus,
                            const SampleWindows &windows) {
  Candidate best = {.start = windows.onset_start, .spectral = {}};
  for (std::uint64_t start = windows.onset_start; start <= windows.onset_end;) {
    const SpectralEvidence spectral =
        MeasureFrequency(Slice(input.mono_samples, start, windows.stimulus_samples),
                         input.sample_rate_hz, stimulus.frequency_hz);
    if (spectral.tone_power > best.spectral.tone_power) {
      best = {.start = start, .spectral = spectral};
    }
    if (windows.onset_end - start < windows.hop_samples) {
      break;
    }
    start += windows.hop_samples;
  }
  return best;
}

bool FrameIsActive(const SpectralEvidence &spectral, const SignalEvidence &signal,
                   const CommandedToneThresholds &thresholds) {
  return signal.event_rms >= thresholds.minimum_event_rms_normalized_amplitude &&
         signal.snr_decibels >= thresholds.minimum_signal_to_noise_decibels &&
         spectral.tone_power_fraction >= thresholds.minimum_fundamental_power_fraction;
}

bool BetterRun(const ActiveRun &candidate, const ActiveRun &best) {
  const std::uint64_t candidate_duration = candidate.end - candidate.start;
  const std::uint64_t best_duration = best.end - best.start;
  return !best.found || candidate_duration > best_duration ||
         (candidate_duration == best_duration &&
          candidate.summed_tone_power > best.summed_tone_power);
}

ActiveRun FindActiveRun(const CommandedToneInput &input, const CommandedToneStimulus &stimulus,
                        const CommandedToneThresholds &thresholds, const SampleWindows &windows,
                        double background_rms, std::uint64_t *active_frames) {
  ActiveRun current;
  ActiveRun best;
  *active_frames = 0;
  const std::uint64_t final_frame_start = windows.required_end - windows.frame_samples;
  for (std::uint64_t start = windows.onset_start; start <= final_frame_start;) {
    const auto frame = Slice(input.mono_samples, start, windows.frame_samples);
    const SpectralEvidence spectral =
        MeasureFrequency(frame, input.sample_rate_hz, stimulus.frequency_hz);
    const SignalEvidence signal = MeasureSignal(frame, background_rms);
    if (FrameIsActive(spectral, signal, thresholds)) {
      ++*active_frames;
      if (!current.found) {
        current = {.found = true,
                   .start = start,
                   .end = start + windows.frame_samples,
                   .frames = 1,
                   .summed_tone_power = spectral.tone_power};
      } else {
        current.end = start + windows.frame_samples;
        ++current.frames;
        current.summed_tone_power += spectral.tone_power;
      }
    } else if (current.found) {
      if (BetterRun(current, best)) {
        best = current;
      }
      current = {};
    }
    if (final_frame_start - start < windows.hop_samples) {
      break;
    }
    start += windows.hop_samples;
  }
  if (current.found && BetterRun(current, best)) {
    best = current;
  }
  return best;
}

double EstimateFrequency(std::span<const std::int16_t> samples, const CommandedToneInput &input,
                         const CommandedToneStimulus &stimulus,
                         const CommandedToneThresholds &thresholds) {
  const double nyquist_hz = static_cast<double>(input.sample_rate_hz) / 2.0;
  const double search_radius_hz = std::max(1000.0, 2.0 * thresholds.maximum_frequency_error_hz);
  const double minimum_hz =
      std::max(1.0, static_cast<double>(stimulus.frequency_hz) - search_radius_hz);
  const double maximum_hz = std::min(std::nextafter(nyquist_hz, 0.0),
                                     static_cast<double>(stimulus.frequency_hz) + search_radius_hz);
  constexpr std::uint32_t kFrequencyStepHz = 10;
  const auto minimum_frequency_hz = static_cast<std::uint32_t>(std::ceil(minimum_hz));
  const auto maximum_frequency_hz = static_cast<std::uint32_t>(std::floor(maximum_hz));
  double best_frequency_hz = minimum_frequency_hz;
  double best_power = -1.0;
  for (std::uint32_t frequency_hz = minimum_frequency_hz; frequency_hz <= maximum_frequency_hz;
       frequency_hz += kFrequencyStepHz) {
    const double power = MeasureFrequency(samples, input.sample_rate_hz, frequency_hz).tone_power;
    if (power > best_power) {
      best_power = power;
      best_frequency_hz = frequency_hz;
    }
  }
  return best_frequency_hz;
}

void PopulateMeasurements(CommandedToneEvaluation *evaluation, const CommandedToneInput &input,
                          const CommandedToneStimulus &stimulus,
                          const CommandedToneThresholds &thresholds, const SampleWindows &windows,
                          const DetectionEvidence &detection) {
  auto &measurements = evaluation->measurements;
  const auto event = Slice(input.mono_samples, detection.candidate.start, windows.stimulus_samples);
  const SignalEvidence signal = MeasureSignal(event, detection.background_rms);
  measurements.analysis_start_sample_offset = detection.candidate.start;
  measurements.analysis_end_sample_offset = detection.candidate.start + windows.stimulus_samples;
  measurements.peak_normalized_amplitude = signal.peak;
  measurements.event_rms_normalized_amplitude = signal.event_rms;
  measurements.background_rms_normalized_amplitude = signal.background_rms;
  measurements.signal_rms_normalized_amplitude = signal.signal_rms;
  measurements.signal_to_noise_decibels = signal.snr_decibels;
  measurements.fundamental_power_fraction = detection.candidate.spectral.tone_power_fraction;
  measurements.estimated_frequency_hz = EstimateFrequency(event, input, stimulus, thresholds);
  measurements.frequency_error_hz =
      std::abs(measurements.estimated_frequency_hz - stimulus.frequency_hz);
  measurements.active_spectral_frames = detection.active_frames;
  if (detection.active_run.found) {
    measurements.detected_onset_sample_offset = detection.active_run.start;
    measurements.detected_end_sample_offset = detection.active_run.end;
    measurements.detected_onset_delay_samples =
        detection.active_run.start - input.commanded_sample_offset;
    measurements.detected_onset_delay_seconds =
        static_cast<double>(measurements.detected_onset_delay_samples) / input.sample_rate_hz;
    measurements.measured_active_duration_samples =
        detection.active_run.end - detection.active_run.start;
    measurements.measured_active_duration_seconds =
        static_cast<double>(measurements.measured_active_duration_samples) / input.sample_rate_hz;
  }
  measurements.event_clipped_samples = std::ranges::count_if(
      event, [](std::int16_t sample) { return Magnitude(sample) >= kClippedMagnitude; });
  measurements.event_clipped_fraction =
      static_cast<double>(measurements.event_clipped_samples) / static_cast<double>(event.size());
}

void AddEvidenceChecks(CommandedToneEvaluation *evaluation, const CommandedToneStimulus &stimulus,
                       const CommandedToneThresholds &thresholds) {
  const auto &measurements = evaluation->measurements;
  const bool energy_passed =
      measurements.event_rms_normalized_amplitude >=
          thresholds.minimum_event_rms_normalized_amplitude &&
      measurements.signal_to_noise_decibels >= thresholds.minimum_signal_to_noise_decibels;
  AddCheck(evaluation, "tone_energy", energy_passed,
           "tone-window RMS " + Number(measurements.event_rms_normalized_amplitude) +
               ", background RMS " + Number(measurements.background_rms_normalized_amplitude) +
               ", SNR " + Number(measurements.signal_to_noise_decibels) +
               " dB; require RMS >= " + Number(thresholds.minimum_event_rms_normalized_amplitude) +
               " and SNR >= " + Number(thresholds.minimum_signal_to_noise_decibels) + " dB" +
               (energy_passed ? ""
                              : "; verify speaker power/level, microphone channel, and "
                                "placement, then inspect the retained WAV"));

  const bool frequency_passed =
      measurements.fundamental_power_fraction >= thresholds.minimum_fundamental_power_fraction &&
      measurements.frequency_error_hz <= thresholds.maximum_frequency_error_hz;
  AddCheck(evaluation, "tone_frequency", frequency_passed,
           "expected " + std::to_string(stimulus.frequency_hz) + " Hz, estimated " +
               Number(measurements.estimated_frequency_hz) + " Hz (error " +
               Number(measurements.frequency_error_hz) + " Hz), fundamental power fraction " +
               Number(measurements.fundamental_power_fraction) +
               "; require error <= " + Number(thresholds.maximum_frequency_error_hz) +
               " Hz and fraction >= " + Number(thresholds.minimum_fundamental_power_fraction) +
               (frequency_passed ? ""
                                 : "; a broadband click or amplifier power-on pop is not the "
                                   "commanded tone"));

  const double minimum_duration_seconds =
      std::chrono::duration<double>(thresholds.minimum_active_duration).count();
  const double maximum_duration_seconds =
      std::chrono::duration<double>(thresholds.maximum_active_duration).count();
  const bool duration_passed =
      measurements.detected_onset_sample_offset.has_value() &&
      measurements.measured_active_duration_seconds >= minimum_duration_seconds &&
      measurements.measured_active_duration_seconds <= maximum_duration_seconds;
  AddCheck(evaluation, "tone_duration", duration_passed,
           "frequency-qualified active duration " +
               Number(measurements.measured_active_duration_seconds * 1000.0) + " ms across " +
               std::to_string(measurements.active_spectral_frames) + " active frames; require [" +
               Number(minimum_duration_seconds * 1000.0) + ", " +
               Number(maximum_duration_seconds * 1000.0) + "] ms" +
               (duration_passed ? ""
                                : "; verify the requested tone duration and reject short GPIO23 "
                                  "amplifier pops or stuck/overlong output"));

  const bool clipping_passed =
      measurements.event_clipped_fraction <= thresholds.maximum_event_clipped_fraction;
  AddCheck(evaluation, "event_clipping", clipping_passed,
           "tone-window clipped fraction " + Number(measurements.event_clipped_fraction) + " (" +
               std::to_string(measurements.event_clipped_samples) + " samples); maximum " +
               Number(thresholds.maximum_event_clipped_fraction) +
               (clipping_passed ? "" : "; lower the speaker level or microphone input gain"));
}

}  // namespace

CommandedToneEvaluation AnalyzeCommandedTone(CommandedToneInput input,
                                             const CommandedToneStimulus &stimulus,
                                             const CommandedToneThresholds &thresholds) {
  CommandedToneEvaluation evaluation;
  auto &measurements = evaluation.measurements;
  measurements.sample_rate_hz = input.sample_rate_hz;
  measurements.captured_samples = input.mono_samples.size();
  measurements.commanded_sample_offset = input.commanded_sample_offset;

  std::string configuration_error;
  const bool configuration_valid =
      ValidateConfiguration(stimulus, thresholds, &configuration_error);
  AddCheck(&evaluation, "configuration", configuration_valid,
           configuration_valid ? "commanded tone stimulus and thresholds are valid"
                               : "invalid commanded tone configuration: " + configuration_error);
  if (!configuration_valid) {
    return evaluation;
  }
  if (input.sample_rate_hz == 0 || input.mono_samples.empty() ||
      stimulus.frequency_hz >= input.sample_rate_hz / 2) {
    AddCheck(&evaluation, "analysis_window", false,
             "PCM must be nonempty with a nonzero sample rate and tone frequency below Nyquist");
    return evaluation;
  }

  std::string window_error;
  const auto windows = BuildWindows(input, stimulus, thresholds, &window_error);
  if (!windows) {
    AddCheck(&evaluation, "analysis_window", false, std::move(window_error));
    return evaluation;
  }
  measurements.background_start_sample_offset = windows->background_start;
  measurements.background_end_sample_offset = windows->background_end;
  measurements.onset_search_start_sample_offset = windows->onset_start;
  measurements.onset_search_end_sample_offset = windows->onset_end;
  AddCheck(&evaluation, "analysis_window", true,
           "guarded background [" + std::to_string(windows->background_start) + ", " +
               std::to_string(windows->background_end) + "), tone onset search [" +
               std::to_string(windows->onset_start) + ", " + std::to_string(windows->onset_end) +
               "]");

  const double background_rms =
      RootMeanSquare(Slice(input.mono_samples, windows->background_start,
                           windows->background_end - windows->background_start));
  DetectionEvidence detection;
  detection.candidate = FindBestCandidate(input, stimulus, *windows);
  detection.background_rms = background_rms;
  detection.active_run = FindActiveRun(input, stimulus, thresholds, *windows, background_rms,
                                       &detection.active_frames);
  PopulateMeasurements(&evaluation, input, stimulus, thresholds, *windows, detection);
  AddEvidenceChecks(&evaluation, stimulus, thresholds);
  evaluation.passed = std::ranges::all_of(evaluation.checks,
                                          [](const AudioHilCheck &check) { return check.passed; });
  return evaluation;
}

}  // namespace swing_capture
