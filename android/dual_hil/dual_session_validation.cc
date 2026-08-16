#include "android/dual_hil/dual_session_validation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr std::string_view kReportType = "android_continuous_capture";
constexpr std::string_view kMime = "video/avc";
constexpr int kFramesPerSecond = 240;
constexpr int kDurationMillis = 3000;
constexpr std::int64_t kMinimumPreRollUs = 1400000;
constexpr std::int64_t kMinimumPostRollUs = 490000;
constexpr std::int64_t kMaximumFrameDeltaNs = 6250000;
constexpr std::int64_t kMaximumImpactSkewUs = 10000;
constexpr std::int64_t kMaximumTriggerUncertaintyNs = 5000000;
constexpr std::int64_t kMaximumMediaTimeResidualUs = 1000;

[[noreturn]] void Invalid(std::string_view diagnostic) {
  throw std::runtime_error(std::string(diagnostic));
}

std::int64_t IntegerString(const Json &value, std::string_view field) {
  if (!value.is_string()) {
    Invalid(std::string(field) + " must be a decimal string");
  }
  try {
    std::size_t consumed = 0;
    const std::string text = value.get<std::string>();
    const std::int64_t parsed = std::stoll(text, &consumed);
    if (consumed != text.size()) {
      Invalid(std::string(field) + " contains trailing characters");
    }
    return parsed;
  } catch (const std::exception &) {
    Invalid(std::string(field) + " is outside the signed 64-bit range");
  }
}

double DecimalString(const Json &value, std::string_view field) {
  if (!value.is_string()) {
    Invalid(std::string(field) + " must be a decimal string");
  }
  try {
    std::size_t consumed = 0;
    const std::string text = value.get<std::string>();
    const double parsed = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(parsed)) {
      Invalid(std::string(field) + " is not a finite decimal");
    }
    return parsed;
  } catch (const std::exception &) {
    Invalid(std::string(field) + " is not a finite decimal");
  }
}

bool SafeRelativeSessionPath(std::string_view path, std::string_view session_id,
                             std::string_view suffix) {
  return !session_id.empty() && !session_id.contains("..") &&
         path == "sessions/" + std::string(session_id) + "/" + std::string(suffix);
}

void PopulateAudioEvidence(const Json &trigger, NodeEvidence *evidence) {
  evidence->trigger_time_ns =
      IntegerString(trigger.at("host_monotonic_time_ns"), "host_monotonic_time_ns");
  evidence->confirmation_time_ns = IntegerString(trigger.at("confirmation_host_monotonic_time_ns"),
                                                 "confirmation_host_monotonic_time_ns");
  evidence->audio_sample_rate_hz = trigger.value("sample_rate_hz", 0);
  evidence->audio_peak_amplitude = trigger.value("peak_amplitude", 0.0);
  evidence->audio_noise_floor = trigger.value("noise_floor", 0.0);
  evidence->audio_threshold = trigger.value("threshold", 0.0);
  if (evidence->trigger_time_ns <= 0 ||
      evidence->confirmation_time_ns <= evidence->trigger_time_ns ||
      evidence->audio_sample_rate_hz != 48000 || evidence->audio_noise_floor < 0.0 ||
      evidence->audio_threshold <= evidence->audio_noise_floor ||
      evidence->audio_peak_amplitude <= evidence->audio_threshold) {
    Invalid("retained local-audio trigger evidence is invalid");
  }
}

void RequireSingleViewTimingContract(const Json &manifest) {
  if (!manifest.contains("mapped_nearest_frame_skew_us") ||
      !manifest.at("mapped_nearest_frame_skew_us").is_null()) {
    Invalid("single-view Android manifest must not claim inter-view skew");
  }
}

struct ValidatedFrameTimeline {
  std::int64_t first_sensor_ns = 0;
  std::int64_t last_sensor_ns = 0;
  std::int64_t closest_impact_ns = 0;
  std::size_t impact_frame_index = 0U;
  std::vector<std::int64_t> media_times_us;
  std::vector<std::int64_t> times_from_impact_us;
};

