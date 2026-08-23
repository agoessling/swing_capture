#include "android/dual_hil/field_recording_smoke_validation.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr std::uint64_t kPcm16WavHeaderBytes = 44U;
constexpr std::uint64_t kMaximumRangeBytes = 1024U;

std::uint64_t DecimalString(const Json &object, std::string_view field) {
  const auto value = object.find(field);
  if (value == object.end() || !value->is_string()) {
    throw std::runtime_error("field recording " + std::string(field) + " must be a decimal string");
  }
  const std::string text = value->get<std::string>();
  const std::string_view text_view(text);
  std::uint64_t parsed = 0;
  const auto [end, error] = std::from_chars(text_view.begin(), text_view.end(), parsed);
  if (error != std::errc() || end != text_view.end()) {
    throw std::runtime_error("field recording " + std::string(field) + " is not decimal");
  }
  return parsed;
}

std::string RequiredString(const Json &object, std::string_view field) {
  const auto value = object.find(field);
  if (value == object.end() || !value->is_string() ||
      value->get_ref<const std::string &>().empty()) {
    throw std::runtime_error("field recording " + std::string(field) + " is missing");
  }
  return value->get<std::string>();
}

std::uint64_t HeaderLength(const std::map<std::string, std::string, std::less<>> &headers,
                           std::string_view context) {
  const auto length = headers.find("content-length");
  if (length == headers.end()) {
    throw std::runtime_error(std::string(context) + " lacks Content-Length");
  }
  const std::string_view length_view(length->second);
  std::uint64_t parsed = 0;
  const auto [end, error] = std::from_chars(length_view.begin(), length_view.end(), parsed);
  if (error != std::errc() || end != length_view.end()) {
    throw std::runtime_error(std::string(context) + " Content-Length is invalid");
  }
  return parsed;
}

struct ArtifactExpectation {
  std::string_view content_type;
  std::string_view context;
};

void ValidateArtifactProbe(const FieldRecordingArtifactProbe &probe, std::uint64_t total_bytes,
                           const ArtifactExpectation expectation) {
  const std::string_view context = expectation.context;
  const std::string_view expected_content_type = expectation.content_type;
  if (total_bytes == 0U || probe.head_status != 200 || probe.head_body_bytes != 0U) {
    throw std::runtime_error(std::string(context) + " HEAD did not describe a nonempty artifact");
  }
  if (HeaderLength(probe.head_headers, context) != total_bytes ||
      !probe.head_headers.contains("accept-ranges") ||
      probe.head_headers.at("accept-ranges") != "bytes") {
    throw std::runtime_error(std::string(context) + " HEAD size or range contract is invalid");
  }
  const auto head_type = probe.head_headers.find("content-type");
  if (head_type == probe.head_headers.end() || head_type->second != expected_content_type) {
    throw std::runtime_error(std::string(context) + " HEAD content type is invalid");
  }

  const std::uint64_t expected_range_bytes = std::min(total_bytes, kMaximumRangeBytes);
  if (probe.range_status != 206 || probe.range_body.size() != expected_range_bytes ||
      HeaderLength(probe.range_headers, context) != expected_range_bytes) {
    throw std::runtime_error(std::string(context) + " byte-range response length is invalid");
  }
  const std::string expected_range =
      "bytes 0-" + std::to_string(expected_range_bytes - 1U) + "/" + std::to_string(total_bytes);
  const auto range = probe.range_headers.find("content-range");
  const auto accept_ranges = probe.range_headers.find("accept-ranges");
  const auto range_type = probe.range_headers.find("content-type");
  if (range == probe.range_headers.end() || range->second != expected_range ||
      accept_ranges == probe.range_headers.end() || accept_ranges->second != "bytes" ||
      range_type == probe.range_headers.end() || range_type->second != expected_content_type) {
    throw std::runtime_error(std::string(context) + " byte-range headers are invalid");
  }
  const bool valid_video_prefix =
      expected_content_type != "video/mp4" ||
      (probe.range_body.size() >= 8U && probe.range_body.substr(4U, 4U) == "ftyp");
  const bool valid_audio_prefix =
      expected_content_type != "audio/wav" ||
      (probe.range_body.size() >= 12U && probe.range_body.starts_with("RIFF") &&
       probe.range_body.substr(8U, 4U) == "WAVE");
  if (!valid_video_prefix || !valid_audio_prefix) {
    throw std::runtime_error(std::string(context) + " has an invalid media signature");
  }
}

void ValidateListingSummary(const Json &listing, const FieldRecordingBundleEvidence &evidence,
                            const std::uint64_t duration) {
  const Json *summary = nullptr;
  for (const Json &candidate : listing.at("recordings")) {
    if (candidate.is_object() && candidate.value("recording_id", "") == evidence.recording_id) {
      if (summary != nullptr) {
        throw std::runtime_error("field recording listing contains a duplicate recording");
      }
      summary = &candidate;
    }
  }
  if (summary == nullptr ||
      summary->value("shared_recording_id", "") != evidence.shared_recording_id ||
      summary->value("role", "") != evidence.role ||
      DecimalString(*summary, "duration_us") != duration ||
      DecimalString(*summary, "video_bytes") != evidence.video_bytes ||
      DecimalString(*summary, "audio_frames") != evidence.audio_frames) {
    throw std::runtime_error("field recording listing does not match its manifest");
  }
  const std::string prefix = "/api/v1/field-recordings/" + evidence.recording_id + "/";
  if (summary->value("video_url", "") != prefix + "video.mp4" ||
      summary->value("audio_url", "") != prefix + "audio.wav" ||
      summary->value("manifest_url", "") != prefix + "manifest") {
    throw std::runtime_error("field recording listing contains unexpected artifact URLs");
  }
}

}  // namespace

