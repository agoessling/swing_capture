#include "android/hil/pose_replay_report_validation.h"

#include <cassert>
#include <nlohmann/json.hpp>
#include <string>

namespace {

nlohmann::json ValidReport() {
  return {
      {"schema_version", 1},
      {"report_type", "pose_replay_hil"},
      {"passed", true},
      {"outcome", "no_arm_request"},
      {"failure_code", "none"},
      {"source_id", "address.mp4"},
      {"role", "face_on"},
      {"projection", "atl"},
      {"delegate_policy", "cpu_only"},
      {"actual_delegate", "cpu"},
      {"expectation", "observe_only"},
      {"maximum_frames", 600},
      {"hitting_region", {{"left", 0.2}, {"top", 0.3}, {"right", 0.8}, {"bottom", 1.0}}},
      {"frame_count", 1},
      {"first_source_timestamp_ns", 0},
      {"last_source_timestamp_ns", 0},
      {"arm_request_count", 0},
      {"first_arm_request_ns", nullptr},
      {"final_state", "watching"},
      {"inference_metrics",
       {{"offered", 1},
        {"cadence_rejected", 0},
        {"scheduled", 1},
        {"dropped_backpressure", 0},
        {"succeeded", 1},
        {"failed", 0},
        {"total_inference_ns", 5},
        {"max_inference_ns", 5}}},
      {"arm_decisions", nlohmann::json::array()},
      {"trace",
       {{{"sequence_index", 0},
         {"source_timestamp_ns", 0},
         {"inference_started_ns", 10},
         {"inference_duration_ns", 5},
         {"pose_count", 0},
         {"observation",
          {{"person_confidence", 0.0},
           {"address_confidence", 0.0},
           {"motion_magnitude", 0.0},
           {"inside_hitting_region", false}}},
         {"decision",
          {{"state", "watching"},
           {"command", "none"},
           {"transition_reason", "waiting_for_address"},
           {"qualification_started_ns", nullptr},
           {"arm_requested_ns", nullptr},
           {"active_until_ns", nullptr},
           {"thermal_hard_stop_ns", nullptr}}}}}},
  };
}

}  // namespace

int main() {
  using swing_capture::android::hil::InspectPoseReplayReport;

  const auto valid = InspectPoseReplayReport(ValidReport().dump());
  assert(valid.valid);
  assert(valid.passed);
  assert(valid.frame_count == 1U);
  assert(valid.arm_request_count == 0U);
  assert(valid.first_source_timestamp_ns == 0U);

  auto npu = ValidReport();
  npu["delegate_policy"] = "npu_required";
  npu["actual_delegate"] = "npu";
  assert(InspectPoseReplayReport(npu.dump()).valid);

  auto contradictory = ValidReport();
  contradictory["first_source_timestamp_ns"] = 1;
  const auto invalid_timestamp = InspectPoseReplayReport(contradictory.dump());
  assert(!invalid_timestamp.valid);
  assert(!invalid_timestamp.diagnostic.empty());

  contradictory = ValidReport();
  contradictory["inference_metrics"]["succeeded"] = 0;
  assert(!InspectPoseReplayReport(contradictory.dump()).valid);

  contradictory = ValidReport();
  contradictory["trace"][0]["decision"]["state"] = "arm_requested";
  contradictory["final_state"] = "arm_requested";
  assert(!InspectPoseReplayReport(contradictory.dump()).valid);

  contradictory = ValidReport();
  contradictory["inference_metrics"]["offered"] = 2;
  contradictory["inference_metrics"]["scheduled"] = 2;
  contradictory["inference_metrics"]["failed"] = 1;
  assert(!InspectPoseReplayReport(contradictory.dump()).valid);

  auto runtime_failure = ValidReport();
  runtime_failure["passed"] = false;
  runtime_failure["outcome"] = "runtime_failure";
  runtime_failure["failure_code"] = "runtime_failure";
  runtime_failure["actual_delegate"] = nullptr;
  runtime_failure["frame_count"] = 0;
  runtime_failure["first_source_timestamp_ns"] = nullptr;
  runtime_failure["last_source_timestamp_ns"] = nullptr;
  runtime_failure["trace"] = nlohmann::json::array();
  runtime_failure["inference_metrics"]["offered"] = 0;
  runtime_failure["inference_metrics"]["scheduled"] = 0;
  runtime_failure["inference_metrics"]["succeeded"] = 0;
  runtime_failure["inference_metrics"]["total_inference_ns"] = 0;
  runtime_failure["inference_metrics"]["max_inference_ns"] = 0;
  assert(InspectPoseReplayReport(runtime_failure.dump()).valid);

  assert(!InspectPoseReplayReport(
              std::string(swing_capture::android::hil::kPoseReplayMaximumReportBytes + 1U, 'x'))
              .valid);
}