ValidatedFrameTimeline ValidateFrameTimeline(const Json &frames, std::int64_t trigger_time_ns) {
  ValidatedFrameTimeline result;
  std::int64_t previous_sensor = 0;
  std::int64_t previous_media_time = -1;
  std::int64_t previous_ordinal = -1;
  result.media_times_us.reserve(frames.size());
  result.times_from_impact_us.reserve(frames.size());
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const Json &frame = frames.at(index);
    const std::int64_t ordinal = IntegerString(frame.at("frame_id"), "frame_id");
    const std::int64_t sensor = IntegerString(frame.at("device_timestamp"), "device_timestamp");
    const std::int64_t media_time = frame.value("media_time_us", -1L);
    const std::int64_t time_from_impact = frame.value("time_from_impact_us", 0L);
    const std::int64_t recomputed_time_from_impact = (sensor - trigger_time_ns) / 1000L;
    const std::int64_t impact_distance_ns = std::abs(sensor - trigger_time_ns);
    if (time_from_impact != recomputed_time_from_impact) {
      Invalid("retained frame time_from_impact_us does not match its device timestamp");
    }
    if (index == 0U || impact_distance_ns < result.closest_impact_ns) {
      result.closest_impact_ns = impact_distance_ns;
      result.impact_frame_index = index;
    }
    if (index == 0U) {
      result.first_sensor_ns = sensor;
    } else if (ordinal != previous_ordinal + 1 || sensor <= previous_sensor ||
               sensor - previous_sensor > kMaximumFrameDeltaNs ||
               media_time <= previous_media_time ||
               media_time - previous_media_time > kMaximumFrameDeltaNs / 1000L) {
      Invalid("retained frames contain an ordinal or timestamp gap");
    }
    previous_ordinal = ordinal;
    previous_sensor = sensor;
    previous_media_time = media_time;
    result.media_times_us.push_back(media_time);
    result.times_from_impact_us.push_back(time_from_impact);
  }
  result.last_sensor_ns = previous_sensor;
  return result;
}

}  // namespace

