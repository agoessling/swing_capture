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

// Converts tightly packed RGB8 pixels to owned, tightly packed luminance
// bytes. Each output is round((77*R + 150*G + 29*B) / 256), implemented as
// (77*R + 150*G + 29*B + 128) >> 8. These fixed Rec.601-style coefficients
// sum to 256, making the conversion deterministic and mapping black/white to
// exactly 0/255. The returned bytes can directly back a Raw8ImageView using
// the source width and height with a zero or width-sized row stride.
[[nodiscard]] std::vector<std::byte> Rgb8ToLuminance(const Rgb8Image &image);

// Encodes a binary P6 portable pixmap, a dependency-free diagnostic format.
[[nodiscard]] std::string EncodePpm(const Rgb8Image &image);

// Encodes a standards-compliant RGB PNG with uncompressed DEFLATE blocks.
// This keeps HIL artifacts viewable without adding an image-codec dependency.
[[nodiscard]] std::string EncodePng(const Rgb8Image &image);

// Encodes a browser-native baseline JPEG using libjpeg-turbo's SIMD path.
// Quality must be in [1, 100]. This lossy format is intended for transient
// setup previews; retained HIL evidence continues to use PNG.
[[nodiscard]] std::string EncodeJpeg(const Rgb8Image &image, int quality);

}  // namespace swing_capture::image

#endif  // SWING_CAPTURE_CAPTURE_IMAGE_BAYER_RG8_H_