FieldRecordingBundleEvidence ValidateFieldRecordingBundle(
    const FieldRecordingBundleInspection &inspection) {
  if (inspection.expected_shared_recording_id.empty() || inspection.expected_role.empty()) {
    throw std::invalid_argument("field recording inspection identity is incomplete");
  }
  const Json listing = Json::parse(inspection.listing_json);
  const Json manifest = Json::parse(inspection.manifest_json);
  if (listing.value("schema_version", 0) != 1 || !listing.contains("recordings") ||
      !listing.at("recordings").is_array()) {
    throw std::runtime_error("field recording listing schema is invalid");
  }
  if (manifest.value("schema_version", 0) != 1 ||
      manifest.value("session_kind", "") != "field_recording") {
    throw std::runtime_error("field recording manifest schema is invalid");
  }

  FieldRecordingBundleEvidence evidence{
      .recording_id = RequiredString(manifest, "recording_id"),
      .shared_recording_id = RequiredString(manifest, "shared_recording_id"),
      .node_id = RequiredString(manifest, "node_id"),
      .role = RequiredString(manifest, "role"),
      .duration_us = 0,
      .video_bytes = 0,
      .audio_frames = 0,
      .audio_bytes = 0,
  };
  if (evidence.recording_id != inspection.expected_shared_recording_id ||
      evidence.shared_recording_id != inspection.expected_shared_recording_id ||
      evidence.role != inspection.expected_role) {
    throw std::runtime_error("field recording manifest identity does not match the request");
  }
  const std::uint64_t duration = DecimalString(manifest, "duration_us");
  if (duration == 0U ||
      duration > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::runtime_error("field recording duration is invalid");
  }
  evidence.duration_us = static_cast<std::int64_t>(duration);
  if (manifest.value("stop_reason", "") != "explicit") {
    throw std::runtime_error("field recording was not stopped explicitly");
  }
  if (!manifest.contains("video") || !manifest.at("video").is_object() ||
      !manifest.contains("audio") || !manifest.at("audio").is_object() ||
      !manifest.contains("timing") || !manifest.at("timing").is_object()) {
    throw std::runtime_error("field recording manifest lacks media or timing metadata");
  }
  const Json &video = manifest.at("video");
  const Json &audio = manifest.at("audio");
  const Json &timing = manifest.at("timing");
  if (video.value("path", "") != "video.mp4" || video.value("mime_type", "") != "video/mp4" ||
      video.value("width", 0) != 1280 || video.value("height", 0) != 720 ||
      video.value("nominal_fps", 0) != 30 || RequiredString(video, "codec").empty() ||
      audio.value("path", "") != "audio.wav" || audio.value("mime_type", "") != "audio/wav" ||
      audio.value("sample_rate_hz", 0) != 48'000 || audio.value("channel_count", 0) != 1 ||
      audio.value("encoding", "") != "pcm_s16le") {
    throw std::runtime_error("field recording media profile is invalid");
  }
  evidence.video_bytes = DecimalString(video, "bytes");
  evidence.audio_frames = DecimalString(audio, "frames");
  evidence.audio_bytes = DecimalString(audio, "bytes");
  if (evidence.video_bytes == 0U || evidence.audio_frames == 0U ||
      evidence.audio_frames >
          (std::numeric_limits<std::uint64_t>::max() - kPcm16WavHeaderBytes) / 2U ||
      evidence.audio_bytes != kPcm16WavHeaderBytes + evidence.audio_frames * 2U ||
      timing.value("clock", "") != "CLOCK_BOOTTIME" || !timing.contains("camera_frames") ||
      !timing.at("camera_frames").is_array() || timing.at("camera_frames").empty() ||
      !timing.contains("encoded_samples") || !timing.at("encoded_samples").is_array() ||
      timing.at("encoded_samples").empty() || !timing.contains("audio_timestamp_observations") ||
      !timing.at("audio_timestamp_observations").is_array() ||
      timing.at("audio_timestamp_observations").empty()) {
    throw std::runtime_error("field recording media sizes or timing evidence are invalid");
  }

  ValidateListingSummary(listing, evidence, duration);
  ValidateArtifactProbe(inspection.video, evidence.video_bytes,
                        {.content_type = "video/mp4", .context = "video artifact"});
  ValidateArtifactProbe(inspection.audio, evidence.audio_bytes,
                        {.content_type = "audio/wav", .context = "audio artifact"});
  return evidence;
}

void ValidateFieldRecordingPair(const FieldRecordingBundleEvidence &down_the_line,
                                const FieldRecordingBundleEvidence &face_on,
                                std::string_view expected_shared_recording_id) {
  if (expected_shared_recording_id.empty() ||
      down_the_line.shared_recording_id != expected_shared_recording_id ||
      face_on.shared_recording_id != expected_shared_recording_id ||
      down_the_line.recording_id != expected_shared_recording_id ||
      face_on.recording_id != expected_shared_recording_id ||
      down_the_line.role != "down_the_line" || face_on.role != "face_on" ||
      down_the_line.node_id == face_on.node_id) {
    throw std::runtime_error("field recording pair identity or roles are inconsistent");
  }
}

}  // namespace swing_capture::android::dual_hil
