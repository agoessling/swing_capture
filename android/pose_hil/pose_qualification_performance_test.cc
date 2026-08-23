#include "android/pose_hil/pose_qualification_performance.h"

#include <cassert>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::android::pose_hil::BuildPoseQualificationPerformanceSummary;
using swing_capture::android::pose_hil::ValidatePoseQualificationPerformanceSummary;

Json RoleTelemetry(bool initial, std::uint64_t inference_p95_ns, double cpu_percent) {
  return {
      {"processor_thermal_complete", true},
      {"maximum_cpu_temperature_celsius", 51.25},
      {"maximum_gpu_temperature_celsius", 47.5},
      {"maximum_cpu_cooling_device_value", 2},
      {"maximum_gpu_cooling_device_value", 1},
      {"interval_seconds", initial ? 0.0 : 30.0},
      {"process_cpu_time_delta_ms", initial ? 0 : static_cast<std::uint64_t>(cpu_percent * 300.0)},
      {"process_cpu_utilization_percent", initial ? 0.0 : cpu_percent},
      {"offered_images_delta", initial ? 0 : 150},
      {"successful_inferences_delta", initial ? 0 : 145},
      {"failed_inferences_delta", 0},
      {"dropped_images_delta", initial ? 0 : 5},
      {"inference_deadline_misses_delta", 0},
      {"inference_outliers_delta", 0},
      {"decision_age_samples_delta", initial ? 0 : 145},
      {"rejected_decision_timestamps_delta", 0},
      {"dropped_fraction", initial ? 0.0 : 1.0 / 30.0},
      {"inference_duration_p95_ns", inference_p95_ns},
      {"maximum_inference_duration_ns", inference_p95_ns + 20'000'000},
      {"decision_age_p95_ns", inference_p95_ns + 60'000'000},
      {"maximum_decision_age_ns", inference_p95_ns + 80'000'000},
  };
}

Json Startup(std::uint64_t first_camera, std::uint64_t first_usable, std::uint64_t full_pre_roll) {
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

Json Report() {
  Json telemetry = Json::array();
  telemetry.push_back({
      {"roles",
       {{"face_on", RoleTelemetry(true, 170'000'000, 0.0)},
        {"down_the_line", RoleTelemetry(true, 190'000'000, 0.0)}}},
  });
  telemetry.push_back({
      {"roles",
       {{"face_on", RoleTelemetry(false, 175'000'000, 31.0)},
        {"down_the_line", RoleTelemetry(false, 195'000'000, 28.0)}}},
  });
  telemetry.push_back({
      {"roles",
       {{"face_on", RoleTelemetry(false, 180'000'000, 33.0)},
        {"down_the_line", RoleTelemetry(false, 198'000'000, 30.0)}}},
  });

  Json cycles = Json::array();
  constexpr std::size_t kStartupCycleCount = 20;
  for (std::size_t index = 0; index < kStartupCycleCount; ++index) {
    const bool outlier = index + 1 == kStartupCycleCount;
    const Json face_startup =
        outlier ? Startup(4'000, 5'000, 8'000)
                : Startup(900 + index * 10, 1'200 + index * 10, 3'600 + index * 10);
    const Json down_startup =
        outlier ? Startup(6'000, 7'000, 10'000)
                : Startup(1'500 + index * 10, 2'100 + index * 10, 4'550 + index * 10);
    cycles.push_back({
        {"nodes",
         {{"face_on", {{"startup_timing", face_startup}}},
          {"down_the_line", {{"startup_timing", down_startup}}}}},
    });
  }
  return {{"telemetry_samples", std::move(telemetry)}, {"cycles", std::move(cycles)}};
}

void EmbedSummary(Json *report) {
  const auto built = BuildPoseQualificationPerformanceSummary(report->dump());
  assert(built.valid);
  (*report)["performance_summary"] = Json::parse(built.summary_json);
}

}  // namespace

int main() {
  Json report = Report();
  const auto built = BuildPoseQualificationPerformanceSummary(report.dump());
  assert(built.valid);
  const Json summary = Json::parse(built.summary_json);
  const Json &face = summary.at("roles").at("face_on");
  const Json &down = summary.at("roles").at("down_the_line");
  assert(face.at("telemetry_interval_count") == 2);
  assert(face.at("inference").at("total_successful_inferences") == 290);
  assert(face.at("inference").at("worst_inference_duration_p95_ns") == "180000000");
  assert(face.at("workload").at("maximum_process_cpu_utilization_percent") == 33.0);
  const Json &face_startup = face.at("startup_under_contention");
  const Json &down_startup = down.at("startup_under_contention");
  assert(face_startup.at("sample_count") == 20);
  assert(face_startup.at("arm_to_first_camera_frame_p95_ns") == "1080");
  assert(face_startup.at("arm_to_first_camera_frame_max_ns") == "4000");
  assert(face_startup.at("arm_to_first_usable_encoded_frame_p95_ns") == "1380");
  assert(face_startup.at("arm_to_first_usable_encoded_frame_max_ns") == "5000");
  assert(face_startup.at("arm_to_full_pre_roll_ready_p95_ns") == "3780");
  assert(face_startup.at("arm_to_full_pre_roll_ready_max_ns") == "8000");
  assert(down_startup.at("sample_count") == 20);
  assert(down_startup.at("arm_to_first_camera_frame_p95_ns") == "1680");
  assert(down_startup.at("arm_to_first_camera_frame_max_ns") == "6000");
  assert(down_startup.at("arm_to_first_usable_encoded_frame_p95_ns") == "2280");
  assert(down_startup.at("arm_to_first_usable_encoded_frame_max_ns") == "7000");
  assert(down_startup.at("arm_to_full_pre_roll_ready_p95_ns") == "4730");
  assert(down_startup.at("arm_to_full_pre_roll_ready_max_ns") == "10000");
  assert(summary.at("startup_bound_policy") == "measurement_only");

  EmbedSummary(&report);
  assert(ValidatePoseQualificationPerformanceSummary(report.dump()).valid);

  Json stale = report;
  stale["performance_summary"]["roles"]["face_on"]["inference"]["total_successful_inferences"] =
      291;
  assert(!ValidatePoseQualificationPerformanceSummary(stale.dump()).valid);

  Json missing_processor_thermal = Report();
  missing_processor_thermal["telemetry_samples"][1]["roles"]["face_on"]
                           ["processor_thermal_complete"] = false;
  assert(!BuildPoseQualificationPerformanceSummary(missing_processor_thermal.dump()).valid);

  Json missing_cpu_work = Report();
  missing_cpu_work["telemetry_samples"][1]["roles"]["face_on"]["process_cpu_time_delta_ms"] = 0;
  assert(!BuildPoseQualificationPerformanceSummary(missing_cpu_work.dump()).valid);

  Json discontinuous_startup = Report();
  discontinuous_startup["cycles"][0]["nodes"]["down_the_line"]["startup_timing"]
                       ["continuity_clean"] = false;
  assert(!BuildPoseQualificationPerformanceSummary(discontinuous_startup.dump()).valid);
}
