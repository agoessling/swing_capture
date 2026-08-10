#include "capture/hil/session_artifact_validator.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "capture/core/camera_source.h"
#include "capture/encoding/clip_session.h"

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::FrameMetadata;
using swing_capture::FrameView;
using swing_capture::encoding::CameraClipInput;
using swing_capture::encoding::ClipFrameInput;
using swing_capture::encoding::ClipMediaEncoder;
using swing_capture::encoding::MakeSoftwareVp8WebmEncoder;
using swing_capture::hil::SessionArtifactExpectations;
using swing_capture::hil::ValidateSessionArtifacts;

SessionArtifactExpectations Expectations() {
  return {
      .session_id = "session-17",
      .views = {{{.role = "down_the_line",
                  .camera_serial = "DOWN123",
                  .expected_synthetic_swing_exposure_us = 500.0,
                  .expected_synthetic_swing_gain_db = 24.0},
                 {.role = "face_on",
                  .camera_serial = "FACE456",
                  .expected_synthetic_swing_exposure_us = 500.0,
                  .expected_synthetic_swing_gain_db = 24.0}}},
      .minimum_frame_count = 3,
  };
}

Json View(std::string role, std::string serial, std::string media_path,
          std::uintmax_t encoded_bytes) {
  return {
      {"role", std::move(role)},
      {"camera_serial", std::move(serial)},
      {"source", {{"pixel_format", "BayerRG8"}, {"width", 4}, {"height", 4}}},
      {"encoded", {{"width", 4}, {"height", 4}}},
      {"frame_count", 3},
      {"nominal_fps", 227.0},
      {"impact_frame_index", 1},
      {"media",
       {{"path", std::move(media_path)},
        {"mime_type", "video/webm"},
        {"codec", "vp8"},
        {"all_frames_keyframes", true},
        {"encoded_bytes", encoded_bytes}}},
      {"frames", Json::array({
                     {{"frame_index", 0},
                      {"frame_id", "41"},
                      {"device_timestamp", "9000"},
                      {"time_from_impact_us", -4405},
                      {"media_time_us", 0}},
                     {{"frame_index", 1},
                      {"frame_id", "42"},
                      {"device_timestamp", "13405"},
                      {"time_from_impact_us", 0},
                      {"media_time_us", 4405}},
                     {{"frame_index", 2},
                      {"frame_id", "43"},
                      {"device_timestamp", "17810"},
                      {"time_from_impact_us", 4405},
                      {"media_time_us", 8810}},
                 })},
  };
}

Json PipelineProfile() {
  return {
      {"schema_version", 2},
      {"capture",
       {{"trigger_estimate_to_confirmation_ms", 2.0},
        {"confirmation_to_acceptance_ms", 0.2},
        {"acceptance_to_freeze_start_ms", 0.3},
        {"freeze_schedule_lateness_ms", -0.25},
        {"freeze_and_rotate_ms", 0.8},
        {"audio_stop_ms", 0.1}}},
      {"session",
       {{"prepublication_analysis_ms", 0.5},
        {"publisher_planning_ms", 0.3},
        {"validation_and_timeline_ms", 0.4},
        {"output_setup_ms", 0.2},
        {"media_encoding_wall_ms", 6.8},
        {"frame_metadata_ms", 0.3},
        {"profile_snapshot_after_confirmation_ms", 100.0},
        {"profile_snapshot_host_monotonic_ns", "225456789"}}},
      {"views", Json::array({
                    {{"role", "down_the_line"},
                     {"frame_count", 3},
                     {"timeline_ms", 0.1},
                     {"bayer_fit_demosaic_ms", 0.2},
                     {"rgb_to_yuv420_ms", 0.3},
                     {"codec_encode_ms", 0.4},
                     {"webm_mux_ms", 0.5},
                     {"finalize_ms", 0.6},
                     {"output_verification_ms", 0.7},
                     {"total_ms", 3.0}},
                    {{"role", "face_on"},
                     {"frame_count", 3},
                     {"timeline_ms", 0.2},
                     {"bayer_fit_demosaic_ms", 0.3},
                     {"rgb_to_yuv420_ms", 0.4},
                     {"codec_encode_ms", 0.5},
                     {"webm_mux_ms", 0.6},
                     {"finalize_ms", 0.7},
                     {"output_verification_ms", 0.8},
                     {"total_ms", 3.7}},
                })},
  };
}

