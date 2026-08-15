#include "capture/optical/rgb_swing.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "capture/image/image_quality.h"
#include "capture/optical/led_pulse.h"

namespace swing_capture::optical {
namespace {

using Clock = std::chrono::steady_clock;

enum class BayerChannel : std::size_t {
  kRed = 0,
  kGreen = 1,
  kBlue = 2,
};

constexpr std::size_t kChannelCount = 3;

struct Bounds {
  std::uint32_t left = 0;
  std::uint32_t top = 0;
  std::uint32_t right = 0;
  std::uint32_t bottom = 0;
};

struct PixelCoordinate {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
};

struct RunningMean {
  std::size_t count = 0;
  double mean = 0.0;

  void Add(double value) {
    ++count;
    const double delta = value - mean;
    mean += delta / static_cast<double>(count);
  }
};

struct RunningMoments {
  std::size_t count = 0;
  double mean = 0.0;
  double squared_deviation_sum = 0.0;

  void Add(double value) {
    const double delta = value - mean;
    ++count;
    mean += delta / static_cast<double>(count);
    const double centered_value = value - mean;
    squared_deviation_sum += delta * centered_value;
  }

  [[nodiscard]] double SampleVariance() const {
    if (count < 2U) {
      return 0.0;
    }
    return std::max(0.0, squared_deviation_sum / static_cast<double>(count - 1U));
  }
};

struct BaselineModel {
  Bounds bounds;
  std::uint32_t width = 0;
  std::vector<RunningMean> pixels;

  [[nodiscard]] const RunningMean &At(PixelCoordinate coordinate) const {
    const std::size_t row = static_cast<std::size_t>(coordinate.y - bounds.top) * width;
    return pixels.at(row + (coordinate.x - bounds.left));
  }
};

struct FrameScheduleMembership {
  Clock::time_point exposure_begin;
  Clock::time_point exposure_end;
  RgbFramePhase phase = RgbFramePhase::kOutsideSchedule;
  std::optional<std::size_t> step_index;
};

struct MeasurementAccumulator {
  std::array<double, kChannelCount> local_positive_sum{};
  std::array<double, kChannelCount> background_positive_sum{};
  std::array<double, kChannelCount> local_value_sum{};
  std::array<double, kChannelCount> background_value_sum{};
  std::array<std::size_t, kChannelCount> local_count{};
  std::array<std::size_t, kChannelCount> background_count{};
  std::size_t saturated_samples = 0;
  std::size_t bloom_samples = 0;
  std::size_t local_samples = 0;
  std::size_t background_samples = 0;
};

struct BrightnessAggregate {
  BrightnessCandidateEvidence evidence;
  std::size_t matching_frames = 0;
};

struct ScheduleRequirements {
  bool has_baseline = false;
  bool has_probe = false;
  bool has_pre_impact = false;
  bool has_impact = false;
  bool has_post_impact = false;
  bool impact_interval_ended = false;
};

struct IntegralImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<double> values;