NodeEvidence ValidateNodeEvidence(const NodeEvidenceInspection &inspection) {
  try {
    const Json report = Json::parse(inspection.report);
    if (report.value("schema_version", 0) != 1 || report.value("report_type", "") != kReportType ||
        !report.value("complete", false) || !report.value("passed", false) ||
        report.value("role", "") != inspection.expected_role ||
        !report.value("retain_session_requested", false)) {
      Invalid("continuous capture report identity or result is invalid");
    }
    const auto request = report.find("request");
    if (request == report.end() || !request->is_object() ||
        request->value("width", 0) != inspection.expected_profile.width ||
        request->value("height", 0) != inspection.expected_profile.height ||
        request->value("frames_per_second", 0) != kFramesPerSecond ||
        request->value("duration_ms", 0) != kDurationMillis ||
        request->value("bitrate_bits_per_second", 0) !=
            inspection.expected_profile.bitrate_bits_per_second ||
        request->value("mime", "") != kMime) {
      Invalid("continuous capture report request does not match its required role profile");
    }
    const auto retained = report.find("retained_session");
    if (retained == report.end() || !retained->is_object()) {
      Invalid("continuous capture report has no retained session");
    }
    NodeEvidence evidence;
    evidence.node_id = report.value("node_id", "");
    evidence.role = report.value("role", "");
    evidence.local_session_id = retained->value("session_id", "");
    evidence.manifest_path = retained->value("manifest", "");
    evidence.media_path = retained->value("media", "");
    evidence.media_bytes = retained->value<std::size_t>("encoded_bytes", 0U);
    const std::string expected_media_name = evidence.role + ".mp4";
    if (evidence.node_id.empty() ||
        !SafeRelativeSessionPath(evidence.manifest_path, evidence.local_session_id,
                                 "manifest.json") ||
        !SafeRelativeSessionPath(evidence.media_path, evidence.local_session_id,
                                 expected_media_name) ||
        evidence.media_bytes == 0U || evidence.media_bytes != inspection.media.size() ||
        inspection.media.size() < 12U || inspection.media.substr(4U, 4U) != "ftyp") {
      Invalid("retained report paths, size, or MP4 signature are invalid");
    }

    const Json manifest = Json::parse(inspection.manifest);
    if (manifest.value("schema_version", 0) != 1 ||
        manifest.value("session_id", "") != evidence.local_session_id) {
      Invalid("retained manifest identity is invalid");
    }
    const auto trigger = manifest.find("trigger");
    const auto views = manifest.find("views");
    const auto android_capture = manifest.find("android_capture");
    if (trigger == manifest.end() || !trigger->is_object() ||
        trigger->value("source", "") != "local_audio" || views == manifest.end() ||
        !views->is_array() || views->size() != 1U || android_capture == manifest.end() ||
        !android_capture->is_object()) {
      Invalid("retained manifest trigger, view, or Android metadata is invalid");
    }
    PopulateAudioEvidence(*trigger, &evidence);
    evidence.shared_session_id = android_capture->value("shared_session_id", "");
    evidence.actual_pre_roll_us = android_capture->value("actual_pre_roll_us", 0L);
    evidence.actual_post_roll_us = android_capture->value("actual_post_roll_us", 0L);
    RequireSingleViewTimingContract(manifest);
    evidence.local_nearest_frame_residual_us =
        android_capture->value("local_nearest_frame_residual_us", -1L);
    evidence.trigger_timestamp_uncertainty_ns =
        android_capture->value("trigger_timestamp_uncertainty_ns", -1L);
    if (android_capture->value("node_id", "") != evidence.node_id ||
        evidence.shared_session_id != inspection.expected_shared_session_id ||
        android_capture->value("timestamp_pair_count", 0) < 16 ||
        android_capture->value("timestamp_offset_span_ns", kMaximumFrameDeltaNs) > 100000L ||
        evidence.actual_pre_roll_us < kMinimumPreRollUs ||
        evidence.actual_post_roll_us < kMinimumPostRollUs ||
        evidence.local_nearest_frame_residual_us < 0 ||
        evidence.trigger_timestamp_uncertainty_ns < 0 ||
        evidence.trigger_timestamp_uncertainty_ns > kMaximumTriggerUncertaintyNs ||
        evidence.local_nearest_frame_residual_us > kMaximumImpactSkewUs) {
      Invalid("retained Android timing metadata is outside acceptance bounds");
    }

    const Json &view = views->at(0);
    const auto source = view.find("source");
    const auto media_description = view.find("media");
    const auto frames = view.find("frames");
    evidence.frame_count = view.value<std::size_t>("frame_count", 0U);
    if (view.value("role", "") != inspection.expected_role ||
        view.value("nominal_fps", 0) != kFramesPerSecond || evidence.frame_count < 450U ||
        view.value<std::size_t>("impact_frame_index", evidence.frame_count) >=
            evidence.frame_count ||
        source == view.end() || !source->is_object() ||
        source->value("width", 0) != inspection.expected_profile.width ||
        source->value("height", 0) != inspection.expected_profile.height ||
        source->value("pixel_format", "") != "camera2_private" || media_description == view.end() ||
        !media_description->is_object() ||
        media_description->value("mime_type", "") != "video/mp4" ||
        media_description->value<std::size_t>("encoded_bytes", 0U) != evidence.media_bytes ||
        frames == view.end() || !frames->is_array() || frames->size() != evidence.frame_count) {
      Invalid("retained view metadata is invalid");
    }
    evidence.width = inspection.expected_profile.width;
    evidence.height = inspection.expected_profile.height;
    evidence.bitrate_bits_per_second = inspection.expected_profile.bitrate_bits_per_second;

    const auto declared_impact_frame_index =
        view.value<std::size_t>("impact_frame_index", evidence.frame_count);
    ValidatedFrameTimeline timeline = ValidateFrameTimeline(*frames, evidence.trigger_time_ns);
    if (declared_impact_frame_index != timeline.impact_frame_index ||
        evidence.local_nearest_frame_residual_us != timeline.closest_impact_ns / 1000L) {
      Invalid("impact frame index or local nearest-frame residual was not recomputed correctly");
    }
    evidence.media_times_us = std::move(timeline.media_times_us);
    evidence.times_from_impact_us = std::move(timeline.times_from_impact_us);
    const double span_seconds =
        static_cast<double>(timeline.last_sensor_ns - timeline.first_sensor_ns) / 1.0e9;
    evidence.measured_sensor_fps = static_cast<double>(evidence.frame_count - 1U) / span_seconds;
    if (!std::isfinite(evidence.measured_sensor_fps) || evidence.measured_sensor_fps < 235.0 ||
        evidence.measured_sensor_fps > 245.0) {
      Invalid("retained sensor frame rate is outside 235-245 fps");
    }
    return evidence;
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse retained Android evidence: ") +
                             failure.what());
  }
}

