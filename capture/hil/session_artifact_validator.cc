#include "capture/hil/session_artifact_validator.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "capture/encoding/clip_session.h"
#include "capture/optical/white_impact_acceptance.h"
#include "embedded/prop_maker/swing_sequence.h"

namespace swing_capture::hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::invalid_argument(std::string(message));
  }
}

std::uint64_t ParseUnsignedString(const Json &value, std::string_view field) {
  Require(value.is_string(), std::string(field) + " must be a decimal string");
  const std::string_view text(value.get_ref<const std::string &>());
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(text.begin(), text.end(), parsed);
  Require(result.ec == std::errc() && result.ptr == text.end(),
          std::string(field) + " must be an unsigned decimal string");
  return parsed;
}

std::uint64_t ParseUnsignedNumber(const Json &value, std::string_view field) {
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  Require(value.is_number_integer(), std::string(field) + " must be an unsigned integer");
  const std::int64_t parsed = value.get<std::int64_t>();
  Require(parsed >= 0, std::string(field) + " must be an unsigned integer");
  return static_cast<std::uint64_t>(parsed);
}

std::int64_t ParseSignedNumber(const Json &value, std::string_view field) {
  if (value.is_number_unsigned()) {
    const std::uint64_t parsed = value.get<std::uint64_t>();
    Require(parsed <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
            std::string(field) + " exceeds the signed integer range");
    return static_cast<std::int64_t>(parsed);
  }
  Require(value.is_number_integer(), std::string(field) + " must be a signed integer");
  return value.get<std::int64_t>();
}

double ParseFiniteNumber(const Json &value, std::string_view field) {
  Require(value.is_number(), std::string(field) + " must be numeric");
  const double parsed = value.get<double>();
  Require(std::isfinite(parsed), std::string(field) + " must be finite");
  return parsed;
}

double ParseNonnegativeMilliseconds(const Json &value, std::string_view field) {
  const double parsed = ParseFiniteNumber(value, field);
  Require(parsed >= 0.0, std::string(field) + " must be nonnegative");
  return parsed;
}

void RequireExactFields(const Json &object, std::initializer_list<std::string_view> fields,
                        std::string_view description) {
  Require(object.is_object() && object.size() == fields.size(),
          std::string(description) + " must contain exactly the schema fields");
  for (const std::string_view field : fields) {
    Require(object.contains(std::string(field)),
            std::string(description) + " is missing " + std::string(field));
  }
}

constexpr double kProfileRoundingToleranceMilliseconds = 0.001;

void RequireMillisecondsMatch(double actual, double expected, std::string_view message) {
  Require(std::abs(actual - expected) <= kProfileRoundingToleranceMilliseconds, message);
}

double ParseFraction(const Json &value, std::string_view field) {
  const double parsed = ParseFiniteNumber(value, field);
  Require(parsed >= 0.0 && parsed <= 1.0, std::string(field) + " must be in [0, 1]");
  return parsed;
}

std::int64_t AbsoluteDistance(std::int64_t value) {
  if (value == std::numeric_limits<std::int64_t>::min()) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return value < 0 ? -value : value;
}

const ExpectedSessionView &ExpectedView(const SessionArtifactExpectations &expectations,
                                        std::string_view role) {
  const auto *const found = std::ranges::find(expectations.views, role, &ExpectedSessionView::role);
  Require(found != expectations.views.end(), "manifest contains an unexpected camera role");
  return *found;
}

