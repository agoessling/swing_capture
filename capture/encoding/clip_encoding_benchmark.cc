#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "capture/core/camera_source.h"
#include "capture/encoding/clip_session.h"

namespace {

constexpr std::uint32_t kSourceWidth = 1440;
constexpr std::uint32_t kSourceHeight = 1080;
constexpr std::size_t kFrameCount = 341;
constexpr auto kFramePeriod = std::chrono::nanoseconds(4'405'286);

std::vector<std::byte> MakeBayerFrame() {
  std::vector<std::byte> payload(static_cast<std::size_t>(kSourceWidth) * kSourceHeight);
  for (std::uint32_t y = 0; y < kSourceHeight; ++y) {
    for (std::uint32_t x = 0; x < kSourceWidth; ++x) {
      const auto value = static_cast<std::uint8_t>((x * 3U + y * 5U + (x ^ y)) & 0xffU);
      payload[static_cast<std::size_t>(y) * kSourceWidth + x] = std::byte{value};
    }
  }
  return payload;
}

std::vector<swing_capture::encoding::ClipFrameInput> MakeTimeline(
    std::span<const std::byte> payload, std::uint64_t first_frame_id) {
  std::vector<swing_capture::encoding::ClipFrameInput> frames;
  frames.reserve(kFrameCount);
  for (std::size_t index = 0; index < kFrameCount; ++index) {
    const auto offset =
        kFramePeriod * static_cast<std::chrono::nanoseconds::rep>(index) - std::chrono::seconds(1);
    frames.push_back({
        .frame =
            swing_capture::FrameView{
                .metadata =
                    swing_capture::FrameMetadata{
                        .frame_id = first_frame_id + index,
                        .device_timestamp = 10'000'000U + index * 4'405U,
                        .host_received_at = std::chrono::steady_clock::time_point{} + offset,
                        .width = kSourceWidth,
                        .height = kSourceHeight,
                        .complete = true,
                    },
                .payload = payload,
            },
        .time_from_impact = offset,
    });
  }
  return frames;
}

bool IsValidViewProfile(const swing_capture::encoding::ClipViewPipelineProfile &profile,
                        std::string_view expected_role) {
  const double stage_total = profile.bayer_fit_demosaic_ms + profile.rgb_to_yuv420_ms +
                             profile.codec_encode_ms + profile.webm_mux_ms + profile.finalize_ms +
                             profile.output_verification_ms;
  return profile.role == expected_role && profile.frame_count == kFrameCount &&
         profile.timeline_ms >= 0.0 && stage_total >= 0.0 && profile.total_ms > 0.0 &&
         std::isfinite(profile.total_ms) && profile.total_ms + 1e-6 >= stage_total;
}

std::optional<std::size_t> ParseSize(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::size_t>(character - '0');
    if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
      return std::nullopt;
    }
    value = value * 10U + digit;
  }
  return value;
}

}  // namespace

