#include "android/dual_hil/paired_pose_qualification_policy.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "android/pose_hil/pose_qualification_performance.h"

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::android::dual_hil::IsPairedPoseQualificationMode;
using swing_capture::android::dual_hil::PairedPoseQualificationPolicyForMode;
using swing_capture::android::dual_hil::ValidatePairedPoseQualificationReport;
using swing_capture::android::pose_hil::BuildPoseQualificationPerformanceSummary;

Json RoleTelemetry(bool initial, bool cadence_eligible) {
  return {
      {"screen_off", true},
      {"phase", "monitoring"},
      {"state", "armed"},
      {"actual_delegate", "gpu"},
      {"thermal_status", 1},
      {"battery_level_percent", 80},
      {"processor_thermal_complete", true},
      {"maximum_cpu_temperature_celsius", 52.0},
      {"maximum_gpu_temperature_celsius", 48.0},
      {"maximum_cpu_cooling_device_value", 2},
      {"maximum_gpu_cooling_device_value", 1},
      {"interval_seconds", initial ? 0.0 : 30.0},
      {"process_cpu_time_delta_ms", initial ? 0 : 900},
      {"process_cpu_utilization_percent", initial ? 0.0 : 3.0},
      {"offered_images_delta", initial ? 0 : 150},
      {"successful_inferences_delta", initial ? 0 : 145},
      {"failed_inferences_delta", 0},
      {"dropped_images_delta", initial ? 0 : 5},
      {"inference_deadline_misses_delta", 0},
      {"inference_outliers_delta", 0},
      {"decision_age_samples_delta", initial ? 0 : 145},
      {"rejected_decision_timestamps_delta", 0},
      {"inference_duration_p95_ns", 175'000'000},
      {"maximum_inference_duration_ns", 230'000'000},
      {"decision_age_p95_ns", 260'000'000},
      {"maximum_decision_age_ns", 259'500'000},
      {"standby_audio_dropped_events", 0},
      {"standby_audio_timestamp_rejections", 0},
      {"standby_audio_discontinuities", 0},
      {"cadence_eligible", cadence_eligible},
      {"metrics_generation_reset", !initial && !cadence_eligible},
      {"successful_inferences_per_second", initial ? 0.0 : 4.83},
      {"dropped_fraction", initial ? 0.0 : 1.0 / 30.0},
  };
}

