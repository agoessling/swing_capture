#include "capture/encoding/clip_session.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "capture/core/camera_source.h"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::FrameMetadata;
using swing_capture::FrameView;
using swing_capture::encoding::CameraClipInput;
using swing_capture::encoding::ClipCapturePipelineProfile;
using swing_capture::encoding::ClipFrameInput;
using swing_capture::encoding::DualViewClipInput;
using swing_capture::encoding::InspectVp8Webm;
using swing_capture::encoding::MakeSoftwareVp8WebmEncoder;
using swing_capture::encoding::SoftwareVp8WebmOptions;
using swing_capture::encoding::SyntheticSwingHilEvidence;
using swing_capture::encoding::SyntheticSwingHilViewEvidence;
using swing_capture::encoding::WriteClipSession;

constexpr std::uint32_t kWidth = 32;
constexpr std::uint32_t kHeight = 24;
constexpr std::array<std::chrono::nanoseconds, 5> kTimes = {
    -8'808us, -4'404us, 0us, 4'405us, 8'810us,
};

struct OwnedCameraClip {
  std::vector<std::vector<std::byte>> payloads;
  std::vector<ClipFrameInput> frames;
};

std::filesystem::path TestRoot() {
  const char *temporary = std::getenv("TEST_TMPDIR");
  assert(temporary != nullptr);
  const std::filesystem::path root = std::filesystem::path(temporary) / "clip-session";
  std::filesystem::create_directories(root);
  return root;
}

OwnedCameraClip MakeCamera(std::uint64_t first_frame_id, std::uint8_t marker) {
  OwnedCameraClip camera;
  camera.payloads.reserve(kTimes.size());
  camera.frames.reserve(kTimes.size());
  for (std::size_t frame_index = 0; frame_index < kTimes.size(); ++frame_index) {
    std::vector<std::byte> payload(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
      for (std::uint32_t x = 0; x < kWidth; ++x) {
        const auto value =
            static_cast<std::uint8_t>((x * 7U + y * 11U + frame_index * 23U + marker) & 0xffU);
        payload[static_cast<std::size_t>(y) * kWidth + x] = std::byte{value};
      }
    }
    camera.payloads.push_back(std::move(payload));
  }
  for (std::size_t frame_index = 0; frame_index < kTimes.size(); ++frame_index) {
    camera.frames.push_back({
        .frame =
            FrameView{
                .metadata =
                    FrameMetadata{
                        .frame_id = first_frame_id + frame_index,
                        .device_timestamp = 9'000'000U + frame_index * 4'404U,
                        .host_received_at =
                            std::chrono::steady_clock::time_point(1s) + kTimes[frame_index],
                        .width = kWidth,
                        .height = kHeight,
                        .complete = true,
                    },
                .payload = camera.payloads[frame_index],
            },
        .time_from_impact = kTimes[frame_index],
    });
  }
  return camera;
}

DualViewClipInput MakeInput(std::string_view session_id, const OwnedCameraClip &down,
                            const OwnedCameraClip &face) {
  return {
      .session_id = session_id,
      .created_at_utc = "2026-08-09T16:00:00Z",
      .trigger =
          {
              .source = "audio_impact",
              .host_monotonic_time_ns = 123'456'789,
              .confirmation_host_monotonic_time_ns = 123'488'039,
              .sample_rate_hz = 32'000,
              .peak_amplitude = 0.91,
              .noise_floor = 0.02,
              .threshold = 0.25,
          },
      .views =
          {
              CameraClipInput{
                  .role = "down_the_line",
                  .camera_serial = "DTL-123",
                  .pixel_format = "BayerRG8",
                  .frames = down.frames,
              },
              CameraClipInput{
                  .role = "face_on",
                  .camera_serial = "FACE-456",
                  .pixel_format = "BayerRG8",
                  .frames = face.frames,
              },
          },
      .mapped_nearest_frame_skew = 123us,
      .hil_evidence = std::nullopt,
      .capture_pipeline_profile = std::nullopt,
  };
}

