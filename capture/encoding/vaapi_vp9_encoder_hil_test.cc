#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <vector>

#include "capture/core/camera_source.h"
#include "capture/encoding/clip_session.h"

namespace {

using namespace std::chrono_literals;
using swing_capture::FrameMetadata;
using swing_capture::FrameView;
using swing_capture::encoding::CameraClipInput;
using swing_capture::encoding::ClipFrameInput;
using swing_capture::encoding::InspectWebm;
using swing_capture::encoding::MakeVaapiVp9WebmEncoder;
using swing_capture::encoding::VaapiVp9WebmOptions;

constexpr std::uint32_t kWidth = 128;
constexpr std::uint32_t kHeight = 128;
constexpr std::size_t kFrameCount = 30;

struct OwnedClip {
  std::vector<std::vector<std::byte>> payloads;
  std::vector<ClipFrameInput> frames;
};

OwnedClip MakeClip() {
  OwnedClip clip;
  clip.payloads.reserve(kFrameCount);
  clip.frames.reserve(kFrameCount);
  for (std::size_t frame_index = 0; frame_index < kFrameCount; ++frame_index) {
    std::vector<std::byte> payload(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
      for (std::uint32_t x = 0; x < kWidth; ++x) {
        const auto value = static_cast<std::uint8_t>(
            (x * 5U + y * 7U + static_cast<std::uint32_t>(frame_index) * 13U) & 0xffU);
        payload[static_cast<std::size_t>(y) * kWidth + x] = std::byte{value};
      }
    }
    clip.payloads.push_back(std::move(payload));
  }
  for (std::size_t frame_index = 0; frame_index < kFrameCount; ++frame_index) {
    clip.frames.push_back({
        .frame =
            FrameView{
                .metadata =
                    FrameMetadata{.frame_id = frame_index + 1U,
                                  .device_timestamp = frame_index * 4'405U,
                                  .host_received_at = std::chrono::steady_clock::time_point{} +
                                                      frame_index * 4'405us,
                                  .width = kWidth,
                                  .height = kHeight,
                                  .complete = true},
                .payload = clip.payloads[frame_index],
            },
        .time_from_impact =
            std::chrono::microseconds(static_cast<std::int64_t>(frame_index) * 4'405 - 66'075),
    });
  }
  return clip;
}

void EncodesAllKeyframeVp9OnRenderNode() {
  const char *const temporary = std::getenv("TEST_TMPDIR");
  assert(temporary != nullptr);
  const std::filesystem::path output = std::filesystem::path(temporary) / "vaapi-vp9.webm";
  const OwnedClip clip = MakeClip();
  const auto encoder = MakeVaapiVp9WebmEncoder(
      VaapiVp9WebmOptions{.maximum_width = 128, .maximum_height = 128, .quality_index = 24});
  const auto capabilities = encoder->capabilities();
  assert(capabilities.codec == "vp9");
  assert(capabilities.hardware_accelerated);
  assert(capabilities.all_frames_keyframes);

  const auto result = encoder->Encode(CameraClipInput{.role = "down_the_line",
                                                      .camera_serial = "HIL",
                                                      .pixel_format = "BayerRG8",
                                                      .frames = clip.frames},
                                      output);
  assert(result.frame_count == kFrameCount);
  assert(result.keyframe_count == kFrameCount);
  assert(result.encoded_bytes > 0U);
  assert(result.pipeline_profile.codec_encode_ms > 0.0);
  const auto inspection = InspectWebm(output);
  assert(inspection.codec == "vp9");
  assert(inspection.width == kWidth);
  assert(inspection.height == kHeight);
  assert(inspection.frame_count == kFrameCount);
  assert(inspection.keyframe_count == kFrameCount);
}

}  // namespace

int main() { EncodesAllKeyframeVp9OnRenderNode(); }