  [[nodiscard]] double Rect(Bounds bounds) const {
    const std::size_t stride = static_cast<std::size_t>(width) + 1U;
    const auto at = [this, stride](std::uint32_t x, std::uint32_t y) {
      return values[static_cast<std::size_t>(y) * stride + x];
    };
    return at(bounds.right, bounds.bottom) - at(bounds.left, bounds.bottom) -
           at(bounds.right, bounds.top) + at(bounds.left, bounds.top);
  }
};

struct LocatorCandidateScore {
  PixelRegion region;
  double score = 0.0;
};

struct LocatorLevelAggregate {
  std::array<double, kChannelCount> channel_sum{};
  double signal_sum = 0.0;
  std::size_t frame_count = 0;
};

struct OffsetGroup {
  std::vector<double> signals;
  double mean = 0.0;
};

struct OffsetCandidateFit {
  Clock::duration correction{};
  double score = 0.0;
};

struct OffsetFitStatistics {
  double within_group_error = 0.0;
  double between_group_variation = 0.0;
  double monotonic_penalty = 0.0;
};

struct LocatorAxis {
  std::uint32_t extent = 0;
  std::uint32_t region_extent = 0;
  std::uint32_t stride = 0;
};

bool IsFiniteFraction(double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; }

bool IsBlack(Rgb8 color) { return color == Rgb8{}; }

bool IsWhite(Rgb8 color) { return color == Rgb8{.red = 255, .green = 255, .blue = 255}; }

bool IsRequiredSwingPhase(RgbSwingPhase phase) { return phase == RgbSwingPhase::kImpact; }

std::size_t EffectiveStride(const image::Raw8ImageView &image) {
  return image.row_stride_bytes == 0 ? image.width : image.row_stride_bytes;
}

std::uint8_t Pixel(const BayerRg8FrameView &frame, PixelCoordinate coordinate) {
  const std::size_t offset =
      static_cast<std::size_t>(coordinate.y) * EffectiveStride(frame.image) + coordinate.x;
  return std::to_integer<std::uint8_t>(frame.image.pixels[offset]);
}

BayerChannel ChannelAt(PixelCoordinate coordinate) {
  if ((coordinate.y & 1U) == 0U) {
    return (coordinate.x & 1U) == 0U ? BayerChannel::kRed : BayerChannel::kGreen;
  }
  return (coordinate.x & 1U) == 0U ? BayerChannel::kGreen : BayerChannel::kBlue;
}

bool Inside(const PixelRegion &region, PixelCoordinate coordinate) {
  return coordinate.x >= region.x && coordinate.y >= region.y &&
         coordinate.x - region.x < region.width && coordinate.y - region.y < region.height;
}

Bounds ExpandedBounds(const PixelRegion &region, std::uint32_t margin, std::uint32_t image_width,
                      std::uint32_t image_height) {
  return {
      .left = region.x > margin ? region.x - margin : 0U,
      .top = region.y > margin ? region.y - margin : 0U,
      .right = static_cast<std::uint32_t>(std::min<std::uint64_t>(
          image_width, static_cast<std::uint64_t>(region.x) + region.width + margin)),
      .bottom = static_cast<std::uint32_t>(std::min<std::uint64_t>(
          image_height, static_cast<std::uint64_t>(region.y) + region.height + margin)),
  };
}

void ValidateOptions(const RgbSwingAnalysisOptions &options) {
  if (options.exposure_duration < Clock::duration::zero() ||
      options.schedule_timing_uncertainty < Clock::duration::zero() ||
      options.frame_delivery_latency_bound < Clock::duration::zero() ||
      options.background_margin == 0U || options.minimum_baseline_frames < 2U ||
      options.minimum_stable_state_frames == 0U || options.minimum_stable_impact_frames == 0U ||
      options.minimum_brightness_probe_frames == 0U) {
    throw std::invalid_argument("RGB swing timing, region, and frame-count options are invalid");
  }
  if (!IsFiniteFraction(options.minimum_state_matching_fraction) ||
      options.minimum_state_matching_fraction <= 0.0 ||
      !std::isfinite(options.maximum_color_distance) || options.maximum_color_distance <= 0.0 ||
      !std::isfinite(options.minimum_signal_delta) || options.minimum_signal_delta <= 0.0 ||
      !std::isfinite(options.minimum_signal_to_background_noise) ||
      options.minimum_signal_to_background_noise <= 0.0 ||
      !IsFiniteFraction(options.maximum_saturated_fraction) ||
      !IsFiniteFraction(options.maximum_bloom_fraction) || options.bloom_delta_threshold == 0U) {
    throw std::invalid_argument("RGB swing optical thresholds are invalid");
  }
}

void ValidateCalibrationOptions(const RgbBrightnessCalibrationOptions &options) {
  ValidateOptions(options.optical);
  if (options.region_width < 2U || options.region_height < 2U || options.region_stride == 0U ||
      options.minimum_probe_levels == 0U || options.minimum_frames_per_probe == 0U ||
      !std::isfinite(options.minimum_channel_delta) || options.minimum_channel_delta <= 0.0 ||
      !IsFiniteFraction(options.minimum_monotonic_fraction) ||
      options.minimum_monotonic_fraction <= 0.0 ||
      options.maximum_absolute_schedule_offset <= Clock::duration::zero() ||
      options.maximum_absolute_schedule_offset > std::chrono::seconds(1) ||
      options.schedule_offset_resolution <= Clock::duration::zero() ||
      options.schedule_offset_resolution > options.maximum_absolute_schedule_offset ||
      options.maximum_schedule_offset_uncertainty < options.schedule_offset_resolution / 2 ||
      !IsFiniteFraction(options.minimum_schedule_offset_fit_score) ||
      options.minimum_schedule_offset_fit_score <= 0.0 ||
      !IsFiniteFraction(options.minimum_schedule_offset_score_margin)) {
    throw std::invalid_argument("RGB brightness-calibration locator options are invalid");
  }
  const auto step_count =
      options.maximum_absolute_schedule_offset / options.schedule_offset_resolution;
  if (step_count > 10'000) {
    throw std::invalid_argument("RGB schedule-offset search has too many candidates");
  }
  if (options.optical.frame_delivery_latency_bound >
      Clock::duration::max() - options.maximum_absolute_schedule_offset) {
    throw std::invalid_argument("RGB schedule-offset guard overflows the steady clock duration");
  }
}

void ValidateStepTime(std::span<const FeatherRgbStep> schedule, std::size_t index) {
  const FeatherRgbStep &step = schedule[index];
  if (step.end_exclusive <= step.start) {
    throw std::invalid_argument("RGB schedule steps must have positive duration");
  }
  if (index > 0U && step.start < schedule[index - 1U].end_exclusive) {
    throw std::invalid_argument("RGB schedule steps must be ordered and nonoverlapping");
  }
}

void ValidateStepEmission(const FeatherRgbStep &step) {
  if (step.phase == RgbSwingPhase::kBaseline) {
    if (!IsBlack(step.color) || step.brightness != 0U) {
      throw std::invalid_argument("RGB baseline steps must be black at zero brightness");
    }
    return;
  }
  if (IsBlack(step.color) || step.brightness == 0U) {
    throw std::invalid_argument("non-baseline RGB steps must emit a nonblack color");
  }
}

void ObserveSchedulePhase(const FeatherRgbStep &step, ScheduleRequirements *requirements) {
  switch (step.phase) {
    case RgbSwingPhase::kBaseline:
      requirements->has_baseline = true;
      break;
    case RgbSwingPhase::kBrightnessProbe:
      requirements->has_probe = true;
      break;
    case RgbSwingPhase::kPreImpact:
      if (requirements->has_impact) {
        throw std::invalid_argument("pre-impact RGB steps cannot follow the impact interval");
      }
      requirements->has_pre_impact = true;
      break;
    case RgbSwingPhase::kImpact:
      if (requirements->impact_interval_ended || !IsWhite(step.color)) {
        throw std::invalid_argument(
            "the RGB impact interval must be contiguous deterministic white");
      }
      requirements->has_impact = true;
      break;
    case RgbSwingPhase::kPostImpact:
      if (!requirements->has_impact) {
        throw std::invalid_argument("post-impact RGB steps require an earlier impact interval");
      }
      requirements->impact_interval_ended = true;
      requirements->has_post_impact = true;
      break;
  }
  if (requirements->has_impact && step.phase != RgbSwingPhase::kImpact &&
      step.phase != RgbSwingPhase::kPostImpact) {
    requirements->impact_interval_ended = true;
  }
}

ScheduleRequirements InspectSchedule(std::span<const FeatherRgbStep> schedule) {
  if (schedule.empty()) {
    throw std::invalid_argument("RGB swing requires a Feather step schedule");
  }
  ScheduleRequirements requirements;
  for (std::size_t index = 0; index < schedule.size(); ++index) {
    const FeatherRgbStep &step = schedule[index];
    ValidateStepTime(schedule, index);
    ValidateStepEmission(step);
    ObserveSchedulePhase(step, &requirements);
  }
  return requirements;
}

void ValidateSwingSchedule(std::span<const FeatherRgbStep> schedule) {
  const ScheduleRequirements requirements = InspectSchedule(schedule);
  if (!requirements.has_baseline || !requirements.has_pre_impact || !requirements.has_impact ||
      !requirements.has_post_impact) {
    throw std::invalid_argument(
        "RGB swing schedule requires baseline, pre-impact, impact, and post-impact steps");
  }
}

void ValidateCalibrationSchedule(std::span<const FeatherRgbStep> schedule) {
  const ScheduleRequirements requirements = InspectSchedule(schedule);
  for (const FeatherRgbStep &step : schedule) {
    if (step.phase != RgbSwingPhase::kBaseline && step.phase != RgbSwingPhase::kBrightnessProbe) {
      throw std::invalid_argument("RGB calibration schedule may contain only OFF and probe steps");
    }
    if (step.phase == RgbSwingPhase::kBrightnessProbe && !IsWhite(step.color)) {
      throw std::invalid_argument("RGB calibration probes must be deterministic white");
    }
  }
  if (!requirements.has_baseline || !requirements.has_probe) {
    throw std::invalid_argument("RGB calibration schedule requires OFF and white probe steps");
  }
}

std::uint64_t ValidateFramesAndTimeline(const RgbSwingCameraInput &input,
                                        bool require_contiguous_frame_ids) {
  if (input.camera_id.empty() || input.frames.empty() ||
      input.frames.size() != input.mapped_host_times.size()) {
    throw std::invalid_argument(
        "RGB swing camera identity, frames, and mapped timeline are required");
  }
  const std::uint32_t width = input.frames.front().image.width;
  const std::uint32_t height = input.frames.front().image.height;
  if (width == 0U || height == 0U) {
    throw std::invalid_argument("RGB swing frames must have nonzero geometry");
  }
  std::uint64_t missing_frame_ids = 0;
  for (std::size_t index = 0; index < input.frames.size(); ++index) {
    const BayerRg8FrameView &frame = input.frames[index];
    if (frame.image.width != width || frame.image.height != height) {
      throw std::invalid_argument("RGB swing frames must have identical geometry");
    }
    const std::size_t stride = EffectiveStride(frame.image);
    if (stride < width) {
      throw std::invalid_argument("RGB swing Bayer row stride is smaller than its width");
    }
    const std::size_t required =
        (static_cast<std::size_t>(height) - 1U) * stride + static_cast<std::size_t>(width);
    if (frame.image.pixels.size() < required) {
      throw std::invalid_argument("RGB swing Bayer payload is smaller than its geometry");
    }
    if (index == 0U) {
      continue;
    }
    const std::uint64_t previous_frame_id = input.frames[index - 1U].frame_id;
    if (frame.frame_id <= previous_frame_id) {
      throw std::invalid_argument("RGB optical frame IDs must be strictly increasing");
    }
    missing_frame_ids += frame.frame_id - previous_frame_id - 1U;
    if (require_contiguous_frame_ids && frame.frame_id != previous_frame_id + 1U) {
      throw std::invalid_argument("RGB swing retained frame IDs must be contiguous");
    }
    if (frame.device_timestamp <= input.frames[index - 1U].device_timestamp) {
      throw std::invalid_argument("RGB swing device timestamps must be strictly increasing");
    }
    if (input.mapped_host_times[index] <= input.mapped_host_times[index - 1U]) {
      throw std::invalid_argument("RGB swing mapped host times must be strictly increasing");
    }
  }
  return missing_frame_ids;
}

void ValidateRegion(const RgbSwingCameraInput &input) {
  const PixelRegion &region = input.fixture_neopixel_region;
  const std::uint32_t width = input.frames.front().image.width;
  const std::uint32_t height = input.frames.front().image.height;
  const std::uint64_t right = static_cast<std::uint64_t>(region.x) + region.width;
  const std::uint64_t bottom = static_cast<std::uint64_t>(region.y) + region.height;
  if (region.width < 2U || region.height < 2U || right > width || bottom > height) {
    throw std::invalid_argument("fixed fixture NeoPixel ROI is outside the Bayer frame");
  }
}

RgbSwingCameraInput CalibrationView(const RgbBrightnessCalibrationInput &input,
                                    PixelRegion region) {
  return {
      .camera_id = input.camera_id,
      .frames = input.frames,
      .mapped_host_times = input.mapped_host_times,
      .fixture_neopixel_region = region,
      .schedule = input.schedule,
      .color_calibration = std::nullopt,
  };
}

FrameScheduleMembership ClassifyFrame(Clock::time_point host_time,
                                      std::span<const FeatherRgbStep> schedule,
                                      Clock::duration conservative_edge_guard) {
  FrameScheduleMembership membership{
      .exposure_begin = host_time - conservative_edge_guard,
      .exposure_end = host_time + conservative_edge_guard,
      .phase = RgbFramePhase::kOutsideSchedule,
      .step_index = std::nullopt,
  };
  bool intersects = false;
  for (std::size_t index = 0; index < schedule.size(); ++index) {
    const FeatherRgbStep &step = schedule[index];
    const bool contains_host_time = host_time >= step.start && host_time < step.end_exclusive;
    const bool stable =
        membership.exposure_begin >= step.start && membership.exposure_end < step.end_exclusive;
    if (contains_host_time) {
      membership.step_index = index;
      membership.phase = stable ? RgbFramePhase::kStableStep : RgbFramePhase::kExposureTransition;
      return membership;
    }
    intersects = intersects || (membership.exposure_begin < step.end_exclusive &&
                                membership.exposure_end >= step.start);
  }
  membership.phase =
      intersects ? RgbFramePhase::kExposureTransition : RgbFramePhase::kOutsideSchedule;
  return membership;
}

std::vector<FrameScheduleMembership> ClassifyFrames(const RgbSwingCameraInput &input,
                                                    const RgbSwingAnalysisOptions &options) {
  std::vector<FrameScheduleMembership> memberships;
  memberships.reserve(input.frames.size());
  const Clock::duration conservative_edge_guard = options.exposure_duration +
                                                  options.schedule_timing_uncertainty +
                                                  options.frame_delivery_latency_bound;
  for (const Clock::time_point host_time : input.mapped_host_times) {
    memberships.push_back(ClassifyFrame(host_time, input.schedule, conservative_edge_guard));
  }
  return memberships;
}

BaselineModel BuildBaselineWithin(const RgbSwingCameraInput &input,
                                  std::span<const FrameScheduleMembership> memberships,
                                  const RgbSwingAnalysisOptions &options, Bounds bounds) {
  const std::uint32_t width = bounds.right - bounds.left;
  const std::uint32_t height = bounds.bottom - bounds.top;
  const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
  BaselineModel baseline{
      .bounds = bounds,
      .width = width,
      .pixels = std::vector<RunningMean>(pixel_count),
  };
  std::size_t baseline_frames = 0;
  for (std::size_t frame_index = 0; frame_index < input.frames.size(); ++frame_index) {
    const FrameScheduleMembership &membership = memberships[frame_index];
    if (membership.phase != RgbFramePhase::kStableStep || !membership.step_index.has_value() ||
        input.schedule[*membership.step_index].phase != RgbSwingPhase::kBaseline) {
      continue;
    }
    ++baseline_frames;
    for (std::uint32_t y = bounds.top; y < bounds.bottom; ++y) {
      for (std::uint32_t x = bounds.left; x < bounds.right; ++x) {
        const std::size_t index = static_cast<std::size_t>(y - bounds.top) * width +
                                  static_cast<std::size_t>(x - bounds.left);
        baseline.pixels[index].Add(Pixel(input.frames[frame_index], {.x = x, .y = y}));
      }
    }
  }
  if (baseline_frames < options.minimum_baseline_frames) {
    throw std::invalid_argument("RGB swing timeline does not contain enough stable OFF frames");
  }
  return baseline;
}

BaselineModel BuildBaseline(const RgbSwingCameraInput &input,
                            std::span<const FrameScheduleMembership> memberships,
                            const RgbSwingAnalysisOptions &options) {
  const Bounds bounds =
      ExpandedBounds(input.fixture_neopixel_region, options.background_margin,
                     input.frames.front().image.width, input.frames.front().image.height);
  const std::size_t pixel_count =
      static_cast<std::size_t>(bounds.right - bounds.left) * (bounds.bottom - bounds.top);
  if (pixel_count <= static_cast<std::size_t>(input.fixture_neopixel_region.width) *
                         input.fixture_neopixel_region.height) {
    throw std::invalid_argument("fixed fixture NeoPixel ROI has no surrounding background annulus");
  }
  return BuildBaselineWithin(input, memberships, options, bounds);
}

std::array<double, kChannelCount> NormalizeRgb(Rgb8 color) {
  const std::array values = {static_cast<double>(color.red), static_cast<double>(color.green),
                             static_cast<double>(color.blue)};
  const double norm =
      std::sqrt(values[0] * values[0] + values[1] * values[1] + values[2] * values[2]);
  if (norm == 0.0) {
    return {};
  }
  return {values[0] / norm, values[1] / norm, values[2] / norm};
}

std::array<double, kChannelCount> NormalizeMeasurement(
    const std::array<BayerChannelMeasurement, kChannelCount> &channels,
    const BayerColorCalibration &calibration) {
  const std::array calibrated = {channels[0].local_excess * calibration.red_scale,
                                 channels[1].local_excess * calibration.green_scale,
                                 channels[2].local_excess * calibration.blue_scale};
  const double norm = std::sqrt(calibrated[0] * calibrated[0] + calibrated[1] * calibrated[1] +
                                calibrated[2] * calibrated[2]);
  if (norm == 0.0) {
    return {};
  }
  return {calibrated[0] / norm, calibrated[1] / norm, calibrated[2] / norm};
}

double ColorDistance(const std::array<double, kChannelCount> &left,
                     const std::array<double, kChannelCount> &right) {
  double squared = 0.0;
  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    const double delta = left[channel] - right[channel];
    squared += delta * delta;
  }
  return std::sqrt(squared);
}

