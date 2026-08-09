#include "capture/image/bayer_rg8.h"

#include <algorithm>
#include <array>
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

void ConstantColorMosaicProducesConstantInterior() {
  constexpr std::uint32_t kWidth = 6;
  constexpr std::uint32_t kHeight = 6;
  constexpr std::uint8_t kRed = 210;
  constexpr std::uint8_t kGreen = 120;
  constexpr std::uint8_t kBlue = 40;
  std::array<std::byte, kWidth * kHeight> bayer{};
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const bool even_x = (x & 1U) == 0U;
      const bool even_y = (y & 1U) == 0U;
      const std::uint8_t value = even_y && even_x ? kRed : !even_y && !even_x ? kBlue : kGreen;
      bayer[static_cast<std::size_t>(y) * kWidth + x] = std::byte{value};
    }
  }

  const auto rgb = swing_capture::image::DemosaicBayerRg8(bayer, kWidth, kHeight);
  for (std::uint32_t y = 1; y + 1 < kHeight; ++y) {
    for (std::uint32_t x = 1; x + 1 < kWidth; ++x) {
      const std::size_t index = (static_cast<std::size_t>(y) * kWidth + x) * 3U;
      Expect(rgb.pixels[index] == kRed, "red interpolation");
      Expect(rgb.pixels[index + 1U] == kGreen, "green interpolation");
      Expect(rgb.pixels[index + 2U] == kBlue, "blue interpolation");
    }
  }

  const std::string ppm = swing_capture::image::EncodePpm(rgb);
  Expect(ppm.starts_with("P6\n6 6\n255\n"), "PPM header");
  Expect(ppm.size() == std::string("P6\n6 6\n255\n").size() + kWidth * kHeight * 3U,
         "PPM encoded size");

  const std::string png = swing_capture::image::EncodePng(rgb);
  Expect(png.starts_with(std::string("\x89PNG\r\n\x1a\n", 8)), "PNG signature");
  Expect(png.find("IHDR") != std::string::npos, "PNG IHDR chunk");
  Expect(png.find("IDAT") != std::string::npos, "PNG IDAT chunk");
  Expect(png.find("IEND") != std::string::npos, "PNG IEND chunk");
}

void RejectsInvalidPayload() {
  const std::array<std::byte, 3> too_short{};
  bool rejected = false;
  try {
    (void)swing_capture::image::DemosaicBayerRg8(too_short, 2, 2);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Expect(rejected, "payload mismatch should be rejected");
}

void RgbToLuminanceUsesDeterministicRec601Rounding() {
  const swing_capture::image::Rgb8Image image = {
      .width = 4,
      .height = 2,
      .pixels =
          {
              0,   0,   0,    // Black.
              255, 255, 255,  // White.
              255, 0,   0,    // Red.
              0,   255, 0,    // Green.
              0,   0,   255,  // Blue.
              0,   0,   5,    // Exercises the +128 round-to-nearest term.
              1,   2,   3,    // Low mixed value.
              10,  20,  30,   // Interleaving and channel weights.
          },
  };

  const auto luminance = swing_capture::image::Rgb8ToLuminance(image);
  const std::array<std::byte, 8> expected = {
      std::byte{0},  std::byte{255}, std::byte{77}, std::byte{149},
      std::byte{29}, std::byte{1},   std::byte{2},  std::byte{18},
  };
  Expect(luminance.size() == image.width * image.height, "luminance output is tightly packed");
  Expect(std::ranges::equal(luminance, expected), "fixed luminance coefficients and rounding");
}

void RgbToLuminanceRejectsInvalidGeometryAndPayload() {
  const auto rejects = [](const swing_capture::image::Rgb8Image &image) {
    try {
      (void)swing_capture::image::Rgb8ToLuminance(image);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };

  Expect(rejects({.width = 0, .height = 1, .pixels = {}}), "zero-width RGB image rejected");
  Expect(rejects({.width = 1, .height = 0, .pixels = {}}), "zero-height RGB image rejected");
  Expect(rejects({.width = 2, .height = 1, .pixels = {0, 0, 0}}), "short RGB payload rejected");
  Expect(rejects({.width = 1, .height = 1, .pixels = {0, 0, 0, 0}}), "padded RGB payload rejected");
}

void PngCompressesUniformImages() {
  swing_capture::image::Rgb8Image image = {
      .width = 128,
      .height = 96,
      .pixels = std::vector<std::uint8_t>(128U * 96U * 3U, 42U),
  };
  const std::string png = swing_capture::image::EncodePng(image);
  Expect(png.size() < image.pixels.size() / 4U, "uniform PNG should be compressed");
}

}  // namespace

int main() {
  ConstantColorMosaicProducesConstantInterior();
  RejectsInvalidPayload();
  RgbToLuminanceUsesDeterministicRec601Rounding();
  RgbToLuminanceRejectsInvalidGeometryAndPayload();
  PngCompressesUniformImages();
  return failures == 0 ? 0 : 1;
}
