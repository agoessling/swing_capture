#include "capture/preview/preview_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "capture/image/bayer_rg8.h"
#include "capture/image/image_quality.h"
#include "capture/preview/latest_frame_sampler.h"

namespace swing_capture::preview {
namespace {

struct SamplingCoordinate {
  double x;
  double y;
};

struct BayerSamplingAxis {
  std::uint32_t output_coordinate;
  std::uint32_t output_extent;
  std::uint32_t source_extent;
};

std::uint32_t RoundedScale(std::uint32_t source_extent, std::uint32_t target_extent,
                           std::uint32_t source_denominator) {
  const std::uint64_t numerator =
      static_cast<std::uint64_t>(source_extent) * target_extent + source_denominator / 2U;
  return std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(numerator / source_denominator));
}

void ValidateRgbImage(const image::Rgb8Image &source) {
  if (source.width == 0 || source.height == 0) {
    throw std::invalid_argument("preview RGB dimensions must be nonzero");
  }
  const std::size_t width = source.width;
  if (source.height > std::numeric_limits<std::size_t>::max() / width) {
    throw std::invalid_argument("preview RGB geometry overflows size_t");
  }
  const std::size_t pixel_count = width * source.height;
  if (pixel_count > std::numeric_limits<std::size_t>::max() / 3U ||
      source.pixels.size() != pixel_count * 3U) {
    throw std::invalid_argument("preview RGB payload does not match its dimensions");
  }
}

struct PreparedPreview {
  image::ImageQualityMetrics source_quality;
  image::Rgb8Image full_resolution_rgb;
  PreviewRenderTimings timings;
};

std::uint32_t CenteredCoordinateWithParity(BayerSamplingAxis axis) {
  const std::uint64_t numerator =
      static_cast<std::uint64_t>(axis.output_coordinate) * axis.source_extent +
      axis.source_extent / 2U;
  auto coordinate = static_cast<std::uint32_t>(numerator / axis.output_extent);
  coordinate = std::min(coordinate, axis.source_extent - 1U);
  if ((coordinate & 1U) == (axis.output_coordinate & 1U)) {
    return coordinate;
  }
  if (coordinate + 1U < axis.source_extent) {
    return coordinate + 1U;
  }
  return coordinate - 1U;
}

image::Rgb8Image DemosaicPreview(std::span<const std::byte> bayer, std::uint32_t width,
                                 std::uint32_t height, const PreviewRenderOptions &options) {
  if (options.maximum_width == 0 || options.maximum_height == 0) {
    throw std::invalid_argument("preview maximum dimensions must be nonzero");
  }
  const PreviewDimensions output =
      FitWithin({.width = width, .height = height},
                {.width = options.maximum_width, .height = options.maximum_height});
  if ((output.width == width && output.height == height) || output.width < 2U ||
      output.height < 2U) {
    return image::DemosaicBayerRg8(bayer, width, height);
  }

  std::vector<std::byte> reduced(static_cast<std::size_t>(output.width) * output.height);
  for (std::uint32_t y = 0; y < output.height; ++y) {
    const std::uint32_t source_y = CenteredCoordinateWithParity(
        {.output_coordinate = y, .output_extent = output.height, .source_extent = height});
    for (std::uint32_t x = 0; x < output.width; ++x) {
      const std::uint32_t source_x = CenteredCoordinateWithParity(
          {.output_coordinate = x, .output_extent = output.width, .source_extent = width});
      reduced[static_cast<std::size_t>(y) * output.width + x] =
          bayer[static_cast<std::size_t>(source_y) * width + source_x];
    }
  }
  return image::DemosaicBayerRg8(reduced, output.width, output.height);
}

PreparedPreview PreparePreview(const SampledPreviewFrame &frame,
                               const PreviewRenderOptions &options, bool reduce_for_preview) {
  const image::Raw8ImageView bayer_view = {
      .pixels = std::span<const std::byte>(frame.bayer_pixels),
      .width = frame.metadata.width,
      .height = frame.metadata.height,
  };
  const auto quality_started_at = std::chrono::steady_clock::now();
  const image::ImageQualityMetrics quality =
      image::MeasureRaw8ImageQuality(bayer_view, options.quality_options);
  const auto quality_completed_at = std::chrono::steady_clock::now();
  image::Rgb8Image rgb =
      reduce_for_preview
          ? DemosaicPreview(bayer_view.pixels, frame.metadata.width, frame.metadata.height, options)
          : image::DemosaicBayerRg8(bayer_view.pixels, frame.metadata.width, frame.metadata.height);
  const auto transform_completed_at = std::chrono::steady_clock::now();
  return {
      .source_quality = quality,
      .full_resolution_rgb = std::move(rgb),
      .timings =
          {
              .quality_analysis = quality_completed_at - quality_started_at,
              .bayer_transform = transform_completed_at - quality_completed_at,
          },
  };
}

RenderedPreviewImage EncodePreview(const SampledPreviewFrame &frame,
                                   const image::ImageQualityMetrics &quality,
                                   const image::Rgb8Image &rgb, const PreviewRenderOptions &options,
                                   PreviewRenderTimings timings,
                                   std::chrono::steady_clock::time_point render_started_at) {
  const auto encode_started_at = std::chrono::steady_clock::now();
  std::string media_type;
  std::string encoded;
  switch (options.image_format) {
    case PreviewImageFormat::kPng:
      media_type = "image/png";
      encoded = image::EncodePng(rgb);
      break;
    case PreviewImageFormat::kJpeg:
      media_type = "image/jpeg";
      encoded = image::EncodeJpeg(rgb, options.jpeg_quality);
      break;
  }
  const auto render_completed_at = std::chrono::steady_clock::now();
  timings.encode = render_completed_at - encode_started_at;
  timings.total = render_completed_at - render_started_at;
  return {
      .source_metadata = frame.metadata,
      .preview_sequence = frame.preview_sequence,
      .dimensions = {.width = rgb.width, .height = rgb.height},
      .source_quality = quality,
      .media_type = std::move(media_type),
      .encoded_bytes = std::move(encoded),
      .timings = timings,
      .render_started_at = render_started_at,
      .render_completed_at = render_completed_at,
  };
}

class SoftwarePreviewProcessor final : public PreviewFrameProcessor {
 public:
  [[nodiscard]] RenderedPreviewImage Render(const SampledPreviewFrame &frame,
                                            const PreviewRenderOptions &options) override {
    return RenderPreview(frame, options);
  }
};

std::uint8_t BilinearChannel(const image::Rgb8Image &source, SamplingCoordinate coordinate,
                             std::size_t channel) {
  const auto x0 = static_cast<std::uint32_t>(std::floor(coordinate.x));
  const auto y0 = static_cast<std::uint32_t>(std::floor(coordinate.y));
  const std::uint32_t x1 = std::min(x0 + 1U, source.width - 1U);
  const std::uint32_t y1 = std::min(y0 + 1U, source.height - 1U);
  const double x_weight = coordinate.x - x0;
  const double y_weight = coordinate.y - y0;

  const auto sample = [&](std::uint32_t x, std::uint32_t y) {
    const std::size_t index = ((static_cast<std::size_t>(y) * source.width) + x) * 3U + channel;
    return static_cast<double>(source.pixels[index]);
  };
  const double top = std::lerp(sample(x0, y0), sample(x1, y0), x_weight);
  const double bottom = std::lerp(sample(x0, y1), sample(x1, y1), x_weight);
  return static_cast<std::uint8_t>(std::lround(std::lerp(top, bottom, y_weight)));
}

}  // namespace