std::vector<Rgb8> Palette(std::span<const FeatherRgbStep> schedule) {
  std::vector<Rgb8> colors;
  for (const FeatherRgbStep &step : schedule) {
    if (IsBlack(step.color) || std::ranges::find(colors, step.color) != colors.end()) {
      continue;
    }
    colors.push_back(step.color);
  }
  return colors;
}

MeasurementAccumulator AccumulateMeasurement(const BayerRg8FrameView &frame,
                                             const PixelRegion &region,
                                             const BaselineModel &baseline,
                                             const RgbSwingAnalysisOptions &options) {
  MeasurementAccumulator accumulated;
  const Bounds measurement_bounds =
      ExpandedBounds(region, options.background_margin, frame.image.width, frame.image.height);
  for (std::uint32_t y = measurement_bounds.top; y < measurement_bounds.bottom; ++y) {
    for (std::uint32_t x = measurement_bounds.left; x < measurement_bounds.right; ++x) {
      const PixelCoordinate coordinate{.x = x, .y = y};
      const auto channel = static_cast<std::size_t>(ChannelAt(coordinate));
      const double value = Pixel(frame, coordinate);
      const double delta = value - baseline.At(coordinate).mean;
      const double positive_delta = std::max(0.0, delta);
      if (Inside(region, coordinate)) {
        accumulated.local_positive_sum[channel] += positive_delta;
        accumulated.local_value_sum[channel] += value;
        ++accumulated.local_count[channel];
        ++accumulated.local_samples;
        if (value >= options.saturation_threshold) {
          ++accumulated.saturated_samples;
        }
      } else {
        accumulated.background_positive_sum[channel] += positive_delta;
        accumulated.background_value_sum[channel] += value;
        ++accumulated.background_count[channel];
        ++accumulated.background_samples;
        if (positive_delta >= options.bloom_delta_threshold) {
          ++accumulated.bloom_samples;
        }
      }
    }
  }
  if (std::ranges::any_of(accumulated.local_count, [](std::size_t count) { return count == 0U; }) ||
      std::ranges::any_of(accumulated.background_count,
                          [](std::size_t count) { return count == 0U; })) {
    throw std::invalid_argument("NeoPixel ROI and annulus must contain every Bayer color phase");
  }
  return accumulated;
}

std::array<BayerChannelMeasurement, kChannelCount> ChannelMeasurements(
    const MeasurementAccumulator &accumulated,
    const std::array<double, kChannelCount> &aggregate_noise_rms) {
  std::array<BayerChannelMeasurement, kChannelCount> channels;
  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    BayerChannelMeasurement &measurement = channels[channel];
    measurement.local_positive_delta = accumulated.local_positive_sum[channel] /
                                       static_cast<double>(accumulated.local_count[channel]);
    measurement.background_positive_delta =
        accumulated.background_positive_sum[channel] /
        static_cast<double>(accumulated.background_count[channel]);
    measurement.local_excess =
        std::max(0.0, measurement.local_positive_delta - measurement.background_positive_delta);
    measurement.background_noise_rms = aggregate_noise_rms[channel];
  }
  return channels;
}

std::array<double, kChannelCount> RawAggregateDifferences(
    const MeasurementAccumulator &accumulated) {
  std::array<double, kChannelCount> differences{};
  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    const double local_mean = accumulated.local_value_sum[channel] /
                              static_cast<double>(accumulated.local_count[channel]);
    const double background_mean = accumulated.background_value_sum[channel] /
                                   static_cast<double>(accumulated.background_count[channel]);
    differences[channel] = local_mean - background_mean;
  }
  return differences;
}

std::array<double, kChannelCount> EstimateAggregateNoise(
    const RgbSwingCameraInput &input, std::span<const FrameScheduleMembership> memberships,
    const BaselineModel &baseline, const RgbSwingAnalysisOptions &options) {
  std::array<RunningMoments, kChannelCount> moments;
  for (std::size_t frame_index = 0; frame_index < input.frames.size(); ++frame_index) {
    const FrameScheduleMembership &membership = memberships[frame_index];
    if (membership.phase != RgbFramePhase::kStableStep || !membership.step_index.has_value() ||
        input.schedule[*membership.step_index].phase != RgbSwingPhase::kBaseline) {
      continue;
    }
    const MeasurementAccumulator accumulated = AccumulateMeasurement(
        input.frames[frame_index], input.fixture_neopixel_region, baseline, options);
    const auto differences = RawAggregateDifferences(accumulated);
    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
      moments[channel].Add(differences[channel]);
    }
  }

  std::array<double, kChannelCount> noise_rms{};
  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    noise_rms[channel] = std::sqrt(moments[channel].SampleVariance());
  }
  return noise_rms;
}

enum class ColorEvaluation : std::uint8_t {
  kAllScheduledEmissions,
  kWhiteImpactOnly,
};

bool ShouldEvaluateColor(const RgbSwingCameraInput &input,
                         const FrameScheduleMembership &membership, ColorEvaluation evaluation) {
  if (evaluation == ColorEvaluation::kAllScheduledEmissions) {
    return true;
  }
  return membership.phase == RgbFramePhase::kStableStep && membership.step_index.has_value() &&
         input.schedule[*membership.step_index].phase == RgbSwingPhase::kImpact;
}

RgbFrameEvidence MeasureFrame(const RgbSwingCameraInput &input, std::size_t frame_index,
                              const FrameScheduleMembership &membership,
                              const BaselineModel &baseline, std::span<const Rgb8> palette,
                              const BayerColorCalibration &calibration,
                              const std::array<double, kChannelCount> &aggregate_noise_rms,
                              const RgbSwingAnalysisOptions &options,
                              ColorEvaluation color_evaluation) {
  const BayerRg8FrameView &frame = input.frames[frame_index];
  const MeasurementAccumulator accumulated =
      AccumulateMeasurement(frame, input.fixture_neopixel_region, baseline, options);
  const auto channels = ChannelMeasurements(accumulated, aggregate_noise_rms);
  RgbFrameEvidence evidence{
      .frame_id = frame.frame_id,
      .device_timestamp = frame.device_timestamp,
      .mapped_host_time = input.mapped_host_times[frame_index],
      .conservative_exposure_begin = membership.exposure_begin,
      .conservative_exposure_end = membership.exposure_end,
      .phase = membership.phase,
      .expected_step_index = membership.step_index,
      .red = channels[0],
      .green = channels[1],
      .blue = channels[2],
      .decoded_color = std::nullopt,
  };
  evidence.signal_delta = std::sqrt(channels[0].local_excess * channels[0].local_excess +
                                    channels[1].local_excess * channels[1].local_excess +
                                    channels[2].local_excess * channels[2].local_excess);
  const double background_noise =
      std::sqrt(channels[0].background_noise_rms * channels[0].background_noise_rms +
                channels[1].background_noise_rms * channels[1].background_noise_rms +
                channels[2].background_noise_rms * channels[2].background_noise_rms);
  evidence.signal_to_background_noise = evidence.signal_delta / std::max(1.0, background_noise);
  evidence.saturated_fraction = static_cast<double>(accumulated.saturated_samples) /
                                static_cast<double>(accumulated.local_samples);
  evidence.bloom_fraction = static_cast<double>(accumulated.bloom_samples) /
                            static_cast<double>(accumulated.background_samples);

  if (evidence.signal_delta == 0.0 || palette.empty() ||
      !ShouldEvaluateColor(input, membership, color_evaluation)) {
    return evidence;
  }
  const auto normalized = NormalizeMeasurement(channels, calibration);
  double nearest_distance = std::numeric_limits<double>::infinity();
  for (const Rgb8 color : palette) {
    const double distance = ColorDistance(normalized, NormalizeRgb(color));
    if (distance < nearest_distance) {
      nearest_distance = distance;
      evidence.decoded_color = color;
    }
  }
  if (membership.phase == RgbFramePhase::kStableStep && membership.step_index.has_value()) {
    const Rgb8 expected = input.schedule[*membership.step_index].color;
    if (!IsBlack(expected)) {
      evidence.expected_color_distance = ColorDistance(normalized, NormalizeRgb(expected));
      evidence.expected_color_match =
          evidence.expected_color_distance <= options.maximum_color_distance;
    }
  }
  return evidence;
}

struct WhiteCalibrationAggregate {
  std::array<double, kChannelCount> channel_sum{};
  std::size_t frame_count = 0;
  double minimum_signal_delta = std::numeric_limits<double>::infinity();
  double maximum_saturated_fraction = 0.0;
  double maximum_bloom_fraction = 0.0;
};

bool ValidCalibrationScale(double scale) { return std::isfinite(scale) && scale > 0.0; }

void ValidateColorCalibration(const BayerColorCalibration &calibration) {
  if (!ValidCalibrationScale(calibration.red_scale) ||
      !ValidCalibrationScale(calibration.green_scale) ||
      !ValidCalibrationScale(calibration.blue_scale)) {
    throw std::invalid_argument("Bayer color-calibration scales must be finite and positive");
  }
}

BayerColorCalibration CalibrationFromAggregate(const WhiteCalibrationAggregate &aggregate,
                                               std::uint8_t brightness) {
  std::array<double, kChannelCount> channel_means{};
  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    channel_means[channel] =
        aggregate.channel_sum[channel] / static_cast<double>(aggregate.frame_count);
    if (channel_means[channel] <= 1.0) {
      return {};
    }
  }
  const double target = (channel_means[0] + channel_means[1] + channel_means[2]) / 3.0;
  const std::array scales = {target / channel_means[0], target / channel_means[1],
                             target / channel_means[2]};
  if (std::ranges::any_of(scales, [](double scale) { return scale < 0.2 || scale > 5.0; })) {
    return {};
  }
  return {
      .red_scale = scales[0],
      .green_scale = scales[1],
      .blue_scale = scales[2],
      .automatic = true,
      .source_brightness = brightness,
      .source_frame_count = aggregate.frame_count,
  };
}

