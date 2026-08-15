#ifndef SWING_CAPTURE_CAPTURE_HIL_SESSION_ARTIFACT_VALIDATOR_H_
#define SWING_CAPTURE_CAPTURE_HIL_SESSION_ARTIFACT_VALIDATOR_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace swing_capture::hil {

struct ExpectedSessionView {
  std::string role;
  std::string camera_serial;
  std::optional<double> expected_synthetic_swing_exposure_us = std::nullopt;
  std::optional<double> expected_synthetic_swing_gain_db = std::nullopt;
};

struct SessionArtifactExpectations {
  std::string session_id;
  std::array<ExpectedSessionView, 2> views;
  std::size_t minimum_frame_count = 2;
  double minimum_nominal_fps = 200.0;
  double maximum_nominal_fps = 250.0;
  std::uint32_t maximum_encoded_width = 640;
  std::uint32_t maximum_encoded_height = 480;
  bool require_source_resolution_encoding = false;
  std::string expected_codec = "vp8";
  // Ordinary audio-triggered sessions may omit HIL evidence. Setting either
  // requirement makes the current synthetic-swing evidence mandatory.
  bool require_synthetic_swing_evidence = false;
  std::optional<std::uint32_t> expected_synthetic_swing_brightness = std::nullopt;
};

struct ValidatedSessionView {
  std::string role;
  std::string camera_serial;
  std::size_t frame_count = 0;
  std::size_t impact_frame_index = 0;
  double nominal_fps = 0.0;
  std::filesystem::path media_path;
  std::uintmax_t encoded_bytes = 0;
};

struct ValidatedSyntheticSwingView {
  std::string role;
  std::size_t optical_white_impact_frame_index = 0;
  // Positive means the audio estimate is later than the white optical frame.
  std::int64_t audio_trigger_estimate_offset_microseconds = 0;
  std::int64_t mapped_time_correction_microseconds = 0;
  std::uint64_t camera_schedule_uncertainty_microseconds = 0;
  bool optical_white_passed = false;
  std::size_t stable_frame_count = 0;
  std::size_t matching_frame_count = 0;
  double matching_fraction = 0.0;
  double mean_signal_delta = 0.0;
  double mean_expected_color_distance = 0.0;
  double maximum_saturated_fraction = 0.0;
  double maximum_bloom_fraction = 0.0;
  double exposure_us = 0.0;
  double gain_db = 0.0;
};

struct ValidatedSyntheticSwingEvidence {
  std::uint32_t selected_brightness = 0;
  std::array<ValidatedSyntheticSwingView, 2> views;
};

struct ValidatedCapturePipelineProfile {
  double trigger_estimate_to_confirmation_ms = 0.0;
  // May be negative when provisional ALSA acceptance precedes the later
  // host-side confirmation timestamp.
  double confirmation_to_acceptance_ms = 0.0;
  double acceptance_to_freeze_start_ms = 0.0;
  double freeze_schedule_lateness_ms = 0.0;
  double freeze_and_rotate_ms = 0.0;
  double audio_stop_ms = 0.0;
};

struct ValidatedSessionPipelineProfile {
  double prepublication_analysis_ms = 0.0;
  double publisher_planning_ms = 0.0;
  double impact_preview_render_ms = 0.0;
  double impact_preview_ready_after_confirmation_ms = 0.0;
  double validation_and_timeline_ms = 0.0;
  double output_setup_ms = 0.0;
  double media_encoding_wall_ms = 0.0;
  double frame_metadata_ms = 0.0;
  double profile_snapshot_after_confirmation_ms = 0.0;
  std::uint64_t profile_snapshot_host_monotonic_ns = 0;
};

struct ValidatedViewPipelineProfile {
  std::string role;
  std::size_t frame_count = 0;
  double timeline_ms = 0.0;
  double bayer_fit_demosaic_ms = 0.0;
  double rgb_to_yuv420_ms = 0.0;
  double codec_encode_ms = 0.0;
  double webm_mux_ms = 0.0;
  double finalize_ms = 0.0;
  double output_verification_ms = 0.0;
  double total_ms = 0.0;
};

struct ValidatedPipelineProfile {
  std::uint32_t schema_version = 0;
  ValidatedCapturePipelineProfile capture;
  ValidatedSessionPipelineProfile session;
  std::array<ValidatedViewPipelineProfile, 2> views;
};

struct SessionArtifactValidation {
  bool passed = false;
  std::string error;
  std::array<ValidatedSessionView, 2> views;
  ValidatedPipelineProfile pipeline_profile;
  std::optional<ValidatedSyntheticSwingEvidence> synthetic_swing_evidence;
};

// Independently validates a published manifest and its relative media files.
// This deliberately consumes serialized JSON rather than encoder input types,
// so the HIL catches publication/schema defects in addition to capture errors.
[[nodiscard]] SessionArtifactValidation ValidateSessionArtifacts(
    std::string_view manifest_json, const std::filesystem::path &session_directory,
    const SessionArtifactExpectations &expectations) noexcept;

}  // namespace swing_capture::hil

#endif  // SWING_CAPTURE_CAPTURE_HIL_SESSION_ARTIFACT_VALIDATOR_H_
