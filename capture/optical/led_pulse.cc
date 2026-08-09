#include "capture/optical/led_pulse.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "capture/image/image_quality.h"

namespace swing_capture::optical {
namespace {

struct RegionResponse {
  std::uint32_t changed_samples = 0;
  std::uint32_t sample_count = 0;
  double local_changed_fraction = 0.0;
  double background_changed_fraction = 0.0;
  double changed_fraction = 0.0;
  double local_mean_positive_delta = 0.0;
  double background_mean_positive_delta = 0.0;
  double mean_positive_delta = 0.0;
  bool supported = false;
  bool active = false;
};

struct RegionSums {
  double local_positive_delta = 0.0;
  double background_positive_delta = 0.0;
  std::uint32_t local_changed_samples = 0;
  std::uint32_t background_changed_samples = 0;
  std::uint32_t local_samples = 0;
  std::uint32_t background_samples = 0;
};

struct IlluminationModel {
  double scale = 1.0;
  double offset = 0.0;
};

struct AffineMoments {
  std::size_t count = 0;
  double baseline_mean = 0.0;
  double sample_mean = 0.0;
  double baseline_sum_squared_delta = 0.0;
  double covariance_sum = 0.0;

  // The argument order mirrors the fitted relation: sample = scale * baseline + offset.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void Add(double baseline, double sample) {
    ++count;
    const auto count_as_double = static_cast<double>(count);
    const double baseline_delta = baseline - baseline_mean;
    const double sample_delta = sample - sample_mean;
    baseline_mean += baseline_delta / count_as_double;
    sample_mean += sample_delta / count_as_double;
    baseline_sum_squared_delta += baseline_delta * (baseline - baseline_mean);
    covariance_sum += baseline_delta * (sample - sample_mean);
  }
};

struct CandidateEvidence {
  double accumulated_active_response = 0.0;
  double peak_mean_positive_delta = 0.0;
  std::size_t active_frames = 0;
};

struct PulseRun {
  std::size_t start = 0;
  std::size_t end = 0;
  std::size_t active_frames = 0;
  double response_sum = 0.0;
};

struct MatchedWindow {
  std::size_t start = 0;
  std::size_t end = 0;
  std::size_t supported_frames = 0;
  std::size_t high_confidence_frames = 0;
  std::size_t longest_contiguous_supported_frames = 0;
  double mean_positive_delta = 0.0;
  double changed_fraction = 0.0;
  double background_mean_positive_delta = 0.0;
  double background_changed_fraction = 0.0;
  double score = 0.0;
};

struct MatchedWindowSpec {
  std::size_t baseline_frame_count = 0;
  std::size_t start = 0;
  std::size_t length = 0;
};

struct PulseTimingSpec {
  std::uint64_t timestamp_ticks_per_second = 0;
  std::size_t start = 0;
  std::size_t end = 0;
};

struct CandidateAxis {
  std::uint32_t extent = 0;
  std::uint32_t region_extent = 0;
  std::uint32_t stride = 0;
};

std::size_t CheckedPixelCount(std::uint32_t width, std::uint32_t height) {
  if (height != 0 && width > std::numeric_limits<std::size_t>::max() / height) {
    throw std::invalid_argument("LED pulse image geometry overflows addressable memory");
  }
  return static_cast<std::size_t>(width) * height;
}

std::size_t EffectiveStride(const image::Raw8ImageView &image) {
  return image.row_stride_bytes == 0 ? image.width : image.row_stride_bytes;
}

void ValidateOptions(const LedPulseOptions &options) {
  if (options.baseline_frame_count < 2) {
    throw std::invalid_argument("LED pulse baseline must contain at least two OFF frames");
  }
  if (options.region_width < 2 || options.region_height < 2 || options.region_stride == 0 ||
      options.background_margin == 0) {
    throw std::invalid_argument(
        "LED pulse candidate region must be at least 2x2 and its stride and background margin "
        "must be nonzero");
  }
  if (options.minimum_changed_red_samples == 0) {
    throw std::invalid_argument("LED pulse minimum changed red samples must be nonzero");
  }
  if (options.minimum_changed_red_fraction <= 0.0 || options.minimum_changed_red_fraction > 1.0) {
    throw std::invalid_argument("LED pulse changed-red fraction must be in (0, 1]");
  }
  if (!(options.minimum_mean_positive_red_delta > 0.0)) {
    throw std::invalid_argument("LED pulse mean positive red delta must be positive");
  }
  if (!(options.minimum_support_changed_red_fraction > 0.0) ||
      options.minimum_support_changed_red_fraction > 1.0 ||
      !(options.minimum_support_mean_positive_red_delta > 0.0)) {
    throw std::invalid_argument(
        "LED pulse support thresholds must contain a positive mean delta and a changed-red "
        "fraction in (0, 1]");
  }
  if (options.minimum_support_changed_red_fraction > options.minimum_changed_red_fraction ||
      options.minimum_support_mean_positive_red_delta > options.minimum_mean_positive_red_delta) {
    throw std::invalid_argument(
        "LED pulse support thresholds cannot be stricter than high-confidence thresholds");
  }
  if (options.minimum_active_frames == 0 || options.maximum_pulse_span_frames == 0 ||
      options.minimum_active_frames > options.maximum_pulse_span_frames) {
    throw std::invalid_argument("LED pulse frame-count limits are inconsistent");
  }
  if (options.expected_start_frame_index.has_value()) {
    if (options.minimum_matched_pulse_frames == 0 ||
        options.minimum_matched_pulse_frames > options.maximum_matched_pulse_frames ||
        options.minimum_matched_active_frames == 0 ||
        options.minimum_matched_active_frames > options.maximum_matched_pulse_frames ||
        options.minimum_matched_contiguous_supported_frames == 0 ||
        options.minimum_matched_contiguous_supported_frames >
            options.maximum_matched_pulse_frames) {
      throw std::invalid_argument("LED matched-window frame-count limits are inconsistent");
    }
    if (!(options.minimum_matched_mean_positive_red_delta > 0.0) ||
        !(options.minimum_matched_changed_red_fraction > 0.0) ||
        options.minimum_matched_changed_red_fraction > 1.0) {
      throw std::invalid_argument("LED matched-window response limits are inconsistent");
    }
  }
}

std::uint64_t ValidateFrames(std::span<const BayerRg8FrameView> frames,
                             std::uint64_t timestamp_ticks_per_second,
                             const LedPulseOptions &options) {
  if (timestamp_ticks_per_second == 0) {
    throw std::invalid_argument("LED pulse device timestamp frequency must be nonzero");
  }
  if (frames.size() <= options.baseline_frame_count) {
    throw std::invalid_argument("LED pulse sequence must contain frames after its OFF baseline");
  }

  const auto width = frames.front().image.width;
  const auto height = frames.front().image.height;
  if (width == 0 || height == 0) {
    throw std::invalid_argument("LED pulse frames must have nonzero geometry");
  }
  if (options.region_width > width || options.region_height > height) {
    throw std::invalid_argument("LED pulse candidate region does not fit in the frame");
  }
  if (options.region_width == width && options.region_height == height) {
    throw std::invalid_argument("LED pulse candidate region leaves no surrounding background");
  }
  (void)CheckedPixelCount(width, height);

  std::uint64_t missing_frame_ids = 0;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const auto &frame = frames[index];
    if (frame.image.width != width || frame.image.height != height) {
      throw std::invalid_argument("LED pulse frames must have identical geometry");
    }
    const std::size_t stride = EffectiveStride(frame.image);
    if (stride < width) {
      throw std::invalid_argument("LED pulse frame row stride is smaller than its width");
    }
    const std::size_t required =
        (static_cast<std::size_t>(height) - 1U) * stride + static_cast<std::size_t>(width);
    if (frame.image.pixels.size() < required) {
      throw std::invalid_argument("LED pulse frame payload is smaller than its geometry");
    }
    if (index == 0) {
      continue;
    }
    const auto &previous = frames[index - 1];
    if (frame.frame_id <= previous.frame_id) {
      throw std::invalid_argument("LED pulse frame IDs must be strictly increasing");
    }
    if (frame.device_timestamp <= previous.device_timestamp) {
      throw std::invalid_argument("LED pulse device timestamps must be strictly increasing");
    }
    missing_frame_ids += frame.frame_id - previous.frame_id - 1U;
  }
  return missing_frame_ids;
}