BayerColorCalibration EstimateColorCalibration(std::span<const FeatherRgbStep> schedule,
                                               std::span<const RgbFrameEvidence> frames,
                                               const RgbSwingAnalysisOptions &options) {
  std::map<std::uint8_t, WhiteCalibrationAggregate> aggregates;
  for (const RgbFrameEvidence &frame : frames) {
    if (frame.phase != RgbFramePhase::kStableStep || !frame.expected_step_index.has_value()) {
      continue;
    }
    const FeatherRgbStep &step = schedule[*frame.expected_step_index];
    if (step.phase != RgbSwingPhase::kBrightnessProbe || !IsWhite(step.color)) {
      continue;
    }
    WhiteCalibrationAggregate &aggregate = aggregates[step.brightness];
    aggregate.channel_sum[0] += frame.red.local_excess;
    aggregate.channel_sum[1] += frame.green.local_excess;
    aggregate.channel_sum[2] += frame.blue.local_excess;
    ++aggregate.frame_count;
    aggregate.minimum_signal_delta = std::min(aggregate.minimum_signal_delta, frame.signal_delta);
    aggregate.maximum_saturated_fraction =
        std::max(aggregate.maximum_saturated_fraction, frame.saturated_fraction);
    aggregate.maximum_bloom_fraction =
        std::max(aggregate.maximum_bloom_fraction, frame.bloom_fraction);
  }
  for (const auto &[brightness, aggregate] : aggregates | std::views::reverse) {
    if (aggregate.frame_count < options.minimum_brightness_probe_frames ||
        aggregate.minimum_signal_delta < options.minimum_signal_delta ||
        aggregate.maximum_saturated_fraction > options.maximum_saturated_fraction ||
        aggregate.maximum_bloom_fraction > options.maximum_bloom_fraction) {
      continue;
    }
    BayerColorCalibration calibration = CalibrationFromAggregate(aggregate, brightness);
    if (calibration.automatic) {
      return calibration;
    }
  }
  return {};
}

std::vector<RgbFrameEvidence> MeasureFrames(
    const RgbSwingCameraInput &input, std::span<const FrameScheduleMembership> memberships,
    const BaselineModel &baseline, const BayerColorCalibration &calibration,
    const RgbSwingAnalysisOptions &options,
    ColorEvaluation color_evaluation = ColorEvaluation::kAllScheduledEmissions) {
  const std::vector<Rgb8> palette = color_evaluation == ColorEvaluation::kWhiteImpactOnly
                                        ? std::vector{Rgb8{.red = 255, .green = 255, .blue = 255}}
                                        : Palette(input.schedule);
  const auto aggregate_noise_rms = EstimateAggregateNoise(input, memberships, baseline, options);
  std::vector<RgbFrameEvidence> frames;
  frames.reserve(input.frames.size());
  for (std::size_t index = 0; index < input.frames.size(); ++index) {
    frames.push_back(MeasureFrame(input, index, memberships[index], baseline, palette, calibration,
                                  aggregate_noise_rms, options, color_evaluation));
  }
  return frames;
}

std::vector<std::uint32_t> LocatorAnchors(const LocatorAxis &axis) {
  if (axis.region_extent > axis.extent) {
    throw std::invalid_argument("RGB locator region does not fit in the camera frame");
  }
  std::vector<std::uint32_t> anchors;
  const std::uint32_t last = axis.extent - axis.region_extent;
  for (std::uint32_t anchor = 0;;) {
    anchors.push_back(anchor);
    if (anchor == last) {
      break;
    }
    const std::uint64_t next = static_cast<std::uint64_t>(anchor) + axis.stride;
    anchor = next >= last ? last : static_cast<std::uint32_t>(next);
  }
  return anchors;
}

std::uint64_t EvenCoordinatesBefore(std::uint32_t end_exclusive) {
  return (static_cast<std::uint64_t>(end_exclusive) + 1U) / 2U;
}

std::array<std::uint64_t, kChannelCount> BayerCounts(Bounds bounds) {
  const std::uint64_t even_x =
      EvenCoordinatesBefore(bounds.right) - EvenCoordinatesBefore(bounds.left);
  const std::uint64_t even_y =
      EvenCoordinatesBefore(bounds.bottom) - EvenCoordinatesBefore(bounds.top);
  const std::uint64_t width = bounds.right - bounds.left;
  const std::uint64_t height = bounds.bottom - bounds.top;
  const std::uint64_t odd_x = width - even_x;
  const std::uint64_t odd_y = height - even_y;
  const std::uint64_t red = even_x * even_y;
  const std::uint64_t blue = odd_x * odd_y;
  return {red, width * height - red - blue, blue};
}

std::array<IntegralImage, kChannelCount> BuildProbeResponseIntegrals(
    const RgbSwingCameraInput &input, std::span<const FrameScheduleMembership> memberships,
    const BaselineModel &baseline) {
  const std::uint32_t width = input.frames.front().image.width;
  const std::uint32_t height = input.frames.front().image.height;
  const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
  std::array<std::vector<double>, kChannelCount> responses;
  for (auto &response : responses) {
    response.resize(pixel_count);
  }
  std::size_t probe_frames = 0;
  for (std::size_t frame_index = 0; frame_index < input.frames.size(); ++frame_index) {
    const FrameScheduleMembership &membership = memberships[frame_index];
    if (membership.phase != RgbFramePhase::kStableStep || !membership.step_index.has_value()) {
      continue;
    }
    const FeatherRgbStep &step = input.schedule[*membership.step_index];
    if (step.phase != RgbSwingPhase::kBrightnessProbe) {
      continue;
    }
    ++probe_frames;
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const PixelCoordinate coordinate{.x = x, .y = y};
        const double delta =
            std::max(0.0, static_cast<double>(Pixel(input.frames[frame_index], coordinate)) -
                              baseline.At(coordinate).mean);
        const auto channel = static_cast<std::size_t>(ChannelAt(coordinate));
        responses[channel][static_cast<std::size_t>(y) * width + x] +=
            delta / static_cast<double>(step.brightness);
      }
    }
  }
  if (probe_frames == 0U) {
    throw std::invalid_argument("RGB calibration timeline has no stable white probe frames");
  }

  std::array<IntegralImage, kChannelCount> integrals;
  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    IntegralImage &integral = integrals[channel];
    integral.width = width;
    integral.height = height;
    const std::size_t stride = static_cast<std::size_t>(width) + 1U;
    integral.values.resize(stride * (static_cast<std::size_t>(height) + 1U));
    for (std::uint32_t y = 0; y < height; ++y) {
      double row_sum = 0.0;
      for (std::uint32_t x = 0; x < width; ++x) {
        row_sum += responses[channel][static_cast<std::size_t>(y) * width + x] /
                   static_cast<double>(probe_frames);
        integral.values[(static_cast<std::size_t>(y) + 1U) * stride + x + 1U] =
            integral.values[static_cast<std::size_t>(y) * stride + x + 1U] + row_sum;
      }
    }
  }
  return integrals;
}

double LocatorRegionScore(const PixelRegion &region,
                          const std::array<IntegralImage, kChannelCount> &integrals,
                          const RgbBrightnessCalibrationOptions &options) {
  const Bounds local{
      .left = region.x,
      .top = region.y,
      .right = region.x + region.width,
      .bottom = region.y + region.height,
  };
  const Bounds expanded = ExpandedBounds(region, options.optical.background_margin,
                                         integrals.front().width, integrals.front().height);
  const auto local_counts = BayerCounts(local);
  const auto expanded_counts = BayerCounts(expanded);
  std::array<double, kChannelCount> excess{};
  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    const std::uint64_t background_count = expanded_counts[channel] - local_counts[channel];
    if (local_counts[channel] == 0U || background_count == 0U) {
      return 0.0;
    }
    const double local_mean =
        integrals[channel].Rect(local) / static_cast<double>(local_counts[channel]);
    const double background_mean =
        (integrals[channel].Rect(expanded) - integrals[channel].Rect(local)) /
        static_cast<double>(background_count);
    excess[channel] = std::max(0.0, local_mean - background_mean);
  }
  const double maximum = *std::ranges::max_element(excess);
  const double minimum = *std::ranges::min_element(excess);
  if (maximum == 0.0 || minimum == 0.0) {
    return 0.0;
  }
  const double signal =
      std::sqrt(excess[0] * excess[0] + excess[1] * excess[1] + excess[2] * excess[2]);
  return signal * (minimum / maximum);
}

std::vector<LocatorCandidateScore> RankLocatorRegions(
    const RgbSwingCameraInput &input, const std::array<IntegralImage, kChannelCount> &integrals,
    const RgbBrightnessCalibrationOptions &options) {
  const auto x_anchors = LocatorAnchors({.extent = input.frames.front().image.width,
                                         .region_extent = options.region_width,
                                         .stride = options.region_stride});
  const auto y_anchors = LocatorAnchors({.extent = input.frames.front().image.height,
                                         .region_extent = options.region_height,
                                         .stride = options.region_stride});
  std::vector<LocatorCandidateScore> candidates;
  candidates.reserve(x_anchors.size() * y_anchors.size());
  for (const std::uint32_t y : y_anchors) {
    for (const std::uint32_t x : x_anchors) {
      const PixelRegion region{
          .x = x,
          .y = y,
          .width = options.region_width,
          .height = options.region_height,
      };
      candidates.push_back(
          {.region = region, .score = LocatorRegionScore(region, integrals, options)});
    }
  }
  std::ranges::sort(candidates,
                    [](const LocatorCandidateScore &left, const LocatorCandidateScore &right) {
                      if (left.score != right.score) {
                        return left.score > right.score;
                      }
                      if (left.region.y != right.region.y) {
                        return left.region.y < right.region.y;
                      }
                      return left.region.x < right.region.x;
                    });
  return candidates;
}

std::map<std::uint8_t, LocatorLevelAggregate> LocatorLevels(
    std::span<const FeatherRgbStep> schedule, std::span<const RgbFrameEvidence> frames) {
  std::map<std::uint8_t, LocatorLevelAggregate> levels;
  for (const RgbFrameEvidence &frame : frames) {
    if (frame.phase != RgbFramePhase::kStableStep || !frame.expected_step_index.has_value()) {
      continue;
    }
    const FeatherRgbStep &step = schedule[*frame.expected_step_index];
    if (step.phase != RgbSwingPhase::kBrightnessProbe) {
      continue;
    }
    LocatorLevelAggregate &level = levels[step.brightness];
    level.channel_sum[0] += frame.red.local_excess;
    level.channel_sum[1] += frame.green.local_excess;
    level.channel_sum[2] += frame.blue.local_excess;
    level.signal_sum += frame.signal_delta;
    ++level.frame_count;
  }
  return levels;
}

bool LevelLocatesWhite(const LocatorLevelAggregate &level,
                       const RgbBrightnessCalibrationOptions &options) {
  if (level.frame_count < options.minimum_frames_per_probe) {
    return false;
  }
  for (const double channel_sum : level.channel_sum) {
    if (channel_sum / static_cast<double>(level.frame_count) < options.minimum_channel_delta) {
      return false;
    }
  }
  return level.signal_sum / static_cast<double>(level.frame_count) >=
         options.optical.minimum_signal_delta;
}

