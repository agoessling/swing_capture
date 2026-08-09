#include "capture/image/bayer_rg8.h"

#include <turbojpeg.h>
#include <zconf.h>
#include <zlib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swing_capture::image {
namespace {

enum class Color { kRed, kGreen, kBlue };

struct BayerDimensions {
  std::uint32_t width;
  std::uint32_t height;
};

struct PixelCoordinate {
  std::uint32_t x;
  std::uint32_t y;
};

struct TurboJpegDestroy {
  void operator()(void *handle) const noexcept { static_cast<void>(tjDestroy(handle)); }
};

struct TurboJpegFree {
  void operator()(unsigned char *buffer) const noexcept { tjFree(buffer); }
};

Color BayerColor(PixelCoordinate coordinate) {
  if ((coordinate.y & 1U) == 0U) {
    return (coordinate.x & 1U) == 0U ? Color::kRed : Color::kGreen;
  }
  return (coordinate.x & 1U) == 0U ? Color::kGreen : Color::kBlue;
}

std::uint8_t Sample(std::span<const std::byte> bayer, std::uint32_t width,
                    PixelCoordinate coordinate) {
  const std::size_t index = (static_cast<std::size_t>(coordinate.y) * width) + coordinate.x;
  return std::to_integer<std::uint8_t>(bayer[index]);
}

template <std::size_t N>
std::uint8_t AverageNeighbors(std::span<const std::byte> bayer, BayerDimensions dimensions,
                              PixelCoordinate coordinate,
                              const std::array<std::array<int, 2>, N> &offsets) {
  std::uint32_t total = 0;
  std::uint32_t count = 0;
  for (const auto &offset : offsets) {
    const auto neighbor_x = static_cast<std::int64_t>(coordinate.x) + offset[0];
    const auto neighbor_y = static_cast<std::int64_t>(coordinate.y) + offset[1];
    if (neighbor_x < 0 || neighbor_y < 0 || std::cmp_greater_equal(neighbor_x, dimensions.width) ||
        std::cmp_greater_equal(neighbor_y, dimensions.height)) {
      continue;
    }
    total += Sample(bayer, dimensions.width,
                    {
                        .x = static_cast<std::uint32_t>(neighbor_x),
                        .y = static_cast<std::uint32_t>(neighbor_y),
                    });
    ++count;
  }
  if (count == 0) {
    throw std::invalid_argument("Bayer image is too small for bilinear demosaic");
  }
  return static_cast<std::uint8_t>((total + count / 2U) / count);
}

constexpr std::array<std::array<int, 2>, 4> kAxial = {{
    {{-1, 0}},
    {{1, 0}},
    {{0, -1}},
    {{0, 1}},
}};
constexpr std::array<std::array<int, 2>, 4> kDiagonal = {{
    {{-1, -1}},
    {{1, -1}},
    {{-1, 1}},
    {{1, 1}},
}};
constexpr std::array<std::array<int, 2>, 2> kHorizontal = {{
    {{-1, 0}},
    {{1, 0}},
}};
constexpr std::array<std::array<int, 2>, 2> kVertical = {{
    {{0, -1}},
    {{0, 1}},
}};

void AppendBigEndian32(std::string &output, std::uint32_t value) {
  output.push_back(static_cast<char>((value >> 24U) & 0xffU));
  output.push_back(static_cast<char>((value >> 16U) & 0xffU));
  output.push_back(static_cast<char>((value >> 8U) & 0xffU));
  output.push_back(static_cast<char>(value & 0xffU));
}

std::uint32_t UpdateCrc32(std::uint32_t crc, std::string_view bytes) {
  for (const unsigned char byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - static_cast<std::uint32_t>(crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return crc;
}

void AppendPngChunk(std::string &output, std::string_view type, std::string_view data) {
  if (type.size() != 4U || data.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("invalid PNG chunk");
  }
  AppendBigEndian32(output, static_cast<std::uint32_t>(data.size()));
  output.append(type);
  output.append(data);
  std::uint32_t crc = UpdateCrc32(0xffffffffU, type);
  crc = UpdateCrc32(crc, data);
  AppendBigEndian32(output, ~crc);
}

void ValidateRgbImage(const Rgb8Image &image) {
  if (image.width == 0 || image.height == 0 ||
      static_cast<std::size_t>(image.height) >
          std::numeric_limits<std::size_t>::max() / image.width) {
    throw std::invalid_argument("invalid RGB image");
  }
  const std::size_t pixel_count = static_cast<std::size_t>(image.width) * image.height;
  if (pixel_count > std::numeric_limits<std::size_t>::max() / 3U ||
      image.pixels.size() != pixel_count * 3U) {
    throw std::invalid_argument("invalid RGB image");
  }
}

}  // namespace

Rgb8Image DemosaicBayerRg8(std::span<const std::byte> bayer, std::uint32_t width,
                           std::uint32_t height) {
  if (width < 2 || height < 2) {
    throw std::invalid_argument("Bayer image dimensions must both be at least two");
  }
  const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
  if (pixel_count != bayer.size()) {
    throw std::invalid_argument("Bayer payload size does not match image dimensions");
  }
  if (pixel_count > std::numeric_limits<std::size_t>::max() / 3U) {
    throw std::overflow_error("RGB image allocation overflows size_t");
  }

  Rgb8Image image = {
      .width = width,
      .height = height,
      .pixels = std::vector<std::uint8_t>(pixel_count * 3U),
  };
  const BayerDimensions dimensions = {.width = width, .height = height};
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const PixelCoordinate coordinate = {.x = x, .y = y};
      const std::uint8_t center = Sample(bayer, width, coordinate);
      std::uint8_t red = 0;
      std::uint8_t green = 0;
      std::uint8_t blue = 0;
      switch (BayerColor(coordinate)) {
        case Color::kRed:
          red = center;
          green = AverageNeighbors(bayer, dimensions, coordinate, kAxial);
          blue = AverageNeighbors(bayer, dimensions, coordinate, kDiagonal);
          break;
        case Color::kBlue:
          red = AverageNeighbors(bayer, dimensions, coordinate, kDiagonal);
          green = AverageNeighbors(bayer, dimensions, coordinate, kAxial);
          blue = center;
          break;
        case Color::kGreen:
          green = center;
          if ((y & 1U) == 0U) {
            red = AverageNeighbors(bayer, dimensions, coordinate, kHorizontal);
            blue = AverageNeighbors(bayer, dimensions, coordinate, kVertical);
          } else {
            red = AverageNeighbors(bayer, dimensions, coordinate, kVertical);
            blue = AverageNeighbors(bayer, dimensions, coordinate, kHorizontal);
          }
          break;
      }
      const std::size_t output = ((static_cast<std::size_t>(y) * width) + x) * 3U;
      image.pixels[output] = red;
      image.pixels[output + 1U] = green;
      image.pixels[output + 2U] = blue;
    }
  }
  return image;
}

