#ifndef SWING_CAPTURE_CAPTURE_OPTICAL_RGB_SWING_H_
#define SWING_CAPTURE_CAPTURE_OPTICAL_RGB_SWING_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "capture/optical/led_pulse.h"
#include "capture/optical/white_impact_acceptance.h"

namespace swing_capture::optical {

struct Rgb8 {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;

  friend bool operator==(const Rgb8 &, const Rgb8 &) = default;
};

enum class RgbSwingPhase : std::uint8_t {
  kBaseline,
  kBrightnessProbe,
  kPreImpact,
  kImpact,
  kPostImpact,
};

// One deterministic Feather state. Times are already mapped into the same
// host steady-clock domain as the camera frame timeline. Brightness is a
// separate scalar because a short qualification sweep can repeat one color at
// several levels without changing the discrete color state.
struct FeatherRgbStep {
  std::chrono::steady_clock::time_point start;
  std::chrono::steady_clock::time_point end_exclusive;
  Rgb8 color;
  std::uint8_t brightness = 0;
  RgbSwingPhase phase = RgbSwingPhase::kBaseline;
};

enum class RgbFramePhase : std::uint8_t {
  kOutsideSchedule,
  kExposureTransition,
  kStableStep,
};

struct BayerChannelMeasurement {
  double local_positive_delta = 0.0;
  double background_positive_delta = 0.0;
  double local_excess = 0.0;
  // RMS temporal noise learned across stable OFF frames from the same
  // per-channel (ROI mean - surrounding-annulus mean) aggregate as the signal.
  // Frame-level estimation retains spatial/common-mode correlations. Current
  // illumination spill is reported only by background_positive_delta and
  // bloom_fraction; it is not temporal noise.
  double background_noise_rms = 0.0;
};

struct RgbFrameEvidence {
  std::uint64_t frame_id = 0;
  std::uint64_t device_timestamp = 0;
  std::chrono::steady_clock::time_point mapped_host_time;
  std::chrono::steady_clock::time_point conservative_exposure_begin;
  std::chrono::steady_clock::time_point conservative_exposure_end;
  RgbFramePhase phase = RgbFramePhase::kOutsideSchedule;
  std::optional<std::size_t> expected_step_index;

  BayerChannelMeasurement red;
  BayerChannelMeasurement green;
  BayerChannelMeasurement blue;
  double signal_delta = 0.0;
  double signal_to_background_noise = 0.0;
  double saturated_fraction = 0.0;
  double bloom_fraction = 0.0;

  // Nearest scheduled chromaticity, retained as diagnostic evidence. Close or
  // quantized palette entries can be indistinguishable, so expected_color_match
  // compares the frame directly with its scheduled step instead of requiring
  // this nearest entry to be byte-identical.
  std::optional<Rgb8> decoded_color;
  double expected_color_distance = 0.0;
  bool expected_color_match = false;
};

struct RgbStateEvidence {
  std::size_t step_index = 0;
  RgbSwingPhase phase = RgbSwingPhase::kBaseline;
  Rgb8 expected_color;
  std::uint8_t brightness = 0;
  std::size_t stable_frame_count = 0;
  std::size_t matching_frame_count = 0;
  double matching_fraction = 0.0;
  double mean_signal_delta = 0.0;
  double mean_expected_color_distance = 0.0;
  double maximum_saturated_fraction = 0.0;
  double maximum_bloom_fraction = 0.0;
  bool required_for_swing = false;
  bool passed = false;
};

struct RgbTransitionEvidence {
  std::size_t from_step_index = 0;
  std::size_t to_step_index = 0;
  std::chrono::steady_clock::time_point scheduled_transition;
  std::optional<std::size_t> last_matching_from_frame_index;
  std::optional<std::size_t> first_matching_to_frame_index;
  std::optional<std::chrono::steady_clock::time_point> observed_bracket_begin;
  std::optional<std::chrono::steady_clock::time_point> observed_bracket_end;
  bool ordered = false;
};

struct BrightnessCandidateEvidence {
  std::uint8_t brightness = 0;
  std::size_t stable_frame_count = 0;
  double minimum_signal_delta = 0.0;
  double minimum_signal_to_background_noise = 0.0;
  double maximum_saturated_fraction = 0.0;
  double maximum_bloom_fraction = 0.0;
  bool safe_for_camera = false;
  std::vector<std::string> rejection_reasons;
};

struct BayerColorCalibration {
  // Multipliers applied to temporal R/G/B excess before chromatic decoding.
  // They are estimated from the strongest usable white brightness-probe state.
  // Unit multipliers with automatic=false mean the schedule offered no safe
  // white reference, so decoding remains in raw sensor-response space.
  double red_scale = 1.0;
  double green_scale = 1.0;
  double blue_scale = 1.0;
  bool automatic = false;
  std::uint8_t source_brightness = 0;
  std::size_t source_frame_count = 0;
};

struct RgbSwingAnalysisOptions {
  // A mapped device timestamp may describe any phase within the exposure.
  // Treat mapped_time +/- (exposure_duration + schedule_timing_uncertainty +
  // frame_delivery_latency_bound) as the conservative support. Only frames
  // whose complete support lies inside one scheduled step are decoded; edge
  // frames remain explicit transition evidence. The delivery bound makes the
  // current unobservable camera/readout/USB bias explicit rather than folding
  // it into a permissive color threshold.
  std::chrono::steady_clock::duration exposure_duration = std::chrono::microseconds(500);
  std::chrono::steady_clock::duration schedule_timing_uncertainty = std::chrono::milliseconds(2);
  std::chrono::steady_clock::duration frame_delivery_latency_bound{};
  std::uint32_t background_margin = 12;
  std::size_t minimum_baseline_frames = 3;
  std::size_t minimum_stable_state_frames = 2;
  std::size_t minimum_stable_impact_frames =
      kDefaultWhiteImpactAcceptancePolicy.minimum_stable_frames;
  std::size_t minimum_brightness_probe_frames = 2;