bool LocatorLevelsAreMonotonic(const std::map<std::uint8_t, LocatorLevelAggregate> &levels,
                               const RgbBrightnessCalibrationOptions &options) {
  std::vector<double> signals;
  for (const auto &[brightness, level] : levels) {
    if (LevelLocatesWhite(level, options)) {
      signals.push_back(level.signal_sum / static_cast<double>(level.frame_count));
    }
    (void)brightness;
  }
  if (signals.size() < options.minimum_probe_levels) {
    return false;
  }
  if (signals.size() == 1U) {
    return true;
  }
  std::size_t monotonic_pairs = 0;
  for (std::size_t index = 1; index < signals.size(); ++index) {
    monotonic_pairs += signals[index] >= signals[index - 1U] * 0.9 ? 1U : 0U;
  }
  const double fraction =
      static_cast<double>(monotonic_pairs) / static_cast<double>(signals.size() - 1U);
  return fraction >= options.minimum_monotonic_fraction;
}

bool LocatorCandidatePasses(const RgbSwingCameraInput &input,
                            std::span<const FrameScheduleMembership> memberships,
                            const BaselineModel &baseline,
                            const RgbBrightnessCalibrationOptions &options) {
  const std::vector<RgbFrameEvidence> frames =
      MeasureFrames(input, memberships, baseline, {}, options.optical);
  return LocatorLevelsAreMonotonic(LocatorLevels(input.schedule, frames), options);
}

std::optional<LocatorCandidateScore> SelectLocatorRegion(
    const RgbBrightnessCalibrationInput &calibration_input,
    std::span<const FrameScheduleMembership> memberships, const BaselineModel &baseline,
    std::span<const LocatorCandidateScore> ranked, const RgbBrightnessCalibrationOptions &options) {
  constexpr std::size_t kMaximumCandidatesToValidate = 64;
  const std::size_t candidate_count = std::min(ranked.size(), kMaximumCandidatesToValidate);
  for (std::size_t index = 0; index < candidate_count; ++index) {
    if (ranked[index].score <= 0.0) {
      break;
    }
    const RgbSwingCameraInput candidate_input =
        CalibrationView(calibration_input, ranked[index].region);
    if (LocatorCandidatePasses(candidate_input, memberships, baseline, options)) {
      return ranked[index];
    }
  }
  return std::nullopt;
}

std::set<std::uint16_t> ExpectedOffsetGroups(std::span<const FeatherRgbStep> schedule) {
  std::set<std::uint16_t> groups = {0U};
  for (const FeatherRgbStep &step : schedule) {
    if (step.phase == RgbSwingPhase::kBrightnessProbe) {
      groups.insert(step.brightness);
    }
  }
  return groups;
}

std::map<std::uint16_t, OffsetGroup> CollectOffsetGroups(
    const RgbBrightnessCalibrationInput &input, std::span<const RgbFrameEvidence> measured_frames,
    Clock::duration correction, const RgbBrightnessCalibrationOptions &options) {
  const Clock::duration edge_guard = options.optical.exposure_duration +
                                     options.optical.schedule_timing_uncertainty +
                                     options.optical.frame_delivery_latency_bound;
  std::map<std::uint16_t, OffsetGroup> groups;
  for (std::size_t index = 0; index < measured_frames.size(); ++index) {
    const FrameScheduleMembership membership =
        ClassifyFrame(input.mapped_host_times[index] + correction, input.schedule, edge_guard);
    if (membership.phase != RgbFramePhase::kStableStep || !membership.step_index.has_value()) {
      continue;
    }
    const FeatherRgbStep &step = input.schedule[*membership.step_index];
    if (step.phase == RgbSwingPhase::kBaseline) {
      groups[0U].signals.push_back(measured_frames[index].signal_delta);
    } else if (step.phase == RgbSwingPhase::kBrightnessProbe) {
      groups[step.brightness].signals.push_back(measured_frames[index].signal_delta);
    }
  }
  return groups;
}

bool ComputeOffsetGroupMeans(std::map<std::uint16_t, OffsetGroup> *groups,
                             const RgbBrightnessCalibrationOptions &options) {
  for (auto &[brightness, group] : *groups) {
    const std::size_t minimum_frames = brightness == 0U ? options.optical.minimum_baseline_frames
                                                        : options.minimum_frames_per_probe;
    if (group.signals.size() < minimum_frames) {
      return false;
    }
    for (const double signal : group.signals) {
      group.mean += signal;
    }
    group.mean /= static_cast<double>(group.signals.size());
  }
  return true;
}

double OffsetGlobalMean(const std::map<std::uint16_t, OffsetGroup> &groups) {
  double total_sum = 0.0;
  std::size_t total_count = 0;
  for (const auto &[brightness, group] : groups) {
    total_sum += group.mean * static_cast<double>(group.signals.size());
    total_count += group.signals.size();
    (void)brightness;
  }
  return total_sum / static_cast<double>(total_count);
}

OffsetFitStatistics ComputeOffsetFitStatistics(const std::map<std::uint16_t, OffsetGroup> &groups,
                                               double global_mean) {
  OffsetFitStatistics statistics;
  std::optional<double> previous_probe_mean;
  for (const auto &[brightness, group] : groups) {
    for (const double signal : group.signals) {
      const double residual = signal - group.mean;
      statistics.within_group_error += residual * residual;
    }
    const double centered = group.mean - global_mean;
    statistics.between_group_variation +=
        centered * centered * static_cast<double>(group.signals.size());
    if (brightness == 0U) {
      continue;
    }
    if (previous_probe_mean.has_value()) {
      const double violation = std::max(0.0, *previous_probe_mean * 0.9 - group.mean);
      statistics.monotonic_penalty +=
          violation * violation * static_cast<double>(group.signals.size());
    }
    previous_probe_mean = group.mean;
  }
  return statistics;
}

std::optional<double> ScheduleOffsetFitScore(const RgbBrightnessCalibrationInput &input,
                                             std::span<const RgbFrameEvidence> measured_frames,
                                             Clock::duration correction,
                                             const RgbBrightnessCalibrationOptions &options) {
  std::map<std::uint16_t, OffsetGroup> groups =
      CollectOffsetGroups(input, measured_frames, correction, options);
  const std::set<std::uint16_t> expected_groups = ExpectedOffsetGroups(input.schedule);
  if (groups.size() != expected_groups.size() ||
      !std::ranges::equal(groups | std::views::keys, expected_groups)) {
    return std::nullopt;
  }
  if (!ComputeOffsetGroupMeans(&groups, options)) {
    return std::nullopt;
  }
  const OffsetFitStatistics statistics =
      ComputeOffsetFitStatistics(groups, OffsetGlobalMean(groups));
  if (statistics.between_group_variation <= std::numeric_limits<double>::epsilon()) {
    return std::nullopt;
  }
  return statistics.between_group_variation /
         (statistics.between_group_variation + statistics.within_group_error +
          statistics.monotonic_penalty);
}

RgbScheduleOffsetEstimate EstimateScheduleOffset(const RgbBrightnessCalibrationInput &input,
                                                 std::span<const RgbFrameEvidence> measured_frames,
                                                 const RgbBrightnessCalibrationOptions &options) {
  const auto half_steps = static_cast<std::int64_t>(options.maximum_absolute_schedule_offset /
                                                    options.schedule_offset_resolution);
  std::vector<OffsetCandidateFit> candidates;
  candidates.reserve(static_cast<std::size_t>(half_steps * 2 + 1));
  for (std::int64_t step = -half_steps; step <= half_steps; ++step) {
    const Clock::duration correction = options.schedule_offset_resolution * step;
    const std::optional<double> score =
        ScheduleOffsetFitScore(input, measured_frames, correction, options);
    candidates.push_back({
        .correction = correction,
        .score = score.value_or(-std::numeric_limits<double>::infinity()),
    });
  }

  RgbScheduleOffsetEstimate estimate{
      .diagnostic = "no schedule-offset candidate retained every calibration level",
  };
  estimate.candidates_evaluated = candidates.size();
  const auto best = std::ranges::max_element(candidates, {}, &OffsetCandidateFit::score);
  if (best == candidates.end() || !std::isfinite(best->score)) {
    return estimate;
  }
  estimate.fit_score = best->score;
  constexpr double kEqualScoreTolerance = 1.0e-12;
  const std::size_t best_index = static_cast<std::size_t>(best - candidates.begin());
  std::size_t first_best = best_index;
  while (first_best > 0U &&
         std::abs(candidates[first_best - 1U].score - best->score) <= kEqualScoreTolerance) {
    --first_best;
  }
  std::size_t last_best = best_index;
  while (last_best + 1U < candidates.size() &&
         std::abs(candidates[last_best + 1U].score - best->score) <= kEqualScoreTolerance) {
    ++last_best;
  }

  // A finite-cadence preview produces a broad, continuous score shoulder:
  // moving the trial correction by one grid cell often changes no stable
  // frame assignments, or changes only one boundary frame.  Treat every
  // adjacent candidate within the configured separation margin as the same
  // physical peak.  A second solution is ambiguous only when a below-margin
  // valley separates it from this component.
  std::vector<double> candidate_scores;
  candidate_scores.reserve(candidates.size());
  std::ranges::transform(candidates, std::back_inserter(candidate_scores),
                         &OffsetCandidateFit::score);
  const rgb_swing_internal::ConnectedOffsetScoreComponent near_best =
      rgb_swing_internal::FindConnectedOffsetScoreComponent(
          candidate_scores, first_best, last_best, options.minimum_schedule_offset_score_margin);
  estimate.best_plateau_touches_search_edge = near_best.touches_search_edge;
  estimate.mapped_time_correction =
      candidates[first_best].correction +
      (candidates[last_best].correction - candidates[first_best].correction) / 2;
  estimate.uncertainty =
      std::max(estimate.mapped_time_correction - candidates[near_best.first_index].correction,
               candidates[near_best.last_index].correction - estimate.mapped_time_correction) +
      options.schedule_offset_resolution / 2 + options.optical.schedule_timing_uncertainty;

  estimate.next_best_score = near_best.next_best_score;

  if (estimate.best_plateau_touches_search_edge) {
    estimate.diagnostic = "schedule-offset fit reaches the configured search edge";
  } else if (estimate.uncertainty > options.maximum_schedule_offset_uncertainty) {
    estimate.diagnostic = "schedule-offset fit plateau is too wide";
  } else if (estimate.fit_score < options.minimum_schedule_offset_fit_score) {
    estimate.diagnostic = "schedule-offset fit score is too low";
  } else if (estimate.fit_score - estimate.next_best_score <
             options.minimum_schedule_offset_score_margin) {
    estimate.diagnostic = "schedule-offset fit is not separated from the next-best candidate";
  } else {
    estimate.available = true;
    estimate.diagnostic = "estimated an unambiguous camera mapped-time correction";
  }
  return estimate;
}