std::vector<std::byte> Rgb8ToLuminance(const Rgb8Image &image) {
  ValidateRgbImage(image);
  const std::size_t pixel_count = static_cast<std::size_t>(image.width) * image.height;
  std::vector<std::byte> luminance(pixel_count);
  for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
    const std::size_t input = pixel * 3U;
    const std::uint32_t weighted = 77U * image.pixels[input] + 150U * image.pixels[input + 1U] +
                                   29U * image.pixels[input + 2U];
    luminance[pixel] = std::byte{static_cast<std::uint8_t>((weighted + 128U) >> 8U)};
  }
  return luminance;
}

std::string EncodePpm(const Rgb8Image &image) {
  ValidateRgbImage(image);

  std::ostringstream header;
  header << "P6\n" << image.width << ' ' << image.height << "\n255\n";
  std::string encoded = header.str();
  encoded.reserve(encoded.size() + image.pixels.size());
  for (const std::uint8_t byte : image.pixels) {
    encoded.push_back(static_cast<char>(byte));
  }
  return encoded;
}

std::string EncodePng(const Rgb8Image &image) {
  ValidateRgbImage(image);
  const std::size_t row_bytes = static_cast<std::size_t>(image.width) * 3U;
  if (row_bytes == std::numeric_limits<std::size_t>::max() ||
      image.height > std::numeric_limits<std::size_t>::max() / (row_bytes + 1U)) {
    throw std::overflow_error("PNG scanline allocation overflows size_t");
  }

  std::string scanlines;
  scanlines.reserve((row_bytes + 1U) * image.height);
  const std::span<const std::uint8_t> pixels(image.pixels);
  for (std::uint32_t y = 0; y < image.height; ++y) {
    // PNG's Sub filter turns spatially correlated camera pixels into small
    // deltas, substantially reducing setup-preview bandwidth at low cost.
    scanlines.push_back('\x01');
    const std::size_t offset = static_cast<std::size_t>(y) * row_bytes;
    const std::span<const std::uint8_t> row = pixels.subspan(offset, row_bytes);
    for (std::size_t index = 0; index < row.size(); ++index) {
      const std::uint8_t left = index < 3U ? 0U : row[index - 3U];
      const auto filtered = static_cast<std::uint8_t>(row[index] - left);
      scanlines.push_back(static_cast<char>(filtered));
    }
  }

  if (scanlines.size() > std::numeric_limits<uLong>::max()) {
    throw std::length_error("PNG scanlines exceed the zlib input limit");
  }
  const auto input_size = static_cast<uLong>(scanlines.size());
  uLongf compressed_size = compressBound(input_size);
  std::string zlib_stream(compressed_size, '\0');
  // zlib's C API predates std::span and requires byte-pointer casts here.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  const int compression_result =
      compress2(reinterpret_cast<Bytef *>(zlib_stream.data()), &compressed_size,
                reinterpret_cast<const Bytef *>(scanlines.data()), input_size, Z_BEST_SPEED);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  if (compression_result != Z_OK) {
    throw std::runtime_error("zlib failed to compress PNG scanlines");
  }
  zlib_stream.resize(compressed_size);

  std::string ihdr;
  ihdr.reserve(13);
  AppendBigEndian32(ihdr, image.width);
  AppendBigEndian32(ihdr, image.height);
  ihdr.push_back('\x08');
  ihdr.push_back('\x02');
  ihdr.append(3, '\0');

  std::string png("\x89PNG\r\n\x1a\n", 8);
  AppendPngChunk(png, "IHDR", ihdr);
  AppendPngChunk(png, "IDAT", zlib_stream);
  AppendPngChunk(png, "IEND", {});
  return png;
}

