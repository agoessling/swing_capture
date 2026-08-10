#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
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

}  // namespace

int main(int argc, char **argv) {
  const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
  const bool use_vaapi = arguments.size() == 3U && std::string_view(arguments[1]) == "--vaapi";
  if (arguments.size() != 2U && !use_vaapi) {
    std::cerr << "usage: clip_encoding_benchmark [--vaapi] <fresh-output-parent>\n";
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
  const auto encoder = use_vaapi ? swing_capture::encoding::MakeVaapiVp9WebmEncoder()
                                 : swing_capture::encoding::MakeSoftwareVp8WebmEncoder();
  const auto started = std::chrono::steady_clock::now();
  const auto result = swing_capture::encoding::WriteClipSession(
      std::filesystem::path(arguments[use_vaapi ? 2U : 1U]), input, *encoder);
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