ValidatedSessionView ValidateView(const Json &view, const std::filesystem::path &session_directory,
                                  const SessionArtifactExpectations &expectations) {
  Require(view.is_object(), "manifest view must be an object");
  const std::string role = view.at("role").get<std::string>();
  const ExpectedSessionView &expected = ExpectedView(expectations, role);
  const std::string serial = view.at("camera_serial").get<std::string>();
  Require(serial == expected.camera_serial, "manifest camera serial does not match station role");

  const Json &source = view.at("source");
  Require(source.at("pixel_format") == "BayerRG8", "manifest source must be BayerRG8");
  const std::uint32_t source_width = source.at("width").get<std::uint32_t>();
  const std::uint32_t source_height = source.at("height").get<std::uint32_t>();
  Require(source_width >= 2U && source_height >= 2U, "manifest source dimensions are invalid");
  const Json &encoded = view.at("encoded");
  const std::uint32_t encoded_width = encoded.at("width").get<std::uint32_t>();
  const std::uint32_t encoded_height = encoded.at("height").get<std::uint32_t>();
  Require(encoded_width > 0U && encoded_height > 0U && encoded_width <= source_width &&
              encoded_height <= source_height &&
              encoded_width <= expectations.maximum_encoded_width &&
              encoded_height <= expectations.maximum_encoded_height,
          "manifest encoded dimensions are invalid or exceed the HIL bound");

  const std::size_t frame_count = view.at("frame_count").get<std::size_t>();
  const Json &frames = view.at("frames");
  Require(frame_count >= expectations.minimum_frame_count && frames.is_array() &&
              frames.size() == frame_count,
          "manifest frame count is below the HIL minimum or disagrees with metadata");
  const std::size_t impact_frame_index = view.at("impact_frame_index").get<std::size_t>();
  Require(impact_frame_index < frame_count, "manifest impact frame index is out of range");

  std::uint64_t previous_frame_id = 0;
  std::uint64_t previous_device_timestamp = 0;
  std::int64_t previous_impact_time = 0;
  std::uint64_t previous_media_time = 0;
  std::int64_t first_impact_time = 0;
  std::int64_t smallest_impact_distance = std::numeric_limits<std::int64_t>::max();
  std::size_t nearest_impact_index = 0;
  for (std::size_t index = 0; index < frame_count; ++index) {
    const Json &frame = frames.at(index);
    Require(frame.at("frame_index").get<std::size_t>() == index,
            "manifest frame indexes are not contiguous");
    const std::uint64_t frame_id = ParseUnsignedString(frame.at("frame_id"), "frame_id");
    const std::uint64_t device_timestamp =
        ParseUnsignedString(frame.at("device_timestamp"), "device_timestamp");
    const std::int64_t impact_time = frame.at("time_from_impact_us").get<std::int64_t>();
    const std::uint64_t media_time = frame.at("media_time_us").get<std::uint64_t>();
    constexpr std::int64_t kMaximumHilClipOffsetMicroseconds = 60'000'000;
    Require(AbsoluteDistance(impact_time) <= kMaximumHilClipOffsetMicroseconds,
            "manifest impact-relative time exceeds the HIL bound");
    if (index == 0U) {
      first_impact_time = impact_time;
      Require(media_time == 0U, "first manifest media time must be zero");
    } else {
      Require(frame_id == previous_frame_id + 1U, "manifest contains a frame ID gap");
      Require(device_timestamp > previous_device_timestamp,
              "manifest device timestamps are not strictly increasing");
      Require(impact_time > previous_impact_time,
              "manifest mapped impact times are not strictly increasing");
      Require(media_time > previous_media_time, "manifest media times are not strictly increasing");
    }
    const std::int64_t expected_media_time = impact_time - first_impact_time;
    Require(media_time <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
                std::abs(static_cast<std::int64_t>(media_time) - expected_media_time) <= 1,
            "manifest media timeline disagrees with impact-relative time");
    const std::int64_t distance = AbsoluteDistance(impact_time);
    if (distance < smallest_impact_distance) {
      smallest_impact_distance = distance;
      nearest_impact_index = index;
    }
    previous_frame_id = frame_id;
    previous_device_timestamp = device_timestamp;
    previous_impact_time = impact_time;
    previous_media_time = media_time;
  }
  Require(frames.front().at("time_from_impact_us").get<std::int64_t>() <= 0 &&
              frames.back().at("time_from_impact_us").get<std::int64_t>() >= 0,
          "manifest clip does not bracket the audio impact");
  Require(impact_frame_index == nearest_impact_index,
          "manifest impact frame is not nearest to the audio impact");

  const double nominal_fps = view.at("nominal_fps").get<double>();
  Require(std::isfinite(nominal_fps) && nominal_fps >= expectations.minimum_nominal_fps &&
              nominal_fps <= expectations.maximum_nominal_fps,
          "manifest nominal frame rate is outside the HIL range");

  const Json &media = view.at("media");
  Require(media.at("mime_type") == "video/webm" &&
              media.at("codec") == expectations.expected_codec &&
              media.at("all_frames_keyframes").get<bool>(),
          "manifest media must use the expected all-intra WebM codec");
  const std::filesystem::path relative_path(media.at("path").get<std::string>());
  Require(!relative_path.empty() && relative_path == relative_path.filename(),
          "manifest media path must be one safe relative filename");
  const std::filesystem::path media_path = session_directory / relative_path;
  const std::uintmax_t encoded_bytes = media.at("encoded_bytes").get<std::uintmax_t>();
  Require(encoded_bytes > 0U && std::filesystem::is_regular_file(media_path) &&
              std::filesystem::file_size(media_path) == encoded_bytes,
          "published media file size disagrees with the manifest");
  const encoding::WebmInspection inspection = encoding::InspectWebm(media_path);
  Require(inspection.codec == expectations.expected_codec && inspection.width == encoded_width &&
              inspection.height == encoded_height && inspection.frame_count == frame_count &&
              inspection.keyframe_count == frame_count &&
              inspection.first_frame_time == std::chrono::nanoseconds::zero() &&
              inspection.last_frame_time > inspection.first_frame_time,
          "parsed WebM structure disagrees with the published manifest");

  return {
      .role = role,
      .camera_serial = serial,
      .frame_count = frame_count,
      .impact_frame_index = impact_frame_index,
      .nominal_fps = nominal_fps,
      .media_path = relative_path,
      .encoded_bytes = encoded_bytes,
  };
}