std::string EncodeJpeg(const Rgb8Image &image, int quality) {
  ValidateRgbImage(image);
  if (quality < 1 || quality > 100) {
    throw std::invalid_argument("JPEG quality must be in [1, 100]");
  }
  if (image.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      image.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("JPEG dimensions exceed the codec limit");
  }

  const std::unique_ptr<void, TurboJpegDestroy> compressor(tjInitCompress());
  if (compressor == nullptr) {
    throw std::runtime_error("libjpeg-turbo failed to create a compressor");
  }

  unsigned char *raw_buffer = nullptr;
  auto encoded_size = 0UL;
  const int result =
      tjCompress2(compressor.get(), image.pixels.data(), static_cast<int>(image.width), 0,
                  static_cast<int>(image.height), TJPF_RGB, &raw_buffer, &encoded_size, TJSAMP_420,
                  quality, TJFLAG_FASTDCT);
  const std::unique_ptr<unsigned char, TurboJpegFree> encoded(raw_buffer);
  if (result != 0) {
    throw std::runtime_error(std::string("libjpeg-turbo failed to encode preview: ") +
                             tjGetErrorStr2(compressor.get()));
  }
  if (encoded_size > std::numeric_limits<std::size_t>::max()) {
    throw std::length_error("JPEG output exceeds size_t");
  }
  // libjpeg-turbo owns a byte buffer whose element type predates std::byte.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {reinterpret_cast<const char *>(encoded.get()), static_cast<std::size_t>(encoded_size)};
}

}  // namespace swing_capture::image