SyntheticSwingHilEvidence CompleteHilEvidence() {
  constexpr std::uint8_t kBrightness = 8;
  return {
      .selected_brightness = kBrightness,
      .step_duration_us = 20'000,
      .pre_impact_step_count = 60,
      .white_impact_duration_us = 20'000,
      .post_impact_step_count = 25,
      .tone_duration_us = 10'000,
      .tone_frequency_hz = 2'000,
      .views =
          std::array{
              SyntheticSwingHilViewEvidence{.role = "down_the_line",
                                            .optical_white_impact_frame_index = 2,
                                            .mapped_time_correction_us = -11'500,
                                            .schedule_uncertainty_us = 2'750,
                                            .audio_trigger_estimate_offset_us = 127'000,
                                            .optical_white_passed = true,
                                            .stable_frame_count = 4,
                                            .matching_frame_count = 3,
                                            .matching_fraction = 0.75,
                                            .mean_signal_delta = 12.0,
                                            .mean_expected_color_distance = 42.0,
                                            .maximum_saturated_fraction = 0.08,
                                            .maximum_bloom_fraction = 0.08,
                                            .exposure_us = 500.0,
                                            .gain_db = 24.0},
              SyntheticSwingHilViewEvidence{.role = "face_on",
                                            .optical_white_impact_frame_index = 2,
                                            .mapped_time_correction_us = -9'000,
                                            .schedule_uncertainty_us = 3'000,
                                            .audio_trigger_estimate_offset_us = 126'500,
                                            .optical_white_passed = true,
                                            .stable_frame_count = 2,
                                            .matching_frame_count = 2,
                                            .matching_fraction = 1.0,
                                            .mean_signal_delta = 12.0,
                                            .mean_expected_color_distance = 99.0,
                                            .maximum_saturated_fraction = 0.08,
                                            .maximum_bloom_fraction = 0.08,
                                            .exposure_us = 500.0,
                                            .gain_db = 24.0},
          },
  };
}

ClipCapturePipelineProfile CompleteCapturePipelineProfile() {
  return {
      .trigger_estimate_to_confirmation = 31'250ns,
      .confirmation_to_acceptance = -3'500us,
      .acceptance_to_freeze_start = 4'250us,
      .freeze_schedule_lateness = -750us,
      .freeze_and_rotate = 8'000us,
      .audio_stop = 1'500us,
      .prepublication_analysis = 6'000us,
      .publisher_planning = 7'000us,
  };
}

std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  assert(input);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

template <typename Function>
void AssertThrows(Function &&function) {
  bool threw = false;
  try {
    function();
  } catch (const std::exception &) {
    threw = true;
  }
  assert(threw);
}

template <typename Mutator>
void AssertHilPolicyRejects(const std::filesystem::path &root, std::string_view session_id,
                            const OwnedCameraClip &down, const OwnedCameraClip &face,
                            const swing_capture::encoding::ClipMediaEncoder &encoder,
                            Mutator mutator) {
  auto input = MakeInput(session_id, down, face);
  input.hil_evidence = CompleteHilEvidence();
  mutator(input.hil_evidence->views[0]);
  AssertThrows([&] { static_cast<void>(WriteClipSession(root, input, encoder)); });
  assert(!std::filesystem::exists(root / session_id));
}

