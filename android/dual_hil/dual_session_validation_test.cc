#include "android/dual_hil/dual_session_validation.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::android::dual_hil::AprilTagFrameEvidence;
using swing_capture::android::dual_hil::CaptureProfileExpectation;
using swing_capture::android::dual_hil::EvaluateTimingCorrelation;
using swing_capture::android::dual_hil::NodeEvidence;
using swing_capture::android::dual_hil::NodeEvidenceInspection;
using swing_capture::android::dual_hil::TimingCorrelationInspection;
using swing_capture::android::dual_hil::ValidateDualSession;
using swing_capture::android::dual_hil::ValidateFfprobeTimeline;
using swing_capture::android::dual_hil::ValidateNodeEvidence;
using swing_capture::android::dual_hil::ValidateRequiredAprilTagPersistence;

constexpr std::string_view kSharedSession = "dual-hil-session";
constexpr CaptureProfileExpectation kPixel6Profile = {
    .width = 1920,
    .height = 1080,
    .bitrate_bits_per_second = 24000000,
};
constexpr CaptureProfileExpectation kPixel5aProfile = {
    .width = 1280,
    .height = 720,
    .bitrate_bits_per_second = 12000000,
};

struct Fixture {
  std::string report;
  std::string manifest;
  std::string media;
};

Fixture MakeFixture(std::string_view role, std::string_view node_id, std::string_view local_session,
                    const CaptureProfileExpectation &profile) {
  constexpr std::size_t kFrameCount = 481;
  Json frames = Json::array();
  for (std::size_t index = 0; index < kFrameCount; ++index) {
    const std::int64_t sensor_timestamp = 10000000000LL + 4166667LL * index;
    frames.push_back({
        {"frame_index", index},
        {"frame_id", std::to_string(200U + index)},
        {"device_timestamp", std::to_string(sensor_timestamp)},
        {"time_from_impact_us", (sensor_timestamp - 11500000000LL) / 1000L},
        {"media_time_us", static_cast<std::int64_t>(index) * 4167},
    });
  }
  const std::string media = std::string("....ftyp....");
  const std::string media_name = std::string(role) + ".mp4";
  Json report = {
      {"schema_version", 1},
      {"report_type", "android_continuous_capture"},
      {"node_id", node_id},
      {"role", role},
      {"request",
       {
           {"width", profile.width},
           {"height", profile.height},
           {"frames_per_second", 240},
           {"duration_ms", 3000},
           {"bitrate_bits_per_second", profile.bitrate_bits_per_second},
           {"mime", "video/avc"},
       }},
      {"retain_session_requested", true},
      {"complete", true},
      {"passed", true},
      {"retained_session",
       {
           {"session_id", local_session},
           {"manifest", "sessions/" + std::string(local_session) + "/manifest.json"},
           {"media", "sessions/" + std::string(local_session) + "/" + media_name},
           {"encoded_bytes", media.size()},
       }},
  };
  Json manifest = {
      {"schema_version", 1},
      {"session_id", local_session},
      {"trigger",
       {
           {"source", "local_audio"},
           {"host_monotonic_time_ns", "11500000000"},
           {"confirmation_host_monotonic_time_ns", "11501500000"},
           {"sample_rate_hz", 48000},
           {"peak_amplitude", 0.2},
           {"noise_floor", 0.001},
           {"threshold", 0.015},
       }},
      {"mapped_nearest_frame_skew_us", nullptr},
      {"views",
       Json::array({{{"role", role},
                     {"frame_count", kFrameCount},
                     {"nominal_fps", 240},
                     {"impact_frame_index", 360},
                     {"source",
                      {{"pixel_format", "camera2_private"},
                       {"width", profile.width},
                       {"height", profile.height}}},
                     {"media", {{"mime_type", "video/mp4"}, {"encoded_bytes", media.size()}}},
                     {"frames", frames}}})},
      {"android_capture",
       {
           {"node_id", node_id},
           {"shared_session_id", kSharedSession},
           {"timestamp_pair_count", 64},
           {"timestamp_offset_span_ns", 1000},
           {"actual_pre_roll_us", 1500000},
           {"actual_post_roll_us", 499000},
           {"trigger_timestamp_uncertainty_ns", 400000},
           {"local_nearest_frame_residual_us", 0},
       }},
  };
  return {.report = report.dump(), .manifest = manifest.dump(), .media = media};
}

std::string MakeFfprobe(std::size_t frame_count, const CaptureProfileExpectation &profile) {
  Json frames = Json::array();
  for (std::size_t index = 0; index < frame_count; ++index) {
    const double seconds = static_cast<double>(index) * 0.004167;
    frames.push_back({{"best_effort_timestamp_time", std::to_string(seconds)}});
  }
  return Json({
                  {"streams", Json::array({{{"codec_name", "h264"},
                                            {"width", profile.width},
                                            {"height", profile.height},
                                            {"nb_frames", std::to_string(frame_count)}}})},
                  {"frames", frames},
              })
      .dump();
}