std::int64_t ValidateFfprobeTimeline(const NodeEvidence &evidence, std::string_view ffprobe_json) {
  try {
    const Json probe = Json::parse(ffprobe_json);
    const auto streams = probe.find("streams");
    const auto frames = probe.find("frames");
    if (streams == probe.end() || !streams->is_array() || streams->size() != 1U ||
        frames == probe.end() || !frames->is_array() || frames->size() != evidence.frame_count) {
      Invalid("ffprobe did not report exactly one complete video stream");
    }
    const Json &stream = streams->at(0);
    if (stream.value("codec_name", "") != "h264" || stream.value("width", 0) != evidence.width ||
        stream.value("height", 0) != evidence.height ||
        IntegerString(stream.at("nb_frames"), "nb_frames") !=
            static_cast<std::int64_t>(evidence.frame_count)) {
      Invalid("ffprobe video stream does not match retained H.264 profile metadata");
    }
    if (evidence.media_times_us.size() != evidence.frame_count) {
      Invalid("manifest media timeline is incomplete");
    }
    std::optional<double> first_pts_seconds;
    std::int64_t maximum_residual_us = 0;
    std::int64_t previous_pts_us = -1;
    for (std::size_t index = 0; index < frames->size(); ++index) {
      const Json &frame = frames->at(index);
      const double pts_seconds =
          DecimalString(frame.at("best_effort_timestamp_time"), "best_effort_timestamp_time");
      if (!first_pts_seconds.has_value()) {
        first_pts_seconds = pts_seconds;
      }
      const auto relative_pts_us =
          static_cast<std::int64_t>(std::llround((pts_seconds - *first_pts_seconds) * 1.0e6));
      if ((index != 0U && (relative_pts_us <= previous_pts_us ||
                           relative_pts_us - previous_pts_us > kMaximumFrameDeltaNs / 1000L)) ||
          relative_pts_us < 0) {
        Invalid("ffprobe display-order timestamps are nonmonotonic or have a cadence gap");
      }
      const std::int64_t residual_us = std::abs(relative_pts_us - evidence.media_times_us[index]);
      maximum_residual_us = std::max(maximum_residual_us, residual_us);
      previous_pts_us = relative_pts_us;
    }
    if (maximum_residual_us > kMaximumMediaTimeResidualUs) {
      Invalid("ffprobe display-order timeline differs from the manifest by more than 1 ms");
    }
    return maximum_residual_us;
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse ffprobe retained-video evidence: ") +
                             failure.what());
  }
}