const Json &ManifestViewByRole(const Json &views, std::string_view role) {
  for (const Json &view : views) {
    if (view.at("role") == role) {
      return view;
    }
  }
  throw std::invalid_argument("synthetic swing evidence references an absent camera role");
}

struct SyntheticSwingManifestContext {
  const Json *manifest;
  const Json *views;
};

void RequireSyntheticSwingRoleMap(const Json &object,
                                  const SessionArtifactExpectations &expectations,
                                  std::string_view field) {
  Require(object.is_object() && object.size() == expectations.views.size(),
          std::string(field) + " must contain exactly the two station roles");
  for (const ExpectedSessionView &view : expectations.views) {
    Require(object.contains(view.role),
            std::string(field) + " is missing camera role " + view.role);
  }
}

std::uint32_t ValidateSyntheticSwingContract(const Json &hil,
                                             const SessionArtifactExpectations &expectations) {
  Require(hil.is_object() && hil.at("kind") == "synthetic_swing",
          "manifest HIL evidence kind must be synthetic_swing");
  const Json &timeline = hil.at("timeline");
  Require(ParseUnsignedNumber(timeline.at("step_duration_us"), "hil timeline step duration") ==
                  SWING_HIL_SWING_STEP_US &&
              ParseUnsignedNumber(timeline.at("pre_impact_step_count"),
                                  "hil pre-impact step count") == SWING_HIL_SWING_PRE_STEPS &&
              ParseUnsignedNumber(timeline.at("white_impact_duration_us"),
                                  "hil white-impact duration") == SWING_HIL_SWING_IMPACT_WHITE_US &&
              ParseUnsignedNumber(timeline.at("post_impact_step_count"),
                                  "hil post-impact step count") == SWING_HIL_SWING_POST_STEPS,
          "manifest HIL timeline does not match the current synthetic swing contract");
  const Json &tone = hil.at("tone");
  Require(ParseUnsignedNumber(tone.at("duration_us"), "hil tone duration") ==
                  SWING_HIL_SWING_TONE_DURATION_US &&
              ParseUnsignedNumber(tone.at("frequency_hz"), "hil tone frequency") ==
                  SWING_HIL_SWING_TONE_FREQUENCY_HZ,
          "manifest HIL tone does not match the current synthetic swing contract");
  const std::uint64_t selected =
      ParseUnsignedNumber(hil.at("selected_brightness"), "hil selected brightness");
  Require(selected <= std::numeric_limits<std::uint8_t>::max() &&
              swing_hil_swing_brightness_is_candidate(static_cast<std::uint32_t>(selected)),
          "manifest HIL selected brightness is not a current calibration candidate");
  if (expectations.expected_synthetic_swing_brightness.has_value()) {
    Require(selected == *expectations.expected_synthetic_swing_brightness,
            "manifest HIL selected brightness disagrees with the requested swing");
  }
  return static_cast<std::uint32_t>(selected);
}

