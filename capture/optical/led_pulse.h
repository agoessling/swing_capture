#ifndef SWING_CAPTURE_CAPTURE_OPTICAL_LED_PULSE_H_
#define SWING_CAPTURE_CAPTURE_OPTICAL_LED_PULSE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "capture/image/image_quality.h"

namespace swing_capture::optical {

struct BayerRg8FrameView {
  image::Raw8ImageView image;
  std::uint64_t frame_id = 0;
  std::uint64_t device_timestamp = 0;
};

struct PixelRegion {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

struct LedPulseOptions {
  // The stimulus protocol must leave the LED off for these leading frames.
  // Their per-pixel mean is the camera-local baseline.
  std::size_t baseline_frame_count = 5;

  // Candidate regions overlap so that a small LED cannot be hidden by one
  // arbitrary grid boundary. Only red samples (even x, even y) in the
  // BayerRG8 mosaic contribute to the response.
  std::uint32_t region_width = 16;
  std::uint32_t region_height = 16;
  std::uint32_t region_stride = 8;
  // The local response is compared with a surrounding clipped annulus this
  // many pixels wide. A per-frame global affine illumination fit is applied
  // first, so the annulus handles remaining spatially varying room light.
  std::uint32_t background_margin = 16;

  // These thresholds apply after global-illumination correction. Fractions and
  // mean deltas are local excess over the surrounding background annulus.
  std::uint8_t minimum_red_sample_delta = 24;
  std::uint32_t minimum_changed_red_samples = 3;
  double minimum_changed_red_fraction = 0.04;
  double minimum_mean_positive_red_delta = 4.0;

  // Support is deliberately weaker than the strict/high-confidence `active`
  // classification below. It establishes that the scheduled LED response is
  // continuously present even when only a few frames clear the strict limits.
  double minimum_support_changed_red_fraction = 0.04;
  double minimum_support_mean_positive_red_delta = 2.0;

  std::size_t minimum_active_frames = 2;
  std::size_t maximum_pulse_span_frames = 32;
  std::size_t maximum_internal_inactive_frames = 1;

  // A locator pulse can constrain the qualification pulse to one previously
  // observed region. This prevents a stronger reflection elsewhere in the
  // frame from replacing the physical feature selected by the locator.
  std::optional<PixelRegion> locked_region;

  // When supplied, qualification uses a continuous-response matched window
  // near this expected start rather than requiring every pulse frame to cross
  // the per-frame threshold. The ordinary threshold-run detector remains the
  // default when expected_start_frame_index is absent.
  std::optional<std::size_t> expected_start_frame_index;
  std::size_t expected_start_tolerance_frames = 2;
  std::size_t minimum_matched_pulse_frames = 9;
  std::size_t maximum_matched_pulse_frames = 11;
  // `active` is the legacy name for a strict/high-confidence frame.
  std::size_t minimum_matched_active_frames = 3;
  std::size_t minimum_matched_contiguous_supported_frames = 9;
  double minimum_matched_mean_positive_red_delta = 4.0;
  double minimum_matched_changed_red_fraction = 0.04;
};

struct LedFrameDiagnostic {
  std::uint64_t frame_id = 0;
  std::uint64_t device_timestamp = 0;
  std::uint32_t changed_red_samples = 0;
  std::uint32_t red_samples = 0;
  double local_changed_red_fraction = 0.0;
  double background_changed_red_fraction = 0.0;
  double changed_red_fraction = 0.0;
  double local_mean_positive_red_delta = 0.0;
  double background_mean_positive_red_delta = 0.0;
  double mean_positive_red_delta = 0.0;
  double global_illumination_scale = 1.0;
  double global_illumination_offset = 0.0;
  bool baseline_frame = false;
  bool supported = false;
  // Strict/high-confidence response. The name is retained for compatibility
  // with threshold-run callers and stored artifacts.
  bool active = false;
};

struct LedPulseResult {
  bool detected = false;
  std::string diagnostic;
  PixelRegion selected_region;
  std::vector<LedFrameDiagnostic> frames;

  std::size_t pulse_start_frame_index = 0;
  std::size_t pulse_end_frame_index = 0;
  // Compatibility count for strict/high-confidence `active` frames.
  std::size_t pulse_active_frame_count = 0;
  std::size_t pulse_span_frame_count = 0;
  std::uint64_t start_device_timestamp = 0;
  std::uint64_t end_device_timestamp_exclusive = 0;
  std::uint64_t nominal_frame_interval_ticks = 0;
  std::uint64_t duration_ticks = 0;
  double duration_seconds = 0.0;
  std::uint64_t missing_frame_ids = 0;

  // Always identifies the strongest response in the selected/locked region,
  // including rejected runs, so failure artifacts can show the real evidence.
  std::size_t strongest_response_frame_index = 0;
  double strongest_mean_positive_red_delta = 0.0;

  // Populated when expected_start_frame_index enables matched-window
  // qualification. These counts explicitly distinguish continuous support
  // from the strict/high-confidence anchors inside the matched span.
  bool matched_window_used = false;
  std::size_t matched_supported_frame_count = 0;
  std::size_t matched_high_confidence_frame_count = 0;
  std::size_t matched_longest_contiguous_supported_frame_count = 0;
  double matched_mean_positive_red_delta = 0.0;
  double matched_changed_red_fraction = 0.0;
  double matched_background_mean_positive_red_delta = 0.0;
  double matched_background_changed_red_fraction = 0.0;
};

// Locates and times one short red LED pulse. Timestamps and durations remain
// in this camera's device-clock domain; comparing two cameras requires mapping
// both results through their independently fitted DeviceClockMapper instances.
//
// Throws std::invalid_argument for malformed geometry, payloads, options,
// nonmonotonic frame IDs/timestamps, or an unusable timestamp frequency.
[[nodiscard]] LedPulseResult AnalyzeLedPulse(std::span<const BayerRg8FrameView> frames,
                                             std::uint64_t timestamp_ticks_per_second,
                                             const LedPulseOptions &options = {});

}  // namespace swing_capture::optical

#endif  // SWING_CAPTURE_CAPTURE_OPTICAL_LED_PULSE_H_
