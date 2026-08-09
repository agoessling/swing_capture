#include "capture/preview/preview_image.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "capture/core/camera_source.h"
#include "capture/image/bayer_rg8.h"
#include "capture/image/image_quality.h"
#include "capture/preview/latest_frame_sampler.h"

namespace {

using swing_capture::FrameMetadata;
using swing_capture::image::Rgb8Image;
using swing_capture::preview::FitWithin;
using swing_capture::preview::PreviewDimensions;
using swing_capture::preview::RenderPreview;
using swing_capture::preview::RenderPreviewSet;
using swing_capture::preview::ResizeToFit;
using swing_capture::preview::SampledPreviewFrame;

std::vector<std::byte> MakeBayer(std::uint32_t width, std::uint32_t height) {
  std::vector<std::byte> bayer(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      bayer[static_cast<std::size_t>(y) * width + x] =
          std::byte{static_cast<std::uint8_t>((x * 17U + y * 29U) & 0xffU)};
    }
  }
  return bayer;
}

void ComputesFitWithinGeometryWithoutUpscaling() {
  assert((FitWithin({.width = 1440, .height = 1080}, {.width = 640, .height = 640}) ==
          PreviewDimensions{.width = 640, .height = 480}));
  assert((FitWithin({.width = 1080, .height = 1440}, {.width = 640, .height = 640}) ==
          PreviewDimensions{.width = 480, .height = 640}));
  assert((FitWithin({.width = 8, .height = 4}, {.width = 3, .height = 3}) ==
          PreviewDimensions{.width = 3, .height = 2}));
  assert((FitWithin({.width = 320, .height = 200}, {.width = 640, .height = 480}) ==
          PreviewDimensions{.width = 320, .height = 200}));

  bool rejected = false;
  try {
    static_cast<void>(FitWithin({.width = 0, .height = 2}, {.width = 2, .height = 2}));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

void BilinearResizePreservesConstantColorAndGeometry() {
  Rgb8Image source = {
      .width = 8,
      .height = 4,
      .pixels = std::vector<std::uint8_t>(8U * 4U * 3U),
  };
  for (std::size_t index = 0; index < source.pixels.size(); index += 3U) {
    source.pixels[index] = 20;
    source.pixels[index + 1U] = 100;
    source.pixels[index + 2U] = 220;
  }

  const Rgb8Image resized = ResizeToFit(source, {.width = 3, .height = 3});
  assert(resized.width == 3);
  assert(resized.height == 2);
  for (std::size_t index = 0; index < resized.pixels.size(); index += 3U) {
    assert(resized.pixels[index] == 20);
    assert(resized.pixels[index + 1U] == 100);
    assert(resized.pixels[index + 2U] == 220);
  }
}

void RenderingPreservesMetadataAndMeasuresFullBayerInput() {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 6;
  const auto bayer = MakeBayer(kWidth, kHeight);
  const SampledPreviewFrame frame = {
      .metadata =
          FrameMetadata{
              .frame_id = 77,
              .device_timestamp = 123456,
              .host_received_at = {},
              .width = kWidth,
              .height = kHeight,
              .complete = true,
          },
      .preview_sequence = 9,
      .bayer_pixels = bayer,
  };
  const auto expected_quality = swing_capture::image::MeasureRaw8ImageQuality({
      .pixels = std::span<const std::byte>(bayer),
      .width = kWidth,
      .height = kHeight,
  });

  const auto rendered = RenderPreview(frame, {
                                                 .maximum_width = 4,
                                                 .maximum_height = 4,
                                                 .quality_options = {},
                                             });
  assert(rendered.source_metadata.frame_id == 77);
  assert(rendered.source_metadata.device_timestamp == 123456);
  assert(rendered.preview_sequence == 9);
  assert(rendered.dimensions.width == 4);
  assert(rendered.dimensions.height == 3);
  assert(rendered.source_quality.sample_count == kWidth * kHeight);
  assert(rendered.source_quality.min_value == expected_quality.min_value);
  assert(rendered.source_quality.max_value == expected_quality.max_value);
  assert(std::abs(rendered.source_quality.mean - expected_quality.mean) < 1e-12);
  assert(std::abs(rendered.source_quality.gradient_energy - expected_quality.gradient_energy) <
         1e-12);
  assert(rendered.png_bytes.starts_with(std::string("\x89PNG\r\n\x1a\n", 8)));
  assert(rendered.png_bytes.find("IHDR") != std::string::npos);
  assert(rendered.png_bytes.find("IEND") != std::string::npos);
}

void RenderingRejectsInvalidBayerPayload() {
  const SampledPreviewFrame frame = {
      .metadata =
          FrameMetadata{
              .frame_id = 0,
              .device_timestamp = 0,
              .host_received_at = {},
              .width = 4,
              .height = 4,
              .complete = true,
          },
      .preview_sequence = 0,
      .bayer_pixels = std::vector<std::byte>(15),
  };
  bool rejected = false;
  try {
    static_cast<void>(RenderPreview(frame, {
                                               .maximum_width = 2,
                                               .maximum_height = 2,
                                               .quality_options = {},
                                           }));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

void RenderSetSharesOneSequenceAcrossRoutineAndFullResolutionImages() {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 6;
  const SampledPreviewFrame frame = {
      .metadata =
          FrameMetadata{
              .frame_id = 0,
              .device_timestamp = 0,
              .host_received_at = {},
              .width = kWidth,
              .height = kHeight,
              .complete = true,
          },
      .preview_sequence = 12,
      .bayer_pixels = MakeBayer(kWidth, kHeight),
  };
  const auto rendered =
      RenderPreviewSet(frame, {.maximum_width = 4, .maximum_height = 4, .quality_options = {}});
  assert(rendered.routine.preview_sequence == 12);
  assert(rendered.full_resolution.preview_sequence == 12);
  assert((rendered.routine.dimensions == PreviewDimensions{.width = 4, .height = 3}));
  assert((rendered.full_resolution.dimensions ==
          PreviewDimensions{.width = kWidth, .height = kHeight}));
  assert(rendered.routine.source_quality.mean == rendered.full_resolution.source_quality.mean);
}

}  // namespace

int main() {
  ComputesFitWithinGeometryWithoutUpscaling();
  BilinearResizePreservesConstantColorAndGeometry();
  RenderingPreservesMetadataAndMeasuresFullBayerInput();
  RenderingRejectsInvalidBayerPayload();
  RenderSetSharesOneSequenceAcrossRoutineAndFullResolutionImages();
  return 0;
}
