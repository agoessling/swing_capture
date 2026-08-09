#ifndef SWING_CAPTURE_CAPTURE_AUDIO_COMMANDED_TONE_ANALYZER_H_
#define SWING_CAPTURE_CAPTURE_AUDIO_COMMANDED_TONE_ANALYZER_H_

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "capture/audio/audio_hil_metrics.h"

namespace swing_capture {

// Describes the waveform requested from the fixture. The default matches the
// Feather HIL command used by the station fixture.
struct CommandedToneStimulus {
  std::chrono::microseconds lead = std::chrono::microseconds(100000);
  std::chrono::microseconds duration = std::chrono::microseconds(20000);
  std::uint32_t frequency_hz = 2000;
};

struct CommandedToneThresholds {
  // The tone begins after the firmware lead. A small early tolerance accounts
  // for a host sample-offset snapshot immediately before the serial write. The
  // upper tolerance includes USB serial handling, device scheduling, acoustic
  // travel, microphone buffering, and their provisional timestamp mapping.
  std::chrono::milliseconds onset_early_tolerance = std::chrono::milliseconds(25);
  std::chrono::milliseconds maximum_additional_latency = std::chrono::milliseconds(250);

  std::chrono::milliseconds background_duration = std::chrono::milliseconds(250);
  std::chrono::milliseconds background_guard = std::chrono::milliseconds(10);
  std::chrono::milliseconds spectral_frame_duration = std::chrono::milliseconds(4);
  std::chrono::milliseconds spectral_hop_duration = std::chrono::milliseconds(1);

  double minimum_event_rms_normalized_amplitude = 0.005;
  double minimum_signal_to_noise_decibels = 12.0;
  double minimum_fundamental_power_fraction = 0.30;
  double maximum_frequency_error_hz = 250.0;
  std::chrono::milliseconds minimum_active_duration = std::chrono::milliseconds(12);
  std::chrono::milliseconds maximum_active_duration = std::chrono::milliseconds(35);
  double maximum_event_clipped_fraction = 0.05;
};

struct CommandedToneMeasurements {
  std::uint32_t sample_rate_hz = 0;
  std::uint64_t captured_samples = 0;
  std::uint64_t commanded_sample_offset = 0;
  std::uint64_t background_start_sample_offset = 0;
  std::uint64_t background_end_sample_offset = 0;
  std::uint64_t onset_search_start_sample_offset = 0;
  std::uint64_t onset_search_end_sample_offset = 0;
  std::optional<std::uint64_t> detected_onset_sample_offset;
  std::optional<std::uint64_t> detected_end_sample_offset;
  std::uint64_t detected_onset_delay_samples = 0;
  double detected_onset_delay_seconds = 0.0;
  std::uint64_t measured_active_duration_samples = 0;
  double measured_active_duration_seconds = 0.0;
  std::uint64_t analysis_start_sample_offset = 0;
  std::uint64_t analysis_end_sample_offset = 0;
  double peak_normalized_amplitude = 0.0;
  double event_rms_normalized_amplitude = 0.0;
  double background_rms_normalized_amplitude = 0.0;
  double signal_rms_normalized_amplitude = 0.0;
  double signal_to_noise_decibels = 0.0;
  double fundamental_power_fraction = 0.0;
  double estimated_frequency_hz = 0.0;
  double frequency_error_hz = 0.0;
  std::uint64_t active_spectral_frames = 0;
  std::uint64_t event_clipped_samples = 0;
  double event_clipped_fraction = 0.0;
};

struct CommandedToneEvaluation {
  bool passed = false;
  CommandedToneMeasurements measurements;
  std::vector<AudioHilCheck> checks;
};

struct CommandedToneInput {
  std::span<const std::int16_t> mono_samples;
  std::uint32_t sample_rate_hz = 0;

  // Atomic completed-sample snapshot taken immediately before the host starts
  // writing the tone command. This is deliberately a sample-domain boundary;
  // absolute host/audio clock calibration is not required for this check.
  std::uint64_t commanded_sample_offset = 0;
};

// Verifies energy, expected-frequency concentration, and active duration in a
// bounded command-relative window. This distinguishes the requested tone from
// a broadband ambient impulse or the Prop-Maker amplifier power-on pop. It
// cannot distinguish another matching tone that occurs in the same window.
[[nodiscard]] CommandedToneEvaluation AnalyzeCommandedTone(
    CommandedToneInput input, const CommandedToneStimulus &stimulus = {},
    const CommandedToneThresholds &thresholds = {});

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_AUDIO_COMMANDED_TONE_ANALYZER_H_