ValidatedSyntheticSwingView ValidateSyntheticSwingView(const SyntheticSwingManifestContext &context,
                                                       const ExpectedSessionView &expected) {
  const Json &hil = context.manifest->at("hil_evidence");
  const Json &white_indices = hil.at("optical_white_impact_frame_index");
  const Json &audio_offsets = hil.at("audio_trigger_estimate_offset_us");
  const Json &schedule_alignments = hil.at("camera_schedule_alignment");
  const Json &optical_impacts = hil.at("optical_white_impact");
  const Json &manifest_view = ManifestViewByRole(*context.views, expected.role);
  const std::uint64_t white_index_value =
      ParseUnsignedNumber(white_indices.at(expected.role), "hil optical white-impact frame index");
  const std::size_t frame_count = manifest_view.at("frame_count").get<std::size_t>();
  Require(white_index_value < frame_count,
          "manifest HIL optical white-impact frame index is out of range");
  const auto white_index = static_cast<std::size_t>(white_index_value);
  const std::int64_t audio_offset =
      ParseSignedNumber(audio_offsets.at(expected.role), "hil audio trigger estimate offset");
  const std::int64_t optical_time =
      manifest_view.at("frames").at(white_index).at("time_from_impact_us").get<std::int64_t>();
  Require(audio_offset == -optical_time,
          "manifest HIL signed audio offset disagrees with the optical white frame");
  const Json &schedule_alignment = schedule_alignments.at(expected.role);
  Require(schedule_alignment.is_object(),
          "manifest HIL camera schedule alignment must be an object");
  const std::int64_t mapped_time_correction = ParseSignedNumber(
      schedule_alignment.at("mapped_time_correction_us"), "hil mapped-time correction");
  const std::uint64_t schedule_uncertainty = ParseUnsignedNumber(
      schedule_alignment.at("uncertainty_us"), "hil camera schedule uncertainty");

  const Json &impact = optical_impacts.at(expected.role);
  Require(impact.is_object() && impact.at("passed").is_boolean(),
          "manifest HIL white-impact evidence is invalid");
  const std::uint64_t stable =
      ParseUnsignedNumber(impact.at("stable_frame_count"), "hil stable white frame count");
  const std::uint64_t matching =
      ParseUnsignedNumber(impact.at("matching_frame_count"), "hil matching white frame count");
  Require(stable <= std::numeric_limits<std::size_t>::max() &&
              matching <= std::numeric_limits<std::size_t>::max() && matching <= stable,
          "manifest HIL white-impact frame counts are invalid");
  const double matching_fraction =
      ParseFraction(impact.at("matching_fraction"), "hil white matching fraction");
  const double expected_fraction =
      stable == 0U ? 0.0 : static_cast<double>(matching) / static_cast<double>(stable);
  Require(std::abs(matching_fraction - expected_fraction) <= 1e-12,
          "manifest HIL white matching fraction disagrees with its counts");
  const double mean_signal =
      ParseFiniteNumber(impact.at("mean_signal_delta"), "hil white mean signal delta");
  const double mean_color_distance = ParseFiniteNumber(impact.at("mean_expected_color_distance"),
                                                       "hil white mean expected-color distance");
  Require(mean_signal > 0.0 && mean_color_distance >= 0.0,
          "manifest HIL white signal/color evidence is invalid");
  const double maximum_saturation = ParseFraction(impact.at("maximum_saturated_fraction"),
                                                  "hil white maximum saturated fraction");
  const double maximum_bloom =
      ParseFraction(impact.at("maximum_bloom_fraction"), "hil white maximum bloom fraction");
  const bool passed = impact.at("passed").get<bool>();
  Require(passed, "manifest HIL optical white impact did not pass for both roles");
  Require(optical::kDefaultWhiteImpactAcceptancePolicy.Accepts(static_cast<std::size_t>(stable),
                                                               matching_fraction, mean_signal,
                                                               maximum_saturation, maximum_bloom),
          "manifest HIL optical white impact does not satisfy the acceptance policy");
  const double exposure_us = ParseFiniteNumber(impact.at("exposure_us"), "hil camera exposure");
  const double gain_db = ParseFiniteNumber(impact.at("gain_db"), "hil camera gain");
  Require(exposure_us > 0.0 && gain_db >= 0.0, "manifest HIL camera capture profile is invalid");
  if (expected.expected_synthetic_swing_exposure_us.has_value()) {
    Require(exposure_us == *expected.expected_synthetic_swing_exposure_us,
            "manifest HIL camera exposure disagrees with the expected profile");
  }
  if (expected.expected_synthetic_swing_gain_db.has_value()) {
    Require(gain_db == *expected.expected_synthetic_swing_gain_db,
            "manifest HIL camera gain disagrees with the expected profile");
  }
  return {
      .role = expected.role,
      .optical_white_impact_frame_index = white_index,
      .audio_trigger_estimate_offset_microseconds = audio_offset,
      .mapped_time_correction_microseconds = mapped_time_correction,
      .camera_schedule_uncertainty_microseconds = schedule_uncertainty,
      .optical_white_passed = passed,
      .stable_frame_count = static_cast<std::size_t>(stable),
      .matching_frame_count = static_cast<std::size_t>(matching),
      .matching_fraction = matching_fraction,
      .mean_signal_delta = mean_signal,
      .mean_expected_color_distance = mean_color_distance,
      .maximum_saturated_fraction = maximum_saturation,
      .maximum_bloom_fraction = maximum_bloom,
      .exposure_us = exposure_us,
      .gain_db = gain_db,
  };
}