  double minimum_state_matching_fraction =
      kDefaultWhiteImpactAcceptancePolicy.minimum_matching_fraction;
  double maximum_color_distance = 0.38;
  double minimum_signal_delta = kDefaultWhiteImpactAcceptancePolicy.minimum_signal_delta;
  double minimum_signal_to_background_noise = 5.0;

  std::uint8_t saturation_threshold = 250;
  double maximum_saturated_fraction =
      kDefaultWhiteImpactAcceptancePolicy.maximum_saturated_fraction;
  std::uint8_t bloom_delta_threshold = 18;
  double maximum_bloom_fraction = kDefaultWhiteImpactAcceptancePolicy.maximum_bloom_fraction;
};

struct RgbSwingAnalysis {
  std::string camera_id;
  PixelRegion fixture_neopixel_region;
  bool passed = false;
  std::string diagnostic;
  BayerColorCalibration color_calibration;
  // AnalyzeRgbWhiteImpact retains schedule membership but evaluates color only
  // for stable white-impact frames. AnalyzeRgbSwing may evaluate every state
  // for generic optical tooling.
  std::vector<RgbFrameEvidence> frames;
  // AnalyzeRgbWhiteImpact contains only white-impact qualification evidence.
  std::vector<RgbStateEvidence> states;
  // Impact-only analysis leaves color transition evidence empty.
  std::vector<RgbTransitionEvidence> transitions;
  std::optional<std::size_t> first_impact_frame_index;
  std::optional<std::size_t> last_impact_frame_index;
  std::vector<BrightnessCandidateEvidence> brightness_candidates;
};

struct RgbSwingCameraInput {
  std::string camera_id;
  std::span<const BayerRg8FrameView> frames;
  std::span<const std::chrono::steady_clock::time_point> mapped_host_times;
  PixelRegion fixture_neopixel_region;
  std::span<const FeatherRgbStep> schedule;
  std::optional<BayerColorCalibration> color_calibration;
};

// Analyzes one camera without hardware access or ownership. BayerRG8 channels
// are sampled on their native RG/GB phases; green is averaged over both green
// sites. A same-pixel OFF baseline removes static scene texture before the ROI
// is compared with its surrounding annulus. The schedule must include at least
// one stable black baseline, pre-impact and post-impact context, and one
// contiguous white impact interval. Only the white impact is an automated
// acceptance requirement; pre/post colors remain diagnostic human context.
//
// Throws std::invalid_argument for malformed frames, timeline, ROI, schedule,
// or options. Valid but optically insufficient evidence returns passed=false
// with deterministic frame/state diagnostics.
[[nodiscard]] RgbSwingAnalysis AnalyzeRgbSwing(const RgbSwingCameraInput &input,
                                               const RgbSwingAnalysisOptions &options = {});

// Production HIL qualification path. Uses the complete schedule only to
// classify stable exposure intervals and locate impact; it does not decode or
// compare any pre/post wheel hue and returns exactly the white-impact state.
[[nodiscard]] RgbSwingAnalysis AnalyzeRgbWhiteImpact(const RgbSwingCameraInput &input,
                                                     const RgbSwingAnalysisOptions &options = {});

struct RgbBrightnessCalibrationOptions {
  RgbSwingAnalysisOptions optical;
  std::uint32_t region_width = 16;
  std::uint32_t region_height = 16;
  std::uint32_t region_stride = 8;
  std::size_t minimum_probe_levels = 2;
  // A 70 ms probe observed by the bounded ~30 fps preview sampler can have
  // only one frame outside all conservative transition guards. The monotonic
  // multi-level sweep and two-camera intersection provide the repetition.
  std::size_t minimum_frames_per_probe = 1;
  double minimum_channel_delta = 4.0;
  double minimum_monotonic_fraction = 0.75;

