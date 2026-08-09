#ifndef SWING_CAPTURE_CAPTURE_PREVIEW_PREVIEW_IMAGE_H_
#define SWING_CAPTURE_CAPTURE_PREVIEW_PREVIEW_IMAGE_H_

#include <cstdint>
#include <string>

#include "capture/core/camera_source.h"
#include "capture/image/bayer_rg8.h"
#include "capture/image/image_quality.h"
#include "capture/preview/latest_frame_sampler.h"

namespace swing_capture::preview {

struct PreviewDimensions {
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  friend bool operator==(const PreviewDimensions &, const PreviewDimensions &) = default;
};

struct PreviewRenderOptions {
  std::uint32_t maximum_width = 0;
  std::uint32_t maximum_height = 0;
  image::ImageQualityOptions quality_options;
};

struct RenderedPreviewImage {
  FrameMetadata source_metadata;
  std::uint64_t preview_sequence = 0;
  PreviewDimensions dimensions;
  image::ImageQualityMetrics source_quality;
  std::string png_bytes;
};

struct PreviewRenderSet {
  RenderedPreviewImage routine;
  RenderedPreviewImage full_resolution;
};

// Returns the largest non-upscaled dimensions that fit within the requested
// bounds while preserving the source aspect ratio to the nearest whole pixel.
[[nodiscard]] PreviewDimensions FitWithin(PreviewDimensions source, PreviewDimensions maximum);

// Pure RGB resize used after demosaic. Bilinear sampling makes this suitable
// for setup previews while the full-resolution Bayer input remains available
// to quality analysis.
[[nodiscard]] image::Rgb8Image ResizeToFit(const image::Rgb8Image &source,
                                           PreviewDimensions maximum);

// Measures quality on the full Bayer input, then demosaics, resizes and PNG
// encodes without retaining references to the input frame.
[[nodiscard]] RenderedPreviewImage RenderPreview(const SampledPreviewFrame &frame,
                                                 const PreviewRenderOptions &options);

// Produces the routine fitted preview and an explicit full-resolution image
// from one quality-analysis and demosaic pass.
[[nodiscard]] PreviewRenderSet RenderPreviewSet(const SampledPreviewFrame &frame,
                                                const PreviewRenderOptions &options);

}  // namespace swing_capture::preview

#endif  // SWING_CAPTURE_CAPTURE_PREVIEW_PREVIEW_IMAGE_H_