std::optional<ValidatedSyntheticSwingEvidence> ValidateSyntheticSwingEvidence(
    const SyntheticSwingManifestContext &context, const SessionArtifactExpectations &expectations) {
  const auto evidence = context.manifest->find("hil_evidence");
  if (evidence == context.manifest->end()) {
    Require(!expectations.require_synthetic_swing_evidence &&
                !expectations.expected_synthetic_swing_brightness.has_value(),
            "manifest is missing required synthetic swing HIL evidence");
    return std::nullopt;
  }
  const std::uint32_t selected = ValidateSyntheticSwingContract(*evidence, expectations);
  RequireSyntheticSwingRoleMap(evidence->at("optical_white_impact_frame_index"), expectations,
                               "hil optical white-impact frame indexes");
  RequireSyntheticSwingRoleMap(evidence->at("audio_trigger_estimate_offset_us"), expectations,
                               "hil audio trigger estimate offsets");
  RequireSyntheticSwingRoleMap(evidence->at("camera_schedule_alignment"), expectations,
                               "hil camera schedule alignments");
  RequireSyntheticSwingRoleMap(evidence->at("optical_white_impact"), expectations,
                               "hil optical white-impact evidence");
  return ValidatedSyntheticSwingEvidence{
      .selected_brightness = selected,
      .views =
          std::array{
              ValidateSyntheticSwingView(context, expectations.views[0]),
              ValidateSyntheticSwingView(context, expectations.views[1]),
          },
  };
}

ValidatedCapturePipelineProfile ValidateCapturePipelineProfile(const Json &capture,
                                                               std::uint64_t strike_time,
                                                               std::uint64_t confirmation_time) {
  RequireExactFields(capture,
                     {"trigger_estimate_to_confirmation_ms", "confirmation_to_acceptance_ms",
                      "acceptance_to_freeze_start_ms", "freeze_schedule_lateness_ms",
                      "freeze_and_rotate_ms", "audio_stop_ms"},
                     "pipeline capture profile");
  ValidatedCapturePipelineProfile result = {
      .trigger_estimate_to_confirmation_ms =
          ParseNonnegativeMilliseconds(capture.at("trigger_estimate_to_confirmation_ms"),
                                       "pipeline capture trigger-estimate-to-confirmation time"),
      .confirmation_to_acceptance_ms =
          ParseFiniteNumber(capture.at("confirmation_to_acceptance_ms"),
                            "pipeline capture confirmation-to-acceptance time"),
      .acceptance_to_freeze_start_ms =
          ParseNonnegativeMilliseconds(capture.at("acceptance_to_freeze_start_ms"),
                                       "pipeline capture acceptance-to-freeze-start time"),
      .freeze_schedule_lateness_ms = ParseFiniteNumber(capture.at("freeze_schedule_lateness_ms"),
                                                       "pipeline capture freeze-schedule lateness"),
      .freeze_and_rotate_ms = ParseNonnegativeMilliseconds(
          capture.at("freeze_and_rotate_ms"), "pipeline capture freeze-and-rotate time"),
      .audio_stop_ms = ParseNonnegativeMilliseconds(capture.at("audio_stop_ms"),
                                                    "pipeline capture audio-stop time"),
  };
  const double trigger_interval_ms =
      static_cast<double>(confirmation_time - strike_time) / 1'000'000.0;
  RequireMillisecondsMatch(result.trigger_estimate_to_confirmation_ms, trigger_interval_ms,
                           "pipeline capture trigger confirmation timing disagrees with trigger");
  return result;
}