std::vector<std::uint32_t> CandidateAnchors(const CandidateAxis &axis) {
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

std::vector<PixelRegion> CandidateRegions(const image::Raw8ImageView &image,
                                          const LedPulseOptions &options) {
  if (options.locked_region.has_value()) {
    const PixelRegion &region = *options.locked_region;
    const std::uint64_t right = static_cast<std::uint64_t>(region.x) + region.width;
    const std::uint64_t bottom = static_cast<std::uint64_t>(region.y) + region.height;
    if (region.width == 0 || region.height == 0 || right > image.width || bottom > image.height ||
        (region.x == 0 && region.y == 0 && region.width == image.width &&
         region.height == image.height)) {
      throw std::invalid_argument("locked LED region is outside the frame or leaves no background");
    }
    return {region};
  }
  const auto x_anchors = CandidateAnchors({
      .extent = image.width,
      .region_extent = options.region_width,
      .stride = options.region_stride,
  });
  const auto y_anchors = CandidateAnchors({
      .extent = image.height,
      .region_extent = options.region_height,
      .stride = options.region_stride,
  });
  std::vector<PixelRegion> regions;
  regions.reserve(x_anchors.size() * y_anchors.size());
  for (const std::uint32_t y : y_anchors) {
    for (const std::uint32_t x : x_anchors) {
      regions.push_back({
          .x = x,
          .y = y,
          .width = options.region_width,
          .height = options.region_height,
      });
    }
  }
  return regions;
}

std::vector<double> MeasureBaseline(std::span<const BayerRg8FrameView> frames,
                                    std::size_t baseline_frame_count) {
  const auto width = frames.front().image.width;
  const auto height = frames.front().image.height;
  std::vector<double> baseline(CheckedPixelCount(width, height), 0.0);
  for (std::size_t frame_index = 0; frame_index < baseline_frame_count; ++frame_index) {
    const auto &image = frames[frame_index].image;
    const std::size_t stride = EffectiveStride(image);
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        baseline[static_cast<std::size_t>(y) * width + x] +=
            std::to_integer<std::uint8_t>(image.pixels[static_cast<std::size_t>(y) * stride + x]);
      }
    }
  }
  const auto denominator = static_cast<double>(baseline_frame_count);
  for (double &sample : baseline) {
    sample /= denominator;
  }
  return baseline;
}

