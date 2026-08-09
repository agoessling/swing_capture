#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <ratio>
#include <utility>
#include <vector>

#include "capture/core/camera_source.h"
#include "capture/preview/latest_frame_sampler.h"
#include "capture/preview/preview_image.h"

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::FrameMetadata;
using swing_capture::preview::MakeSoftwarePreviewProcessor;
using swing_capture::preview::PreviewFrameProcessor;
using swing_capture::preview::PreviewRenderTimings;
using swing_capture::preview::RenderedPreviewImage;
using swing_capture::preview::SampledPreviewFrame;

constexpr std::uint32_t kSourceWidth = 1440;
constexpr std::uint32_t kSourceHeight = 1080;
constexpr std::uint32_t kPreviewWidth = 640;
constexpr std::uint32_t kPreviewHeight = 480;
constexpr std::size_t kIterations = 8;

double Milliseconds(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

double Median(std::vector<double> values) {
  std::ranges::sort(values);
  const std::size_t middle = values.size() / 2U;
  if ((values.size() & 1U) != 0U) {
    return values[middle];
  }
  return (values[middle - 1U] + values[middle]) / 2.0;
}

std::vector<std::byte> MakeHighEntropyBayer() {
  std::vector<std::byte> pixels(static_cast<std::size_t>(kSourceWidth) * kSourceHeight);
  std::uint32_t state = 0x9e3779b9U;
  for (std::byte &pixel : pixels) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    pixel = static_cast<std::byte>(state & 0xffU);
  }
  return pixels;
}

Json TimingJson(const PreviewRenderTimings &timings) {
  return {
      {"quality_analysis_ms", Milliseconds(timings.quality_analysis)},
      {"bayer_transform_ms", Milliseconds(timings.bayer_transform)},
      {"resize_ms", Milliseconds(timings.resize)},
      {"encode_ms", Milliseconds(timings.encode)},
      {"total_ms", Milliseconds(timings.total)},
  };
}

Json RunBenchmark() {
  SampledPreviewFrame frame = {
      .metadata =
          FrameMetadata{
              .frame_id = 1,
              .device_timestamp = 1,
              .host_received_at = std::chrono::steady_clock::now(),
              .width = kSourceWidth,
              .height = kSourceHeight,
              .complete = true,
          },
      .preview_sequence = 1,
      .bayer_pixels = MakeHighEntropyBayer(),
  };
  std::unique_ptr<PreviewFrameProcessor> processor = MakeSoftwarePreviewProcessor();
  const auto options = swing_capture::preview::PreviewRenderOptions{
      .maximum_width = kPreviewWidth,
      .maximum_height = kPreviewHeight,
      .image_format = swing_capture::preview::PreviewImageFormat::kJpeg,
      .jpeg_quality = 85,
      .quality_options = {},
  };
  static_cast<void>(processor->Render(frame, options));

  std::vector<double> quality;
  std::vector<double> transform;
  std::vector<double> resize;
  std::vector<double> encode;
  std::vector<double> total;
  std::vector<double> payload_bytes;
  Json samples = Json::array();
  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    frame.metadata.frame_id += 1U;
    frame.metadata.device_timestamp += 1U;
    frame.metadata.host_received_at = std::chrono::steady_clock::now();
    frame.preview_sequence += 1U;
    const RenderedPreviewImage rendered = processor->Render(frame, options);
    const PreviewRenderTimings &timings = rendered.timings;
    quality.push_back(Milliseconds(timings.quality_analysis));
    transform.push_back(Milliseconds(timings.bayer_transform));
    resize.push_back(Milliseconds(timings.resize));
    encode.push_back(Milliseconds(timings.encode));
    total.push_back(Milliseconds(timings.total));
    payload_bytes.push_back(static_cast<double>(rendered.encoded_bytes.size()));
    Json sample = TimingJson(timings);
    sample["encoded_bytes"] = rendered.encoded_bytes.size();
    samples.push_back(std::move(sample));
  }

  const double median_total = Median(total);
  return {
      {"schema_version", 1},
      {"processor", "software_jpeg"},
      {"source",
       {{"width", kSourceWidth}, {"height", kSourceHeight}, {"pixel_format", "BayerRG8"}}},
      {"output", {{"maximum_width", kPreviewWidth}, {"maximum_height", kPreviewHeight}}},
      {"iterations", kIterations},
      {"median",
       {
           {"quality_analysis_ms", Median(quality)},
           {"bayer_transform_ms", Median(transform)},
           {"resize_ms", Median(resize)},
           {"encode_ms", Median(encode)},
           {"total_ms", median_total},
           {"encoded_bytes", Median(payload_bytes)},
           {"maximum_serial_fps", 1000.0 / median_total},
       }},
      {"samples", std::move(samples)},
  };
}

}  // namespace

int main() {
  try {
    std::cout << RunBenchmark().dump(2) << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "preview_benchmark: " << error.what() << '\n';
    return 1;
  }
}
