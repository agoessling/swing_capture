#include "capture/image/bayer_rg8.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
  const std::size_t pixel_count = static_cast<std::size_t>(image.width) * image.height;
  if (image.width == 0 || image.height == 0 ||
      pixel_count > std::numeric_limits<std::size_t>::max() / 3U ||
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
    scanlines.push_back('\0');
    const std::size_t offset = static_cast<std::size_t>(y) * row_bytes;
    for (const std::uint8_t byte : pixels.subspan(offset, row_bytes)) {
      scanlines.push_back(static_cast<char>(byte));
    }
  }

  std::string zlib_stream;
  zlib_stream.reserve(scanlines.size() + (((scanlines.size() / 65535U) + 1U) * 5U) + 6U);
  zlib_stream.push_back(static_cast<char>(0x78));
  zlib_stream.push_back(static_cast<char>(0x01));
  std::size_t offset = 0;
  while (offset < scanlines.size()) {
    const std::size_t block_size = std::min<std::size_t>(65535U, scanlines.size() - offset);
    const bool final_block = offset + block_size == scanlines.size();
    zlib_stream.push_back(final_block ? '\x01' : '\0');
    const auto length = static_cast<std::uint16_t>(block_size);
    const auto inverted = static_cast<std::uint16_t>(~length);
    zlib_stream.push_back(static_cast<char>(length & 0xffU));
    zlib_stream.push_back(static_cast<char>(length >> 8U));
    zlib_stream.push_back(static_cast<char>(inverted & 0xffU));
    zlib_stream.push_back(static_cast<char>(inverted >> 8U));
    zlib_stream.append(scanlines, offset, block_size);
    offset += block_size;
  }

  constexpr std::uint32_t kAdlerModulus = 65521U;
  std::uint32_t adler_a = 1U;
  std::uint32_t adler_b = 0U;
  for (const unsigned char byte : scanlines) {
    adler_a = (adler_a + byte) % kAdlerModulus;
    adler_b = (adler_b + adler_a) % kAdlerModulus;
  }
  AppendBigEndian32(zlib_stream, (adler_b << 16U) | adler_a);

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

}  // namespace swing_capture::image