AffineMoments MeasureIlluminationMoments(const BayerRg8FrameView &frame,
                                         std::span<const double> baseline,
                                         bool exclude_clipped_samples) {
  constexpr std::uint32_t kSampleStep = 4;
  constexpr double kLowReliableSample = 4.0;
  constexpr double kHighReliableSample = 247.0;
  AffineMoments moments;
  const std::size_t stride = EffectiveStride(frame.image);
  for (std::uint32_t y = 0; y < frame.image.height; y += kSampleStep) {
    for (std::uint32_t x = 0; x < frame.image.width; x += kSampleStep) {
      const double baseline_sample = baseline[static_cast<std::size_t>(y) * frame.image.width + x];
      const double sample = std::to_integer<std::uint8_t>(
          frame.image.pixels[static_cast<std::size_t>(y) * stride + x]);
      if (exclude_clipped_samples &&
          (baseline_sample < kLowReliableSample || baseline_sample > kHighReliableSample ||
           sample < kLowReliableSample || sample > kHighReliableSample)) {
        continue;
      }
      moments.Add(baseline_sample, sample);
    }
  }
  return moments;
}

IlluminationModel FitGlobalIllumination(const BayerRg8FrameView &frame,
                                        std::span<const double> baseline) {
  constexpr std::size_t kMinimumReliableSamples = 16;
  constexpr double kMinimumBaselineVariance = 1e-6;
  AffineMoments moments = MeasureIlluminationMoments(frame, baseline, true);
  if (moments.count < kMinimumReliableSamples) {
    moments = MeasureIlluminationMoments(frame, baseline, false);
  }
  double scale = 1.0;
  if (moments.count >= 2 && moments.baseline_sum_squared_delta > kMinimumBaselineVariance) {
    scale = moments.covariance_sum / moments.baseline_sum_squared_delta;
  }
  if (!std::isfinite(scale) || !(scale > 0.0)) {
    scale = 1.0;
  }
  return {
      .scale = scale,
      .offset = moments.sample_mean - scale * moments.baseline_mean,
  };
}

std::vector<IlluminationModel> FitGlobalIlluminationModels(
    std::span<const BayerRg8FrameView> frames, std::span<const double> baseline) {
  std::vector<IlluminationModel> models;
  models.reserve(frames.size());
  for (const BayerRg8FrameView &frame : frames) {
    models.push_back(FitGlobalIllumination(frame, baseline));
  }
  return models;
}

std::size_t IntegralIndex(std::uint32_t x, std::uint32_t y, std::uint32_t columns) {
  return static_cast<std::size_t>(y) * columns + x;
}

template <typename Value>
Value RegionIntegralSum(std::span<const Value> integral, std::uint32_t integral_columns,
                        const PixelRegion &region) {
  const std::uint32_t right = region.x + region.width;
  const std::uint32_t bottom = region.y + region.height;
  return integral[IntegralIndex(right, bottom, integral_columns)] -
         integral[IntegralIndex(region.x, bottom, integral_columns)] -
         integral[IntegralIndex(right, region.y, integral_columns)] +
         integral[IntegralIndex(region.x, region.y, integral_columns)];
}

std::uint32_t EvenCoordinatesBefore(std::uint32_t exclusive_end) {
  return (exclusive_end + 1U) / 2U;
}

std::uint32_t RedSamplesInRegion(const PixelRegion &region) {
  const std::uint32_t red_columns =
      EvenCoordinatesBefore(region.x + region.width) - EvenCoordinatesBefore(region.x);
  const std::uint32_t red_rows =
      EvenCoordinatesBefore(region.y + region.height) - EvenCoordinatesBefore(region.y);
  return red_columns * red_rows;
}

PixelRegion ExpandedBackgroundRegion(const PixelRegion &region, const image::Raw8ImageView &image,
                                     std::uint32_t margin) {
  const std::uint32_t left = region.x > margin ? region.x - margin : 0U;
  const std::uint32_t top = region.y > margin ? region.y - margin : 0U;
  const std::uint32_t right = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      image.width, static_cast<std::uint64_t>(region.x) + region.width + margin));
  const std::uint32_t bottom = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      image.height, static_cast<std::uint64_t>(region.y) + region.height + margin));
  return {
      .x = left,
      .y = top,
      .width = right - left,
      .height = bottom - top,
  };
}

