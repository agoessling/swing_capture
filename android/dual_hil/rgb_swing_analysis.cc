#include "android/dual_hil/rgb_swing_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace swing_capture::android::dual_hil {
namespace {

constexpr std::size_t kTileSize = 8U;
constexpr std::size_t kMaximumFrames = 800U;
constexpr std::int64_t kReferenceStartUs = -120000;
constexpr std::int64_t kReferenceEndUs = -35000;
constexpr std::int64_t kSearchStartUs = -25000;
constexpr std::int64_t kSearchEndUs = 30000;
constexpr std::int64_t kRepresentativeTimeUs = -50000;
constexpr std::int64_t kDiagnosticRetainStartUs = -120000;
constexpr std::int64_t kDiagnosticRetainEndUs = 100000;
constexpr std::int64_t kPostReferenceStartUs = 45000;
constexpr std::int64_t kPostReferenceEndUs = 100000;
constexpr double kMinimumWhiteDelta = 7.0;
constexpr std::size_t kMinimumWhiteFrames = 2U;
constexpr std::size_t kMaximumWhiteFrames = 9U;
constexpr double kMinimumWhiteDurationUs = 8000.0;
constexpr double kMaximumWhiteDurationUs = 35000.0;
constexpr std::int64_t kMaximumOpticalAudioOffsetUs = 20000;

std::size_t CheckedFrameBytes(std::uint32_t width, std::uint32_t height) {
  if (width == 0U || height == 0U ||
      static_cast<std::size_t>(width) >
          std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(height) / 3U) {
    throw std::invalid_argument("RGB swing frame geometry is invalid");
  }
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U;
}

std::size_t NearestFrame(std::span<const RgbFrameTiming> timings, std::int64_t requested_us) {
  const auto nearest = std::ranges::min_element(timings, {}, [requested_us](const auto &timing) {
    return std::abs(timing.time_from_impact_us - requested_us);
  });
  return static_cast<std::size_t>(std::distance(timings.begin(), nearest));
}

bool InRange(std::int64_t value, std::int64_t lower, std::int64_t upper) {
  return value >= lower && value <= upper;
}

struct TileHistoryInspection {
  std::span<const RgbFrameTiming> timings;
  std::span<const float> whiteness;
  std::size_t tile_count = 0;
  std::size_t selected_tile = 0;
};

double PostImpactSequenceLevel(const TileHistoryInspection &inspection) {
  double total = 0.0;
  std::size_t count = 0U;
  for (std::size_t frame = 0; frame < inspection.timings.size(); ++frame) {
    if (InRange(inspection.timings[frame].time_from_impact_us, kPostReferenceStartUs,
                kPostReferenceEndUs)) {
      total += inspection.whiteness[frame * inspection.tile_count + inspection.selected_tile];
      ++count;
    }
  }
  if (count < 4U) {
    throw std::runtime_error("decoded RGB stream lacks a post-impact optical baseline");
  }
  return total / static_cast<double>(count);
}

std::pair<std::int64_t, std::int64_t> OpticalOnsetBounds(std::span<const RgbFrameTiming> timings,
                                                         std::size_t first_white_frame) {
  if (first_white_frame == 0U || first_white_frame + 1U >= timings.size()) {
    throw std::runtime_error("white LED onset lacks surrounding sensor-timestamp evidence");
  }
  return {
      timings[first_white_frame - 1U].time_from_impact_us,
      timings[first_white_frame + 1U].time_from_impact_us,
  };
}

}  // namespace

RgbSwingSequenceAnalyzer::RgbSwingSequenceAnalyzer(
    const RgbSwingSequenceConfiguration &configuration)
    : width_(configuration.width),
      height_(configuration.height),
      timings_(configuration.timings.begin(), configuration.timings.end()) {
  static_cast<void>(CheckedFrameBytes(width_, height_));
  if (timings_.size() < 10U || timings_.size() > kMaximumFrames) {
    throw std::invalid_argument("RGB swing timeline must contain 10-800 frames");
  }
  for (std::size_t index = 1; index < timings_.size(); ++index) {
    if (timings_[index].media_time_us <= timings_[index - 1U].media_time_us ||
        timings_[index].time_from_impact_us <= timings_[index - 1U].time_from_impact_us) {
      throw std::invalid_argument("RGB swing timeline must be strictly monotonic");
    }
  }
  tile_columns_ = (static_cast<std::size_t>(width_) + kTileSize - 1U) / kTileSize;
  tile_rows_ = (static_cast<std::size_t>(height_) + kTileSize - 1U) / kTileSize;
  const std::size_t tile_count = tile_columns_ * tile_rows_;
  if (timings_.size() > std::numeric_limits<std::size_t>::max() / tile_count) {
    throw std::invalid_argument("RGB swing tile history is too large");
  }
  tile_whiteness_.resize(timings_.size() * tile_count);
  representative_frame_index_ = NearestFrame(timings_, kRepresentativeTimeUs);
}