TimingCorrelationEvidence EvaluateTimingCorrelation(const TimingCorrelationInspection &inspection) {
  constexpr std::int64_t kMaximumInputUs = 1000000;
  if (inspection.optical_onset_lower_bound_us < -kMaximumInputUs ||
      inspection.optical_onset_lower_bound_us > kMaximumInputUs ||
      inspection.optical_onset_upper_bound_us < -kMaximumInputUs ||
      inspection.optical_onset_upper_bound_us > kMaximumInputUs ||
      inspection.optical_onset_lower_bound_us > inspection.optical_onset_upper_bound_us ||
      inspection.audio_trigger_uncertainty_ns < 0 ||
      inspection.audio_trigger_uncertainty_ns > kMaximumInputUs * 1000L ||
      inspection.media_pts_residual_us < 0 || inspection.media_pts_residual_us > kMaximumInputUs) {
    throw std::invalid_argument("timing-correlation evidence is outside its supported range");
  }
  TimingCorrelationEvidence evidence;
  evidence.optical_onset_lower_bound_us = inspection.optical_onset_lower_bound_us;
  evidence.optical_onset_upper_bound_us = inspection.optical_onset_upper_bound_us;
  evidence.optical_interval_width_us =
      inspection.optical_onset_upper_bound_us - inspection.optical_onset_lower_bound_us;
  evidence.audio_trigger_uncertainty_us = (inspection.audio_trigger_uncertainty_ns + 999L) / 1000L;
  evidence.media_pts_residual_us = inspection.media_pts_residual_us;
  evidence.accounted_uncertainty_us =
      evidence.audio_trigger_uncertainty_us + evidence.media_pts_residual_us;
  evidence.minimum_residual_us =
      evidence.optical_onset_lower_bound_us - evidence.accounted_uncertainty_us;
  evidence.maximum_residual_us =
      evidence.optical_onset_upper_bound_us + evidence.accounted_uncertainty_us;
  evidence.total_bound_us =
      std::max(std::abs(evidence.minimum_residual_us), std::abs(evidence.maximum_residual_us));
  evidence.passed = evidence.minimum_residual_us >= -evidence.acceptance_limit_us &&
                    evidence.maximum_residual_us <= evidence.acceptance_limit_us;
  return evidence;
}

void ValidateRequiredAprilTagPersistence(std::span<const AprilTagFrameEvidence> frames) {
  if (frames.size() != 3U) {
    throw std::invalid_argument("fixture AprilTag evidence must cover pre, impact, and post");
  }
  std::size_t previous_frame_index = 0U;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const AprilTagFrameEvidence &frame = frames[index];
    if ((index != 0U && frame.frame_index <= previous_frame_index) || frame.family != "tag36h11" ||
        frame.id != 0 || frame.hamming < 0 || frame.hamming > 1 || frame.decision_margin < 10.0) {
      throw std::runtime_error(
          "required tag36h11 ID 0 did not persist across pre, impact, and post frames");
    }
    previous_frame_index = frame.frame_index;
  }
}

void ValidateDualSession(const NodeEvidence &down_the_line, const NodeEvidence &face_on) {
  if (down_the_line.role != "down_the_line" || face_on.role != "face_on") {
    Invalid("dual session roles are incomplete or swapped");
  }
  if (down_the_line.node_id.empty() || face_on.node_id.empty() ||
      down_the_line.node_id == face_on.node_id) {
    Invalid("dual session must contain two distinct node identities");
  }
  if (down_the_line.shared_session_id.empty() ||
      down_the_line.shared_session_id != face_on.shared_session_id) {
    Invalid("dual session shared identity does not match");
  }
  if (down_the_line.local_session_id.empty() || face_on.local_session_id.empty() ||
      down_the_line.local_session_id == face_on.local_session_id) {
    Invalid("sequential HIL captures must retain distinct local session identities");
  }
}

}  // namespace swing_capture::android::dual_hil
