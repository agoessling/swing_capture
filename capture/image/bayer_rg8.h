#ifndef SWING_CAPTURE_CAPTURE_IMAGE_BAYER_RG8_H_
#define SWING_CAPTURE_CAPTURE_IMAGE_BAYER_RG8_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace swing_capture::image {

struct Rgb8Image {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;
};

// Offline bilinear demosaic for diagnostics and fixtures. This is deliberately
// not used on camera acquisition threads.
[[nodiscard]] Rgb8Image DemosaicBayerRg8(std::span<const std::byte> bayer, std::uint32_t width,
                                         std::uint32_t height);

// Encodes a binary P6 portable pixmap, a dependency-free diagnostic format.
[[nodiscard]] std::string EncodePpm(const Rgb8Image &image);

// Encodes a standards-compliant RGB PNG with uncompressed DEFLATE blocks.
// This keeps HIL artifacts viewable without adding an image-codec dependency.
[[nodiscard]] std::string EncodePng(const Rgb8Image &image);

}  // namespace swing_capture::image

#endif  // SWING_CAPTURE_CAPTURE_IMAGE_BAYER_RG8_H_