  // The bounded preview sampler timestamps frames in the camera's mapped host
  // domain, which can retain a constant readout/USB delivery bias relative to
  // the Feather schedule. Search for a correction to add to every supplied
  // mapped_host_time before schedule comparison. The sweep is deterministic
  // and inclusive over a symmetric range kept below one 70 ms calibration
  // step, so a fixed camera delivery delay can be found without silently
  // accepting an adjacent probe interval.
  std::chrono::steady_clock::duration maximum_absolute_schedule_offset =
      std::chrono::milliseconds(50);
  std::chrono::steady_clock::duration schedule_offset_resolution = std::chrono::microseconds(500);
  std::chrono::steady_clock::duration maximum_schedule_offset_uncertainty =
      std::chrono::milliseconds(5);
  double minimum_schedule_offset_fit_score = 0.90;
  double minimum_schedule_offset_score_margin = 1.0e-6;
};

struct RgbBrightnessCalibrationInput {
  std::string camera_id;
  std::span<const BayerRg8FrameView> frames;
  std::span<const std::chrono::steady_clock::time_point> mapped_host_times;
  std::span<const FeatherRgbStep> schedule;
};

struct RgbScheduleOffsetEstimate {
  bool available = false;
  // Add this signed correction to each supplied camera mapped_host_time before
  // comparing it with the Feather schedule. For example, camera timestamps
  // delayed by +12 ms should produce approximately -12 ms here.
  std::chrono::steady_clock::duration mapped_time_correction{};
  // Maximum distance from the selected correction to either edge of its
  // connected near-best score component, including half a search-resolution
  // cell and the configured Feather step-timing uncertainty.
  std::chrono::steady_clock::duration uncertainty{};
  double fit_score = 0.0;
  // Best candidate outside the connected near-best score shoulder. A
  // competing solution must be separated from the selected peak by a score
  // valley; no fixed time-radius exclusion can hide one.
  double next_best_score = 0.0;
  std::size_t candidates_evaluated = 0;
  // True when the connected near-best peak reaches a search edge, even if the
  // exactly-equal plateau itself does not.
  bool best_plateau_touches_search_edge = false;
  std::string diagnostic;
};

namespace rgb_swing_internal {

// Implementation seam kept visible for deterministic score-topology tests.
// The component contains the exactly-best plateau plus every adjacent sample
// whose score remains within `score_margin` of that plateau's score.
struct ConnectedOffsetScoreComponent {
  std::size_t first_index = 0;
  std::size_t last_index = 0;
  bool touches_search_edge = false;
  double next_best_score = 0.0;
};

[[nodiscard]] ConnectedOffsetScoreComponent FindConnectedOffsetScoreComponent(
    std::span<const double> scores, std::size_t first_best_index, std::size_t last_best_index,
    double score_margin);

}  // namespace rgb_swing_internal

struct RgbBrightnessCalibration {
  std::string camera_id;
  bool located = false;
  std::string diagnostic;
  PixelRegion selected_region;
  double locator_score = 0.0;
  std::uint64_t missing_frame_ids = 0;
  RgbScheduleOffsetEstimate schedule_offset;
  BayerColorCalibration color_calibration;
  std::vector<RgbFrameEvidence> frames;
  std::vector<BrightnessCandidateEvidence> brightness_candidates;
};

// Pre-arm, camera-local calibration. The schedule contains only stable black
// baseline intervals and discrete white probe intervals. The analyzer scans
// overlapping Bayer-aware regions, chooses one localized response whose
// temporal strength follows the commanded brightness sweep, derives raw
// channel normalization, estimates the fixed camera-to-schedule timestamp
// correction, and evaluates every candidate with the same signal, saturation,
// and bloom gates used by AnalyzeRgbSwing. No ROI or timestamp correction is
// assumed. `located` is true only when both localization and offset estimation
// are unambiguous.
[[nodiscard]] RgbBrightnessCalibration AnalyzeRgbBrightnessCalibration(
    const RgbBrightnessCalibrationInput &input,
    const RgbBrightnessCalibrationOptions &options = {});

struct CameraBrightnessSweep {
  std::string camera_id;
  std::span<const BrightnessCandidateEvidence> candidates;
};

struct SharedBrightnessCandidate {
  std::uint8_t brightness = 0;
  std::size_t cameras_observed = 0;
  bool safe_for_every_camera = false;
  double worst_minimum_signal_delta = 0.0;
  double worst_minimum_signal_to_background_noise = 0.0;
  double worst_maximum_saturated_fraction = 0.0;
  double worst_maximum_bloom_fraction = 0.0;
  std::vector<std::string> rejection_reasons;
};

struct SharedBrightnessRecommendation {
  bool available = false;
  std::optional<std::uint8_t> brightness;
  std::string diagnostic;
  std::vector<SharedBrightnessCandidate> candidates;
};

// Intersects the observed brightness levels across every camera and chooses
// the lowest level that is clearly above local background while remaining
// below the configured saturation and bloom gates in every view. Candidate
// safety is computed by AnalyzeRgbSwing or AnalyzeRgbBrightnessCalibration;
// this function never weakens it.
[[nodiscard]] SharedBrightnessRecommendation RecommendSharedRgbBrightness(
    std::span<const CameraBrightnessSweep> camera_sweeps);

}  // namespace swing_capture::optical

#endif  // SWING_CAPTURE_CAPTURE_OPTICAL_RGB_SWING_H_