RegionResponse MakeResponse(const RegionSums &sums, const LedPulseOptions &options) {
  if (sums.local_samples == 0 || sums.background_samples == 0) {
    throw std::invalid_argument("LED pulse candidate requires local and background red samples");
  }
  const double local_changed_fraction =
      static_cast<double>(sums.local_changed_samples) / sums.local_samples;
  const double background_changed_fraction =
      static_cast<double>(sums.background_changed_samples) / sums.background_samples;
  const double local_mean_positive_delta = sums.local_positive_delta / sums.local_samples;
  const double background_mean_positive_delta =
      sums.background_positive_delta / sums.background_samples;
  RegionResponse response{
      .changed_samples = sums.local_changed_samples,
      .sample_count = sums.local_samples,
      .local_changed_fraction = local_changed_fraction,
      .background_changed_fraction = background_changed_fraction,
      .changed_fraction = std::max(0.0, local_changed_fraction - background_changed_fraction),
      .local_mean_positive_delta = local_mean_positive_delta,
      .background_mean_positive_delta = background_mean_positive_delta,
      .mean_positive_delta =
          std::max(0.0, local_mean_positive_delta - background_mean_positive_delta),
  };
  response.supported =
      response.changed_fraction >= options.minimum_support_changed_red_fraction &&
      response.mean_positive_delta >= options.minimum_support_mean_positive_red_delta;
  response.active = response.supported &&
                    response.changed_samples >= options.minimum_changed_red_samples &&
                    response.changed_fraction >= options.minimum_changed_red_fraction &&
                    response.mean_positive_delta >= options.minimum_mean_positive_red_delta;
  return response;
}

RegionSums IntegralRegionSums(std::span<const double> delta_integral,
                              std::span<const std::uint32_t> changed_integral,
                              std::uint32_t integral_columns, const PixelRegion &region,
                              const image::Raw8ImageView &image, const LedPulseOptions &options) {
  const PixelRegion expanded = ExpandedBackgroundRegion(region, image, options.background_margin);
  const auto local_positive_delta =
      RegionIntegralSum<double>(delta_integral, integral_columns, region);
  const auto expanded_positive_delta =
      RegionIntegralSum<double>(delta_integral, integral_columns, expanded);
  const auto local_changed =
      RegionIntegralSum<std::uint32_t>(changed_integral, integral_columns, region);
  const auto expanded_changed =
      RegionIntegralSum<std::uint32_t>(changed_integral, integral_columns, expanded);
  const std::uint32_t local_samples = RedSamplesInRegion(region);
  const std::uint32_t expanded_samples = RedSamplesInRegion(expanded);
  return {
      .local_positive_delta = local_positive_delta,
      .background_positive_delta = std::max(0.0, expanded_positive_delta - local_positive_delta),
      .local_changed_samples = local_changed,
      .background_changed_samples = expanded_changed - local_changed,
      .local_samples = local_samples,
      .background_samples = expanded_samples - local_samples,
  };
}

void BuildResponseIntegrals(const BayerRg8FrameView &frame, std::span<const double> baseline,
                            const IlluminationModel &illumination, const LedPulseOptions &options,
                            std::vector<double> &delta_integral,
                            std::vector<std::uint32_t> &changed_integral) {
  const std::uint32_t width = frame.image.width;
  const std::uint32_t height = frame.image.height;
  const std::uint32_t columns = width + 1U;
  std::ranges::fill(delta_integral, 0.0);
  std::ranges::fill(changed_integral, 0U);
  const std::size_t stride = EffectiveStride(frame.image);

  for (std::uint32_t y = 0; y < height; ++y) {
    double delta_row_sum = 0.0;
    std::uint32_t changed_row_sum = 0;
    for (std::uint32_t x = 0; x < width; ++x) {
      if ((x & 1U) == 0U && (y & 1U) == 0U) {
        const double sample = std::to_integer<std::uint8_t>(
            frame.image.pixels[static_cast<std::size_t>(y) * stride + x]);
        const double expected =
            illumination.scale * baseline[static_cast<std::size_t>(y) * width + x] +
            illumination.offset;
        const double delta = std::max(0.0, sample - expected);
        delta_row_sum += delta;
        changed_row_sum += delta >= options.minimum_red_sample_delta ? 1U : 0U;
      }
      const std::size_t position = IntegralIndex(x + 1U, y + 1U, columns);
      const std::size_t above = IntegralIndex(x + 1U, y, columns);
      delta_integral[position] = delta_integral[above] + delta_row_sum;
      changed_integral[position] = changed_integral[above] + changed_row_sum;
    }
  }
}