Json StartupTiming(std::size_t index, bool face_on) {
  const std::uint64_t first_camera = (face_on ? 900U : 1'500U) + index * 10U;
  const std::uint64_t first_usable = (face_on ? 1'200U : 2'100U) + index * 10U;
  const std::uint64_t full_pre_roll = (face_on ? 3'600U : 4'550U) + index * 10U;
  return {
      {"schema_version", 1},
      {"clock", "CLOCK_BOOTTIME"},
      {"continuity_clean", true},
      {"startup_continuity_reset_count", "0"},
      {"maximum_startup_continuity_gap_ns", "0"},
      {"durations",
       {{"arm_to_first_camera_frame_ns", std::to_string(first_camera)},
        {"arm_to_first_usable_encoded_frame_ns", std::to_string(first_usable)},
        {"arm_to_full_pre_roll_ready_ns", std::to_string(full_pre_roll)}}},
  };
}

Json PairNetworkDirection() {
  return {
      {"schema_version", 1},
      {"attempts", 3},
      {"successes", 3},
      {"timeouts", 0},
      {"round_trip_ns", Json::array({"1000000", "2000000", "3000000"})},
      {"transfer_bytes", "1048576"},
      {"transfer_duration_ns", "100000000"},
      {"transfer_complete", true},
      {"minimum_round_trip_ns", "1000000"},
      {"median_round_trip_ns", "2000000"},
      {"p95_round_trip_ns", "3000000"},
      {"maximum_round_trip_ns", "3000000"},
      {"jitter_ns", "2000000"},
      {"transfer_bits_per_second", 83'886'080.0},
  };
}

Json PairNetworkHealth(std::string_view state = "good") {
  return {
      {"schema_version", 1},
      {"configured", true},
      {"state", state},
      {"raw_state", state},
      {"measured", true},
      {"stale", false},
      {"transition_pending", false},
      {"age_ns", "1000000"},
      {"issues",
       state == "good" ? Json::array() : Json::array({"pair network health is not good"})},
      {"peer", {{"origin", "http://10.0.0.5:8088"}, {"node_id", "shadow-node"}}},
      {"measured_at_elapsed_realtime_ns", "9000000"},
      {"local_to_peer", PairNetworkDirection()},
      {"peer_to_local", PairNetworkDirection()},
  };
}

Json PassingReport(std::string_view mode) {
  const auto &policy = PairedPoseQualificationPolicyForMode(mode);
  Json cycles = Json::array();
  Json cycle_artifacts = Json::array();
  for (std::size_t index = 0; index < policy.minimum_cycle_count; ++index) {
    const std::string prefix = "cycles/cycle-" + std::to_string(index) + "/";
    const Json artifacts = {
        {"cycle", prefix + "cycle.json"},
        {"coordination", prefix + "coordination.json"},
        {"face_on",
         {{"clock", prefix + "face_on/clock.json"},
          {"report", prefix + "face_on/report.json"},
          {"manifest", prefix + "face_on/manifest.json"},
          {"media", prefix + "face_on/face_on.mp4"},
          {"audio", prefix + "face_on/audio_evidence.wav"},
          {"ffprobe", prefix + "face_on/ffprobe.json"},
          {"diagnostic_frames",
           {{"pre", prefix + "face_on/diagnostic-01.png"},
            {"marker", prefix + "face_on/diagnostic-02.png"},
            {"post", prefix + "face_on/diagnostic-03.png"}}}}},
        {"down_the_line",
         {{"clock", prefix + "down_the_line/clock.json"},
          {"report", prefix + "down_the_line/report.json"},
          {"manifest", prefix + "down_the_line/manifest.json"},
          {"media", prefix + "down_the_line/down_the_line.mp4"},
          {"audio", prefix + "down_the_line/diagnostic_audio.wav"},
          {"ffprobe", prefix + "down_the_line/ffprobe.json"},
          {"diagnostic_frames",
           {{"pre", prefix + "down_the_line/diagnostic-01.png"},
            {"marker", prefix + "down_the_line/diagnostic-02.png"},
            {"post", prefix + "down_the_line/diagnostic-03.png"}}}}},
    };
    cycles.push_back({
        {"cycle_index", index},
        {"passed", true},
        {"artifact_path_scope", "undeclared_output_root"},
        {"shared_session_id", "cycle-" + std::to_string(index)},
        {"pose_to_high_speed", true},
        {"feather_audio_trigger", true},
        {"publication", {{"face_on", true}, {"down_the_line", true}}},
        {"rearmed", true},
        {"coordination", {{"canonical_record", artifacts.at("coordination")}}},
        {"nodes",
         {{"face_on",
           {{"passed", true},
            {"frame_count", 480},
            {"report", artifacts.at("face_on").at("report")},
            {"manifest", artifacts.at("face_on").at("manifest")},
            {"media", artifacts.at("face_on").at("media")},
            {"audio", artifacts.at("face_on").at("audio")},
            {"ffprobe", artifacts.at("face_on").at("ffprobe")},
            {"diagnostic_frames", artifacts.at("face_on").at("diagnostic_frames")},
            {"decoded_video",
             {{"passed", true},
              {"exact_frame_count", true},
              {"decoded_frame_count", 480},
              {"manifest_frame_count", 480},
              {"analysis_width", 640},
              {"analysis_height", 360}}},
            {"optical", {{"passed", true}, {"timing_correlation_passed", true}}},
            {"april_tag", {{"passed", true}, {"persistent_pre_marker_post", true}}},
            {"startup_timing", StartupTiming(index, true)}}},
          {"down_the_line",
           {{"passed", true},
            {"frame_count", 480},
            {"report", artifacts.at("down_the_line").at("report")},
            {"manifest", artifacts.at("down_the_line").at("manifest")},
            {"media", artifacts.at("down_the_line").at("media")},
            {"audio", artifacts.at("down_the_line").at("audio")},
            {"ffprobe", artifacts.at("down_the_line").at("ffprobe")},
            {"diagnostic_frames", artifacts.at("down_the_line").at("diagnostic_frames")},
            {"decoded_video",
             {{"passed", true},
              {"exact_frame_count", true},
              {"decoded_frame_count", 480},
              {"manifest_frame_count", 480},
              {"analysis_width", 640},
              {"analysis_height", 360}}},
            {"optical", {{"passed", true}, {"timing_correlation_passed", true}}},
            {"april_tag", {{"passed", true}, {"persistent_pre_marker_post", true}}},
            {"startup_timing", StartupTiming(index, false)}}}}},
        {"clock",
         {{"face_on", artifacts.at("face_on").at("clock")},
          {"down_the_line", artifacts.at("down_the_line").at("clock")}}},
        {"artifacts", artifacts},
    });
    cycle_artifacts.push_back(artifacts);
  }
  Json samples = Json::array();
  for (std::size_t index = 0; index <= policy.minimum_telemetry_sample_count; ++index) {
    samples.push_back({
        {"sample_index", index},
        {"elapsed_seconds", static_cast<double>(index * policy.telemetry_interval_seconds)},
        {"roles",
         {{"face_on", RoleTelemetry(index == 0, index > 0)},
          {"down_the_line", RoleTelemetry(index == 0, index > 0)}}},
    });
  }
  Json report = {
      {"schema_version", 1},
      {"report_type", policy.report_type},
      {"mode", policy.mode},
      {"passed", true},
      {"qualification_eligible", true},
      {"requested_duration_seconds", policy.duration_seconds},
      {"observed_duration_seconds", policy.duration_seconds},
      {"telemetry_interval_seconds", policy.telemetry_interval_seconds},
      {"cycle_interval_seconds", policy.cycle_interval_seconds},
      {"screen_off", {{"passed", true}, {"face_on", true}, {"down_the_line", true}}},
      {"high_speed_profile", "720p240"},
      {"standby_inference", "real_5hz_on_device"},
      {"peer_transport", "wifi_lan_direct"},
      {"artifact_path_scope", "undeclared_output_root"},
      {"setup",
       {{"leader_role", "face_on"},
        {"shadow_role", "down_the_line"},
        {"leader_node_id", "leader-node"},
        {"shadow_node_id", "shadow-node"},
        {"lan_endpoint_validation",
         {{"face_on",
           {{"role", "face_on"},
            {"pose_mode", "leader"},
            {"origin", "http://10.0.0.6:8088"},
            {"peer_origin", "http://10.0.0.5:8088"}}},
          {"down_the_line",
           {{"role", "down_the_line"},
            {"pose_mode", "shadow"},
            {"origin", "http://10.0.0.5:8088"},
            {"peer_origin", nullptr}}}}},
        {"pair_network_health_admission",
         {{"schema_version", 1},
          {"passed", true},
          {"poll_count", 3},
          {"elapsed_milliseconds", 500},
          {"accepted_state", "good"},
          {"degraded_override", false},
          {"expected_peer_origin", "http://10.0.0.5:8088"},
          {"expected_peer_node_id", "shadow-node"},
          {"snapshot", PairNetworkHealth()}}}}},
      {"cycles", std::move(cycles)},
      {"telemetry_samples", std::move(samples)},
      {"artifacts", {{"cycles", std::move(cycle_artifacts)}}},
  };
  const auto performance = BuildPoseQualificationPerformanceSummary(report.dump());
  assert(performance.valid);
  report["performance_summary"] = Json::parse(performance.summary_json);
  return report;
}

void CheckRejected(const Json &report, std::string_view mode) {
  assert(!ValidatePairedPoseQualificationReport(report.dump(), mode).passed);
}

void TestClosedModesAndDurations() {
  assert(IsPairedPoseQualificationMode("paired-pose-qualify-5m"));
  assert(IsPairedPoseQualificationMode("paired-pose-soak-30m"));
  assert(!IsPairedPoseQualificationMode("paired-pose-qualify-60s"));
  assert(PairedPoseQualificationPolicyForMode("paired-pose-qualify-5m").duration_seconds == 300);
  assert(PairedPoseQualificationPolicyForMode("paired-pose-soak-30m").duration_seconds == 1800);
  try {
    static_cast<void>(PairedPoseQualificationPolicyForMode("paired-pose-qualify-60s"));
    assert(false);
  } catch (const std::invalid_argument &) {
  }
}

void TestPassingReports() {
  for (const std::string_view mode : {"paired-pose-qualify-5m", "paired-pose-soak-30m"}) {
    const auto result = ValidatePairedPoseQualificationReport(PassingReport(mode).dump(), mode);
    assert(result.passed);
  }
}

void TestDurationAndModeCannotBeSpoofed() {
  Json shortened = PassingReport("paired-pose-qualify-5m");
  shortened["requested_duration_seconds"] = 60;
  CheckRejected(shortened, "paired-pose-qualify-5m");

  Json mislabeled = PassingReport("paired-pose-qualify-5m");
  mislabeled["mode"] = "paired-pose-soak-30m";
  CheckRejected(mislabeled, "paired-pose-qualify-5m");
  CheckRejected(PassingReport("paired-pose-qualify-5m"), "paired-pose-soak-30m");

  Json overlong = PassingReport("paired-pose-qualify-5m");
  overlong["observed_duration_seconds"] = 1'800;
  CheckRejected(overlong, "paired-pose-qualify-5m");
}

void TestIncompleteCyclesAndTelemetryFail() {
  Json report = PassingReport("paired-pose-qualify-5m");
  report["cycles"].erase(report["cycles"].begin());
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["cycles"][2]["rearmed"] = false;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["telemetry_samples"][3]["elapsed_seconds"] = 200.0;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["telemetry_samples"].erase(report["telemetry_samples"].begin() + 5,
                                    report["telemetry_samples"].end());
  CheckRejected(report, "paired-pose-qualify-5m");
}

void TestThermalCadenceDropAndLatencyFailuresFail() {
  Json report = PassingReport("paired-pose-qualify-5m");
  report["telemetry_samples"][4]["roles"]["down_the_line"]["thermal_status"] = 3;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["telemetry_samples"][4]["roles"]["face_on"]["successful_inferences_per_second"] = 3.49;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["telemetry_samples"][4]["roles"]["face_on"]["dropped_fraction"] = 0.21;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["telemetry_samples"][4]["roles"]["face_on"]["inference_duration_p95_ns"] = 200'000'001;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["telemetry_samples"][4]["roles"]["face_on"]["standby_audio_discontinuities"] = 1;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["telemetry_samples"][4]["roles"]["face_on"]["rejected_decision_timestamps_delta"] = 1;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["telemetry_samples"][4]["roles"]["face_on"]["decision_age_samples_delta"] = 144;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["telemetry_samples"][4]["roles"]["face_on"]["decision_age_p95_ns"] = 261'000'000;
  CheckRejected(report, "paired-pose-qualify-5m");
}

void TestMetricGenerationResetsAreBoundToCaptureIntervals() {
  Json report = PassingReport("paired-pose-qualify-5m");
  for (const std::string_view role : {"face_on", "down_the_line"}) {
    report["telemetry_samples"][2]["roles"][role]["cadence_eligible"] = false;
    report["telemetry_samples"][2]["roles"][role]["metrics_generation_reset"] = true;
  }
  assert(ValidatePairedPoseQualificationReport(report.dump(), "paired-pose-qualify-5m").passed);

  report["telemetry_samples"][3]["roles"]["face_on"]["metrics_generation_reset"] = true;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  for (const std::string_view role : {"face_on", "down_the_line"}) {
    report["telemetry_samples"][2]["roles"][role]["cadence_eligible"] = false;
  }
  CheckRejected(report, "paired-pose-qualify-5m");
}

void TestCycleArtifactPathsMustBeRootRelativeAndIndexed() {
  Json report = PassingReport("paired-pose-qualify-5m");
  report["cycles"][1]["nodes"]["face_on"]["report"] = "face_on/report.json";
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["artifacts"]["cycles"][1]["face_on"]["audio"] = "cycles/cycle-1/face_on/other.wav";
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["cycles"][1]["nodes"]["face_on"]["decoded_video"]["decoded_frame_count"] = 479;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["cycles"][1]["nodes"]["down_the_line"]["optical"]["timing_correlation_passed"] = false;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["artifact_path_scope"] = "cycle_directory";
  CheckRejected(report, "paired-pose-qualify-5m");
}

void TestPairNetworkHealthAdmissionIsFailClosed() {
  Json report = PassingReport("paired-pose-qualify-5m");
  report["setup"]["pair_network_health_admission"]["accepted_state"] = "degraded";
  report["setup"]["pair_network_health_admission"]["degraded_override"] = true;
  report["setup"]["pair_network_health_admission"]["snapshot"] = PairNetworkHealth("degraded");
  assert(ValidatePairedPoseQualificationReport(report.dump(), "paired-pose-qualify-5m").passed);

  report = PassingReport("paired-pose-qualify-5m");
  report["setup"].erase("pair_network_health_admission");
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["setup"]["pair_network_health_admission"] = "malformed";
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  Json &stale = report["setup"]["pair_network_health_admission"]["snapshot"];
  stale["state"] = "unusable";
  stale["raw_state"] = "unusable";
  stale["stale"] = true;
  stale["issues"] = Json::array({"pair network health evidence is stale"});
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["setup"]["pair_network_health_admission"]["expected_peer_node_id"] = "other-node";
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["setup"]["pair_network_health_admission"]["snapshot"]["peer"]["origin"] =
      "http://10.0.0.7:8088";
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["setup"]["pair_network_health_admission"]["degraded_override"] = true;
  CheckRejected(report, "paired-pose-qualify-5m");

  report = PassingReport("paired-pose-qualify-5m");
  report["setup"]["lan_endpoint_validation"]["face_on"]["peer_origin"] = "http://10.0.0.7:8088";
  CheckRejected(report, "paired-pose-qualify-5m");
}

}  // namespace

int main() {
  TestClosedModesAndDurations();
  TestPassingReports();
  TestDurationAndModeCannotBeSpoofed();
  TestIncompleteCyclesAndTelemetryFail();
  TestThermalCadenceDropAndLatencyFailuresFail();
  TestMetricGenerationResetsAreBoundToCaptureIntervals();
  TestCycleArtifactPathsMustBeRootRelativeAndIndexed();
  TestPairNetworkHealthAdmissionIsFailClosed();
}
