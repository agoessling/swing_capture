#include "capture/image/image_quality.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace swing_capture::image {
namespace {

std::size_t ResolveStrideAndValidate(const Raw8ImageView &image,
                                     const ImageQualityOptions &options) {
  if (image.width == 0 || image.height == 0) {
    throw std::invalid_argument("raw 8-bit image dimensions must both be non-zero");
  }
  if (options.near_black_threshold >= options.near_white_threshold) {
    throw std::invalid_argument("near-black threshold must be less than near-white threshold");
  }

  const std::size_t width = image.width;
  const std::size_t stride = image.row_stride_bytes == 0 ? width : image.row_stride_bytes;
  if (stride < width) {
    throw std::invalid_argument("raw 8-bit image row stride is smaller than its width");
  }

  const auto preceding_rows = static_cast<std::size_t>(image.height - 1U);
  if (preceding_rows > (std::numeric_limits<std::size_t>::max() - width) / stride) {
    throw std::invalid_argument("raw 8-bit image geometry overflows size_t");
  }
  const std::size_t required_bytes = (preceding_rows * stride) + width;
  if (image.pixels.size() < required_bytes) {
    throw std::invalid_argument("raw 8-bit image payload is smaller than its geometry");
  }
  return stride;
}

std::uint8_t Sample(const Raw8ImageView &image, std::size_t stride, std::uint32_t x,
                    std::uint32_t y) {
  return std::to_integer<std::uint8_t>(image.pixels[(static_cast<std::size_t>(y) * stride) + x]);
}

std::uint8_t NearestRank(const std::array<std::uint64_t, 256> &histogram,
                         std::uint64_t target_rank) {
  std::uint64_t cumulative = 0;
  for (std::size_t value = 0; value < histogram.size(); ++value) {
    cumulative += histogram[value];
    if (cumulative >= target_rank) {
      return static_cast<std::uint8_t>(value);
    }
  }
  throw std::logic_error("raw image histogram has no target rank");
}

}  // namespace

ImageQualityMetrics MeasureRaw8ImageQuality(const Raw8ImageView &image,
                                            const ImageQualityOptions &options) {
  const std::size_t stride = ResolveStrideAndValidate(image, options);
  const auto sample_count = static_cast<std::uint64_t>(image.width) * image.height;

  std::array<std::uint64_t, 256> histogram{};
  for (std::uint32_t y = 0; y < image.height; ++y) {
    for (std::uint32_t x = 0; x < image.width; ++x) {
      ++histogram[Sample(image, stride, x, y)];
    }
  }

  std::uint64_t near_black_count = 0;
  std::uint64_t near_white_count = 0;
  std::uint64_t weighted_sum = 0;
  for (std::size_t value = 0; value < histogram.size(); ++value) {
    const std::uint64_t count = histogram[value];
    weighted_sum += static_cast<std::uint64_t>(value) * count;
    if (value <= options.near_black_threshold) {
      near_black_count += count;
    }
    if (value >= options.near_white_threshold) {
      near_white_count += count;
    }
  }

  const std::uint64_t p01_rank = (sample_count / 100U) + (sample_count % 100U != 0U ? 1U : 0U);
  const std::uint64_t p50_rank = (sample_count / 2U) + (sample_count % 2U);
  const std::uint64_t p99_rank = sample_count - (sample_count / 100U);

  std::uint64_t gradient_sum = 0;
  std::uint64_t gradient_count = 0;
  if (image.width > 2U) {
    for (std::uint32_t y = 0; y < image.height; ++y) {
      for (std::uint32_t x = 0; (x + 2U) < image.width; ++x) {
        const int difference = static_cast<int>(Sample(image, stride, x + 2U, y)) -
                               static_cast<int>(Sample(image, stride, x, y));
        gradient_sum += static_cast<std::uint64_t>(difference * difference);
        ++gradient_count;
      }
    }
  }
  if (image.height > 2U) {
    for (std::uint32_t y = 0; (y + 2U) < image.height; ++y) {
      for (std::uint32_t x = 0; x < image.width; ++x) {
        const int difference = static_cast<int>(Sample(image, stride, x, y + 2U)) -
                               static_cast<int>(Sample(image, stride, x, y));
        gradient_sum += static_cast<std::uint64_t>(difference * difference);
        ++gradient_count;
      }
    }
  }

  ImageQualityMetrics metrics = {
      .sample_count = sample_count,
      .min_value = NearestRank(histogram, 1),
      .max_value = NearestRank(histogram, sample_count),
      .mean = static_cast<double>(weighted_sum) / static_cast<double>(sample_count),
      .p01 = NearestRank(histogram, p01_rank),
      .p50 = NearestRank(histogram, p50_rank),
      .p99 = NearestRank(histogram, p99_rank),
      .near_black_fraction =
          static_cast<double>(near_black_count) / static_cast<double>(sample_count),
      .near_white_fraction =
          static_cast<double>(near_white_count) / static_cast<double>(sample_count),
      .gradient_energy = gradient_count == 0 ? 0.0
                                             : static_cast<double>(gradient_sum) /
                                                   static_cast<double>(gradient_count),
  };
  return metrics;
}

}  // namespace swing_capture::image