void NominalDualSessionPasses() {
  const Fixture down_the_line =
      MakeFixture("down_the_line", "node-dtl", "local-dtl", kPixel6Profile);
  const Fixture face_on = MakeFixture("face_on", "node-fo", "local-fo", kPixel5aProfile);
  const NodeEvidence down_the_line_evidence = ValidateNodeEvidence(NodeEvidenceInspection{
      .report = down_the_line.report,
      .manifest = down_the_line.manifest,
      .media = down_the_line.media,
      .expected_role = "down_the_line",
      .expected_shared_session_id = kSharedSession,
      .expected_profile = kPixel6Profile,
  });
  const NodeEvidence face_on_evidence = ValidateNodeEvidence(NodeEvidenceInspection{
      .report = face_on.report,
      .manifest = face_on.manifest,
      .media = face_on.media,
      .expected_role = "face_on",
      .expected_shared_session_id = kSharedSession,
      .expected_profile = kPixel5aProfile,
  });
  assert(down_the_line_evidence.frame_count == 481U);
  assert(down_the_line_evidence.measured_sensor_fps > 239.9);
  assert(ValidateFfprobeTimeline(down_the_line_evidence, MakeFfprobe(481U, kPixel6Profile)) <=
         160L);
  assert(ValidateFfprobeTimeline(face_on_evidence, MakeFfprobe(481U, kPixel5aProfile)) <= 160L);
  ValidateDualSession(down_the_line_evidence, face_on_evidence);
}

template <typename Action>
void ExpectFailure(Action action) {
  try {
    action();
  } catch (const std::exception &) {
    return;
  }
  assert(false);
}

void InvalidEvidenceFails() {
  Fixture fixture = MakeFixture("down_the_line", "node-dtl", "local-dtl", kPixel6Profile);
  Json manifest = Json::parse(fixture.manifest);
  manifest["views"][0]["frames"][100]["frame_id"] = "999";
  fixture.manifest = manifest.dump();
  ExpectFailure([&] {
    static_cast<void>(ValidateNodeEvidence(NodeEvidenceInspection{
        .report = fixture.report,
        .manifest = fixture.manifest,
        .media = fixture.media,
        .expected_role = "down_the_line",
        .expected_shared_session_id = kSharedSession,
        .expected_profile = kPixel6Profile,
    }));
  });

  NodeEvidence node;
  node.node_id = "same-node";
  node.role = "down_the_line";
  node.local_session_id = "one";
  node.shared_session_id = "shared";
  NodeEvidence other = node;
  other.role = "face_on";
  other.local_session_id = "two";
  ExpectFailure([&] { ValidateDualSession(node, other); });

  const Fixture nominal = MakeFixture("down_the_line", "node-dtl", "local-dtl", kPixel6Profile);
  const NodeEvidence evidence = ValidateNodeEvidence(NodeEvidenceInspection{
      .report = nominal.report,
      .manifest = nominal.manifest,
      .media = nominal.media,
      .expected_role = "down_the_line",
      .expected_shared_session_id = kSharedSession,
      .expected_profile = kPixel6Profile,
  });
  const auto expect_invalid_manifest = [&](const Json &candidate) {
    ExpectFailure([&] {
      static_cast<void>(ValidateNodeEvidence(NodeEvidenceInspection{
          .report = nominal.report,
          .manifest = candidate.dump(),
          .media = nominal.media,
          .expected_role = "down_the_line",
          .expected_shared_session_id = kSharedSession,
          .expected_profile = kPixel6Profile,
      }));
    });
  };
  Json wrong_probe = Json::parse(MakeFfprobe(evidence.frame_count, kPixel6Profile));
  wrong_probe["frames"].erase(wrong_probe["frames"].end() - 1);
  ExpectFailure([&] { static_cast<void>(ValidateFfprobeTimeline(evidence, wrong_probe.dump())); });
  Json gapped_probe = Json::parse(MakeFfprobe(evidence.frame_count, kPixel6Profile));
  gapped_probe["frames"][2]["best_effort_timestamp_time"] = "0.020000";
  ExpectFailure([&] { static_cast<void>(ValidateFfprobeTimeline(evidence, gapped_probe.dump())); });

  const Fixture wrong_profile = MakeFixture("face_on", "node-fo", "local-fo", kPixel5aProfile);
  ExpectFailure([&] {
    static_cast<void>(ValidateNodeEvidence(NodeEvidenceInspection{
        .report = wrong_profile.report,
        .manifest = wrong_profile.manifest,
        .media = wrong_profile.media,
        .expected_role = "face_on",
        .expected_shared_session_id = kSharedSession,
        .expected_profile = kPixel6Profile,
    }));
  });

  Json invalid_manifest = Json::parse(nominal.manifest);
  invalid_manifest["mapped_nearest_frame_skew_us"] = 0;
  ExpectFailure([&] {
    static_cast<void>(ValidateNodeEvidence(NodeEvidenceInspection{
        .report = nominal.report,
        .manifest = invalid_manifest.dump(),
        .media = nominal.media,
        .expected_role = "down_the_line",
        .expected_shared_session_id = kSharedSession,
        .expected_profile = kPixel6Profile,
    }));
  });

  invalid_manifest = Json::parse(nominal.manifest);
  invalid_manifest["trigger"]["peak_amplitude"] = 0.01;
  expect_invalid_manifest(invalid_manifest);

  invalid_manifest = Json::parse(nominal.manifest);
  invalid_manifest["views"][0]["frames"][100]["time_from_impact_us"] = 123;
  expect_invalid_manifest(invalid_manifest);

  invalid_manifest = Json::parse(nominal.manifest);
  invalid_manifest["views"][0]["impact_frame_index"] = 359;
  expect_invalid_manifest(invalid_manifest);

  invalid_manifest = Json::parse(nominal.manifest);
  invalid_manifest["android_capture"]["local_nearest_frame_residual_us"] = 1;
  expect_invalid_manifest(invalid_manifest);

  invalid_manifest = Json::parse(nominal.manifest);
  const std::int64_t gapped_sensor =
      std::stoll(
          invalid_manifest["views"][0]["frames"][100]["device_timestamp"].get<std::string>()) +
      7000000L;
  invalid_manifest["views"][0]["frames"][100]["device_timestamp"] = std::to_string(gapped_sensor);
  invalid_manifest["views"][0]["frames"][100]["time_from_impact_us"] =
      (gapped_sensor - 11500000000LL) / 1000L;
  expect_invalid_manifest(invalid_manifest);

  ExpectFailure([&] {
    static_cast<void>(ValidateNodeEvidence(NodeEvidenceInspection{
        .report = nominal.report,
        .manifest = nominal.manifest,
        .media = nominal.media,
        .expected_role = "down_the_line",
        .expected_shared_session_id = "different-run-id",
        .expected_profile = kPixel6Profile,
    }));
  });
}