std::vector<Clock::time_point> CorrectedHostTimes(
    std::span<const Clock::time_point> mapped_host_times, Clock::duration correction) {
  std::vector<Clock::time_point> corrected;
  corrected.reserve(mapped_host_times.size());
  for (const Clock::time_point time : mapped_host_times) {
    corrected.push_back(time + correction);
  }
  return corrected;
}

RgbStateEvidence SummarizeWhiteImpact(std::span<const FeatherRgbStep> schedule,
                                      std::span<const RgbFrameEvidence> frames,
                                      const RgbSwingAnalysisOptions &options) {
  const auto impact = std::ranges::find(schedule, RgbSwingPhase::kImpact, &FeatherRgbStep::phase);
  if (impact == schedule.end()) {
    throw std::logic_error("white impact schedule is unavailable");
  }
  RgbStateEvidence state{
      .step_index = static_cast<std::size_t>(impact - schedule.begin()),
      .phase = RgbSwingPhase::kImpact,
      .expected_color = impact->color,
      .brightness = impact->brightness,
      .required_for_swing = true,
  };
  double signal_sum = 0.0;
  double distance_sum = 0.0;
  for (const RgbFrameEvidence &frame : frames) {
    if (frame.phase != RgbFramePhase::kStableStep || !frame.expected_step_index.has_value() ||
        schedule[*frame.expected_step_index].phase != RgbSwingPhase::kImpact) {
      continue;
    }
    ++state.stable_frame_count;
    state.matching_frame_count += frame.expected_color_match ? 1U : 0U;
    signal_sum += frame.signal_delta;
    distance_sum += frame.expected_color_distance;
    state.maximum_saturated_fraction =
        std::max(state.maximum_saturated_fraction, frame.saturated_fraction);
    state.maximum_bloom_fraction = std::max(state.maximum_bloom_fraction, frame.bloom_fraction);
  }
  if (state.stable_frame_count == 0U) {
    return state;
  }
  const auto count = static_cast<double>(state.stable_frame_count);
  state.matching_fraction = static_cast<double>(state.matching_frame_count) / count;
  state.mean_signal_delta = signal_sum / count;
  state.mean_expected_color_distance = distance_sum / count;
  state.passed = state.stable_frame_count >= options.minimum_stable_impact_frames &&
                 state.matching_fraction >= options.minimum_state_matching_fraction &&
                 state.mean_signal_delta >= options.minimum_signal_delta &&
                 state.maximum_saturated_fraction <= options.maximum_saturated_fraction &&
                 state.maximum_bloom_fraction <= options.maximum_bloom_fraction;
  return state;
}

RgbStateEvidence SummarizeState(std::size_t step_index, const FeatherRgbStep &step,
                                std::span<const RgbFrameEvidence> frames,
                                const RgbSwingAnalysisOptions &options) {
  RgbStateEvidence state{
      .step_index = step_index,
      .phase = step.phase,
      .expected_color = step.color,
      .brightness = step.brightness,
      .required_for_swing = IsRequiredSwingPhase(step.phase),
  };
  double signal_sum = 0.0;
  double distance_sum = 0.0;
  for (const RgbFrameEvidence &frame : frames) {
    if (frame.phase != RgbFramePhase::kStableStep || frame.expected_step_index != step_index) {
      continue;
    }
    ++state.stable_frame_count;
    state.matching_frame_count += frame.expected_color_match ? 1U : 0U;
    signal_sum += frame.signal_delta;
    distance_sum += frame.expected_color_distance;
    state.maximum_saturated_fraction =
        std::max(state.maximum_saturated_fraction, frame.saturated_fraction);
    state.maximum_bloom_fraction = std::max(state.maximum_bloom_fraction, frame.bloom_fraction);
  }
  if (state.stable_frame_count == 0U) {
    return state;
  }
  const auto count = static_cast<double>(state.stable_frame_count);
  state.matching_fraction = static_cast<double>(state.matching_frame_count) / count;
  state.mean_signal_delta = signal_sum / count;
  state.mean_expected_color_distance = distance_sum / count;
  const std::size_t minimum_frames = step.phase == RgbSwingPhase::kImpact
                                         ? options.minimum_stable_impact_frames
                                         : options.minimum_stable_state_frames;
  if (step.phase == RgbSwingPhase::kBaseline) {
    state.passed = state.stable_frame_count >= options.minimum_baseline_frames;
  } else {
    state.passed = state.stable_frame_count >= minimum_frames &&
                   state.matching_fraction >= options.minimum_state_matching_fraction &&
                   state.mean_signal_delta >= options.minimum_signal_delta &&
                   state.maximum_saturated_fraction <= options.maximum_saturated_fraction &&
                   state.maximum_bloom_fraction <= options.maximum_bloom_fraction;
  }
  return state;
}

RgbTransitionEvidence SummarizeTransition(std::size_t from_step_index,
                                          std::span<const FeatherRgbStep> schedule,
                                          std::span<const RgbFrameEvidence> frames) {
  const std::size_t to_step_index = from_step_index + 1U;
  RgbTransitionEvidence transition{
      .from_step_index = from_step_index,
      .to_step_index = to_step_index,
      .scheduled_transition = schedule[to_step_index].start,
      .last_matching_from_frame_index = std::nullopt,
      .first_matching_to_frame_index = std::nullopt,
      .observed_bracket_begin = std::nullopt,
      .observed_bracket_end = std::nullopt,
  };
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const RgbFrameEvidence &frame = frames[index];
    if (frame.phase != RgbFramePhase::kStableStep || !frame.expected_color_match) {
      continue;
    }
    if (frame.expected_step_index == from_step_index) {
      transition.last_matching_from_frame_index = index;
      transition.observed_bracket_begin = frame.mapped_host_time;
    }
    if (frame.expected_step_index == to_step_index &&
        !transition.first_matching_to_frame_index.has_value()) {
      transition.first_matching_to_frame_index = index;
      transition.observed_bracket_end = frame.mapped_host_time;
    }
  }
  transition.ordered =
      transition.last_matching_from_frame_index.has_value() &&
      transition.first_matching_to_frame_index.has_value() &&
      *transition.last_matching_from_frame_index < *transition.first_matching_to_frame_index;
  return transition;
}

void AddRejection(bool rejected, std::string reason, BrightnessCandidateEvidence *candidate) {
  if (rejected) {
    candidate->rejection_reasons.push_back(std::move(reason));
  }
}

std::vector<BrightnessCandidateEvidence> SummarizeBrightnessSweep(
    std::span<const FeatherRgbStep> schedule, std::span<const RgbFrameEvidence> frames,
    const RgbSwingAnalysisOptions &options) {
  std::map<std::uint8_t, BrightnessAggregate> aggregates;
  for (const FeatherRgbStep &step : schedule) {
    if (step.phase == RgbSwingPhase::kBrightnessProbe) {
      aggregates.try_emplace(step.brightness,
                             BrightnessAggregate{.evidence = {.brightness = step.brightness,
                                                              .rejection_reasons = {}}});
    }
  }
  for (const RgbFrameEvidence &frame : frames) {
    if (frame.phase != RgbFramePhase::kStableStep || !frame.expected_step_index.has_value()) {
      continue;
    }
    const FeatherRgbStep &step = schedule[*frame.expected_step_index];
    if (step.phase != RgbSwingPhase::kBrightnessProbe) {
      continue;
    }
    BrightnessAggregate &aggregate = aggregates.at(step.brightness);
    BrightnessCandidateEvidence &candidate = aggregate.evidence;
    if (candidate.stable_frame_count == 0U) {
      candidate.minimum_signal_delta = frame.signal_delta;
      candidate.minimum_signal_to_background_noise = frame.signal_to_background_noise;
    } else {
      candidate.minimum_signal_delta = std::min(candidate.minimum_signal_delta, frame.signal_delta);
      candidate.minimum_signal_to_background_noise =
          std::min(candidate.minimum_signal_to_background_noise, frame.signal_to_background_noise);
    }
    ++candidate.stable_frame_count;
    aggregate.matching_frames += frame.expected_color_match ? 1U : 0U;
    candidate.maximum_saturated_fraction =
        std::max(candidate.maximum_saturated_fraction, frame.saturated_fraction);
    candidate.maximum_bloom_fraction =
        std::max(candidate.maximum_bloom_fraction, frame.bloom_fraction);
  }

  std::vector<BrightnessCandidateEvidence> candidates;
  candidates.reserve(aggregates.size());
  for (auto &[brightness, aggregate] : aggregates) {
    BrightnessCandidateEvidence &candidate = aggregate.evidence;
    AddRejection(candidate.stable_frame_count < options.minimum_brightness_probe_frames,
                 "insufficient stable probe frames", &candidate);
    AddRejection(candidate.minimum_signal_delta < options.minimum_signal_delta,
                 "signal is not clearly above the local background", &candidate);
    AddRejection(
        candidate.minimum_signal_to_background_noise < options.minimum_signal_to_background_noise,
        "signal-to-background-noise is too low", &candidate);
    AddRejection(candidate.maximum_saturated_fraction > options.maximum_saturated_fraction,
                 "ROI saturation exceeds the safe bound", &candidate);
    AddRejection(candidate.maximum_bloom_fraction > options.maximum_bloom_fraction,
                 "background-annulus bloom exceeds the safe bound", &candidate);
    const double matching_fraction = candidate.stable_frame_count == 0U
                                         ? 0.0
                                         : static_cast<double>(aggregate.matching_frames) /
                                               static_cast<double>(candidate.stable_frame_count);
    AddRejection(matching_fraction < options.minimum_state_matching_fraction,
                 "probe color does not decode consistently", &candidate);
    candidate.safe_for_camera = candidate.rejection_reasons.empty();
    candidates.push_back(std::move(candidate));
    (void)brightness;
  }
  return candidates;
}

void LocateImpactFrames(RgbSwingAnalysis *analysis, std::span<const FeatherRgbStep> schedule) {
  for (std::size_t index = 0; index < analysis->frames.size(); ++index) {
    const RgbFrameEvidence &frame = analysis->frames[index];
    if (frame.phase != RgbFramePhase::kStableStep || !frame.expected_step_index.has_value() ||
        schedule[*frame.expected_step_index].phase != RgbSwingPhase::kImpact ||
        !frame.expected_color_match) {
      continue;
    }
    if (!analysis->first_impact_frame_index.has_value()) {
      analysis->first_impact_frame_index = index;
    }
    analysis->last_impact_frame_index = index;
  }
}

