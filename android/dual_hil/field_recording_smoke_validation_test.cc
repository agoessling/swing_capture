#include "android/dual_hil/field_recording_smoke_validation.h"

#include <cassert>
#include <cstddef>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using swing_capture::android::dual_hil::FieldRecordingArtifactProbe;
using swing_capture::android::dual_hil::FieldRecordingBundleEvidence;
using swing_capture::android::dual_hil::FieldRecordingBundleInspection;
using swing_capture::android::dual_hil::ValidateFieldRecordingBundle;
using swing_capture::android::dual_hil::ValidateFieldRecordingPair;
using Json = nlohmann::json;

constexpr std::string_view kSharedRecordingId = "field-hil-123";

std::string Manifest(std::string_view role, std::string_view node_id) {
  return Json({
                  {"schema_version", 1},
                  {"session_kind", "field_recording"},
                  {"recording_id", kSharedRecordingId},
                  {"shared_recording_id", kSharedRecordingId},
                  {"node_id", node_id},
                  {"role", role},
                  {"created_at_utc", "2026-08-23T00:00:00Z"},
                  {"duration_us", "2000000"},
                  {"stop_reason", "explicit"},
                  {"video",
                   {{"path", "video.mp4"},
                    {"mime_type", "video/mp4"},
                    {"width", 1280},
                    {"height", 720},
                    {"nominal_fps", 30},
                    {"codec", "avc1.64001f"},
                    {"bytes", "4096"}}},
                  {"audio",
                   {{"path", "audio.wav"},
                    {"mime_type", "audio/wav"},
                    {"sample_rate_hz", 48000},
                    {"channel_count", 1},
                    {"encoding", "pcm_s16le"},
                    {"frames", "96000"},
                    {"bytes", "192044"}}},
                  {"timing",
                   {{"clock", "CLOCK_BOOTTIME"},
                    {"camera_frames", Json::array({{{"frame_number", "1"}}})},
                    {"encoded_samples", Json::array({{{"sample_index", 0}}})},
                    {"audio_timestamp_observations",
                     Json::array({{{"wav_end_frame_position", "4800"}}})}}},
              })
      .dump();
}

std::string Listing(std::string_view role) {
  const std::string prefix = "/api/v1/field-recordings/field-hil-123/";
  return Json({
                  {"schema_version", 1},
                  {"recordings", Json::array({Json({{"recording_id", kSharedRecordingId},
                                                    {"shared_recording_id", kSharedRecordingId},
                                                    {"created_at_utc", "2026-08-23T00:00:00Z"},
                                                    {"role", role},
                                                    {"duration_us", "2000000"},
                                                    {"video_bytes", "4096"},
                                                    {"audio_frames", "96000"},
                                                    {"video_url", prefix + "video.mp4"},
                                                    {"audio_url", prefix + "audio.wav"},
                                                    {"manifest_url", prefix + "manifest"}})})},
              })
      .dump();
}

FieldRecordingArtifactProbe Probe(std::size_t total_bytes, std::string content_type) {
  const std::size_t range_bytes = std::min<std::size_t>(total_bytes, 1024U);
  std::string range_body(range_bytes, '\0');
  if (content_type == "video/mp4") {
    range_body.replace(4U, 4U, "ftyp");
  } else {
    range_body.replace(0U, 4U, "RIFF");
    range_body.replace(8U, 4U, "WAVE");
  }
  return {
      .head_status = 200,
      .head_headers = {{"accept-ranges", "bytes"},
                       {"content-length", std::to_string(total_bytes)},
                       {"content-type", content_type}},
      .head_body_bytes = 0,
      .range_status = 206,
      .range_headers = {{"accept-ranges", "bytes"},
                        {"content-length", std::to_string(range_bytes)},
                        {"content-range", "bytes 0-" + std::to_string(range_bytes - 1U) + "/" +
                                              std::to_string(total_bytes)},
                        {"content-type", std::move(content_type)}},
      .range_body = std::move(range_body),
  };
}