Json Manifest(const std::filesystem::path &directory) {
  return {
      {"schema_version", 1},
      {"session_id", "session-17"},
      {"created_at_utc", "2026-08-09T09:00:00Z"},
      {"trigger",
       {{"source", "audio"},
        {"host_monotonic_time_ns", "123456789"},
        {"confirmation_host_monotonic_time_ns", "125456789"},
        {"sample_rate_hz", 32'000},
        {"peak_amplitude", 0.17},
        {"noise_floor", 0.012},
        {"threshold", 0.05}}},
      {"mapped_nearest_frame_skew_us", 100},
      {"pipeline_profile", PipelineProfile()},
      {"views", Json::array({View("down_the_line", "DOWN123", "down_the_line.webm",
                                  std::filesystem::file_size(directory / "down_the_line.webm")),
                             View("face_on", "FACE456", "face_on.webm",
                                  std::filesystem::file_size(directory / "face_on.webm"))})},
  };
}

void AddSyntheticSwingEvidence(Json *manifest) {
  constexpr std::uint8_t kBrightness = 8;
  (*manifest)["hil_evidence"] = {
      {"kind", "synthetic_swing"},
      {"selected_brightness", kBrightness},
      {"timeline",
       {{"step_duration_us", 20'000},
        {"pre_impact_step_count", 60},
        {"white_impact_duration_us", 20'000},
        {"post_impact_step_count", 25}}},
      {"tone", {{"duration_us", 10'000}, {"frequency_hz", 2'000}}},
      {"optical_white_impact_frame_index", {{"down_the_line", 0}, {"face_on", 2}}},
      {"audio_trigger_estimate_offset_us", {{"down_the_line", 4'405}, {"face_on", -4'405}}},
      {"camera_schedule_alignment",
       {{"down_the_line", {{"mapped_time_correction_us", -3'500}, {"uncertainty_us", 300}}},
        {"face_on", {{"mapped_time_correction_us", 3'250}, {"uncertainty_us", 275}}}}},
      {"optical_white_impact",
       {{"down_the_line",
         {{"passed", true},
          {"stable_frame_count", 4},
          {"matching_frame_count", 3},
          {"matching_fraction", 0.75},
          {"mean_signal_delta", 12.0},
          {"mean_expected_color_distance", 42.0},
          {"maximum_saturated_fraction", 0.08},
          {"maximum_bloom_fraction", 0.08},
          {"exposure_us", 500.0},
          {"gain_db", 24.0}}},
        {"face_on",
         {{"passed", true},
          {"stable_frame_count", 2},
          {"matching_frame_count", 2},
          {"matching_fraction", 1.0},
          {"mean_signal_delta", 12.0},
          {"mean_expected_color_distance", 99.0},
          {"maximum_saturated_fraction", 0.08},
          {"maximum_bloom_fraction", 0.08},
          {"exposure_us", 500.0},
          {"gain_db", 24.0}}}}},
  };
}

SessionArtifactExpectations SyntheticSwingExpectations() {
  SessionArtifactExpectations expectations = Expectations();
  expectations.require_synthetic_swing_evidence = true;
  expectations.expected_synthetic_swing_brightness = 8U;
  return expectations;
}

void WriteWebm(const std::filesystem::path &path, std::string_view role) {
  constexpr std::array<std::int64_t, 3> kTimesUs = {-4'405, 0, 4'405};
  std::array<std::array<std::byte, 16>, 3> payloads{};
  std::array<ClipFrameInput, 3> frames;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    for (std::size_t pixel = 0; pixel < payloads[index].size(); ++pixel) {
      payloads[index][pixel] = static_cast<std::byte>((index * 31U + pixel * 17U) & 0xffU);
    }
    frames[index] = {
        .frame =
            FrameView{
                .metadata =
                    FrameMetadata{
                        .frame_id = 41U + index,
                        .device_timestamp = 9'000U + index * 4'405U,
                        .host_received_at = {},
                        .width = 4,
                        .height = 4,
                        .complete = true,
                    },
                .payload = payloads[index],
            },
        .time_from_impact = std::chrono::microseconds(kTimesUs[index]),
    };
  }
  const std::unique_ptr<ClipMediaEncoder> encoder = MakeSoftwareVp8WebmEncoder();
  const auto encoded = encoder->Encode(
      CameraClipInput{
          .role = role, .camera_serial = "fixture", .pixel_format = "BayerRG8", .frames = frames},
      path);
  assert(encoded.frame_count == frames.size());
}

std::filesystem::path TemporarySession() {
  const char *temporary = std::getenv("TEST_TMPDIR");
  assert(temporary != nullptr);
  const std::filesystem::path directory =
      std::filesystem::path(temporary) / "session-artifact-validator";
  std::filesystem::create_directories(directory);
  WriteWebm(directory / "down_the_line.webm", "down_the_line");
  WriteWebm(directory / "face_on.webm", "face_on");
  return directory;
}

void AcceptsCompleteAudioTriggeredDualViewSession() {
  const std::filesystem::path directory = TemporarySession();
  const auto validation =
      ValidateSessionArtifacts(Manifest(directory).dump(), directory, Expectations());
  assert(validation.passed);
  assert(validation.error.empty());
  assert(validation.views[0].frame_count == 3U);
  assert(validation.views[0].impact_frame_index == 1U);
  assert(validation.views[1].camera_serial == "FACE456");
  assert(validation.pipeline_profile.schema_version == 2U);
  assert(validation.pipeline_profile.capture.trigger_estimate_to_confirmation_ms == 2.0);
  assert(validation.pipeline_profile.capture.freeze_schedule_lateness_ms == -0.25);
  assert(validation.pipeline_profile.session.prepublication_analysis_ms == 0.5);
  assert(validation.pipeline_profile.session.publisher_planning_ms == 0.3);
  assert(validation.pipeline_profile.session.media_encoding_wall_ms == 6.8);
  assert(validation.pipeline_profile.session.profile_snapshot_host_monotonic_ns == 225'456'789U);
  assert(validation.pipeline_profile.views[0].role == "down_the_line");
  assert(validation.pipeline_profile.views[0].frame_count == 3U);
  assert(validation.pipeline_profile.views[0].codec_encode_ms == 0.4);
  assert(validation.pipeline_profile.views[1].role == "face_on");
  assert(validation.pipeline_profile.views[1].total_ms == 3.7);
  assert(!validation.synthetic_swing_evidence.has_value());
}

void AcceptsExactSyntheticSwingEvidence() {
  const std::filesystem::path directory = TemporarySession();
  Json manifest = Manifest(directory);
  AddSyntheticSwingEvidence(&manifest);
  const auto validation =
      ValidateSessionArtifacts(manifest.dump(), directory, SyntheticSwingExpectations());
  assert(validation.passed);
  assert(validation.synthetic_swing_evidence.has_value());
  const auto &evidence = *validation.synthetic_swing_evidence;
  assert(evidence.selected_brightness == 8U);
  assert(evidence.views[0].role == "down_the_line");
  assert(evidence.views[0].optical_white_impact_frame_index == 0U);
  assert(evidence.views[0].audio_trigger_estimate_offset_microseconds == 4'405);
  assert(evidence.views[0].mapped_time_correction_microseconds == -3'500);
  assert(evidence.views[0].camera_schedule_uncertainty_microseconds == 300U);
  assert(evidence.views[1].role == "face_on");
  assert(evidence.views[1].audio_trigger_estimate_offset_microseconds == -4'405);
  assert(evidence.views[1].mapped_time_correction_microseconds == 3'250);
  assert(evidence.views[0].optical_white_passed);
  assert(evidence.views[0].stable_frame_count == 4U);
  assert(evidence.views[0].matching_frame_count == 3U);
  assert(evidence.views[0].matching_fraction == 0.75);
  assert(evidence.views[0].mean_signal_delta == 12.0);
  assert(evidence.views[0].mean_expected_color_distance == 42.0);
  assert(evidence.views[0].maximum_saturated_fraction == 0.08);
  assert(evidence.views[0].maximum_bloom_fraction == 0.08);
  assert(evidence.views[0].exposure_us == 500.0);
  assert(evidence.views[0].gain_db == 24.0);
  assert(evidence.views[1].stable_frame_count == 2U);
  assert(evidence.views[1].matching_frame_count == 2U);
}

void RejectsManualTriggerAndFrameGap() {
  const std::filesystem::path directory = TemporarySession();
  Json manual = Manifest(directory);
  manual["trigger"]["source"] = "manual";
  auto validation = ValidateSessionArtifacts(manual.dump(), directory, Expectations());
  assert(!validation.passed);
  assert(validation.error.find("microphone audio") != std::string::npos);

  Json gap = Manifest(directory);
  gap["views"][0]["frames"][1]["frame_id"] = "44";
  validation = ValidateSessionArtifacts(gap.dump(), directory, Expectations());
  assert(!validation.passed);
  assert(validation.error.find("frame ID gap") != std::string::npos);

  Json incomplete_audio = Manifest(directory);
  incomplete_audio["trigger"]["confirmation_host_monotonic_time_ns"] = nullptr;
  validation = ValidateSessionArtifacts(incomplete_audio.dump(), directory, Expectations());
  assert(!validation.passed);

  Json below_threshold = Manifest(directory);
  below_threshold["trigger"]["peak_amplitude"] = 0.04;
  validation = ValidateSessionArtifacts(below_threshold.dump(), directory, Expectations());
  assert(!validation.passed);
  assert(validation.error.find("amplitude evidence") != std::string::npos);
}

void RejectsUnsafeOrMalformedMedia() {
  const std::filesystem::path directory = TemporarySession();
  Json traversal = Manifest(directory);
  traversal["views"][0]["media"]["path"] = "../down_the_line.webm";
  auto validation = ValidateSessionArtifacts(traversal.dump(), directory, Expectations());
  assert(!validation.passed);
  assert(validation.error.find("safe relative filename") != std::string::npos);

  Json wrong_size = Manifest(directory);
  wrong_size["views"][0]["media"]["encoded_bytes"] = 7;
  validation = ValidateSessionArtifacts(wrong_size.dump(), directory, Expectations());
  assert(!validation.passed);
  assert(validation.error.find("file size") != std::string::npos);
}

void ExpectPipelineProfileFailure(Json manifest, const std::filesystem::path &directory,
                                  std::string_view expected_error) {
  const auto validation = ValidateSessionArtifacts(manifest.dump(), directory, Expectations());
  assert(!validation.passed);
  assert(validation.error.find(expected_error) != std::string::npos);
}

void RejectsMissingOrMalformedPipelineProfile() {
  const std::filesystem::path directory = TemporarySession();
  Json invalid = Manifest(directory);
  invalid.erase("pipeline_profile");
  ExpectPipelineProfileFailure(std::move(invalid), directory, "missing required pipeline profile");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["schema_version"] = 1;
  ExpectPipelineProfileFailure(std::move(invalid), directory, "profile schema");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["capture"]["unexpected"] = 1.0;
  ExpectPipelineProfileFailure(std::move(invalid), directory, "exactly the schema fields");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["capture"]["audio_stop_ms"] = -0.1;
  ExpectPipelineProfileFailure(std::move(invalid), directory, "nonnegative");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["session"]["prepublication_analysis_ms"] = -0.1;
  ExpectPipelineProfileFailure(std::move(invalid), directory, "nonnegative");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["capture"]["freeze_schedule_lateness_ms"] = -2.0;
  invalid["pipeline_profile"]["capture"]["confirmation_to_acceptance_ms"] = -1.0;
  const auto signed_capture_times =
      ValidateSessionArtifacts(invalid.dump(), directory, Expectations());
  assert(signed_capture_times.passed);
  assert(signed_capture_times.pipeline_profile.capture.freeze_schedule_lateness_ms == -2.0);
  assert(signed_capture_times.pipeline_profile.capture.confirmation_to_acceptance_ms == -1.0);
}

void RejectsInconsistentPipelineProfileTimingsAndViews() {
  const std::filesystem::path directory = TemporarySession();
  Json invalid = Manifest(directory);
  invalid["pipeline_profile"]["capture"]["trigger_estimate_to_confirmation_ms"] = 2.01;
  ExpectPipelineProfileFailure(std::move(invalid), directory, "disagrees with trigger");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["session"]["profile_snapshot_host_monotonic_ns"] = "125456788";
  ExpectPipelineProfileFailure(std::move(invalid), directory, "precedes trigger confirmation");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["session"]["profile_snapshot_after_confirmation_ms"] = 99.9;
  ExpectPipelineProfileFailure(std::move(invalid), directory, "snapshot timing is inconsistent");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["views"][1]["role"] = "down_the_line";
  ExpectPipelineProfileFailure(std::move(invalid), directory, "duplicate camera roles");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["views"][0]["frame_count"] = 2;
  ExpectPipelineProfileFailure(std::move(invalid), directory, "frame count disagrees");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["views"][0]["timeline_ms"] = 3.1;
  ExpectPipelineProfileFailure(std::move(invalid), directory, "individual stage");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["views"][0]["total_ms"] = 2.6;
  ExpectPipelineProfileFailure(std::move(invalid), directory, "sequential encoding stages");

  invalid = Manifest(directory);
  invalid["pipeline_profile"]["session"]["media_encoding_wall_ms"] = 3.6;
  ExpectPipelineProfileFailure(std::move(invalid), directory, "shorter than a parallel view");
}

void RejectsMissingOrInconsistentSyntheticSwingEvidence() {
  const std::filesystem::path directory = TemporarySession();
  const SessionArtifactExpectations expectations = SyntheticSwingExpectations();
  auto validation = ValidateSessionArtifacts(Manifest(directory).dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("missing required") != std::string::npos);

  Json invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["kind"] = "other";
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("kind") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["timeline"]["step_duration_us"] = 19'999;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("timeline") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["selected_brightness"] = 47;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("calibration candidate") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["optical_white_impact_frame_index"]["face_on"] = 3;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("out of range") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["audio_trigger_estimate_offset_us"]["down_the_line"] = -4'405;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("signed audio offset") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["optical_white_impact"]["face_on"]["passed"] = false;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("did not pass") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["optical_white_impact"]["face_on"]["stable_frame_count"] = 1;
  invalid["hil_evidence"]["optical_white_impact"]["face_on"]["matching_frame_count"] = 1;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("acceptance policy") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["optical_white_impact"]["down_the_line"]["matching_frame_count"] = 2;
  invalid["hil_evidence"]["optical_white_impact"]["down_the_line"]["matching_fraction"] = 0.5;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("acceptance policy") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["optical_white_impact"]["down_the_line"]["mean_signal_delta"] = 11.999;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("acceptance policy") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["optical_white_impact"]["down_the_line"]["maximum_saturated_fraction"] =
      0.080'001;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("acceptance policy") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["optical_white_impact"]["down_the_line"]["maximum_bloom_fraction"] =
      0.080'001;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("acceptance policy") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["optical_white_impact"]["face_on"]["exposure_us"] = 499.0;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("expected profile") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["optical_white_impact"].erase("face_on");
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("two station roles") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["audio_trigger_estimate_offset_us"].erase("face_on");
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("two station roles") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["camera_schedule_alignment"].erase("face_on");
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("two station roles") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["camera_schedule_alignment"]["down_the_line"]
         ["mapped_time_correction_us"] = 1.5;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("signed integer") != std::string::npos);

  invalid = Manifest(directory);
  AddSyntheticSwingEvidence(&invalid);
  invalid["hil_evidence"]["camera_schedule_alignment"]["face_on"]["uncertainty_us"] = -1;
  validation = ValidateSessionArtifacts(invalid.dump(), directory, expectations);
  assert(!validation.passed);
  assert(validation.error.find("unsigned integer") != std::string::npos);
}

}  // namespace

int main() {
  AcceptsCompleteAudioTriggeredDualViewSession();
  AcceptsExactSyntheticSwingEvidence();
  RejectsManualTriggerAndFrameGap();
  RejectsUnsafeOrMalformedMedia();
  RejectsMissingOrMalformedPipelineProfile();
  RejectsInconsistentPipelineProfileTimingsAndViews();
  RejectsMissingOrInconsistentSyntheticSwingEvidence();
  return 0;
}