int main(int argc, char **argv) {
  const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
  const bool use_vaapi =
      (arguments.size() == 3U || arguments.size() == 5U || arguments.size() == 6U) &&
      (std::string_view(arguments[1]) == "--vaapi" ||
       std::string_view(arguments[1]) == "--vaapi-full");
  const bool use_full_resolution = use_vaapi && std::string_view(arguments[1]) == "--vaapi-full";
  const bool tuned = arguments.size() >= 5U;
  const std::optional<std::size_t> preprocessing_threads =
      tuned ? ParseSize(arguments[2]) : std::optional<std::size_t>{4U};
  const std::optional<std::size_t> encoding_queue_depth =
      tuned ? ParseSize(arguments[3]) : std::optional<std::size_t>{4U};
  const std::optional<std::size_t> quality_index =
      arguments.size() == 6U ? ParseSize(arguments[4]) : std::optional<std::size_t>{24U};
  if ((arguments.size() != 2U && !use_vaapi) || !preprocessing_threads.has_value() ||
      !encoding_queue_depth.has_value() || !quality_index.has_value() ||
      preprocessing_threads.value_or(0U) == 0U || encoding_queue_depth.value_or(0U) == 0U ||
      quality_index.value_or(256U) > 255U) {
    std::cerr << "usage: clip_encoding_benchmark [--vaapi|--vaapi-full "
                 "[preprocessing-threads queue-depth [quality-index]]] <fresh-output-parent>\n";
    return 2;
  }
  const std::vector<std::byte> payload = MakeBayerFrame();
  const auto down_frames = MakeTimeline(payload, 1'000);
  const auto face_frames = MakeTimeline(payload, 9'000);
  const swing_capture::encoding::DualViewClipInput input{
      .session_id = "synthetic-dual-1p5s",
      .created_at_utc = "2026-08-09T00:00:00Z",
      .trigger = {.source = "synthetic",
                  .host_monotonic_time_ns = 0,
                  .confirmation_host_monotonic_time_ns = 0},
      .views =
          {
              swing_capture::encoding::CameraClipInput{
                  .role = "down_the_line",
                  .camera_serial = "synthetic-dtl",
                  .pixel_format = "BayerRG8",
                  .frames = down_frames,
              },
              swing_capture::encoding::CameraClipInput{
                  .role = "face_on",
                  .camera_serial = "synthetic-face",
                  .pixel_format = "BayerRG8",
                  .frames = face_frames,
              },
          },
      .mapped_nearest_frame_skew = std::chrono::nanoseconds::zero(),
      .hil_evidence = std::nullopt,
      .capture_pipeline_profile = swing_capture::encoding::ClipCapturePipelineProfile{},
  };
  const auto encoder =
      use_vaapi
          ? swing_capture::encoding::MakeVaapiVp9WebmEncoder(
                use_full_resolution
                    ? swing_capture::encoding::VaapiVp9WebmOptions{
                          .maximum_width = kSourceWidth,
                          .maximum_height = kSourceHeight,
                          .preprocessing_threads = preprocessing_threads.value_or(0U),
                          .encoding_queue_depth = encoding_queue_depth.value_or(0U),
                          .quality_index = static_cast<std::uint32_t>(quality_index.value_or(0U)),
                      }
                    : swing_capture::encoding::VaapiVp9WebmOptions{})
          : swing_capture::encoding::MakeSoftwareVp8WebmEncoder();
  const auto started = std::chrono::steady_clock::now();
  const auto result = swing_capture::encoding::WriteClipSession(
      std::filesystem::path(arguments[arguments.size() - 1U]), input, *encoder);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  if (!result.pipeline_profile.has_value() ||
      !IsValidViewProfile(result.pipeline_profile->views[0], "down_the_line") ||
      !IsValidViewProfile(result.pipeline_profile->views[1], "face_on") ||
      result.pipeline_profile->session.media_encoding_wall_ms + 1e-6 <
          std::max(result.pipeline_profile->views[0].total_ms,
                   result.pipeline_profile->views[1].total_ms)) {
    std::cerr << "invalid pipeline profile\n";
    return 1;
  }
  const auto &profile = *result.pipeline_profile;
  std::cout << "session=" << result.session_directory << '\n'
            << "frames_per_view=" << kFrameCount << '\n'
            << "elapsed_seconds=" << std::chrono::duration<double>(elapsed).count() << '\n'
            << "aggregate_frames_per_second="
            << static_cast<double>(kFrameCount * 2U) /
                   std::chrono::duration<double>(elapsed).count()
            << '\n'
            << "validation_and_timeline_ms=" << profile.session.validation_and_timeline_ms << '\n'
            << "output_setup_ms=" << profile.session.output_setup_ms << '\n'
            << "media_encoding_wall_ms=" << profile.session.media_encoding_wall_ms << '\n'
            << "frame_metadata_ms=" << profile.session.frame_metadata_ms << '\n';
  for (const auto &view : profile.views) {
    std::cout << view.role << ".total_ms=" << view.total_ms << '\n'
              << view.role << ".bayer_fit_demosaic_ms=" << view.bayer_fit_demosaic_ms << '\n'
              << view.role << ".rgb_to_yuv420_ms=" << view.rgb_to_yuv420_ms << '\n'
              << view.role << ".codec_encode_ms=" << view.codec_encode_ms << '\n'
              << view.role << ".webm_mux_ms=" << view.webm_mux_ms << '\n';
  }
  return 0;
}
