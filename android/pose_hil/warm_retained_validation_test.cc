#include "android/pose_hil/warm_retained_validation.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace pose_hil = swing_capture::android::pose_hil;

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

struct Fixture {
  std::string manifest;
  std::string media;
  std::string preview;
  std::string trace;
};

Fixture ValidFixture() {
  Fixture fixture;
  fixture.media = "....ftyp....";
  Json preview_index = Json::array();
  std::size_t offset = 0;
  for (std::size_t index = 0; index < 5; ++index) {
    const std::int64_t timestamp = 1'000'000'000 + static_cast<std::int64_t>(index) * 200'000'000;
    const bool has_frame = index == 0U || index == 2U || index == 4U;
    Json row = {
        {"schema_version", 1},
        {"sequence_index", index},
        {"timestamp_boottime_ns", std::to_string(timestamp)},
        {"frame_available", has_frame},
        {"model_id", "pose_landmarker_lite"},
        {"inference_duration_ns", "100000000"},
        {"controller_state", "monitoring"},
    };
    if (has_frame) {
      std::string jpeg = "\xff\xd8" + std::string(index + 1U, 'x') + "\xff\xd9";
      preview_index.push_back({
          {"sequence_index", index},
          {"timestamp_boottime_ns", std::to_string(timestamp)},
          {"byte_offset", std::to_string(offset)},
          {"byte_length", jpeg.size()},
          {"content_type", "image/jpeg"},
      });
      row["frame_content_type"] = "image/jpeg";
      row["frame_byte_offset"] = std::to_string(offset);
      row["frame_byte_length"] = jpeg.size();
      fixture.preview += jpeg;
      offset += jpeg.size();
    } else {
      row["frame_content_type"] = nullptr;
      row["frame_byte_offset"] = nullptr;
      row["frame_byte_length"] = 0;
    }
    fixture.trace += row.dump() + "\n";
  }

  Json frames = Json::array();
  for (std::size_t index = 0; index < 450; ++index) {
    frames.push_back({
        {"frame_index", index},
        {"device_timestamp", std::to_string(2'000'000'000ULL + index * 4'166'667ULL)},
        {"media_time_us", static_cast<std::int64_t>(index * 4'167ULL)},
    });
  }
  Json preview = {
      {"schema_version", 1},
      {"frames_path", "pose_diagnostics/preview_frames.mjpeg"},
      {"frames_content_type", "image/jpeg"},
      {"frames_bytes", std::to_string(fixture.preview.size())},
      {"trace_path", "pose_diagnostics/pose_trace.ndjson"},
      {"trace_content_type", "application/x-ndjson"},
      {"trace_bytes", fixture.trace.size()},
      {"first_timestamp_boottime_ns", "1000000000"},
      {"end_timestamp_boottime_ns_exclusive", "1800000001"},
      {"observation_count", 5},
      {"jpeg_frame_count", 3},
      {"frame_count", 3},
      {"frame_index", std::move(preview_index)},
  };
  fixture.manifest =
      Json({
               {"schema_version", 1},
               {"session_id", "session-1"},
               {"trigger", {{"source", "manual"}}},
               {"views", Json::array({{{"role", "down_the_line"},
                                       {"source", {{"width", 1280}, {"height", 720}}},
                                       {"encoded", {{"width", 1280}, {"height", 720}}},
                                       {"frame_count", 450},
                                       {"nominal_fps", 240},
                                       {"media",
                                        {{"path", "down_the_line.mp4"},
                                         {"mime_type", "video/mp4"},
                                         {"encoded_bytes", fixture.media.size()}}},
                                       {"frames", std::move(frames)}}})},
               {"android_capture",
                {{"shared_session_id", "shared-1"},
                 {"camera_timestamp_source", 1},
                 {"timestamp_mapping", "streaming_camera2_frame_to_encoder_ordinal"},
                 {"camera_to_encoder_ordinal_shift", -7},
                 {"encoder_to_sensor_offset_ns", "500"},
                 {"expected_encoder_to_sensor_offset_ns", "0"},
                 {"timestamp_offset_span_ns", 500},
                 {"timestamp_pair_count", 64},
                 {"local_nearest_frame_residual_us", 1'000},
                 {"actual_pre_roll_us", 1'400'000},
                 {"actual_post_roll_us", 500'000},
                 {"diagnostic_evidence",
                  {{"schema_version", 1},
                   {"preview_status", "available"},
                   {"preview", std::move(preview)}}}}},
           })
          .dump();
  return fixture;
}

pose_hil::WarmRetainedEvidence Inspect(const Fixture &fixture) {
  return pose_hil::ValidateWarmRetainedEvidence({
      .manifest = fixture.manifest,
      .media = fixture.media,
      .preview_mjpeg = fixture.preview,
      .pose_trace_ndjson = fixture.trace,
      .expected_session_id = "session-1",
      .expected_shared_session_id = "shared-1",
      .expected_role = "down_the_line",
  });
}

}  // namespace

int main() {
  Fixture valid = ValidFixture();
  pose_hil::WarmRetainedEvidence evidence = Inspect(valid);
  Check(evidence.valid, evidence.diagnostic.c_str());
  Check(evidence.frame_count == 450, "video frame count preserved");
  Check(evidence.preview_frame_count == 3, "preview frame count preserved");
  Check(evidence.camera_to_encoder_ordinal_shift == -7, "ordinal shift preserved");
  Check(evidence.encoder_to_sensor_offset_ns == 500, "timestamp offset preserved");
  Check(evidence.encoder_to_sensor_offset_residual_ns == 500, "timestamp residual preserved");

  Fixture suspended_clock = ValidFixture();
  Json suspended_clock_manifest = Json::parse(suspended_clock.manifest);
  suspended_clock_manifest["android_capture"]["encoder_to_sensor_offset_ns"] = "195865000500";
  suspended_clock_manifest["android_capture"]["expected_encoder_to_sensor_offset_ns"] =
      "195865000000";
  suspended_clock.manifest = suspended_clock_manifest.dump();
  Check(Inspect(suspended_clock).valid, "measured suspend clock offset accepted");

  Fixture missing_shift = ValidFixture();
  Json missing_shift_manifest = Json::parse(missing_shift.manifest);
  missing_shift_manifest["android_capture"].erase("camera_to_encoder_ordinal_shift");
  missing_shift.manifest = missing_shift_manifest.dump();
  Check(!Inspect(missing_shift).valid, "missing ordinal shift rejected");

  Fixture wrong_shift_type = ValidFixture();
  Json wrong_shift_type_manifest = Json::parse(wrong_shift_type.manifest);
  wrong_shift_type_manifest["android_capture"]["camera_to_encoder_ordinal_shift"] = "-7";
  wrong_shift_type.manifest = wrong_shift_type_manifest.dump();
  Check(!Inspect(wrong_shift_type).valid, "non-integer ordinal shift rejected");

  Fixture large_positive_shift = ValidFixture();
  Json large_positive_shift_manifest = Json::parse(large_positive_shift.manifest);
  large_positive_shift_manifest["android_capture"]["camera_to_encoder_ordinal_shift"] = 33;
  large_positive_shift.manifest = large_positive_shift_manifest.dump();
  Check(!Inspect(large_positive_shift).valid, "large positive ordinal shift rejected");

  Fixture large_negative_shift = ValidFixture();
  Json large_negative_shift_manifest = Json::parse(large_negative_shift.manifest);
  large_negative_shift_manifest["android_capture"]["camera_to_encoder_ordinal_shift"] = -33;
  large_negative_shift.manifest = large_negative_shift_manifest.dump();
  Check(!Inspect(large_negative_shift).valid, "large negative ordinal shift rejected");

  Fixture large_offset = ValidFixture();
  Json large_offset_manifest = Json::parse(large_offset.manifest);
  large_offset_manifest["android_capture"]["encoder_to_sensor_offset_ns"] = "4166667";
  large_offset.manifest = large_offset_manifest.dump();
  Check(!Inspect(large_offset).valid, "one-frame timestamp offset rejected");

  Fixture broken_jpeg = ValidFixture();
  broken_jpeg.preview.at(0) = 'x';
  Check(!Inspect(broken_jpeg).valid, "invalid MJPEG frame rejected");

  Fixture missing_preview = ValidFixture();
  Json missing_preview_manifest = Json::parse(missing_preview.manifest);
  missing_preview_manifest["android_capture"]["diagnostic_evidence"]["preview_status"] =
      "not_available";
  missing_preview.manifest = missing_preview_manifest.dump();
  Check(!Inspect(missing_preview).valid, "missing pose diagnostics rejected");

  std::cout << "warm retained pose HIL validation tests passed\n";
  return 0;
}
