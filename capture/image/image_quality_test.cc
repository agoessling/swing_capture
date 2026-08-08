#include "capture/image/image_quality.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void ExpectNear(double actual, double expected, double tolerance, const std::string &message) {
  Expect(std::abs(actual - expected) <= tolerance,
         message + ": actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
}

std::vector<std::byte> ToBytes(std::span<const std::uint8_t> values) {
  std::vector<std::byte> bytes;
  bytes.reserve(values.size());
  for (const std::uint8_t value : values) {
    bytes.push_back(std::byte{value});
  }
  return bytes;
}

void ComputesHistogramStatisticsAndInclusiveFractions() {
  std::array<std::uint8_t, 100> values{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    values[index] = static_cast<std::uint8_t>(index);
  }
  const auto bytes = ToBytes(values);

  const auto metrics = swing_capture::image::MeasureRaw8ImageQuality(
      {
          .pixels = bytes,
          .width = 10,
          .height = 10,
      },
      {
          .near_black_threshold = 9,
          .near_white_threshold = 90,
      });

  Expect(metrics.sample_count == 100, "sample count");
  Expect(metrics.min_value == 0, "minimum");
  Expect(metrics.max_value == 99, "maximum");
  ExpectNear(metrics.mean, 49.5, 1e-12, "mean");
  Expect(metrics.p01 == 0, "nearest-rank p01");
  Expect(metrics.p50 == 49, "nearest-rank p50");
  Expect(metrics.p99 == 98, "nearest-rank p99");
  ExpectNear(metrics.near_black_fraction, 0.10, 1e-12, "inclusive near-black fraction");
  ExpectNear(metrics.near_white_fraction, 0.10, 1e-12, "inclusive near-white fraction");
}

void IgnoresRowPadding() {
  constexpr std::array<std::uint8_t, 10> kPadded = {
      0, 10, 20, 255, 255, 30, 40, 50, 255, 255,
  };
  const auto bytes = ToBytes(kPadded);
  const auto metrics = swing_capture::image::MeasureRaw8ImageQuality({
      .pixels = bytes,
      .width = 3,
      .height = 2,
      .row_stride_bytes = 5,
  });

  Expect(metrics.sample_count == 6, "padded sample count");
  Expect(metrics.min_value == 0, "padding excluded from minimum");
  Expect(metrics.max_value == 50, "padding excluded from maximum");
  ExpectNear(metrics.mean, 25.0, 1e-12, "padding excluded from mean");
  ExpectNear(metrics.near_white_fraction, 0.0, 1e-12, "padding excluded from white fraction");
}

void BayerPatternDoesNotLookSharpByItself() {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 8;
  std::array<std::uint8_t, kWidth * kHeight> mosaic{};
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const bool even_x = (x & 1U) == 0U;
      const bool even_y = (y & 1U) == 0U;
      mosaic[static_cast<std::size_t>(y) * kWidth + x] = even_x && even_y     ? 220
                                                         : !even_x && !even_y ? 20
                                                                              : 100;
    }
  }
  const auto bytes = ToBytes(mosaic);
  const auto metrics = swing_capture::image::MeasureRaw8ImageQuality({
      .pixels = bytes,
      .width = kWidth,
      .height = kHeight,
  });

  ExpectNear(metrics.gradient_energy, 0.0, 1e-12, "constant-color Bayer mosaic gradient");
}

void SharpEdgeScoresAboveBlurredEdge() {
  constexpr std::uint32_t kWidth = 16;
  constexpr std::uint32_t kHeight = 8;
  std::array<std::uint8_t, kWidth * kHeight> sharp{};
  std::array<std::uint8_t, kWidth * kHeight> blurred{};
  constexpr std::array<std::uint8_t, kWidth> kBlurredRow = {
      0, 0, 0, 0, 0, 32, 64, 96, 128, 160, 192, 224, 255, 255, 255, 255,
  };
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      sharp[static_cast<std::size_t>(y) * kWidth + x] = x < kWidth / 2U ? 0 : 255;
      blurred[static_cast<std::size_t>(y) * kWidth + x] = kBlurredRow[x];
    }
  }
  const auto sharp_bytes = ToBytes(sharp);
  const auto blurred_bytes = ToBytes(blurred);
  const auto sharp_metrics = swing_capture::image::MeasureRaw8ImageQuality({
      .pixels = sharp_bytes,
      .width = kWidth,
      .height = kHeight,
  });
  const auto blurred_metrics = swing_capture::image::MeasureRaw8ImageQuality({
      .pixels = blurred_bytes,
      .width = kWidth,
      .height = kHeight,
  });

  Expect(sharp_metrics.gradient_energy > blurred_metrics.gradient_energy * 2.0,
         "sharp edge should have substantially more gradient energy");
}

void FlatAndTinyImagesHaveZeroGradient() {
  const std::array<std::uint8_t, 1> pixel = {42};
  const auto bytes = ToBytes(pixel);
  const auto metrics = swing_capture::image::MeasureRaw8ImageQuality({
      .pixels = bytes,
      .width = 1,
      .height = 1,
  });

  Expect(metrics.min_value == 42 && metrics.max_value == 42, "single-pixel extrema");
  Expect(metrics.p01 == 42 && metrics.p50 == 42 && metrics.p99 == 42, "single-pixel percentiles");
  ExpectNear(metrics.gradient_energy, 0.0, 1e-12, "single-pixel gradient");
}

template <typename Function>
void ExpectInvalidArgument(Function &&function, const std::string &message) {
  bool rejected = false;
  try {
    function();
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Expect(rejected, message);
}

void RejectsInvalidInputs() {
  const std::array<std::byte, 8> pixels{};
  ExpectInvalidArgument(
      [&] {
        (void)swing_capture::image::MeasureRaw8ImageQuality({
            .pixels = pixels,
            .width = 0,
            .height = 1,
        });
      },
      "zero width");
  ExpectInvalidArgument(
      [&] {
        (void)swing_capture::image::MeasureRaw8ImageQuality({
            .pixels = pixels,
            .width = 4,
            .height = 2,
            .row_stride_bytes = 3,
        });
      },
      "stride smaller than width");
  ExpectInvalidArgument(
      [&] {
        (void)swing_capture::image::MeasureRaw8ImageQuality({
            .pixels = pixels,
            .width = 5,
            .height = 2,
        });
      },
      "payload smaller than geometry");
  ExpectInvalidArgument(
      [&] {
        (void)swing_capture::image::MeasureRaw8ImageQuality(
            {
                .pixels = pixels,
                .width = 4,
                .height = 2,
            },
            {
                .near_black_threshold = 200,
                .near_white_threshold = 200,
            });
      },
      "overlapping thresholds");
}

}  // namespace

int main() {
  ComputesHistogramStatisticsAndInclusiveFractions();
  IgnoresRowPadding();
  BayerPatternDoesNotLookSharpByItself();
  SharpEdgeScoresAboveBlurredEdge();
  FlatAndTinyImagesHaveZeroGradient();
  RejectsInvalidInputs();
  return failures == 0 ? 0 : 1;
}