void ConservativeTimingBoundIncludesResidual() {
  const auto passing = EvaluateTimingCorrelation(TimingCorrelationInspection{
      .optical_onset_lower_bound_us = -19400,
      .optical_onset_upper_bound_us = -11000,
      .audio_trigger_uncertainty_ns = 400000,
      .media_pts_residual_us = 200,
  });
  assert(passing.optical_interval_width_us == 8400L);
  assert(passing.accounted_uncertainty_us == 600L);
  assert(passing.minimum_residual_us == -20000L);
  assert(passing.maximum_residual_us == -10400L);
  assert(passing.total_bound_us == 20000L);
  assert(passing.passed);

  const auto failing = EvaluateTimingCorrelation(TimingCorrelationInspection{
      .optical_onset_lower_bound_us = -19401,
      .optical_onset_upper_bound_us = -11000,
      .audio_trigger_uncertainty_ns = 400000,
      .media_pts_residual_us = 200,
  });
  assert(failing.minimum_residual_us == -20001L);
  assert(failing.total_bound_us == 20001L);
  assert(!failing.passed);
  const auto positive_endpoint = EvaluateTimingCorrelation(TimingCorrelationInspection{
      .optical_onset_lower_bound_us = 11000,
      .optical_onset_upper_bound_us = 19400,
      .audio_trigger_uncertainty_ns = 400000,
      .media_pts_residual_us = 200,
  });
  assert(positive_endpoint.minimum_residual_us == 10400L);
  assert(positive_endpoint.maximum_residual_us == 20000L);
  assert(positive_endpoint.passed);
  const auto outward_rounding = EvaluateTimingCorrelation(TimingCorrelationInspection{
      .optical_onset_lower_bound_us = 0,
      .optical_onset_upper_bound_us = 20000,
      .audio_trigger_uncertainty_ns = 1,
  });
  assert(outward_rounding.audio_trigger_uncertainty_us == 1L);
  assert(outward_rounding.maximum_residual_us == 20001L);
  assert(!outward_rounding.passed);
  ExpectFailure([] {
    static_cast<void>(EvaluateTimingCorrelation(TimingCorrelationInspection{
        .optical_onset_lower_bound_us = 1,
        .optical_onset_upper_bound_us = 0,
        .audio_trigger_uncertainty_ns = -1,
    }));
  });
}

void RequiredAprilTagMustPersist() {
  std::array<AprilTagFrameEvidence, 3> frames = {
      AprilTagFrameEvidence{
          .frame_index = 10U,
          .family = "tag36h11",
          .id = 0,
          .hamming = 0,
          .decision_margin = 42.0,
      },
      AprilTagFrameEvidence{
          .frame_index = 20U,
          .family = "tag36h11",
          .id = 0,
          .hamming = 1,
          .decision_margin = 30.0,
      },
      AprilTagFrameEvidence{
          .frame_index = 30U,
          .family = "tag36h11",
          .id = 0,
          .hamming = 0,
          .decision_margin = 35.0,
      },
  };
  ValidateRequiredAprilTagPersistence(frames);
  frames[1].id = 1;
  ExpectFailure([&] { ValidateRequiredAprilTagPersistence(frames); });
  frames[1].id = 0;
  frames[2].decision_margin = 9.9;
  ExpectFailure([&] { ValidateRequiredAprilTagPersistence(frames); });
}

}  // namespace

int main() {
  NominalDualSessionPasses();
  InvalidEvidenceFails();
  ConservativeTimingBoundIncludesResidual();
  RequiredAprilTagMustPersist();
}