std::string FailureDiagnostic(std::span<const RgbStateEvidence> states, bool impact_located) {
  std::ostringstream message;
  const bool impact_passed = std::ranges::any_of(states, [](const RgbStateEvidence &state) {
    return state.required_for_swing && state.passed;
  });
  message << (impact_passed ? "white impact evidence is incomplete"
                            : "white impact optical qualification failed");
  if (!impact_located) {
    message << "; no stable white impact frames were located";
  }
  return message.str();
}

const BrightnessCandidateEvidence *FindCandidate(const CameraBrightnessSweep &sweep,
                                                 std::uint8_t brightness) {
  const auto found =
      std::ranges::find(sweep.candidates, brightness, &BrightnessCandidateEvidence::brightness);
  return found == sweep.candidates.end() ? nullptr : &*found;
}

void ValidateSweeps(std::span<const CameraBrightnessSweep> sweeps) {
  if (sweeps.size() < 2U) {
    throw std::invalid_argument("shared RGB brightness selection requires both camera sweeps");
  }
  std::set<std::string> camera_ids;
  for (const CameraBrightnessSweep &sweep : sweeps) {
    if (sweep.camera_id.empty() || !camera_ids.insert(sweep.camera_id).second ||
        sweep.candidates.empty()) {
      throw std::invalid_argument("RGB brightness sweeps require unique cameras and candidates");
    }
    std::set<std::uint8_t> brightnesses;
    for (const BrightnessCandidateEvidence &candidate : sweep.candidates) {
      if (candidate.brightness == 0U || !brightnesses.insert(candidate.brightness).second) {
        throw std::invalid_argument(
            "camera RGB sweep brightness levels must be unique and nonzero");
      }
    }
  }
}

std::set<std::uint8_t> ObservedBrightnesses(std::span<const CameraBrightnessSweep> sweeps) {
  std::set<std::uint8_t> brightnesses;
  for (const CameraBrightnessSweep &sweep : sweeps) {
    for (const BrightnessCandidateEvidence &candidate : sweep.candidates) {
      brightnesses.insert(candidate.brightness);
    }
  }
  return brightnesses;
}

void AddCameraRejections(const CameraBrightnessSweep &sweep,
                         const BrightnessCandidateEvidence &candidate,
                         SharedBrightnessCandidate *shared) {
  if (candidate.rejection_reasons.empty()) {
    shared->rejection_reasons.push_back(sweep.camera_id + ": candidate is unsafe");
    return;
  }
  for (const std::string &reason : candidate.rejection_reasons) {
    shared->rejection_reasons.push_back(sweep.camera_id + ": " + reason);
  }
}

bool MergeCameraCandidate(const CameraBrightnessSweep &sweep, std::uint8_t brightness,
                          SharedBrightnessCandidate *shared) {
  const BrightnessCandidateEvidence *candidate = FindCandidate(sweep, brightness);
  if (candidate == nullptr) {
    shared->rejection_reasons.push_back(sweep.camera_id + ": candidate was not observed");
    return false;
  }
  ++shared->cameras_observed;
  shared->worst_minimum_signal_delta =
      std::min(shared->worst_minimum_signal_delta, candidate->minimum_signal_delta);
  shared->worst_minimum_signal_to_background_noise =
      std::min(shared->worst_minimum_signal_to_background_noise,
               candidate->minimum_signal_to_background_noise);
  shared->worst_maximum_saturated_fraction =
      std::max(shared->worst_maximum_saturated_fraction, candidate->maximum_saturated_fraction);
  shared->worst_maximum_bloom_fraction =
      std::max(shared->worst_maximum_bloom_fraction, candidate->maximum_bloom_fraction);
  if (candidate->safe_for_camera) {
    return true;
  }
  AddCameraRejections(sweep, *candidate, shared);
  return false;
}

bool MergeNonSaturatingCameraCandidate(const CameraBrightnessSweep &sweep, std::uint8_t brightness,
                                       const PwmTolerantBrightnessSelectionOptions &options,
                                       SharedBrightnessCandidate *shared) {
  const BrightnessCandidateEvidence *candidate = FindCandidate(sweep, brightness);
  if (candidate == nullptr) {
    shared->rejection_reasons.push_back(sweep.camera_id + ": candidate was not observed");
    return false;
  }
  ++shared->cameras_observed;
  shared->worst_minimum_signal_delta =
      std::min(shared->worst_minimum_signal_delta, candidate->minimum_signal_delta);
  shared->worst_minimum_signal_to_background_noise =
      std::min(shared->worst_minimum_signal_to_background_noise,
               candidate->minimum_signal_to_background_noise);
  shared->worst_maximum_saturated_fraction =
      std::max(shared->worst_maximum_saturated_fraction, candidate->maximum_saturated_fraction);
  shared->worst_maximum_bloom_fraction =
      std::max(shared->worst_maximum_bloom_fraction, candidate->maximum_bloom_fraction);

  bool accepted = true;
  const auto reject = [&](bool condition, std::string reason) {
    if (condition) {
      accepted = false;
      shared->rejection_reasons.push_back(sweep.camera_id + ": " + std::move(reason));
    }
  };
  reject(candidate->stable_frame_count < options.minimum_stable_frames,
         "candidate has too few stable calibration frames");
  reject(candidate->maximum_saturated_fraction > options.maximum_saturated_fraction,
         "ROI saturation exceeds the fallback safe bound");
  reject(candidate->maximum_bloom_fraction > options.maximum_bloom_fraction,
         "background-annulus bloom exceeds the fallback safe bound");
  return accepted;
}

SharedBrightnessCandidate BuildSharedCandidate(std::uint8_t brightness,
                                               std::span<const CameraBrightnessSweep> sweeps) {
  SharedBrightnessCandidate shared{
      .brightness = brightness,
      .worst_minimum_signal_delta = std::numeric_limits<double>::infinity(),
      .worst_minimum_signal_to_background_noise = std::numeric_limits<double>::infinity(),
      .rejection_reasons = {},
  };
  bool every_safe = true;
  for (const CameraBrightnessSweep &sweep : sweeps) {
    every_safe = MergeCameraCandidate(sweep, brightness, &shared) && every_safe;
  }
  shared.safe_for_every_camera = every_safe && shared.cameras_observed == sweeps.size();
  return shared;
}

SharedBrightnessCandidate BuildNonSaturatingSharedCandidate(
    std::uint8_t brightness, std::span<const CameraBrightnessSweep> sweeps,
    const PwmTolerantBrightnessSelectionOptions &options) {
  SharedBrightnessCandidate shared{
      .brightness = brightness,
      .worst_minimum_signal_delta = std::numeric_limits<double>::infinity(),
      .worst_minimum_signal_to_background_noise = std::numeric_limits<double>::infinity(),
      .rejection_reasons = {},
  };
  bool every_safe = true;
  for (const CameraBrightnessSweep &sweep : sweeps) {
    every_safe =
        MergeNonSaturatingCameraCandidate(sweep, brightness, options, &shared) && every_safe;
  }
  shared.safe_for_every_camera = every_safe && shared.cameras_observed == sweeps.size();
  return shared;
}

void AppendRejectionReasons(std::ostringstream *message, std::span<const std::string> reasons) {
  if (reasons.empty()) {
    *message << "none";
    return;
  }
  for (std::size_t index = 0; index < reasons.size(); ++index) {
    if (index != 0U) {
      *message << '|';
    }
    *message << reasons[index];
  }
}

std::string SharedBrightnessFailureDiagnostic(
    std::span<const CameraBrightnessSweep> sweeps,
    std::span<const SharedBrightnessCandidate> shared_candidates) {
  std::ostringstream message;
  message << std::fixed << std::setprecision(3)
          << "no brightness candidate was safe in every camera; per_camera=[";
  for (std::size_t sweep_index = 0; sweep_index < sweeps.size(); ++sweep_index) {
    if (sweep_index != 0U) {
      message << ';';
    }
    const CameraBrightnessSweep &sweep = sweeps[sweep_index];
    message << sweep.camera_id << ":[";
    for (std::size_t candidate_index = 0; candidate_index < sweep.candidates.size();
         ++candidate_index) {
      if (candidate_index != 0U) {
        message << ',';
      }
      const BrightnessCandidateEvidence &candidate = sweep.candidates[candidate_index];
      message << static_cast<unsigned int>(candidate.brightness)
              << "{eligible=" << (candidate.safe_for_camera ? "true" : "false")
              << ",stable_frames=" << candidate.stable_frame_count
              << ",min_signal=" << candidate.minimum_signal_delta
              << ",min_snr=" << candidate.minimum_signal_to_background_noise
              << ",max_saturation=" << candidate.maximum_saturated_fraction
              << ",max_bloom=" << candidate.maximum_bloom_fraction << ",reasons=";
      AppendRejectionReasons(&message, candidate.rejection_reasons);
      message << '}';
    }
    message << ']';
  }
  message << "]; shared_intersection=[]; shared_candidates=[";
  for (std::size_t index = 0; index < shared_candidates.size(); ++index) {
    if (index != 0U) {
      message << ',';
    }
    const SharedBrightnessCandidate &candidate = shared_candidates[index];
    message << static_cast<unsigned int>(candidate.brightness)
            << "{eligible=" << (candidate.safe_for_every_camera ? "true" : "false")
            << ",observed=" << candidate.cameras_observed << '/' << sweeps.size() << ",reasons=";
    AppendRejectionReasons(&message, candidate.rejection_reasons);
    message << '}';
  }
  message << ']';
  return message.str();
}

}  // namespace

rgb_swing_internal::ConnectedOffsetScoreComponent
rgb_swing_internal::FindConnectedOffsetScoreComponent(std::span<const double> scores,
                                                      std::size_t first_best_index,
                                                      std::size_t last_best_index,
                                                      double score_margin) {
  if (scores.empty() || first_best_index > last_best_index || last_best_index >= scores.size() ||
      !std::isfinite(scores[first_best_index]) || !std::isfinite(score_margin) ||
      score_margin < 0.0) {
    throw std::invalid_argument("RGB offset score component arguments are invalid");
  }
  const double best_score = scores[first_best_index];
  std::size_t first = first_best_index;
  while (first > 0U && best_score - scores[first - 1U] < score_margin) {
    --first;
  }
  std::size_t last = last_best_index;
  while (last + 1U < scores.size() && best_score - scores[last + 1U] < score_margin) {
    ++last;
  }
  double next_best_score = -std::numeric_limits<double>::infinity();
  for (std::size_t index = 0; index < scores.size(); ++index) {
    if (index < first || index > last) {
      next_best_score = std::max(next_best_score, scores[index]);
    }
  }
  if (!std::isfinite(next_best_score)) {
    next_best_score = 0.0;
  }
  return {
      .first_index = first,
      .last_index = last,
      .touches_search_edge = first == 0U || last + 1U == scores.size(),
      .next_best_score = next_best_score,
  };
}