ValidatedSessionPipelineProfile ValidateSessionPipelineProfile(const Json &session,
                                                               std::uint64_t confirmation_time) {
  RequireExactFields(
      session,
      {"prepublication_analysis_ms", "publisher_planning_ms", "validation_and_timeline_ms",
       "output_setup_ms", "media_encoding_wall_ms", "frame_metadata_ms",
       "profile_snapshot_after_confirmation_ms", "profile_snapshot_host_monotonic_ns"},
      "pipeline session profile");
  ValidatedSessionPipelineProfile result = {
      .prepublication_analysis_ms =
          ParseNonnegativeMilliseconds(session.at("prepublication_analysis_ms"),
                                       "pipeline session prepublication-analysis time"),
      .publisher_planning_ms = ParseNonnegativeMilliseconds(
          session.at("publisher_planning_ms"), "pipeline session publisher-planning time"),
      .validation_and_timeline_ms = ParseNonnegativeMilliseconds(
          session.at("validation_and_timeline_ms"), "pipeline session validation/timeline time"),
      .output_setup_ms = ParseNonnegativeMilliseconds(session.at("output_setup_ms"),
                                                      "pipeline session output-setup time"),
      .media_encoding_wall_ms = ParseNonnegativeMilliseconds(
          session.at("media_encoding_wall_ms"), "pipeline session media-encoding wall time"),
      .frame_metadata_ms = ParseNonnegativeMilliseconds(session.at("frame_metadata_ms"),
                                                        "pipeline session frame-metadata time"),
      .profile_snapshot_after_confirmation_ms =
          ParseNonnegativeMilliseconds(session.at("profile_snapshot_after_confirmation_ms"),
                                       "pipeline session profile-snapshot time"),
      .profile_snapshot_host_monotonic_ns =
          ParseUnsignedString(session.at("profile_snapshot_host_monotonic_ns"),
                              "pipeline session profile_snapshot_host_monotonic_ns"),
  };
  Require(result.profile_snapshot_host_monotonic_ns >= confirmation_time,
          "pipeline profile snapshot precedes trigger confirmation");
  const double snapshot_after_confirmation_ms =
      static_cast<double>(result.profile_snapshot_host_monotonic_ns - confirmation_time) /
      1'000'000.0;
  RequireMillisecondsMatch(result.profile_snapshot_after_confirmation_ms,
                           snapshot_after_confirmation_ms,
                           "pipeline profile snapshot timing is inconsistent");
  return result;
}