std::size_t SelectCandidate(std::span<const BayerRg8FrameView> frames,
                            std::span<const double> baseline, std::span<const PixelRegion> regions,
                            std::span<const IlluminationModel> illumination_models,
                            const LedPulseOptions &options) {
  const std::uint32_t columns = frames.front().image.width + 1U;
  const std::size_t integral_size =
      static_cast<std::size_t>(columns) * (frames.front().image.height + 1U);
  std::vector<double> delta_integral(integral_size);
  std::vector<std::uint32_t> changed_integral(integral_size);
  std::vector<CandidateEvidence> evidence(regions.size());

  for (std::size_t frame_index = options.baseline_frame_count; frame_index < frames.size();
       ++frame_index) {
    BuildResponseIntegrals(frames[frame_index], baseline, illumination_models[frame_index], options,
                           delta_integral, changed_integral);
    for (std::size_t region_index = 0; region_index < regions.size(); ++region_index) {
      const auto &region = regions[region_index];
      const auto response =
          MakeResponse(IntegralRegionSums(delta_integral, changed_integral, columns, region,
                                          frames[frame_index].image, options),
                       options);
      auto &candidate = evidence[region_index];
      candidate.peak_mean_positive_delta =
          std::max(candidate.peak_mean_positive_delta, response.mean_positive_delta);
      if (response.active) {
        candidate.accumulated_active_response += response.mean_positive_delta;
        ++candidate.active_frames;
      }
    }
  }

  const auto best = std::ranges::max_element(evidence, {}, [](const CandidateEvidence &candidate) {
    return std::tuple(candidate.accumulated_active_response, candidate.active_frames,
                      candidate.peak_mean_positive_delta);
  });
  return static_cast<std::size_t>(std::distance(evidence.begin(), best));
}

RegionResponse MeasureRegion(const BayerRg8FrameView &frame, std::span<const double> baseline,
                             const IlluminationModel &illumination, const PixelRegion &region,
                             const LedPulseOptions &options) {
  const std::size_t stride = EffectiveStride(frame.image);
  const PixelRegion expanded =
      ExpandedBackgroundRegion(region, frame.image, options.background_margin);
  RegionSums sums;
  for (std::uint32_t y = expanded.y; y < expanded.y + expanded.height; ++y) {
    for (std::uint32_t x = expanded.x; x < expanded.x + expanded.width; ++x) {
      if ((x & 1U) != 0U || (y & 1U) != 0U) {
        continue;
      }
      const double sample = std::to_integer<std::uint8_t>(
          frame.image.pixels[static_cast<std::size_t>(y) * stride + x]);
      const double expected =
          illumination.scale * baseline[static_cast<std::size_t>(y) * frame.image.width + x] +
          illumination.offset;
      const double delta = std::max(0.0, sample - expected);
      const bool local = x >= region.x && x < region.x + region.width && y >= region.y &&
                         y < region.y + region.height;
      if (local) {
        sums.local_positive_delta += delta;
        sums.local_changed_samples += delta >= options.minimum_red_sample_delta ? 1U : 0U;
        ++sums.local_samples;
      } else {
        sums.background_positive_delta += delta;
        sums.background_changed_samples += delta >= options.minimum_red_sample_delta ? 1U : 0U;
        ++sums.background_samples;
      }
    }
  }
  return MakeResponse(sums, options);
}

std::vector<PulseRun> FindRuns(std::span<const LedFrameDiagnostic> diagnostics,
                               const LedPulseOptions &options) {
  std::vector<PulseRun> runs;
  std::size_t last_active = 0;
  bool running = false;
  PulseRun current;
  for (std::size_t index = options.baseline_frame_count; index < diagnostics.size(); ++index) {
    if (!diagnostics[index].active) {
      continue;
    }
    if (!running || index - last_active - 1U > options.maximum_internal_inactive_frames) {
      if (running) {
        current.end = last_active;
        runs.push_back(current);
      }
      current = {
          .start = index,
          .end = index,
          .active_frames = 0,
          .response_sum = 0.0,
      };
      running = true;
    }
    last_active = index;
    ++current.active_frames;
    current.response_sum += diagnostics[index].mean_positive_red_delta;
  }
  if (running) {
    current.end = last_active;
    runs.push_back(current);
  }
  return runs;
}