PreviewDimensions FitWithin(PreviewDimensions source, PreviewDimensions maximum) {
  if (source.width == 0 || source.height == 0 || maximum.width == 0 || maximum.height == 0) {
    throw std::invalid_argument("preview source and maximum dimensions must be nonzero");
  }
  if (source.width <= maximum.width && source.height <= maximum.height) {
    return source;
  }

  const std::uint64_t width_limited_height =
      static_cast<std::uint64_t>(maximum.width) * source.height;
  const std::uint64_t height_limited_width =
      static_cast<std::uint64_t>(maximum.height) * source.width;
  if (width_limited_height <= height_limited_width) {
    return {
        .width = maximum.width,
        .height = RoundedScale(source.height, maximum.width, source.width),
    };
  }
  return {
      .width = RoundedScale(source.width, maximum.height, source.height),
      .height = maximum.height,
  };
}

image::Rgb8Image ResizeToFit(const image::Rgb8Image &source, PreviewDimensions maximum) {
  ValidateRgbImage(source);
  const PreviewDimensions output_dimensions =
      FitWithin({.width = source.width, .height = source.height}, maximum);
  if (output_dimensions.width == source.width && output_dimensions.height == source.height) {
    return source;
  }

  const std::size_t output_pixel_count =
      static_cast<std::size_t>(output_dimensions.width) * output_dimensions.height;
  if (output_pixel_count > std::numeric_limits<std::size_t>::max() / 3U) {
    throw std::overflow_error("preview RGB allocation overflows size_t");
  }
  image::Rgb8Image output = {
      .width = output_dimensions.width,
      .height = output_dimensions.height,
      .pixels = std::vector<std::uint8_t>(output_pixel_count * 3U),
  };

  const double x_scale = static_cast<double>(source.width) / output.width;
  const double y_scale = static_cast<double>(source.height) / output.height;
  for (std::uint32_t y = 0; y < output.height; ++y) {
    const double source_y = std::clamp((static_cast<double>(y) + 0.5) * y_scale - 0.5, 0.0,
                                       static_cast<double>(source.height - 1U));
    for (std::uint32_t x = 0; x < output.width; ++x) {
      const double source_x = std::clamp((static_cast<double>(x) + 0.5) * x_scale - 0.5, 0.0,
                                         static_cast<double>(source.width - 1U));
      const std::size_t output_index = ((static_cast<std::size_t>(y) * output.width) + x) * 3U;
      for (std::size_t channel = 0; channel < 3U; ++channel) {
        output.pixels[output_index + channel] =
            BilinearChannel(source, {.x = source_x, .y = source_y}, channel);
      }
    }
  }
  return output;
}

