#include "android/pose_hil/warm_retained_validation.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace swing_capture::android::pose_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr std::uint64_t kMaximumEncoderToSensorOffsetResidualNs = 1'000'000;
constexpr std::int64_t kMaximumTimestampOffsetSpanNs = 100'000;
constexpr std::int64_t kMaximumAbsoluteCameraToEncoderOrdinalShift = 32;
constexpr std::size_t kMinimumTimestampPairCount = 16;
constexpr std::size_t kMinimumVideoFrameCount = 450;
constexpr std::size_t kMinimumPreviewFrameCount = 3;
constexpr std::int64_t kMinimumPreRollUs = 1'300'000;
constexpr std::int64_t kMinimumPostRollUs = 450'000;

std::int64_t DecimalString(const Json &value, std::string_view field) {
  if (!value.contains(field) || !value.at(field).is_string()) {
    throw std::invalid_argument(std::string(field) + " must be a decimal string");
  }
  const auto &text = value.at(field).get_ref<const std::string &>();
  std::int64_t parsed = 0;
  // std::from_chars exposes a bounded pointer-pair interface.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char *const text_end = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), text_end, parsed);
  if (error != std::errc() || end != text_end) {
    throw std::invalid_argument(std::string(field) + " is not a decimal integer");
  }
  return parsed;
}

std::size_t NonnegativeSize(const Json &value, std::string_view field) {
  if (!value.contains(field) || !value.at(field).is_number_unsigned()) {
    if (!value.contains(field) || !value.at(field).is_number_integer() ||
        value.at(field).get<std::int64_t>() < 0) {
      throw std::invalid_argument(std::string(field) + " must be nonnegative");
    }
  }
  return value.at(field).get<std::size_t>();
}

std::uint64_t SignedDistance(std::int64_t first, std::int64_t second) {
  constexpr std::uint64_t kSignBit = std::uint64_t{1} << 63U;
  const std::uint64_t ordered_first = static_cast<std::uint64_t>(first) ^ kSignBit;
  const std::uint64_t ordered_second = static_cast<std::uint64_t>(second) ^ kSignBit;
  return ordered_first >= ordered_second ? ordered_first - ordered_second
                                         : ordered_second - ordered_first;
}

std::int64_t SignedInteger(const Json &value, std::string_view field) {
  if (!value.contains(field)) {
    throw std::invalid_argument(std::string(field) + " must be a signed integer");
  }
  const Json &field_value = value.at(field);
  if (field_value.is_number_unsigned()) {
    const std::uint64_t parsed = field_value.get<std::uint64_t>();
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::invalid_argument(std::string(field) + " exceeds signed 64-bit range");
    }
    return static_cast<std::int64_t>(parsed);
  }
  if (!field_value.is_number_integer()) {
    throw std::invalid_argument(std::string(field) + " must be a signed integer");
  }
  return field_value.get<std::int64_t>();
}

bool SafeIdentifier(std::string_view value) {
  if (value.empty() || value.size() > 128U) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '_';
  });
}

void ValidateVideoFrames(const Json &view, std::size_t frame_count) {
  const Json &frames = view.at("frames");
  if (!frames.is_array() || frames.size() != frame_count) {
    throw std::invalid_argument("retained video frame index is inconsistent");
  }
  std::int64_t previous_sensor_ns = -1;
  std::int64_t previous_media_us = -1;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const Json &frame = frames.at(index);
    if (!frame.is_object() || NonnegativeSize(frame, "frame_index") != index) {
      throw std::invalid_argument("retained video frame indices are not contiguous");
    }
    const std::int64_t sensor_ns = DecimalString(frame, "device_timestamp");
    const std::int64_t media_us = frame.at("media_time_us").get<std::int64_t>();
    if (sensor_ns <= previous_sensor_ns || media_us <= previous_media_us) {
      throw std::invalid_argument("retained video timestamps are not strictly increasing");
    }
    previous_sensor_ns = sensor_ns;
    previous_media_us = media_us;
  }
}

void ValidateJpeg(std::string_view bytes, std::size_t offset, std::size_t length) {
  if (length < 4U || offset > bytes.size() || length > bytes.size() - offset) {
    throw std::invalid_argument("pose preview JPEG span is outside the MJPEG file");
  }
  if (static_cast<unsigned char>(bytes.at(offset)) != 0xFFU ||
      static_cast<unsigned char>(bytes.at(offset + 1U)) != 0xD8U ||
      static_cast<unsigned char>(bytes.at(offset + length - 2U)) != 0xFFU ||
      static_cast<unsigned char>(bytes.at(offset + length - 1U)) != 0xD9U) {
    throw std::invalid_argument("pose preview span is not one complete JPEG image");
  }
}

struct PreviewCursor {
  std::size_t expected_offset = 0;
  std::int64_t first_timestamp = -1;
  std::int64_t previous_timestamp = -1;
  std::size_t next_frame = 0;
};