ValidatedViewPipelineProfile ValidateViewPipelineProfile(
    const Json &profile, const std::array<ValidatedSessionView, 2> &manifest_views) {
  RequireExactFields(
      profile,
      {"role", "frame_count", "timeline_ms", "bayer_fit_demosaic_ms", "rgb_to_yuv420_ms",
       "codec_encode_ms", "webm_mux_ms", "finalize_ms", "output_verification_ms", "total_ms"},
      "pipeline view profile");
  const std::string role = profile.at("role").get<std::string>();
  const auto *const manifest_view =
      std::ranges::find(manifest_views, role, &ValidatedSessionView::role);
  Require(manifest_view != manifest_views.end(),
          "pipeline profile contains an unexpected camera role");
  const std::uint64_t frame_count =
      ParseUnsignedNumber(profile.at("frame_count"), "pipeline view frame count");
  Require(frame_count <= std::numeric_limits<std::size_t>::max() &&
              static_cast<std::size_t>(frame_count) == manifest_view->frame_count,
          "pipeline view frame count disagrees with the manifest");

  ValidatedViewPipelineProfile result = {
      .role = role,
      .frame_count = static_cast<std::size_t>(frame_count),
      .timeline_ms =
          ParseNonnegativeMilliseconds(profile.at("timeline_ms"), "pipeline view timeline time"),
      .bayer_fit_demosaic_ms = ParseNonnegativeMilliseconds(
          profile.at("bayer_fit_demosaic_ms"), "pipeline view Bayer-fit/demosaic time"),
      .rgb_to_yuv420_ms = ParseNonnegativeMilliseconds(profile.at("rgb_to_yuv420_ms"),
                                                       "pipeline view RGB-to-YUV420 time"),
      .codec_encode_ms = ParseNonnegativeMilliseconds(profile.at("codec_encode_ms"),
                                                      "pipeline view codec-encode time"),
      .webm_mux_ms =
          ParseNonnegativeMilliseconds(profile.at("webm_mux_ms"), "pipeline view WebM-mux time"),
      .finalize_ms =
          ParseNonnegativeMilliseconds(profile.at("finalize_ms"), "pipeline view finalize time"),
      .output_verification_ms = ParseNonnegativeMilliseconds(
          profile.at("output_verification_ms"), "pipeline view output-verification time"),
      .total_ms = ParseNonnegativeMilliseconds(profile.at("total_ms"), "pipeline view total time"),
  };
  const std::array encoding_components = {
      result.bayer_fit_demosaic_ms, result.rgb_to_yuv420_ms, result.codec_encode_ms,
      result.webm_mux_ms,           result.finalize_ms,      result.output_verification_ms,
  };
  const double encoding_component_sum = result.bayer_fit_demosaic_ms + result.rgb_to_yuv420_ms +
                                        result.codec_encode_ms + result.webm_mux_ms +
                                        result.finalize_ms + result.output_verification_ms;
  const double maximum_component =
      std::max(result.timeline_ms, std::ranges::max(encoding_components));
  Require(result.total_ms + kProfileRoundingToleranceMilliseconds >= maximum_component,
          "pipeline view total is shorter than an individual stage");
  Require(std::isfinite(encoding_component_sum) &&
              result.total_ms + kProfileRoundingToleranceMilliseconds >= encoding_component_sum,
          "pipeline view total does not cover its sequential encoding stages");
  return result;
}

ValidatedPipelineProfile ValidatePipelineProfile(
    const Json &manifest, std::uint64_t strike_time, std::uint64_t confirmation_time,
    const std::array<ValidatedSessionView, 2> &manifest_views) {
  const auto profile_entry = manifest.find("pipeline_profile");
  Require(profile_entry != manifest.end(), "manifest is missing required pipeline profile");
  const Json &profile = *profile_entry;
  RequireExactFields(profile, {"schema_version", "capture", "session", "views"},
                     "pipeline profile");
  const std::uint64_t schema_version =
      ParseUnsignedNumber(profile.at("schema_version"), "pipeline profile schema version");
  Require(schema_version == 2U, "unsupported pipeline profile schema");
  const Json &view_profiles = profile.at("views");
  Require(view_profiles.is_array() && view_profiles.size() == manifest_views.size(),
          "pipeline profile must contain exactly two views");

  ValidatedPipelineProfile result = {
      .schema_version = static_cast<std::uint32_t>(schema_version),
      .capture =
          ValidateCapturePipelineProfile(profile.at("capture"), strike_time, confirmation_time),
      .session = ValidateSessionPipelineProfile(profile.at("session"), confirmation_time),
      .views =
          std::array{
              ValidateViewPipelineProfile(view_profiles.at(0), manifest_views),
              ValidateViewPipelineProfile(view_profiles.at(1), manifest_views),
          },
  };
  Require(result.views[0].role != result.views[1].role,
          "pipeline profile contains duplicate camera roles");
  const double longest_view_total_ms = std::max(result.views[0].total_ms, result.views[1].total_ms);
  Require(std::isfinite(longest_view_total_ms) &&
              result.session.media_encoding_wall_ms + kProfileRoundingToleranceMilliseconds >=
                  longest_view_total_ms,
          "pipeline session media-encoding wall time is shorter than a parallel view");
  return result;
}