RenderedPreviewImage RenderPreview(const SampledPreviewFrame &frame,
                                   const PreviewRenderOptions &options) {
  const auto render_started_at = std::chrono::steady_clock::now();
  const PreparedPreview prepared = PreparePreview(frame, options, true);
  const PreviewDimensions fitted = FitWithin(
      {.width = prepared.full_resolution_rgb.width, .height = prepared.full_resolution_rgb.height},
      {.width = options.maximum_width, .height = options.maximum_height});
  if (fitted.width == prepared.full_resolution_rgb.width &&
      fitted.height == prepared.full_resolution_rgb.height) {
    return EncodePreview(frame, prepared.source_quality, prepared.full_resolution_rgb, options,
                         prepared.timings, render_started_at);
  }
  const auto resize_started_at = std::chrono::steady_clock::now();
  const image::Rgb8Image resized =
      ResizeToFit(prepared.full_resolution_rgb,
                  {.width = options.maximum_width, .height = options.maximum_height});
  PreviewRenderTimings timings = prepared.timings;
  timings.resize = std::chrono::steady_clock::now() - resize_started_at;
  return EncodePreview(frame, prepared.source_quality, resized, options, timings,
                       render_started_at);
}

PreviewRenderSet RenderPreviewSet(const SampledPreviewFrame &frame,
                                  const PreviewRenderOptions &options) {
  const auto render_started_at = std::chrono::steady_clock::now();
  const PreparedPreview prepared = PreparePreview(frame, options, false);
  const auto resize_started_at = std::chrono::steady_clock::now();
  const image::Rgb8Image routine =
      ResizeToFit(prepared.full_resolution_rgb,
                  {.width = options.maximum_width, .height = options.maximum_height});
  PreviewRenderTimings routine_timings = prepared.timings;
  routine_timings.resize = std::chrono::steady_clock::now() - resize_started_at;
  return {
      .routine = EncodePreview(frame, prepared.source_quality, routine, options, routine_timings,
                               render_started_at),
      .full_resolution = EncodePreview(frame, prepared.source_quality, prepared.full_resolution_rgb,
                                       options, prepared.timings, render_started_at),
  };
}

std::unique_ptr<PreviewFrameProcessor> MakeSoftwarePreviewProcessor() {
  return std::make_unique<SoftwarePreviewProcessor>();
}

}  // namespace swing_capture::preview