MatchedWindow MeasureMatchedWindow(std::span<const LedFrameDiagnostic> diagnostics,
                                   const MatchedWindowSpec &spec) {
  MatchedWindow result{
      .start = spec.start,
      .end = spec.start + spec.length - 1U,
  };
  double signal_delta_sum = 0.0;
  double signal_fraction_sum = 0.0;
  double background_delta_sum = 0.0;
  double background_fraction_sum = 0.0;
  std::size_t background_frames = 0;
  std::size_t contiguous_supported_frames = 0;
  for (std::size_t index = spec.baseline_frame_count; index < diagnostics.size(); ++index) {
    const LedFrameDiagnostic &frame = diagnostics[index];
    if (index >= result.start && index <= result.end) {
      signal_delta_sum += frame.mean_positive_red_delta;
      signal_fraction_sum += frame.changed_red_fraction;
      result.supported_frames += frame.supported ? 1U : 0U;
      result.high_confidence_frames += frame.active ? 1U : 0U;
      contiguous_supported_frames = frame.supported ? contiguous_supported_frames + 1U : 0U;
      result.longest_contiguous_supported_frames =
          std::max(result.longest_contiguous_supported_frames, contiguous_supported_frames);
    } else {
      background_delta_sum += frame.mean_positive_red_delta;
      background_fraction_sum += frame.changed_red_fraction;
      ++background_frames;
    }
  }
  const auto signal_frames = static_cast<double>(spec.length);
  const auto background_denominator = static_cast<double>(background_frames);
  result.background_mean_positive_delta =
      background_frames == 0 ? 0.0 : background_delta_sum / background_denominator;
  result.background_changed_fraction =
      background_frames == 0 ? 0.0 : background_fraction_sum / background_denominator;
  result.mean_positive_delta =
      std::max(0.0, signal_delta_sum / signal_frames - result.background_mean_positive_delta);
  result.changed_fraction =
      std::max(0.0, signal_fraction_sum / signal_frames - result.background_changed_fraction);
  result.score = result.mean_positive_delta * signal_frames;
  return result;
}

std::size_t StartDistance(std::size_t start, std::size_t expected) {
  return start > expected ? start - expected : expected - start;
}

bool IsBetterMatchedWindow(const MatchedWindow &left, const MatchedWindow &right,
                           std::size_t expected_start) {
  const auto left_evidence =
      std::tuple(left.longest_contiguous_supported_frames, left.supported_frames,
                 left.high_confidence_frames, left.score, left.changed_fraction);
  const auto right_evidence =
      std::tuple(right.longest_contiguous_supported_frames, right.supported_frames,
                 right.high_confidence_frames, right.score, right.changed_fraction);
  if (left_evidence != right_evidence) {
    return left_evidence > right_evidence;
  }
  const std::size_t left_unsupported = left.end - left.start + 1U - left.supported_frames;
  const std::size_t right_unsupported = right.end - right.start + 1U - right.supported_frames;
  if (left_unsupported != right_unsupported) {
    return left_unsupported < right_unsupported;
  }
  return StartDistance(left.start, expected_start) < StartDistance(right.start, expected_start);
}

MatchedWindow FindBestMatchedWindow(std::span<const LedFrameDiagnostic> diagnostics,
                                    const LedPulseOptions &options) {
  if (!options.expected_start_frame_index.has_value()) {
    throw std::invalid_argument("LED matched-window search requires an expected start");
  }
  const std::size_t expected = options.expected_start_frame_index.value();
  if (expected < options.baseline_frame_count || expected >= diagnostics.size()) {
    throw std::invalid_argument("LED matched-window expected start is outside analyzable frames");
  }
  const std::size_t earliest = std::max(options.baseline_frame_count,
                                        expected > options.expected_start_tolerance_frames
                                            ? expected - options.expected_start_tolerance_frames
                                            : std::size_t{0});
  const std::size_t latest_by_tolerance =
      expected > std::numeric_limits<std::size_t>::max() - options.expected_start_tolerance_frames
          ? std::numeric_limits<std::size_t>::max()
          : expected + options.expected_start_tolerance_frames;
  if (diagnostics.size() < options.minimum_matched_pulse_frames) {
    throw std::invalid_argument("LED matched-window input is shorter than its minimum span");
  }
  const std::size_t latest =
      std::min(latest_by_tolerance, diagnostics.size() - options.minimum_matched_pulse_frames);
  if (earliest > latest) {
    throw std::invalid_argument("LED matched-window has no candidate near the expected start");
  }

  std::optional<MatchedWindow> best;
  for (std::size_t start = earliest; start <= latest; ++start) {
    for (std::size_t length = options.minimum_matched_pulse_frames;
         length <= options.maximum_matched_pulse_frames && length <= diagnostics.size() - start;
         ++length) {
      const MatchedWindow candidate = MeasureMatchedWindow(
          diagnostics,
          {.baseline_frame_count = options.baseline_frame_count, .start = start, .length = length});
      if (!best.has_value() || IsBetterMatchedWindow(candidate, *best, expected)) {
        best = candidate;
      }
    }
  }
  if (!best.has_value()) {
    throw std::invalid_argument("LED matched-window search produced no candidate");
  }
  return *best;
}

std::uint64_t MedianFrameInterval(std::span<const BayerRg8FrameView> frames) {
  std::vector<std::uint64_t> intervals;
  intervals.reserve(frames.size() - 1U);
  for (std::size_t index = 1; index < frames.size(); ++index) {
    intervals.push_back(frames[index].device_timestamp - frames[index - 1].device_timestamp);
  }
  std::ranges::sort(intervals);
  const std::size_t middle = intervals.size() / 2U;
  if ((intervals.size() & 1U) != 0U) {
    return intervals[middle];
  }
  return intervals[middle - 1U] + (intervals[middle] - intervals[middle - 1U]) / 2U;
}