void RgbSwingSequenceAnalyzer::Append(std::span<const std::uint8_t> rgb24) {
  if (appended_frames_ >= timings_.size()) {
    throw std::invalid_argument("decoded RGB stream has more frames than its manifest");
  }
  if (rgb24.size() != CheckedFrameBytes(width_, height_)) {
    throw std::invalid_argument("decoded RGB frame size does not match its geometry");
  }
  const std::size_t tile_count = tile_columns_ * tile_rows_;
  for (std::size_t tile_y = 0; tile_y < tile_rows_; ++tile_y) {
    const std::size_t begin_y = tile_y * kTileSize;
    const std::size_t end_y = std::min(begin_y + kTileSize, static_cast<std::size_t>(height_));
    for (std::size_t tile_x = 0; tile_x < tile_columns_; ++tile_x) {
      const std::size_t begin_x = tile_x * kTileSize;
      const std::size_t end_x = std::min(begin_x + kTileSize, static_cast<std::size_t>(width_));
      std::uint64_t red = 0U;
      std::uint64_t green = 0U;
      std::uint64_t blue = 0U;
      for (std::size_t y = begin_y; y < end_y; ++y) {
        for (std::size_t x = begin_x; x < end_x; ++x) {
          const std::size_t pixel = (y * static_cast<std::size_t>(width_) + x) * 3U;
          red += rgb24[pixel];
          green += rgb24[pixel + 1U];
          blue += rgb24[pixel + 2U];
        }
      }
      const auto pixel_count = static_cast<double>((end_x - begin_x) * (end_y - begin_y));
      const double whiteness = static_cast<double>(std::min({red, green, blue})) / pixel_count;
      tile_whiteness_[appended_frames_ * tile_count + tile_y * tile_columns_ + tile_x] =
          static_cast<float>(whiteness);
    }
  }

  if (InRange(timings_[appended_frames_].time_from_impact_us, kDiagnosticRetainStartUs,
              kDiagnosticRetainEndUs)) {
    RetainedLuminanceFrame retained = {
        .frame_index = appended_frames_,
        .pixels = std::vector<std::byte>(static_cast<std::size_t>(width_) * height_),
    };
    for (std::size_t pixel = 0; pixel < retained.pixels.size(); ++pixel) {
      const std::size_t rgb = pixel * 3U;
      const unsigned luminance =
          77U * rgb24[rgb] + 150U * rgb24[rgb + 1U] + 29U * rgb24[rgb + 2U] + 128U;
      retained.pixels[pixel] = static_cast<std::byte>(luminance >> 8U);
    }
    if (appended_frames_ == representative_frame_index_) {
      representative_luminance_ = retained.pixels;
    }
    diagnostic_luminance_frames_.push_back(std::move(retained));
  }
  ++appended_frames_;
}

