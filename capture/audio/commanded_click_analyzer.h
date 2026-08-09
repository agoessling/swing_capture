#ifndef SWING_CAPTURE_CAPTURE_AUDIO_COMMANDED_CLICK_ANALYZER_H_
#define SWING_CAPTURE_CAPTURE_AUDIO_COMMANDED_CLICK_ANALYZER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "capture/audio/audio_hil_metrics.h"

namespace swing_capture {

// Defines a bounded verification window for one externally commanded speaker
// click. The command offset is supplied separately because the serial command
// and PCM capture are owned by the host orchestration layer.
struct CommandedClickThresholds {
  // Search after, rather than before, the sample corresponding to the host's
  // command time. The broad default accommodates provisional arecord and USB
  // serial buffering; retained evidence should be used to narrow it later.
  std::chrono::milliseconds minimum_peak_delay = std::chrono::milliseconds(0);
  std::chrono::milliseconds maximum_peak_delay = std::chrono::milliseconds(500);

  // Estimate background from a quiet interval before the command, excluding a
  // guard interval that could contain command-related handling noise.
  std::chrono::milliseconds background_duration = std::chrono::milliseconds(250);
  std::chrono::milliseconds background_guard = std::chrono::milliseconds(10);

  // RMS and clipping are measured in a short window centered on the strongest
  // sample in the commanded-event search window.
  std::chrono::milliseconds event_radius = std::chrono::milliseconds(5);

  double minimum_peak_normalized_amplitude = 0.02;
  double minimum_event_rms_normalized_amplitude = 0.005;
  double minimum_signal_to_noise_decibels = 12.0;
  double maximum_event_clipped_fraction = 0.05;
};

struct CommandedClickMeasurements {
  std::uint32_t sample_rate_hz = 0;
  std::uint64_t captured_samples = 0;
  std::uint64_t commanded_sample_offset = 0;
  std::uint64_t search_start_sample_offset = 0;
  std::uint64_t search_end_sample_offset = 0;
  std::uint64_t background_start_sample_offset = 0;
  std::uint64_t background_end_sample_offset = 0;
  std::uint64_t event_start_sample_offset = 0;
  std::uint64_t event_end_sample_offset = 0;
  std::optional<std::uint64_t> detected_peak_sample_offset;
  std::optional<std::uint64_t> capture_peak_sample_offset;
  std::uint64_t detected_delay_samples = 0;
  double detected_delay_seconds = 0.0;
  double peak_normalized_amplitude = 0.0;
  double capture_peak_normalized_amplitude = 0.0;
  double event_rms_normalized_amplitude = 0.0;
  double background_rms_normalized_amplitude = 0.0;
  double signal_rms_normalized_amplitude = 0.0;
  double signal_to_noise_decibels = 0.0;
  std::uint64_t event_clipped_samples = 0;
  double event_clipped_fraction = 0.0;
};

struct CommandedClickEvaluation {
  bool passed = false;
  CommandedClickMeasurements measurements;
  std::vector<AudioHilCheck> checks;
};

struct CommandedClickInput {
  std::span<const std::int16_t> mono_samples;
  std::uint32_t sample_rate_hz = 0;
  std::uint64_t commanded_sample_offset = 0;
};

// Analyzes a complete mono S16_LE capture containing exactly one commanded
// stimulus opportunity. It never searches outside the configured event window
// for a passing click. A stronger capture-wide peak is retained only to make a
// timing-window failure actionable.
[[nodiscard]] CommandedClickEvaluation AnalyzeCommandedClick(
    CommandedClickInput input, const CommandedClickThresholds &thresholds = {});

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_AUDIO_COMMANDED_CLICK_ANALYZER_H_