bool IsBetterRun(const PulseRun &left, const PulseRun &right) {
  return std::tuple(left.active_frames, left.response_sum) >
         std::tuple(right.active_frames, right.response_sum);
}

void PopulatePulseTiming(LedPulseResult *result, std::span<const BayerRg8FrameView> frames,
                         const PulseTimingSpec &spec) {
  result->pulse_start_frame_index = spec.start;
  result->pulse_end_frame_index = spec.end;
  result->pulse_span_frame_count = spec.end - spec.start + 1U;
  result->start_device_timestamp = frames[spec.start].device_timestamp;
  result->nominal_frame_interval_ticks = MedianFrameInterval(frames);
  const std::uint64_t timestamp_span =
      frames[spec.end].device_timestamp - frames[spec.start].device_timestamp;
  if (timestamp_span >
      std::numeric_limits<std::uint64_t>::max() - result->nominal_frame_interval_ticks) {
    throw std::invalid_argument("LED pulse device-timestamp duration overflows");
  }
  result->duration_ticks = timestamp_span + result->nominal_frame_interval_ticks;
  if (result->start_device_timestamp >
      std::numeric_limits<std::uint64_t>::max() - result->duration_ticks) {
    throw std::invalid_argument("LED pulse exclusive end timestamp overflows");
  }
  result->end_device_timestamp_exclusive = result->start_device_timestamp + result->duration_ticks;
  result->duration_seconds = static_cast<double>(result->duration_ticks) /
                             static_cast<double>(spec.timestamp_ticks_per_second);
}

void MeasureSelectedRegion(LedPulseResult *result, std::span<const BayerRg8FrameView> frames,
                           std::span<const double> baseline,
                           std::span<const IlluminationModel> illumination_models,
                           const LedPulseOptions &options) {
  result->frames.reserve(frames.size());
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const RegionResponse response = MeasureRegion(
        frames[index], baseline, illumination_models[index], result->selected_region, options);
    result->frames.push_back({
        .frame_id = frames[index].frame_id,
        .device_timestamp = frames[index].device_timestamp,
        .changed_red_samples = response.changed_samples,
        .red_samples = response.sample_count,
        .local_changed_red_fraction = response.local_changed_fraction,
        .background_changed_red_fraction = response.background_changed_fraction,
        .changed_red_fraction = response.changed_fraction,
        .local_mean_positive_red_delta = response.local_mean_positive_delta,
        .background_mean_positive_red_delta = response.background_mean_positive_delta,
        .mean_positive_red_delta = response.mean_positive_delta,
        .global_illumination_scale = illumination_models[index].scale,
        .global_illumination_offset = illumination_models[index].offset,
        .baseline_frame = index < options.baseline_frame_count,
        .supported = index >= options.baseline_frame_count && response.supported,
        .active = index >= options.baseline_frame_count && response.active,
    });
  }
}

void RecordStrongestResponse(LedPulseResult *result, std::size_t baseline_frame_count) {
  result->strongest_response_frame_index = baseline_frame_count;
  for (std::size_t index = baseline_frame_count; index < result->frames.size(); ++index) {
    if (result->frames[index].mean_positive_red_delta > result->strongest_mean_positive_red_delta) {
      result->strongest_response_frame_index = index;
      result->strongest_mean_positive_red_delta = result->frames[index].mean_positive_red_delta;
    }
  }
}

void ApplyMatchedWindowDetection(LedPulseResult *result, std::span<const BayerRg8FrameView> frames,
                                 std::uint64_t timestamp_ticks_per_second,
                                 const LedPulseOptions &options) {
  const MatchedWindow matched = FindBestMatchedWindow(result->frames, options);
  result->matched_window_used = true;
  result->matched_mean_positive_red_delta = matched.mean_positive_delta;
  result->matched_changed_red_fraction = matched.changed_fraction;
  result->matched_background_mean_positive_red_delta = matched.background_mean_positive_delta;
  result->matched_background_changed_red_fraction = matched.background_changed_fraction;
  result->matched_supported_frame_count = matched.supported_frames;
  result->matched_high_confidence_frame_count = matched.high_confidence_frames;
  result->matched_longest_contiguous_supported_frame_count =
      matched.longest_contiguous_supported_frames;
  result->pulse_active_frame_count = matched.high_confidence_frames;
  PopulatePulseTiming(result, frames,
                      {.timestamp_ticks_per_second = timestamp_ticks_per_second,
                       .start = matched.start,
                       .end = matched.end});
  result->detected =
      matched.high_confidence_frames >= options.minimum_matched_active_frames &&
      matched.longest_contiguous_supported_frames >=
          options.minimum_matched_contiguous_supported_frames &&
      matched.mean_positive_delta >= options.minimum_matched_mean_positive_red_delta &&
      matched.changed_fraction >= options.minimum_matched_changed_red_fraction;
  if (result->detected) {
    return;
  }
  std::ostringstream message;
  message << "no scheduled LED pulse met the matched-window thresholds in locked region x="
          << result->selected_region.x << ", y=" << result->selected_region.y << "; best "
          << result->pulse_span_frame_count << "-frame window had "
          << result->matched_supported_frame_count << " supported frames and "
          << result->matched_high_confidence_frame_count
          << " high-confidence frames; longest contiguous support="
          << result->matched_longest_contiguous_supported_frame_count
          << " frames; mean red excess=" << result->matched_mean_positive_red_delta
          << ", changed-red excess=" << result->matched_changed_red_fraction;
  result->diagnostic = message.str();
}