void WritesPortableAtomicDualViewSession() {
  const std::filesystem::path root = TestRoot() / "portable";
  const OwnedCameraClip down = MakeCamera(100, 17);
  const OwnedCameraClip face = MakeCamera(900, 91);
  const auto encoder =
      MakeSoftwareVp8WebmEncoder(SoftwareVp8WebmOptions{.maximum_width = 24, .maximum_height = 18});
  const auto result = WriteClipSession(root, MakeInput("clip-0001", down, face), *encoder);

  assert(result.session_directory == root / "clip-0001");
  assert(std::filesystem::is_regular_file(result.manifest_path));
  assert(std::filesystem::is_regular_file(result.media_paths[0]));
  assert(std::filesystem::is_regular_file(result.media_paths[1]));
  const Json manifest = Json::parse(ReadFile(result.manifest_path));
  assert(manifest.at("schema_version") == 1);
  assert(manifest.at("session_id") == "clip-0001");
  assert(manifest.at("created_at_utc") == "2026-08-09T16:00:00Z");
  assert(manifest.at("trigger").at("source") == "audio_impact");
  assert(manifest.at("trigger").at("host_monotonic_time_ns") == "123456789");
  assert(manifest.at("trigger").at("confirmation_host_monotonic_time_ns") == "123488039");
  assert(manifest.at("trigger").at("sample_rate_hz") == 32'000);
  assert(manifest.at("trigger").at("peak_amplitude") == 0.91);
  assert(manifest.at("trigger").at("noise_floor") == 0.02);
  assert(manifest.at("trigger").at("threshold") == 0.25);
  assert(manifest.at("mapped_nearest_frame_skew_us") == 123);
  assert(manifest.at("views").size() == 2U);
  assert(!manifest.contains("pipeline_profile"));
  assert(!result.pipeline_profile.has_value());

  const Json &view = manifest.at("views").at(0);
  assert(view.at("role") == "down_the_line");
  assert(view.at("camera_serial") == "DTL-123");
  assert(view.at("source").at("pixel_format") == "BayerRG8");
  assert(view.at("source").at("width") == kWidth);
  assert(view.at("source").at("height") == kHeight);
  assert(view.at("encoded").at("width") == 24);
  assert(view.at("encoded").at("height") == 18);
  assert(view.at("frame_count") == kTimes.size());
  assert(view.at("impact_frame_index") == 2);
  assert(std::abs(view.at("nominal_fps").get<double>() - 227.066) < 0.1);
  assert(view.at("media").at("path") == "down_the_line.webm");
  assert(view.at("media").at("mime_type") == "video/webm");
  assert(view.at("media").at("codec") == "vp8");
  assert(view.at("media").at("all_frames_keyframes") == true);
  assert(view.at("media").at("encoded_bytes") == std::filesystem::file_size(result.media_paths[0]));
  assert(view.at("frames").at(0).at("frame_id") == "100");
  assert(view.at("frames").at(0).at("time_from_impact_us") == -8'808);
  assert(view.at("frames").at(0).at("media_time_us") == 0);
  assert(view.at("frames").at(4).at("media_time_us") == 17'618);

  for (const std::filesystem::path &media_path : result.media_paths) {
    const auto inspection = InspectVp8Webm(media_path);
    assert(inspection.width == 24U);
    assert(inspection.height == 18U);
    assert(inspection.frame_count == kTimes.size());
    assert(inspection.keyframe_count == inspection.frame_count);
    assert(inspection.first_frame_time == 0us);
    assert(inspection.last_frame_time == 17'618us);
  }
}

void WritesDetailedPipelineProfile() {
  const std::filesystem::path root = TestRoot() / "pipeline-profile";
  const OwnedCameraClip down = MakeCamera(100, 17);
  const OwnedCameraClip face = MakeCamera(900, 91);
  auto input = MakeInput("profiled-clip", down, face);
  input.capture_pipeline_profile = CompleteCapturePipelineProfile();
  const auto encoder =
      MakeSoftwareVp8WebmEncoder(SoftwareVp8WebmOptions{.maximum_width = 24, .maximum_height = 18});
  const auto result = WriteClipSession(root, input, *encoder);
  assert(result.pipeline_profile.has_value());

  const Json profile = Json::parse(ReadFile(result.manifest_path)).at("pipeline_profile");
  assert(profile.at("schema_version") == 2);
  const Json &capture = profile.at("capture");
  assert(capture.at("trigger_estimate_to_confirmation_ms") == 0.03125);
  assert(capture.at("confirmation_to_acceptance_ms") == -3.5);
  assert(capture.at("acceptance_to_freeze_start_ms") == 4.25);
  assert(capture.at("freeze_schedule_lateness_ms") == -0.75);
  assert(capture.at("freeze_and_rotate_ms") == 8.0);
  assert(capture.at("audio_stop_ms") == 1.5);

  const Json &session = profile.at("session");
  assert(session.at("prepublication_analysis_ms") == 6.0);
  assert(session.at("publisher_planning_ms") == 7.0);
  assert(session.at("validation_and_timeline_ms").get<double>() >= 0.0);
  assert(session.at("output_setup_ms").get<double>() >= 0.0);
  assert(session.at("media_encoding_wall_ms").get<double>() > 0.0);
  assert(session.at("frame_metadata_ms").get<double>() >= 0.0);
  assert(session.at("profile_snapshot_after_confirmation_ms").get<double>() > 0.0);
  assert(session.at("profile_snapshot_host_monotonic_ns").is_string());
  assert(session.at("profile_snapshot_host_monotonic_ns") ==
         std::to_string(result.pipeline_profile->session.profile_snapshot_host_monotonic_ns));

  const Json &views = profile.at("views");
  assert(views.size() == 2U);
  double longest_view_ms = 0.0;
  for (std::size_t index = 0; index < views.size(); ++index) {
    const Json &view = views.at(index);
    assert(view.at("role") == input.views[index].role);
    assert(view.at("frame_count") == input.views[index].frames.size());
    assert(view.at("timeline_ms").get<double>() >= 0.0);
    const double stage_total =
        view.at("bayer_fit_demosaic_ms").get<double>() + view.at("rgb_to_yuv420_ms").get<double>() +
        view.at("codec_encode_ms").get<double>() + view.at("webm_mux_ms").get<double>() +
        view.at("finalize_ms").get<double>() + view.at("output_verification_ms").get<double>();
    const double total_ms = view.at("total_ms").get<double>();
    assert(total_ms > 0.0);
    assert(total_ms + 1e-6 >= stage_total);
    longest_view_ms = std::max(longest_view_ms, total_ms);
  }
  assert(session.at("media_encoding_wall_ms").get<double>() + 1e-6 >= longest_view_ms);
}

void EncodingIsByteDeterministic() {
  const std::filesystem::path root = TestRoot() / "deterministic";
  std::filesystem::create_directories(root);
  const OwnedCameraClip camera = MakeCamera(300, 44);
  const CameraClipInput input{
      .role = "down_the_line",
      .camera_serial = "DTL-123",
      .pixel_format = "BayerRG8",
      .frames = camera.frames,
  };
  const auto encoder =
      MakeSoftwareVp8WebmEncoder(SoftwareVp8WebmOptions{.maximum_width = 24, .maximum_height = 18});
  const auto first = encoder->Encode(input, root / "first.webm");
  const auto second = encoder->Encode(input, root / "second.webm");
  assert(first.frame_count == second.frame_count);
  assert(first.keyframe_count == second.keyframe_count);
  assert(first.encoded_bytes == second.encoded_bytes);
  assert(ReadFile(root / "first.webm") == ReadFile(root / "second.webm"));
}

void WritesAbsentTriggerEvidenceAsJsonNull() {
  const std::filesystem::path root = TestRoot() / "manual-trigger";
  const OwnedCameraClip down = MakeCamera(500, 33);
  const OwnedCameraClip face = MakeCamera(700, 55);
  auto input = MakeInput("manual-clip", down, face);
  input.trigger = {.source = "manual", .host_monotonic_time_ns = 400};
  const auto encoder =
      MakeSoftwareVp8WebmEncoder(SoftwareVp8WebmOptions{.maximum_width = 8, .maximum_height = 6});
  const auto result = WriteClipSession(root, input, *encoder);
  const Json trigger = Json::parse(ReadFile(result.manifest_path)).at("trigger");
  assert(trigger.at("confirmation_host_monotonic_time_ns").is_null());
  assert(trigger.at("sample_rate_hz").is_null());
  assert(trigger.at("peak_amplitude").is_null());
  assert(trigger.at("noise_floor").is_null());
  assert(trigger.at("threshold").is_null());
}

void WritesSyntheticSwingHilEvidence() {
  const std::filesystem::path root = TestRoot() / "hil-evidence";
  const OwnedCameraClip down = MakeCamera(100, 33);
  const OwnedCameraClip face = MakeCamera(700, 55);
  auto input = MakeInput("hil-clip", down, face);
  input.hil_evidence = CompleteHilEvidence();
  const auto encoder =
      MakeSoftwareVp8WebmEncoder(SoftwareVp8WebmOptions{.maximum_width = 8, .maximum_height = 6});
  const auto result = WriteClipSession(root, input, *encoder);
  const Json hil = Json::parse(ReadFile(result.manifest_path)).at("hil_evidence");
  assert(hil.at("kind") == "synthetic_swing");
  assert(hil.at("selected_brightness") == 8);
  assert(hil.at("optical_white_impact_frame_index").at("face_on") == 2);
  assert(hil.at("audio_trigger_estimate_offset_us").at("down_the_line") == 127'000);
  assert(hil.at("camera_schedule_alignment").at("down_the_line").at("mapped_time_correction_us") ==
         -11'500);
  assert(hil.at("camera_schedule_alignment").at("face_on").at("uncertainty_us") == 3'000);
  const Json &down_white = hil.at("optical_white_impact").at("down_the_line");
  assert(down_white.at("passed") == true);
  assert(down_white.at("stable_frame_count") == 4U);
  assert(down_white.at("matching_frame_count") == 3U);
  assert(down_white.at("matching_fraction") == 0.75);
  assert(down_white.at("mean_signal_delta") == 12.0);
  assert(down_white.at("mean_expected_color_distance") == 42.0);
  assert(down_white.at("maximum_saturated_fraction") == 0.08);
  assert(down_white.at("maximum_bloom_fraction") == 0.08);
  assert(down_white.at("exposure_us") == 500.0);
  assert(hil.at("optical_white_impact").at("face_on").at("gain_db") == 24.0);
}

void RejectsInvalidInputsWithoutPublishing() {
  const std::filesystem::path root = TestRoot() / "invalid";
  OwnedCameraClip down = MakeCamera(100, 17);
  const OwnedCameraClip face = MakeCamera(900, 91);
  const auto encoder = MakeSoftwareVp8WebmEncoder();

  down.frames[2].frame.metadata.frame_id += 1U;
  AssertThrows([&] {
    static_cast<void>(WriteClipSession(root, MakeInput("bad-gap", down, face), *encoder));
  });
  assert(!std::filesystem::exists(root / "bad-gap"));

  const OwnedCameraClip valid_down = MakeCamera(100, 17);
  AssertThrows([&] {
    static_cast<void>(WriteClipSession(root, MakeInput("../escape", valid_down, face), *encoder));
  });
  assert(!std::filesystem::exists(TestRoot() / "escape"));

  auto invalid_audio = MakeInput("bad-audio", valid_down, face);
  invalid_audio.trigger.peak_amplitude = std::numeric_limits<double>::quiet_NaN();
  AssertThrows([&] { static_cast<void>(WriteClipSession(root, invalid_audio, *encoder)); });
  assert(!std::filesystem::exists(root / "bad-audio"));

  auto invalid_confirmation = MakeInput("bad-confirmation", valid_down, face);
  invalid_confirmation.trigger.confirmation_host_monotonic_time_ns =
      invalid_confirmation.trigger.host_monotonic_time_ns - 1;
  AssertThrows([&] { static_cast<void>(WriteClipSession(root, invalid_confirmation, *encoder)); });
  assert(!std::filesystem::exists(root / "bad-confirmation"));

  auto missing_profile_confirmation = MakeInput("profile-without-confirmation", valid_down, face);
  missing_profile_confirmation.trigger.confirmation_host_monotonic_time_ns = std::nullopt;
  missing_profile_confirmation.capture_pipeline_profile = CompleteCapturePipelineProfile();
  AssertThrows(
      [&] { static_cast<void>(WriteClipSession(root, missing_profile_confirmation, *encoder)); });
  assert(!std::filesystem::exists(root / "profile-without-confirmation"));

  auto negative_capture_duration = MakeInput("negative-capture-duration", valid_down, face);
  negative_capture_duration.capture_pipeline_profile = CompleteCapturePipelineProfile();
  negative_capture_duration.capture_pipeline_profile->audio_stop = -1ns;
  AssertThrows(
      [&] { static_cast<void>(WriteClipSession(root, negative_capture_duration, *encoder)); });
  assert(!std::filesystem::exists(root / "negative-capture-duration"));

  auto mismatched_confirmation = MakeInput("mismatched-confirmation", valid_down, face);
  mismatched_confirmation.capture_pipeline_profile = CompleteCapturePipelineProfile();
  mismatched_confirmation.capture_pipeline_profile->trigger_estimate_to_confirmation += 2us;
  AssertThrows(
      [&] { static_cast<void>(WriteClipSession(root, mismatched_confirmation, *encoder)); });
  assert(!std::filesystem::exists(root / "mismatched-confirmation"));

  auto inconsistent_fraction = MakeInput("bad-hil-white-fraction", valid_down, face);
  inconsistent_fraction.hil_evidence = CompleteHilEvidence();
  inconsistent_fraction.hil_evidence->views[1].matching_fraction = 0.5;
  AssertThrows([&] { static_cast<void>(WriteClipSession(root, inconsistent_fraction, *encoder)); });
  assert(!std::filesystem::exists(root / "bad-hil-white-fraction"));

  AssertHilPolicyRejects(root, "bad-hil-one-frame", valid_down, face, *encoder,
                         [](SyntheticSwingHilViewEvidence &view) {
                           view.stable_frame_count = 1;
                           view.matching_frame_count = 1;
                           view.matching_fraction = 1.0;
                         });
  AssertHilPolicyRejects(root, "bad-hil-match", valid_down, face, *encoder,
                         [](SyntheticSwingHilViewEvidence &view) {
                           view.matching_frame_count = 2;
                           view.matching_fraction = 0.5;
                         });
  AssertHilPolicyRejects(
      root, "bad-hil-signal", valid_down, face, *encoder,
      [](SyntheticSwingHilViewEvidence &view) { view.mean_signal_delta = 11.999; });
  AssertHilPolicyRejects(
      root, "bad-hil-saturation", valid_down, face, *encoder,
      [](SyntheticSwingHilViewEvidence &view) { view.maximum_saturated_fraction = 0.080'001; });
  AssertHilPolicyRejects(
      root, "bad-hil-bloom", valid_down, face, *encoder,
      [](SyntheticSwingHilViewEvidence &view) { view.maximum_bloom_fraction = 0.080'001; });
}

void NeverOverwritesPublishedSession() {
  const std::filesystem::path root = TestRoot() / "no-overwrite";
  const OwnedCameraClip down = MakeCamera(100, 17);
  const OwnedCameraClip face = MakeCamera(900, 91);
  const auto encoder =
      MakeSoftwareVp8WebmEncoder(SoftwareVp8WebmOptions{.maximum_width = 16, .maximum_height = 12});
  const auto input = MakeInput("clip-keep", down, face);
  const auto first = WriteClipSession(root, input, *encoder);
  const std::string original_manifest = ReadFile(first.manifest_path);
  AssertThrows([&] { static_cast<void>(WriteClipSession(root, input, *encoder)); });
  assert(ReadFile(first.manifest_path) == original_manifest);
}

void RejectsMalformedWebmAndInvalidOptions() {
  const std::filesystem::path malformed = TestRoot() / "not-webm.webm";
  {
    std::ofstream output(malformed, std::ios::binary);
    output << "not a webm";
  }
  AssertThrows([&] { static_cast<void>(InspectVp8Webm(malformed)); });
  AssertThrows([] {
    static_cast<void>(MakeSoftwareVp8WebmEncoder(
        SoftwareVp8WebmOptions{.maximum_width = 1, .maximum_height = 480}));
  });
  AssertThrows([] {
    static_cast<void>(MakeSoftwareVp8WebmEncoder(
        SoftwareVp8WebmOptions{.minimum_quantizer = 40, .maximum_quantizer = 20}));
  });
  AssertThrows(
      [] { static_cast<void>(MakeSoftwareVp8WebmEncoder(SoftwareVp8WebmOptions{.cpu_used = 6})); });
}

}  // namespace

int main() {
  WritesPortableAtomicDualViewSession();
  WritesDetailedPipelineProfile();
  EncodingIsByteDeterministic();
  WritesAbsentTriggerEvidenceAsJsonNull();
  WritesSyntheticSwingHilEvidence();
  RejectsInvalidInputsWithoutPublishing();
  NeverOverwritesPublishedSession();
  RejectsMalformedWebmAndInvalidOptions();
  return 0;
}