void ValidateObservationRow(const Json &row, std::size_t index, const Json &frame_index,
                            std::size_t frame_count, std::string_view mjpeg,
                            PreviewCursor *cursor) {
  const std::int64_t timestamp = DecimalString(row, "timestamp_boottime_ns");
  if (row.value("schema_version", 0) != 1 || NonnegativeSize(row, "sequence_index") != index ||
      timestamp <= cursor->previous_timestamp || DecimalString(row, "inference_duration_ns") < 0 ||
      !row.contains("controller_state") || !row.at("controller_state").is_string() ||
      !row.contains("frame_available") || !row.at("frame_available").is_boolean()) {
    throw std::invalid_argument("pose observation trace row is invalid");
  }
  if (index == 0U) {
    cursor->first_timestamp = timestamp;
  }
  if (row.at("frame_available").get<bool>()) {
    if (cursor->next_frame >= frame_count) {
      throw std::invalid_argument("pose trace advertises an unindexed JPEG");
    }
    const Json &frame = frame_index.at(cursor->next_frame);
    const std::int64_t offset = DecimalString(frame, "byte_offset");
    const std::size_t length = NonnegativeSize(frame, "byte_length");
    if (NonnegativeSize(frame, "sequence_index") != index || offset < 0 ||
        !std::cmp_equal(offset, cursor->expected_offset) ||
        DecimalString(frame, "timestamp_boottime_ns") != timestamp ||
        frame.value("content_type", "") != "image/jpeg" ||
        DecimalString(row, "frame_byte_offset") != offset ||
        NonnegativeSize(row, "frame_byte_length") != length ||
        row.value("frame_content_type", "") != "image/jpeg") {
      throw std::invalid_argument("pose trace row disagrees with its MJPEG index");
    }
    ValidateJpeg(mjpeg, cursor->expected_offset, length);
    cursor->expected_offset += length;
    ++cursor->next_frame;
  } else if (!row.at("frame_content_type").is_null() || !row.at("frame_byte_offset").is_null() ||
             NonnegativeSize(row, "frame_byte_length") != 0U) {
    throw std::invalid_argument("trace-only pose row claims JPEG metadata");
  }
  cursor->previous_timestamp = timestamp;
}

std::size_t ValidatePreview(const Json &preview, std::string_view mjpeg, std::string_view trace) {
  if (preview.value("schema_version", 0) != 1 ||
      preview.value("frames_path", "") != "pose_diagnostics/preview_frames.mjpeg" ||
      preview.value("frames_content_type", "") != "image/jpeg" ||
      preview.value("trace_path", "") != "pose_diagnostics/pose_trace.ndjson" ||
      preview.value("trace_content_type", "") != "application/x-ndjson") {
    throw std::invalid_argument("pose preview manifest contract is invalid");
  }
  const std::int64_t frames_bytes = DecimalString(preview, "frames_bytes");
  if (frames_bytes < 0 || static_cast<std::size_t>(frames_bytes) != mjpeg.size() ||
      NonnegativeSize(preview, "trace_bytes") != trace.size()) {
    throw std::invalid_argument("pose preview file sizes disagree with the manifest");
  }
  const std::size_t frame_count = NonnegativeSize(preview, "frame_count");
  const std::size_t jpeg_frame_count = NonnegativeSize(preview, "jpeg_frame_count");
  const std::size_t observation_count = NonnegativeSize(preview, "observation_count");
  const Json &frame_index = preview.at("frame_index");
  const std::int64_t first_manifest_timestamp =
      DecimalString(preview, "first_timestamp_boottime_ns");
  const std::int64_t end_manifest_timestamp =
      DecimalString(preview, "end_timestamp_boottime_ns_exclusive");
  if (frame_count != jpeg_frame_count || frame_count < kMinimumPreviewFrameCount ||
      observation_count < frame_count || !frame_index.is_array() ||
      frame_index.size() != frame_count) {
    throw std::invalid_argument("pose preview does not contain enough indexed frames");
  }

  PreviewCursor cursor;
  std::size_t trace_start = 0;
  for (std::size_t index = 0; index < observation_count; ++index) {
    const std::size_t trace_end = trace.find('\n', trace_start);
    if (trace_end == std::string_view::npos || trace_end == trace_start) {
      throw std::invalid_argument("pose trace does not contain every five-Hz observation row");
    }
    const Json row = Json::parse(trace.substr(trace_start, trace_end - trace_start));
    ValidateObservationRow(row, index, frame_index, frame_count, mjpeg, &cursor);
    trace_start = trace_end + 1U;
  }
  if (cursor.next_frame != frame_count || cursor.expected_offset != mjpeg.size() ||
      trace_start != trace.size() || first_manifest_timestamp != cursor.first_timestamp ||
      end_manifest_timestamp <= cursor.previous_timestamp) {
    throw std::invalid_argument("pose preview files contain unindexed trailing bytes");
  }
  return frame_count;
}

}  // namespace

