#ifndef SWING_CAPTURE_CAPTURE_AUDIO_AUDIO_HIL_METRICS_H_
#define SWING_CAPTURE_CAPTURE_AUDIO_AUDIO_HIL_METRICS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace swing_capture {

struct AudioHilThresholds {
  // A live C925e baseline captured 3.008 seconds of samples in 3.38 seconds.
  // These deliberately broad limits allow process/device startup overhead
  // while still rejecting buffered/non-real-time or badly stalled capture.
  double minimum_cadence_ratio = 0.75;
  double maximum_cadence_ratio = 1.75;

  // The same quiet-room baseline measured RMS 0.00368 and peak 0.045.
  // Requiring both rejects zeros and near-digital-silence with ample margin.
  double minimum_rms_amplitude = 0.0001;
  double minimum_peak_amplitude = 0.001;

  // A few saturated samples from an impulsive strike are acceptable, but
  // clipping more than 0.1% of the capture is considered material.
  double maximum_clipped_fraction = 0.001;
};

struct AudioHilMeasurements {
  std::uint32_t sample_rate_hz = 0;
  std::uint64_t target_frames = 0;
  std::uint64_t received_frames = 0;
  std::uint64_t nonzero_samples = 0;
  std::uint64_t clipped_samples = 0;
  double peak_amplitude = 0.0;
  double rms_amplitude = 0.0;
  double elapsed_seconds = 0.0;
  std::string source_error;
};

struct AudioHilCheck {
  std::string name;
  bool passed = false;
  std::string message;
};

struct AudioHilEvaluation {
  bool passed = false;
  double captured_audio_seconds = 0.0;
  double cadence_ratio = 0.0;
  double clipped_fraction = 0.0;
  std::vector<AudioHilCheck> checks;
};

[[nodiscard]] AudioHilEvaluation EvaluateAudioHil(const AudioHilMeasurements &measurements,
                                                  const AudioHilThresholds &thresholds = {});

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_AUDIO_AUDIO_HIL_METRICS_H_