FieldRecordingBundleInspection ValidInspection(std::string_view role, std::string_view node_id,
                                               std::string *listing, std::string *manifest) {
  *listing = Listing(role);
  *manifest = Manifest(role, node_id);
  return {
      .listing_json = *listing,
      .manifest_json = *manifest,
      .expected_shared_recording_id = kSharedRecordingId,
      .expected_role = role,
      .video = Probe(4096, "video/mp4"),
      .audio = Probe(192044, "audio/wav"),
  };
}

template <typename Action>
void ExpectFailure(Action action, std::string_view expected) {
  try {
    action();
    assert(false && "validation should fail");
  } catch (const std::runtime_error &failure) {
    assert(std::string_view(failure.what()).find(expected) != std::string_view::npos);
  }
}

void AcceptsCompletePair() {
  std::string down_listing;
  std::string down_manifest;
  std::string face_listing;
  std::string face_manifest;
  const FieldRecordingBundleEvidence down = ValidateFieldRecordingBundle(
      ValidInspection("down_the_line", "node-dtl", &down_listing, &down_manifest));
  const FieldRecordingBundleEvidence face = ValidateFieldRecordingBundle(
      ValidInspection("face_on", "node-face", &face_listing, &face_manifest));
  assert(down.video_bytes == 4096U);
  assert(down.audio_frames == 96000U);
  assert(face.audio_bytes == 192044U);
  ValidateFieldRecordingPair(down, face, kSharedRecordingId);
}

void RejectsListingAndManifestMismatch() {
  std::string listing;
  std::string manifest;
  FieldRecordingBundleInspection inspection =
      ValidInspection("down_the_line", "node-dtl", &listing, &manifest);
  Json changed = Json::parse(listing);
  changed["recordings"][0]["video_bytes"] = "4095";
  listing = changed.dump();
  inspection.listing_json = listing;
  ExpectFailure([&] { static_cast<void>(ValidateFieldRecordingBundle(inspection)); },
                "listing does not match");
}

void RejectsIncompleteRetrievalEvidence() {
  std::string listing;
  std::string manifest;
  FieldRecordingBundleInspection inspection =
      ValidInspection("face_on", "node-face", &listing, &manifest);
  inspection.audio.range_headers["content-range"] = "bytes 0-100/192044";
  ExpectFailure([&] { static_cast<void>(ValidateFieldRecordingBundle(inspection)); },
                "byte-range headers");
  inspection = ValidInspection("face_on", "node-face", &listing, &manifest);
  inspection.video.head_status = 404;
  ExpectFailure([&] { static_cast<void>(ValidateFieldRecordingBundle(inspection)); }, "HEAD");
  inspection = ValidInspection("face_on", "node-face", &listing, &manifest);
  inspection.audio.range_body.replace(0U, 4U, "NOPE");
  ExpectFailure([&] { static_cast<void>(ValidateFieldRecordingBundle(inspection)); },
                "media signature");
}

void RejectsIncompleteTimingAndWrongPair() {
  std::string listing;
  std::string manifest;
  FieldRecordingBundleInspection inspection =
      ValidInspection("down_the_line", "node-dtl", &listing, &manifest);
  Json changed = Json::parse(manifest);
  changed["timing"]["camera_frames"] = Json::array();
  manifest = changed.dump();
  inspection.manifest_json = manifest;
  ExpectFailure([&] { static_cast<void>(ValidateFieldRecordingBundle(inspection)); },
                "timing evidence");

  FieldRecordingBundleEvidence down{
      .recording_id = std::string(kSharedRecordingId),
      .shared_recording_id = std::string(kSharedRecordingId),
      .node_id = "same-node",
      .role = "down_the_line",
  };
  FieldRecordingBundleEvidence face = down;
  face.role = "face_on";
  ExpectFailure([&] { ValidateFieldRecordingPair(down, face, kSharedRecordingId); }, "roles");
}

}  // namespace

int main() {
  AcceptsCompletePair();
  RejectsListingAndManifestMismatch();
  RejectsIncompleteRetrievalEvidence();
  RejectsIncompleteTimingAndWrongPair();
  return 0;
}