WarmRetainedEvidence ValidateWarmRetainedEvidence(
    const WarmRetainedInspection &inspection) noexcept {
  WarmRetainedEvidence evidence;
  try {
    if (!SafeIdentifier(inspection.expected_session_id) ||
        !SafeIdentifier(inspection.expected_shared_session_id) ||
        (inspection.expected_role != "down_the_line" && inspection.expected_role != "face_on")) {
      throw std::invalid_argument("expected retained identity is unsafe");
    }
    const Json manifest = Json::parse(inspection.manifest);
    if (manifest.value("schema_version", 0) != 1 ||
        manifest.value("session_id", "") != inspection.expected_session_id ||
        manifest.at("trigger").value("source", "") != "manual") {
      throw std::invalid_argument("retained manifest identity or trigger is invalid");
    }
    const Json &views = manifest.at("views");
    if (!views.is_array() || views.size() != 1U) {
      throw std::invalid_argument("retained manifest must contain exactly one view");
    }
    const Json &view = views.at(0);
    evidence.frame_count = NonnegativeSize(view, "frame_count");
    const Json &source = view.at("source");
    const Json &encoded = view.at("encoded");
    const Json &media = view.at("media");
    evidence.media_path = media.value("path", "");
    evidence.media_bytes = NonnegativeSize(media, "encoded_bytes");
    const std::string expected_media_path = std::string(inspection.expected_role) + ".mp4";
    if (view.value("role", "") != inspection.expected_role || view.value("nominal_fps", 0) != 240 ||
        evidence.frame_count < kMinimumVideoFrameCount || source.value("width", 0) != 1280 ||
        source.value("height", 0) != 720 || encoded.value("width", 0) != 1280 ||
        encoded.value("height", 0) != 720 || evidence.media_path != expected_media_path ||
        media.value("mime_type", "") != "video/mp4" ||
        evidence.media_bytes != inspection.media.size() || inspection.media.size() < 12U ||
        inspection.media.substr(4U, 4U) != "ftyp") {
      throw std::invalid_argument("retained MP4 or view metadata is invalid");
    }
    ValidateVideoFrames(view, evidence.frame_count);

    const Json &capture = manifest.at("android_capture");
    if (capture.value("shared_session_id", "") != inspection.expected_shared_session_id ||
        capture.value("camera_timestamp_source", -1) != 1 ||
        capture.value("timestamp_mapping", "") != "streaming_camera2_frame_to_encoder_ordinal") {
      throw std::invalid_argument("warm capture identity or timestamp clock is invalid");
    }
    evidence.camera_to_encoder_ordinal_shift =
        SignedInteger(capture, "camera_to_encoder_ordinal_shift");
    evidence.encoder_to_sensor_offset_ns = DecimalString(capture, "encoder_to_sensor_offset_ns");
    evidence.expected_encoder_to_sensor_offset_ns =
        DecimalString(capture, "expected_encoder_to_sensor_offset_ns");
    evidence.encoder_to_sensor_offset_residual_ns = SignedDistance(
        evidence.encoder_to_sensor_offset_ns, evidence.expected_encoder_to_sensor_offset_ns);
    evidence.timestamp_offset_span_ns = capture.at("timestamp_offset_span_ns").get<std::int64_t>();
    evidence.timestamp_pair_count = NonnegativeSize(capture, "timestamp_pair_count");
    evidence.actual_pre_roll_us = capture.at("actual_pre_roll_us").get<std::int64_t>();
    evidence.actual_post_roll_us = capture.at("actual_post_roll_us").get<std::int64_t>();
    const std::int64_t local_residual =
        capture.at("local_nearest_frame_residual_us").get<std::int64_t>();
    if (evidence.camera_to_encoder_ordinal_shift < -kMaximumAbsoluteCameraToEncoderOrdinalShift ||
        evidence.camera_to_encoder_ordinal_shift > kMaximumAbsoluteCameraToEncoderOrdinalShift ||
        evidence.encoder_to_sensor_offset_residual_ns > kMaximumEncoderToSensorOffsetResidualNs ||
        evidence.timestamp_offset_span_ns < 0 ||
        evidence.timestamp_offset_span_ns > kMaximumTimestampOffsetSpanNs ||
        evidence.timestamp_pair_count < kMinimumTimestampPairCount ||
        evidence.actual_pre_roll_us < kMinimumPreRollUs ||
        evidence.actual_post_roll_us < kMinimumPostRollUs || local_residual < 0 ||
        local_residual > 5'000) {
      throw std::invalid_argument("warm capture timing evidence is outside acceptance bounds");
    }
    const Json &diagnostics = capture.at("diagnostic_evidence");
    if (diagnostics.value("schema_version", 0) != 1 ||
        diagnostics.value("preview_status", "") != "available" ||
        !diagnostics.contains("preview") || !diagnostics.at("preview").is_object()) {
      throw std::invalid_argument("pose diagnostic publication is unavailable");
    }
    evidence.preview_frame_count = ValidatePreview(
        diagnostics.at("preview"), inspection.preview_mjpeg, inspection.pose_trace_ndjson);
    evidence.valid = true;
    evidence.diagnostic = "warm retained capture and pose diagnostics are valid";
  } catch (const std::exception &failure) {
    evidence.diagnostic = failure.what();
  }
  return evidence;
}

}  // namespace swing_capture::android::pose_hil