RgbSwingAnalysis AnalyzeRgbSwing(const RgbSwingCameraInput &input,
                                 const RgbSwingAnalysisOptions &options) {
  ValidateOptions(options);
  ValidateSwingSchedule(input.schedule);
  static_cast<void>(ValidateFramesAndTimeline(input, true));
  ValidateRegion(input);
  const std::vector<FrameScheduleMembership> memberships = ClassifyFrames(input, options);
  const BaselineModel baseline = BuildBaseline(input, memberships, options);
  const std::vector<RgbFrameEvidence> preliminary =
      MeasureFrames(input, memberships, baseline, {}, options);
  const BayerColorCalibration calibration =
      input.color_calibration.has_value()
          ? *input.color_calibration
          : EstimateColorCalibration(input.schedule, preliminary, options);
  ValidateColorCalibration(calibration);

  RgbSwingAnalysis analysis{
      .camera_id = input.camera_id,
      .fixture_neopixel_region = input.fixture_neopixel_region,
      .diagnostic = {},
      .color_calibration = calibration,
      .frames = {},
      .states = {},
      .transitions = {},
      .first_impact_frame_index = std::nullopt,
      .last_impact_frame_index = std::nullopt,
      .brightness_candidates = {},
  };
  analysis.frames = MeasureFrames(input, memberships, baseline, calibration, options);
  analysis.states.reserve(input.schedule.size());
  for (std::size_t index = 0; index < input.schedule.size(); ++index) {
    analysis.states.push_back(
        SummarizeState(index, input.schedule[index], analysis.frames, options));
  }
  analysis.transitions.reserve(input.schedule.size() - 1U);
  for (std::size_t index = 0; index + 1U < input.schedule.size(); ++index) {
    analysis.transitions.push_back(SummarizeTransition(index, input.schedule, analysis.frames));
  }
  LocateImpactFrames(&analysis, input.schedule);
  analysis.brightness_candidates =
      SummarizeBrightnessSweep(input.schedule, analysis.frames, options);

  const bool states_passed = std::ranges::all_of(
      analysis.states,
      [](const RgbStateEvidence &state) { return !state.required_for_swing || state.passed; });
  const bool impact_located =
      analysis.first_impact_frame_index.has_value() && analysis.last_impact_frame_index.has_value();
  analysis.passed = states_passed && impact_located;
  analysis.diagnostic = analysis.passed ? "stable white impact qualified"
                                        : FailureDiagnostic(analysis.states, impact_located);
  return analysis;
}

RgbSwingAnalysis AnalyzeRgbWhiteImpact(const RgbSwingCameraInput &input,
                                       const RgbSwingAnalysisOptions &options) {
  ValidateOptions(options);
  ValidateSwingSchedule(input.schedule);
  static_cast<void>(ValidateFramesAndTimeline(input, true));
  ValidateRegion(input);
  const std::vector<FrameScheduleMembership> memberships = ClassifyFrames(input, options);
  const BaselineModel baseline = BuildBaseline(input, memberships, options);
  const BayerColorCalibration calibration =
      input.color_calibration.value_or(BayerColorCalibration{});
  ValidateColorCalibration(calibration);

  RgbSwingAnalysis analysis{
      .camera_id = input.camera_id,
      .fixture_neopixel_region = input.fixture_neopixel_region,
      .diagnostic = {},
      .color_calibration = calibration,
      .frames = {},
      .states = {},
      .transitions = {},
      .first_impact_frame_index = std::nullopt,
      .last_impact_frame_index = std::nullopt,
      .brightness_candidates = {},
  };
  analysis.frames = MeasureFrames(input, memberships, baseline, calibration, options,
                                  ColorEvaluation::kWhiteImpactOnly);
  analysis.states.push_back(SummarizeWhiteImpact(input.schedule, analysis.frames, options));
  LocateImpactFrames(&analysis, input.schedule);

  const bool states_passed = std::ranges::all_of(analysis.states, &RgbStateEvidence::passed);
  const bool impact_located =
      analysis.first_impact_frame_index.has_value() && analysis.last_impact_frame_index.has_value();
  analysis.passed = states_passed && impact_located;
  analysis.diagnostic = analysis.passed ? "stable white impact qualified"
                                        : FailureDiagnostic(analysis.states, impact_located);
  return analysis;
}

RgbBrightnessCalibration AnalyzeRgbBrightnessCalibration(
    const RgbBrightnessCalibrationInput &input, const RgbBrightnessCalibrationOptions &options) {
  ValidateCalibrationOptions(options);
  ValidateCalibrationSchedule(input.schedule);
  RgbSwingAnalysisOptions measurement_options = options.optical;
  measurement_options.minimum_brightness_probe_frames = options.minimum_frames_per_probe;
  RgbSwingAnalysisOptions locator_timing_options = measurement_options;
  locator_timing_options.frame_delivery_latency_bound += options.maximum_absolute_schedule_offset;
  // The coarse locator must remain valid across the entire unknown offset
  // interval. One provably OFF frame is sufficient to rank schedule-correlated
  // regions; after offset estimation the corrected acceptance analysis still
  // requires the normal multi-frame baseline.
  locator_timing_options.minimum_baseline_frames = 1;
  const RgbSwingCameraInput timeline = CalibrationView(input, {.width = 2, .height = 2});
  const std::uint64_t missing_frame_ids = ValidateFramesAndTimeline(timeline, false);
  const std::vector<FrameScheduleMembership> baseline_memberships =
      ClassifyFrames(timeline, locator_timing_options);
  const std::vector<FrameScheduleMembership> memberships =
      ClassifyFrames(timeline, measurement_options);
  const Bounds full_frame{
      .right = input.frames.front().image.width,
      .bottom = input.frames.front().image.height,
  };
  const BaselineModel baseline =
      BuildBaselineWithin(timeline, baseline_memberships, locator_timing_options, full_frame);
  const auto integrals = BuildProbeResponseIntegrals(timeline, memberships, baseline);
  const std::vector<LocatorCandidateScore> ranked =
      RankLocatorRegions(timeline, integrals, options);
  const std::optional<LocatorCandidateScore> selected =
      SelectLocatorRegion(input, memberships, baseline, ranked, options);

  RgbBrightnessCalibration calibration{
      .camera_id = input.camera_id,
      .diagnostic = {},
      .selected_region = {},
      .missing_frame_ids = missing_frame_ids,
      .schedule_offset = {},
      .color_calibration = {},
      .frames = {},
      .brightness_candidates = {},
  };
  if (!selected.has_value()) {
    calibration.diagnostic = "no localized white response followed the scheduled brightness probes";
    return calibration;
  }

  calibration.selected_region = selected->region;
  calibration.locator_score = selected->score;
  const RgbSwingCameraInput selected_input = CalibrationView(input, selected->region);
  const std::vector<RgbFrameEvidence> offset_measurements =
      MeasureFrames(selected_input, memberships, baseline, {}, measurement_options);
  calibration.schedule_offset = EstimateScheduleOffset(input, offset_measurements, options);
  if (!calibration.schedule_offset.available) {
    calibration.diagnostic = calibration.schedule_offset.diagnostic;
    return calibration;
  }

  const std::vector<Clock::time_point> corrected_times = CorrectedHostTimes(
      input.mapped_host_times, calibration.schedule_offset.mapped_time_correction);
  RgbSwingCameraInput corrected_input = selected_input;
  corrected_input.mapped_host_times = corrected_times;
  const std::vector<FrameScheduleMembership> corrected_memberships =
      ClassifyFrames(corrected_input, measurement_options);
  const BaselineModel corrected_baseline =
      BuildBaselineWithin(corrected_input, corrected_memberships, measurement_options, full_frame);
  const std::vector<RgbFrameEvidence> preliminary = MeasureFrames(
      corrected_input, corrected_memberships, corrected_baseline, {}, measurement_options);
  calibration.color_calibration =
      EstimateColorCalibration(input.schedule, preliminary, measurement_options);
  calibration.frames = MeasureFrames(corrected_input, corrected_memberships, corrected_baseline,
                                     calibration.color_calibration, measurement_options);
  calibration.brightness_candidates =
      SummarizeBrightnessSweep(input.schedule, calibration.frames, measurement_options);
  calibration.located = true;
  calibration.diagnostic =
      "localized scheduled white response, aligned its timeline, and evaluated brightness sweep";
  return calibration;
}

SharedBrightnessRecommendation RecommendSharedRgbBrightness(
    std::span<const CameraBrightnessSweep> camera_sweeps) {
  ValidateSweeps(camera_sweeps);
  const std::set<std::uint8_t> brightnesses = ObservedBrightnesses(camera_sweeps);

  SharedBrightnessRecommendation recommendation;
  recommendation.candidates.reserve(brightnesses.size());
  for (const std::uint8_t brightness : brightnesses) {
    SharedBrightnessCandidate shared = BuildSharedCandidate(brightness, camera_sweeps);
    if (shared.safe_for_every_camera && !recommendation.brightness.has_value()) {
      recommendation.brightness = brightness;
    }
    recommendation.candidates.push_back(std::move(shared));
  }
  recommendation.available = recommendation.brightness.has_value();
  recommendation.diagnostic =
      recommendation.available
          ? "selected the lowest shared brightness above background and below saturation and "
            "bloom limits"
          : SharedBrightnessFailureDiagnostic(camera_sweeps, recommendation.candidates);
  return recommendation;
}

SharedBrightnessRecommendation RecommendSharedNonSaturatingRgbBrightness(
    std::span<const CameraBrightnessSweep> camera_sweeps,
    const PwmTolerantBrightnessSelectionOptions &options) {
  ValidateSweeps(camera_sweeps);
  if (options.minimum_stable_frames == 0U ||
      !IsFiniteFraction(options.maximum_saturated_fraction) ||
      !IsFiniteFraction(options.maximum_bloom_fraction)) {
    throw std::invalid_argument("PWM-tolerant brightness selection options are invalid");
  }
  const std::set<std::uint8_t> brightnesses = ObservedBrightnesses(camera_sweeps);

  SharedBrightnessRecommendation recommendation;
  recommendation.candidates.reserve(brightnesses.size());
  for (const std::uint8_t brightness : brightnesses) {
    SharedBrightnessCandidate shared =
        BuildNonSaturatingSharedCandidate(brightness, camera_sweeps, options);
    if (shared.safe_for_every_camera && !recommendation.brightness.has_value()) {
      recommendation.brightness = brightness;
    }
    recommendation.candidates.push_back(std::move(shared));
  }
  recommendation.available = recommendation.brightness.has_value();
  recommendation.diagnostic =
      recommendation.available
          ? "selected the lowest repeatedly sampled shared brightness below saturation and bloom "
            "limits; retained white impact remains the signal qualification gate"
          : "no repeatedly sampled brightness remained below saturation and bloom limits in every "
            "camera";
  return recommendation;
}

}  // namespace swing_capture::optical