void ApplyThresholdRunDetection(LedPulseResult *result, std::span<const BayerRg8FrameView> frames,
                                std::uint64_t timestamp_ticks_per_second,
                                const LedPulseOptions &options) {
  const auto runs = FindRuns(result->frames, options);
  auto valid = runs.end();
  for (auto candidate = runs.begin(); candidate != runs.end(); ++candidate) {
    const std::size_t span = candidate->end - candidate->start + 1U;
    if (candidate->active_frames >= options.minimum_active_frames &&
        span <= options.maximum_pulse_span_frames &&
        (valid == runs.end() || IsBetterRun(*candidate, *valid))) {
      valid = candidate;
    }
  }
  if (valid != runs.end()) {
    result->detected = true;
    result->pulse_active_frame_count = valid->active_frames;
    PopulatePulseTiming(result, frames,
                        {.timestamp_ticks_per_second = timestamp_ticks_per_second,
                         .start = valid->start,
                         .end = valid->end});
    return;
  }

  const auto best = std::ranges::max_element(runs, {}, [](const PulseRun &run) {
    return std::tuple(run.active_frames, run.response_sum);
  });
  std::ostringstream message;
  message << "no short LED pulse met the " << options.minimum_active_frames << " active-frame and "
          << options.maximum_pulse_span_frames
          << " span-frame limits in region x=" << result->selected_region.x
          << ", y=" << result->selected_region.y;
  if (best != runs.end()) {
    message << "; best run had " << best->active_frames << " active frames across "
            << (best->end - best->start + 1U) << " frame positions";
  } else {
    message << "; no frame crossed the global-corrected local/background excess thresholds";
  }
  result->diagnostic = message.str();
}

std::string SuccessfulDiagnostic(const LedPulseResult &result) {
  std::ostringstream message;
  message << (result.matched_window_used ? "matched scheduled" : "detected") << " LED pulse with "
          << result.pulse_active_frame_count << " high-confidence frames across "
          << result.pulse_span_frame_count
          << " frame positions in region x=" << result.selected_region.x
          << ", y=" << result.selected_region.y
          << "; response is global-affine corrected and background normalized"
          << "; device-local duration=" << result.duration_seconds << " seconds";
  if (result.matched_window_used) {
    message << "; matched supported frames=" << result.matched_supported_frame_count
            << ", high-confidence frames=" << result.matched_high_confidence_frame_count
            << ", longest contiguous support="
            << result.matched_longest_contiguous_supported_frame_count
            << "; matched mean red excess=" << result.matched_mean_positive_red_delta
            << ", changed-red excess=" << result.matched_changed_red_fraction;
  }
  if (result.missing_frame_ids != 0) {
    message << "; input sequence is missing " << result.missing_frame_ids << " frame IDs";
  }
  return message.str();
}

}  // namespace

LedPulseResult AnalyzeLedPulse(std::span<const BayerRg8FrameView> frames,
                               std::uint64_t timestamp_ticks_per_second,
                               const LedPulseOptions &options) {
  ValidateOptions(options);
  LedPulseResult result;
  result.missing_frame_ids = ValidateFrames(frames, timestamp_ticks_per_second, options);
  const auto baseline = MeasureBaseline(frames, options.baseline_frame_count);
  const auto illumination_models = FitGlobalIlluminationModels(frames, baseline);
  const auto regions = CandidateRegions(frames.front().image, options);
  const std::size_t selected_index =
      SelectCandidate(frames, baseline, regions, illumination_models, options);
  result.selected_region = regions[selected_index];
  MeasureSelectedRegion(&result, frames, baseline, illumination_models, options);
  RecordStrongestResponse(&result, options.baseline_frame_count);

  if (options.expected_start_frame_index.has_value()) {
    ApplyMatchedWindowDetection(&result, frames, timestamp_ticks_per_second, options);
  } else {
    ApplyThresholdRunDetection(&result, frames, timestamp_ticks_per_second, options);
  }
  if (result.detected) {
    result.diagnostic = SuccessfulDiagnostic(result);
  }
  return result;
}

}  // namespace swing_capture::optical