// The acceptance evidence is populated beside the measurements it qualifies so a failed physical
// run retains one coherent, inspectable decision record.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
RgbSwingAnalysis RgbSwingSequenceAnalyzer::Finish() {
  if (appended_frames_ != timings_.size() || representative_luminance_.empty()) {
    throw std::runtime_error("decoded RGB stream frame count is incomplete");
  }
  const std::size_t tile_count = tile_columns_ * tile_rows_;
  std::vector<double> baseline(tile_count, 0.0);
  std::size_t reference_count = 0U;
  for (std::size_t frame = 0; frame < timings_.size(); ++frame) {
    if (!InRange(timings_[frame].time_from_impact_us, kReferenceStartUs, kReferenceEndUs)) {
      continue;
    }
    ++reference_count;
    for (std::size_t tile = 0; tile < tile_count; ++tile) {
      baseline[tile] += tile_whiteness_[frame * tile_count + tile];
    }
  }
  if (reference_count < 4U) {
    throw std::runtime_error("decoded RGB stream lacks a pre-impact optical baseline");
  }
  for (double &value : baseline) {
    value /= static_cast<double>(reference_count);
  }

  double maximum_delta = -std::numeric_limits<double>::infinity();
  std::size_t peak_frame = 0U;
  std::size_t peak_tile = 0U;
  for (std::size_t frame = 0; frame < timings_.size(); ++frame) {
    if (!InRange(timings_[frame].time_from_impact_us, kSearchStartUs, kSearchEndUs)) {
      continue;
    }
    for (std::size_t tile = 0; tile < tile_count; ++tile) {
      const double delta = tile_whiteness_[frame * tile_count + tile] - baseline[tile];
      if (delta > maximum_delta) {
        maximum_delta = delta;
        peak_frame = frame;
        peak_tile = tile;
      }
    }
  }

  RgbSwingAnalysis result;
  result.decoded_frame_count = appended_frames_;
  result.representative_frame_index = representative_frame_index_;
  result.peak_frame_index = peak_frame;
  result.tile_x = peak_tile % tile_columns_;
  result.tile_y = peak_tile / tile_columns_;
  result.maximum_white_delta = maximum_delta;
  result.representative_luminance = std::move(representative_luminance_);
  result.diagnostic_luminance_frames = std::move(diagnostic_luminance_frames_);
  result.acceptance = {
      .minimum_white_delta = kMinimumWhiteDelta,
      .minimum_white_frames = kMinimumWhiteFrames,
      .maximum_white_frames = kMaximumWhiteFrames,
      .minimum_white_duration_us = kMinimumWhiteDurationUs,
      .maximum_white_duration_us = kMaximumWhiteDurationUs,
      .maximum_absolute_optical_audio_offset_us = kMaximumOpticalAudioOffsetUs,
      .minimum_white_delta_passed =
          std::isfinite(maximum_delta) && maximum_delta >= kMinimumWhiteDelta,
  };
  if (!result.acceptance.minimum_white_delta_passed) {
    result.diagnostic = "no temporally isolated white Feather LED response exceeded 7 DN";
    return result;
  }

  const double support_threshold = std::max(kMinimumWhiteDelta, maximum_delta * 0.40);
  const double localized_threshold = std::max(kMinimumWhiteDelta, maximum_delta * 0.50);
  result.localized_response_tile_count = static_cast<std::size_t>(
      std::ranges::count_if(std::views::iota(std::size_t{0}, tile_count), [&](std::size_t tile) {
        return tile_whiteness_[peak_frame * tile_count + tile] - baseline[tile] >=
               localized_threshold;
      }));
  const double post_sequence_level = PostImpactSequenceLevel({
      .timings = timings_,
      .whiteness = tile_whiteness_,
      .tile_count = tile_count,
      .selected_tile = peak_tile,
  });
  result.post_sequence_baseline_shift = std::abs(post_sequence_level - baseline[peak_tile]);
  auto supported = [&](std::size_t frame) {
    return tile_whiteness_[frame * tile_count + peak_tile] - baseline[peak_tile] >=
           support_threshold;
  };
  std::size_t first = peak_frame;
  while (first > 0U && supported(first - 1U)) {
    --first;
  }
  std::size_t last = peak_frame;
  while (last + 1U < timings_.size() && supported(last + 1U)) {
    ++last;
  }
  const std::size_t support_frames = last - first + 1U;
  const double frame_interval_us =
      static_cast<double>(timings_.back().media_time_us - timings_.front().media_time_us) /
      static_cast<double>(timings_.size() - 1U);
  result.first_white_frame_index = first;
  result.last_white_frame_index = last;
  result.white_frame_count = support_frames;
  result.white_duration_us =
      static_cast<double>(timings_[last].media_time_us - timings_[first].media_time_us) +
      frame_interval_us;
  result.optical_to_audio_offset_us = timings_[first].time_from_impact_us;
  // A lit tile proves that the optical transition occurred near this exposure, but the exact
  // row-exposure instant is not reported by the retained manifest. Bound it by the adjacent
  // sensor timestamps instead of assuming an ideal 1/240-second cadence.
  const auto onset_bounds = OpticalOnsetBounds(timings_, first);
  result.optical_onset_lower_bound_us = onset_bounds.first;
  result.optical_onset_upper_bound_us = onset_bounds.second;
  result.acceptance.white_frame_count_passed =
      support_frames >= kMinimumWhiteFrames && support_frames <= kMaximumWhiteFrames;
  result.acceptance.white_duration_passed = result.white_duration_us >= kMinimumWhiteDurationUs &&
                                            result.white_duration_us <= kMaximumWhiteDurationUs;
  result.acceptance.optical_audio_offset_passed =
      std::abs(result.optical_to_audio_offset_us) <= kMaximumOpticalAudioOffsetUs;
  result.detected =
      result.acceptance.minimum_white_delta_passed && result.acceptance.white_frame_count_passed &&
      result.acceptance.white_duration_passed && result.acceptance.optical_audio_offset_passed;
  std::ostringstream diagnostic;
  diagnostic << "white LED tile=(" << result.tile_x << ',' << result.tile_y
             << ") delta=" << result.maximum_white_delta << " frames=" << support_frames
             << " duration_us=" << result.white_duration_us
             << " optical_to_audio_offset_us=" << result.optical_to_audio_offset_us
             << " optical_onset_interval_us=[" << result.optical_onset_lower_bound_us << ','
             << result.optical_onset_upper_bound_us << ']'
             << " localized_tiles=" << result.localized_response_tile_count
             << " post_sequence_baseline_shift=" << result.post_sequence_baseline_shift;
  result.diagnostic = diagnostic.str();
  return result;
}

}  // namespace swing_capture::android::dual_hil
