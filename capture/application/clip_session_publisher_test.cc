#include "capture/application/clip_session_publisher.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "capture/application/camera_clip_buffer.h"
#include "capture/application/capture_controller.h"
#include "capture/core/camera_source.h"
#include "capture/encoding/clip_session.h"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::FrameMetadata;
using swing_capture::FrameView;
using swing_capture::application::CameraClipBuffer;
using swing_capture::application::CapturedCameraWindow;
using swing_capture::application::CapturedSession;
using swing_capture::application::CapturedTrigger;
using swing_capture::application::CaptureTriggerSource;
using swing_capture::application::ClipSessionPublisher;
using swing_capture::application::PublishedSession;
using swing_capture::application::SessionIdentity;
using swing_capture::application::SyntheticSwingCameraEvidence;
using swing_capture::application::SyntheticSwingSessionEvidence;
using swing_capture::encoding::CameraClipInput;
using swing_capture::encoding::ClipMediaEncoder;
using swing_capture::encoding::ClipMediaEncoderCapabilities;
using swing_capture::encoding::EncodedClipMedia;

class FakeEncoder final : public ClipMediaEncoder {
 public:
  ClipMediaEncoderCapabilities capabilities() const override {
    return {
        .encoder_id = "fake-all-intra",
        .file_extension = "webm",
        .mime_type = "video/webm",
        .codec = "vp8",
        .hardware_accelerated = false,
        .deterministic = true,
        .all_frames_keyframes = true,
    };
  }

  EncodedClipMedia Encode(const CameraClipInput &input,
                          const std::filesystem::path &output_path) const override {
    std::ofstream output(output_path, std::ios::binary);
    output << "fake-webm-" << input.role;
    output.close();
    return {
        .width = input.frames.front().frame.metadata.width,
        .height = input.frames.front().frame.metadata.height,
        .frame_count = input.frames.size(),
        .keyframe_count = input.frames.size(),
        .encoded_bytes = std::filesystem::file_size(output_path),
        .pipeline_profile =
            swing_capture::encoding::ClipViewPipelineProfile{
                .role = std::string(input.role),
                .frame_count = input.frames.size(),
            },
    };
  }
};

FrameView Frame(std::uint64_t frame_id, std::chrono::steady_clock::time_point host_time,
                const std::array<std::byte, 4> &pixels) {
  return {
      .metadata =
          FrameMetadata{
              .frame_id = frame_id,
              .device_timestamp = frame_id * 1000U,
              .host_received_at = host_time,
              .width = 2,
              .height = 2,
              .complete = true,
          },
      .payload = pixels,
  };
}

std::filesystem::path TestOutputRoot() {
  // Bazel gives every test process a private writable directory.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *test_tmpdir = std::getenv("TEST_TMPDIR");
  assert(test_tmpdir != nullptr);
  return std::filesystem::path(test_tmpdir) / "published-sessions";
}