SessionArtifactValidation Validate(std::string_view manifest_json,
                                   const std::filesystem::path &session_directory,
                                   const SessionArtifactExpectations &expectations) {
  Require(!expectations.session_id.empty(), "expected session ID cannot be empty");
  Require(expectations.views[0].role != expectations.views[1].role,
          "expected camera roles must be unique");
  if (expectations.expected_synthetic_swing_brightness.has_value()) {
    Require(
        swing_hil_swing_brightness_is_candidate(*expectations.expected_synthetic_swing_brightness),
        "expected synthetic swing brightness is not a current calibration candidate");
  }
  const Json manifest = Json::parse(manifest_json);
  Require(manifest.is_object() && manifest.at("schema_version") == 1,
          "unsupported session manifest schema");
  Require(manifest.at("session_id") == expectations.session_id,
          "manifest session ID does not match the requested session");
  Require(!manifest.at("created_at_utc").get<std::string>().empty(),
          "manifest creation timestamp is empty");
  const Json &trigger = manifest.at("trigger");
  Require(trigger.at("source") == "audio", "session was not triggered by microphone audio");
  const std::uint64_t strike_time =
      ParseUnsignedString(trigger.at("host_monotonic_time_ns"), "trigger.host_monotonic_time_ns");
  const std::uint64_t confirmation_time =
      ParseUnsignedString(trigger.at("confirmation_host_monotonic_time_ns"),
                          "trigger.confirmation_host_monotonic_time_ns");
  constexpr std::uint64_t kMinimumConfirmationNanoseconds = 1'500'000;
  Require(confirmation_time > strike_time &&
              confirmation_time - strike_time >= kMinimumConfirmationNanoseconds,
          "audio trigger confirmation interval is shorter than the detector gate");
  Require(trigger.at("sample_rate_hz").get<std::uint32_t>() == 32'000U,
          "audio trigger sample rate must be 32 kHz");
  const double peak_amplitude = trigger.at("peak_amplitude").get<double>();
  const double noise_floor = trigger.at("noise_floor").get<double>();
  const double threshold = trigger.at("threshold").get<double>();
  Require(std::isfinite(peak_amplitude) && std::isfinite(noise_floor) && std::isfinite(threshold) &&
              peak_amplitude > threshold && threshold > 0.0 && noise_floor >= 0.0 &&
              noise_floor <= threshold && peak_amplitude <= 1.0,
          "audio trigger amplitude evidence is invalid");
  const Json &views = manifest.at("views");
  Require(views.is_array() && views.size() == 2U, "session manifest must contain two views");

  SessionArtifactValidation result;
  result.views[0] = ValidateView(views.at(0), session_directory, expectations);
  result.views[1] = ValidateView(views.at(1), session_directory, expectations);
  Require(result.views[0].role != result.views[1].role,
          "session manifest contains duplicate camera roles");
  result.pipeline_profile =
      ValidatePipelineProfile(manifest, strike_time, confirmation_time, result.views);
  result.synthetic_swing_evidence =
      ValidateSyntheticSwingEvidence({.manifest = &manifest, .views = &views}, expectations);
  result.passed = true;
  return result;
}

}  // namespace

SessionArtifactValidation ValidateSessionArtifacts(
    std::string_view manifest_json, const std::filesystem::path &session_directory,
    const SessionArtifactExpectations &expectations) noexcept {
  try {
    return Validate(manifest_json, session_directory, expectations);
  } catch (const std::exception &error) {
    return {.passed = false,
            .error = error.what(),
            .views = {},
            .pipeline_profile = {},
            .synthetic_swing_evidence = std::nullopt};
  } catch (...) {
    return {.passed = false,
            .error = "unknown session artifact validation failure",
            .views = {},
            .pipeline_profile = {},
            .synthetic_swing_evidence = std::nullopt};
  }
}

}  // namespace swing_capture::hil
