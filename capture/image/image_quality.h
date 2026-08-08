#ifndef SWING_CAPTURE_CAPTURE_IMAGE_IMAGE_QUALITY_H_
#define SWING_CAPTURE_CAPTURE_IMAGE_IMAGE_QUALITY_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace swing_capture::image {

// A view of a single-channel 8-bit image. A zero row stride means tightly
// packed rows. Non-zero strides allow camera buffers with row padding.
struct Raw8ImageView {
  std::span<const std::byte> pixels;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t row_stride_bytes = 0;
};

struct ImageQualityOptions {
  // Threshold comparisons are inclusive.
  std::uint8_t near_black_threshold = 8;
  std::uint8_t near_white_threshold = 247;
};

struct ImageQualityMetrics {
  std::uint64_t sample_count = 0;
  std::uint8_t min_value = 0;
  std::uint8_t max_value = 0;
  double mean = 0.0;
  std::uint8_t p01 = 0;
  std::uint8_t p50 = 0;
  std::uint8_t p99 = 0;
  double near_black_fraction = 0.0;
  double near_white_fraction = 0.0;

  // Mean squared difference, in digital-number squared, between horizontal
  // and vertical samples two pixels apart. The two-pixel spacing compares the
  // same color phase in a Bayer mosaic instead of treating its RG/GB pattern
  // as image detail. Higher values generally indicate more edge detail, but
  // exposure and sensor noise also affect this diagnostic.
  double gradient_energy = 0.0;
};

// Computes one histogram pass plus a Bayer-aware gradient pass without
// allocating. Percentiles use the nearest-rank definition.
//
// Throws std::invalid_argument for an empty image, invalid thresholds or row
// stride, or a payload too small for the declared geometry.
[[nodiscard]] ImageQualityMetrics MeasureRaw8ImageQuality(const Raw8ImageView &image,
                                                          const ImageQualityOptions &options = {});

}  // namespace swing_capture::image

#endif  // SWING_CAPTURE_CAPTURE_IMAGE_IMAGE_QUALITY_H_