void TestPlansAndPublishesRetainedSession() {
  CameraClipBuffer down(
      {.active_frame_capacity = 32, .reserve_frame_blocks = 0, .maximum_payload_bytes = 4});
  CameraClipBuffer face(
      {.active_frame_capacity = 32, .reserve_frame_blocks = 0, .maximum_payload_bytes = 4});
  down.Arm();
  face.Arm();
  const auto origin = std::chrono::steady_clock::time_point{} + 1s;
  const std::array pixels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  for (std::uint64_t index = 0; index < 21; ++index) {
    down.ObserveFrame(Frame(index + 1U, origin + std::chrono::milliseconds(index), pixels));
    face.ObserveFrame(
        Frame(index + 101U, origin + std::chrono::milliseconds(index) + 100us, pixels));
  }
  const auto impact_time = origin + 10ms;
  CapturedSession captured = {
      .identity =
          SessionIdentity{.session_id = "session-test", .created_at_utc = "2026-08-09T00:00:00Z"},
      .trigger = CapturedTrigger{.source = CaptureTriggerSource::kAudio,
                                 .impact = {.strike_time = impact_time,
                                            .confirmation_time = impact_time + 2ms,
                                            .source_block_start = impact_time - 1ms,
                                            .sample_index_in_block = 10,
                                            .sample_rate_hz = 32000,
                                            .peak_amplitude = 0.5F,
                                            .noise_floor_at_detection = 0.01F,
                                            .threshold_at_detection = 0.08F}},
      .cameras =
          std::array{
              CapturedCameraWindow{.role = "down_the_line",
                                   .serial = "DOWN",
                                   .device_ticks_per_second = 1'000'000,
                                   .frames = down.Freeze()},
              CapturedCameraWindow{.role = "face_on",
                                   .serial = "FACE",
                                   .device_ticks_per_second = 1'000'000,
                                   .frames = face.Freeze()},
          },
      .pipeline_timing =
          swing_capture::application::CapturePipelineTiming{
              .trigger_accepted_at = impact_time + 3ms,
              .freeze_target_at = impact_time + 5ms,
              .freeze_started_at = impact_time + 6ms,
              .freeze_completed_at = impact_time + 7ms,
              .audio_stop_started_at = impact_time + 7ms,
              .audio_stop_completed_at = impact_time + 8ms,
          },
  };
  const std::filesystem::path output_root = TestOutputRoot();
  ClipSessionPublisher publisher({.output_root = output_root, .pre_roll = 5ms, .post_roll = 5ms},
                                 std::make_unique<FakeEncoder>());
  const PublishedSession published = publisher.Publish(
      std::move(captured),
      SyntheticSwingSessionEvidence{
          .selected_brightness = 32,
          .step_duration_us = 20'000,
          .pre_impact_step_count = 60,
          .white_impact_duration_us = 20'000,
          .post_impact_step_count = 25,
          .tone_duration_us = 10'000,
          .tone_frequency_hz = 2'000,
          .cameras =
              std::array{
                  SyntheticSwingCameraEvidence{.role = "down_the_line",
                                               .optical_white_impact_frame_id = 10,
                                               .mapped_time_correction_us = -12'000,
                                               .schedule_uncertainty_us = 2'500,
                                               .optical_white_passed = true,
                                               .stable_frame_count = 4,
                                               .matching_frame_count = 3,
                                               .matching_fraction = 0.75,
                                               .mean_signal_delta = 30.0,
                                               .mean_expected_color_distance = 0.08,
                                               .maximum_saturated_fraction = 0.01,
                                               .maximum_bloom_fraction = 0.02,
                                               .exposure_us = 500.0,
                                               .gain_db = 24.0},
                  SyntheticSwingCameraEvidence{.role = "face_on",
                                               .optical_white_impact_frame_id = 110,
                                               .mapped_time_correction_us = -9'500,
                                               .schedule_uncertainty_us = 3'000,
                                               .optical_white_passed = true,
                                               .stable_frame_count = 5,
                                               .matching_frame_count = 5,
                                               .matching_fraction = 1.0,
                                               .mean_signal_delta = 32.0,
                                               .mean_expected_color_distance = 0.06,
                                               .maximum_saturated_fraction = 0.02,
                                               .maximum_bloom_fraction = 0.01,
                                               .exposure_us = 500.0,
                                               .gain_db = 24.0},
              },
      });
  assert(published.session_id == "session-test");
  assert(published.manifest_path == "session-test/manifest.json");

  std::ifstream manifest_file(output_root / published.manifest_path);
  const Json manifest = Json::parse(manifest_file);
  assert(manifest.at("schema_version") == 1);
  assert(manifest.at("session_id") == "session-test");
  assert(manifest.at("trigger").at("source") == "audio");
  const Json &profile = manifest.at("pipeline_profile");
  assert(profile.at("schema_version") == 2);
  assert(profile.at("capture").at("trigger_estimate_to_confirmation_ms") == 2.0);
  assert(profile.at("capture").at("confirmation_to_acceptance_ms") == 1.0);
  assert(profile.at("capture").at("acceptance_to_freeze_start_ms") == 3.0);
  assert(profile.at("capture").at("freeze_schedule_lateness_ms") == 1.0);
  assert(profile.at("capture").at("freeze_and_rotate_ms") == 1.0);
  assert(profile.at("capture").at("audio_stop_ms") == 1.0);
  assert(profile.at("session").at("prepublication_analysis_ms").get<double>() >= 0.0);
  assert(profile.at("session").at("publisher_planning_ms").get<double>() >= 0.0);
  assert(profile.at("session").at("profile_snapshot_after_confirmation_ms").get<double>() > 0.0);
  assert(profile.at("views").at(0).at("role") == "down_the_line");
  assert(profile.at("views").at(1).at("role") == "face_on");
  assert(manifest.at("views").size() == 2);
  assert(manifest.at("views").at(0).at("role") == "down_the_line");
  assert(manifest.at("views").at(1).at("role") == "face_on");
  assert(manifest.at("views").at(0).at("impact_frame_index") == 5);
  assert(manifest.at("views").at(1).at("impact_frame_index") == 6);
  const Json &hil = manifest.at("hil_evidence");
  assert(hil.at("kind") == "synthetic_swing");
  assert(hil.at("selected_brightness") == 32);
  assert(hil.at("timeline").at("pre_impact_step_count") == 60);
  assert(hil.at("optical_white_impact_frame_index").at("down_the_line") == 4);
  assert(hil.at("optical_white_impact_frame_index").at("face_on") == 5);
  assert(hil.at("audio_trigger_estimate_offset_us").at("down_the_line") == 1'000);
  assert(hil.at("audio_trigger_estimate_offset_us").at("face_on") == 900);
  assert(hil.at("camera_schedule_alignment").at("down_the_line").at("mapped_time_correction_us") ==
         -12'000);
  assert(hil.at("camera_schedule_alignment").at("face_on").at("uncertainty_us") == 3'000);
  assert(hil.at("optical_white_impact").at("down_the_line").at("passed") == true);
  assert(hil.at("optical_white_impact").at("down_the_line").at("stable_frame_count") == 4U);
  assert(hil.at("optical_white_impact").at("face_on").at("matching_fraction") == 1.0);
  assert(hil.at("optical_white_impact").at("face_on").at("exposure_us") == 500.0);
  assert(hil.at("optical_white_impact").at("face_on").at("gain_db") == 24.0);
  assert(std::filesystem::is_regular_file(output_root / "session-test/down_the_line.webm"));
  assert(std::filesystem::is_regular_file(output_root / "session-test/face_on.webm"));
}

}  // namespace

int main() {
  TestPlansAndPublishesRetainedSession();
  return 0;
}
