#include "android/pose_hil/pose_standby_validation.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace pose_hil = swing_capture::android::pose_hil;

namespace {

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::string Status(std::uint64_t elapsed_ns, std::uint64_t offered, std::uint64_t scheduled,
                   std::uint64_t dropped, std::uint64_t successful, std::uint64_t failed,
                   std::string phase = "monitoring", std::uint64_t audio_end = 48'000,
                   std::uint64_t timestamp_rejections = 1, std::uint64_t audio_drops = 0,
                   std::uint64_t discontinuities = 0, std::string last_error = "",
                   std::uint64_t encoded_evidence_frames = 0,
                   std::uint64_t inference_p95_ns = 180'000'000,
                   std::uint64_t inference_max_ns = 190'000'000,
                   std::uint64_t inference_outliers = 0,
                   std::string configured_delegate = "gpu_preferred",
                   std::string actual_delegate = "gpu", std::uint64_t successful_warmup = 1,
                   std::uint64_t failed_warmup = 0, std::uint64_t warmup_duration_ns = 714'000'000,
                   std::uint64_t rejected_decision_timestamps = 0) {
  const std::uint64_t warmup_count = successful_warmup + failed_warmup;
  const bool has_steady_inference = successful + failed != 0;
  const bool has_decision = successful != 0;
  return "{\"schema_version\":2,\"state\":\"armed\",\"armed\":true,"
         "\"server_elapsed_realtime_ns\":\"" +
         std::to_string(elapsed_ns) +
         "\",\"pose\":{\"mode\":\"shadow\","
         "\"configured_delegate\":\"" +
         configured_delegate + "\",\"phase\":\"" + phase +
         "\",\"process_cpu_time_ms\":12345,\"metrics\":{\"delegate\":\"" + actual_delegate +
         "\",\"offered_images\":" + std::to_string(offered + warmup_count) +
         " ,\"scheduled_images\":" + std::to_string(scheduled + warmup_count) +
         " ,\"dropped_images\":" + std::to_string(dropped) +
         " ,\"successful_warmup_inferences\":" + std::to_string(successful_warmup) +
         " ,\"failed_warmup_inferences\":" + std::to_string(failed_warmup) +
         " ,\"total_warmup_duration_ns\":" +
         std::to_string(warmup_count == 0 ? 0 : warmup_duration_ns) +
         " ,\"maximum_warmup_duration_ns\":" +
         std::to_string(warmup_count == 0 ? 0 : warmup_duration_ns) +
         " ,\"successful_inferences\":" + std::to_string(successful) +
         " ,\"failed_inferences\":" + std::to_string(failed) +
         " ,\"maximum_inference_duration_ns\":" + std::to_string(inference_max_ns) +
         " ,\"inference_duration_p50_ns\":" +
         std::to_string(has_steady_inference ? 120'000'000 : 0) +
         " ,\"inference_duration_p90_ns\":" +
         std::to_string(has_steady_inference ? 160'000'000 : 0) +
         " ,\"inference_duration_p95_ns\":" + std::to_string(inference_p95_ns) +
         " ,\"inference_duration_p99_ns\":" + std::to_string(inference_max_ns) +
         " ,\"inference_deadline_ns\":200000000"
         " ,\"inference_outlier_bound_ns\":400000000"
         " ,\"inference_deadline_misses\":" +
         std::to_string(inference_p95_ns > 200'000'000 || inference_outliers > 0 ? 1 : 0) +
         " ,\"inference_outliers\":" + std::to_string(inference_outliers) +
         " ,\"decision_age_samples\":" + std::to_string(successful - rejected_decision_timestamps) +
         " ,\"rejected_decision_timestamps\":" + std::to_string(rejected_decision_timestamps) +
         " ,\"maximum_decision_age_ns\":" + std::to_string(has_decision ? 250'000'000 : 0) +
         " ,\"decision_age_p50_ns\":" + std::to_string(has_decision ? 180'000'000 : 0) +
         " ,\"decision_age_p90_ns\":" + std::to_string(has_decision ? 220'000'000 : 0) +
         " ,\"decision_age_p95_ns\":" + std::to_string(has_decision ? 240'000'000 : 0) +
         " ,\"decision_age_p99_ns\":" + std::to_string(has_decision ? 250'000'000 : 0) +
         " ,\"encoded_evidence_frames\":" + std::to_string(encoded_evidence_frames) +
         "},\"standby_audio\":{\"ready\":true,\"audio_source\":7,"
         "\"end_frame_position\":\"" +
         std::to_string(audio_end) + "\",\"dropped_events\":" + std::to_string(audio_drops) +
         " ,\"timestamp_rejections\":" + std::to_string(timestamp_rejections) +
         " ,\"discontinuities\":" + std::to_string(discontinuities) + " ,\"last_error\":\"" +
         last_error + "\"}}}";
}

}  // namespace

int main() {
  const pose_hil::PoseStatusSample first =
      pose_hil::InspectPoseStatus(Status(10'000'000'000, 20, 20, 0, 20, 0, "monitoring", 48'000));
  const pose_hil::PoseStatusSample last =
      pose_hil::InspectPoseStatus(Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000));
  Check(first.valid && last.valid, "valid monitoring status accepted");
  Check(first.successful_warmup_inferences == 1 && first.failed_warmup_inferences == 0,
        "successful warm-up parsed independently from steady inference");
  Check(first.total_warmup_duration_ns == 714'000'000, "cold warm-up duration remains observable");
  Check(first.process_cpu_time_ms == 12'345, "process CPU time remains observable");
  std::string rounded_histogram = Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 1,
                                         0, 0, "", 0, 180'000'000, 190'123'456);
  const std::string exact_p99 = "\"inference_duration_p99_ns\":190123456";
  rounded_histogram.replace(rounded_histogram.find(exact_p99), exact_p99.size(),
                            "\"inference_duration_p99_ns\":191000000");
  Check(pose_hil::InspectPoseStatus(rounded_histogram).valid,
        "one-millisecond histogram upper bound may round above exact maximum");
  const pose_hil::PoseStatusSample evidence_ready = pose_hil::InspectPoseStatus(
      Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 1, 0, 0, "", 3));
  Check(evidence_ready.valid && evidence_ready.encoded_evidence_frames == 3,
        "completed evidence JPEG count parsed exactly");
  Check(pose_hil::HasMinimumEncodedPoseEvidence(evidence_ready, 3),
        "three completed evidence JPEGs satisfy readiness");
  Check(!pose_hil::HasMinimumEncodedPoseEvidence(evidence_ready, 4),
        "inference success does not substitute for a fourth completed JPEG");
  Check(!pose_hil::HasMinimumEncodedPoseEvidence(pose_hil::InspectPoseStatus("{}"), 0),
        "malformed status never satisfies evidence readiness");
  const pose_hil::PoseCadenceAcceptance accepted = pose_hil::EvaluatePoseCadence(first, last);
  Check(accepted.passed, "5 Hz cadence accepted");
  Check(accepted.successful_inferences == 12, "successful delta preserved");
  Check(accepted.dropped_images == 1, "drop delta preserved");
  Check(pose_hil::EvaluatePoseLatency(last).passed, "bounded pose latency accepted");

  const pose_hil::PoseStatusSample slow_tail =
      pose_hil::InspectPoseStatus(Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 1,
                                         0, 0, "", 0, 205'000'000, 250'000'000));
  Check(!pose_hil::EvaluatePoseLatency(slow_tail).passed,
        "pose inference p95 above 200 ms rejected");
  const pose_hil::PoseStatusSample outlier =
      pose_hil::InspectPoseStatus(Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 1,
                                         0, 0, "", 0, 180'000'000, 700'000'000, 1));
  Check(!pose_hil::EvaluatePoseLatency(outlier).passed,
        "pose inference outlier above 400 ms rejected");
  const pose_hil::PoseStatusSample rejected_decision = pose_hil::InspectPoseStatus(
      Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 1, 0, 0, "", 0, 180'000'000,
             190'000'000, 0, "gpu_preferred", "gpu", 1, 0, 714'000'000, 1));
  Check(rejected_decision.valid, "rejected timestamp remains structurally diagnosable");
  Check(!pose_hil::EvaluatePoseLatency(rejected_decision).passed,
        "rejected decision timestamp fails latency qualification");

  std::string missing_decision = Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000);
  const std::string complete_decision_count = "\"decision_age_samples\":32";
  missing_decision.replace(missing_decision.find(complete_decision_count),
                           complete_decision_count.size(), "\"decision_age_samples\":31");
  Check(!pose_hil::InspectPoseStatus(missing_decision).valid,
        "every successful inference requires decision-age accounting");

  const pose_hil::PoseStatusSample warming =
      pose_hil::InspectPoseStatus(Status(10'000'000'000, 1, 0, 1, 0, 0, "warming_up", 48'000, 1, 0,
                                         0, "", 0, 0, 0, 0, "gpu_preferred", "gpu", 0, 0, 0));
  Check(warming.valid, "pre-inference warming phase may expose an unscheduled startup drop");
  Check(!pose_hil::EvaluatePoseLatency(warming).passed,
        "warming phase cannot satisfy steady-state latency qualification");
  const pose_hil::PoseStatusSample failed_warmup = pose_hil::InspectPoseStatus(
      Status(10'000'000'000, 0, 0, 0, 0, 0, "warmup_failed", 48'000, 1, 0, 0, "", 0, 0, 0, 0,
             "gpu_preferred", "gpu", 0, 1, 333'000'000));
  Check(failed_warmup.valid, "failed warm-up remains diagnosable");
  Check(!pose_hil::EvaluatePoseLatency(failed_warmup).passed,
        "failed warm-up cannot satisfy latency qualification");

  const pose_hil::PoseStatusSample cpu = pose_hil::InspectPoseStatus(
      Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 1, 0, 0, "", 0, 180'000'000,
             190'000'000, 0, "cpu_only", "cpu"));
  Check(cpu.valid, "explicit CPU delegate policy accepted");
  const pose_hil::PoseStatusSample mismatched_delegate = pose_hil::InspectPoseStatus(
      Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 1, 0, 0, "", 0, 180'000'000,
             190'000'000, 0, "gpu_required", "cpu"));
  Check(!mismatched_delegate.valid, "required GPU cannot silently fall back to CPU");
  const pose_hil::PoseStatusSample npu = pose_hil::InspectPoseStatus(
      Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 1, 0, 0, "", 0, 180'000'000,
             190'000'000, 0, "npu_required", "npu"));
  Check(npu.valid, "explicit NPU delegate policy accepted");

  const pose_hil::PoseCadenceAcceptance slow = pose_hil::EvaluatePoseCadence(
      first,
      pose_hil::InspectPoseStatus(Status(12'500'000'000, 29, 29, 0, 29, 0, "monitoring", 168'000)));
  Check(!slow.passed && slow.diagnostic.find("below 4 Hz") != std::string::npos,
        "slow cadence rejected");

  const pose_hil::PoseCadenceAcceptance failed = pose_hil::EvaluatePoseCadence(
      first,
      pose_hil::InspectPoseStatus(Status(12'500'000'000, 33, 33, 0, 32, 1, "monitoring", 168'000)));
  Check(!failed.passed && failed.diagnostic.find("failed") != std::string::npos,
        "failed inference rejected");

  const pose_hil::PoseCadenceAcceptance excessive_drops = pose_hil::EvaluatePoseCadence(
      first,
      pose_hil::InspectPoseStatus(Status(12'500'000'000, 35, 35, 4, 31, 0, "monitoring", 168'000)));
  Check(!excessive_drops.passed && excessive_drops.diagnostic.find("dropped") != std::string::npos,
        "excessive drop fraction rejected");

  const pose_hil::PoseCadenceAcceptance wrong_phase = pose_hil::EvaluatePoseCadence(
      first,
      pose_hil::InspectPoseStatus(Status(12'500'000'000, 33, 33, 1, 32, 0, "high_speed", 168'000)));
  Check(!wrong_phase.passed, "phase transition rejected");
  Check(!pose_hil::EvaluatePoseCadence(
             first, pose_hil::InspectPoseStatus(
                        Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 48'000)))
             .passed,
        "nonadvancing standby audio rejected");
  Check(!pose_hil::EvaluatePoseCadence(
             first, pose_hil::InspectPoseStatus(
                        Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 3)))
             .passed,
        "excessive startup timestamp rejections rejected");
  Check(!pose_hil::EvaluatePoseCadence(
             first, pose_hil::InspectPoseStatus(
                        Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 1, 1)))
             .passed,
        "standby diagnostic audio event drop rejected");
  Check(!pose_hil::EvaluatePoseCadence(
             first, pose_hil::InspectPoseStatus(
                        Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000, 1, 0, 1)))
             .passed,
        "standby audio discontinuity rejected");
  Check(!pose_hil::EvaluatePoseCadence(first, pose_hil::InspectPoseStatus(Status(
                                                  12'500'000'000, 33, 33, 1, 32, 0, "monitoring",
                                                  168'000, 1, 0, 0, "audio_capture:Failure")))
             .passed,
        "standby audio error rejected");
  std::string unready_audio = Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000);
  unready_audio.replace(unready_audio.find("\"ready\":true"), 12, "\"ready\":false");
  Check(!pose_hil::EvaluatePoseCadence(first, pose_hil::InspectPoseStatus(unready_audio)).passed,
        "unready standby audio rejected");
  std::string missing_audio_source =
      Status(12'500'000'000, 33, 33, 1, 32, 0, "monitoring", 168'000);
  missing_audio_source.replace(missing_audio_source.find("\"audio_source\":7"), 16,
                               "\"audio_source\":null");
  Check(!pose_hil::InspectPoseStatus(missing_audio_source).valid,
        "null standby audio source rejected");
  Check(!pose_hil::InspectPoseStatus("{}").valid, "malformed status rejected");
  Check(!pose_hil::InspectPoseStatus(Status(10'000'000'000, 2, 3, 0, 2, 0)).valid,
        "impossible counters rejected");
  Check(!pose_hil::InspectPoseStatus(
             Status(10'000'000'000, 2, 2, 0, 2, 0, "monitoring", 48'000, 1, 0, 0, "", 3))
             .valid,
        "evidence count exceeding successful inference count rejected");
  std::string missing_encoded_evidence =
      Status(10'000'000'000, 2, 2, 0, 2, 0, "monitoring", 48'000, 1, 0, 0, "", 1);
  const std::string encoded_field = ",\"encoded_evidence_frames\":1";
  missing_encoded_evidence.erase(missing_encoded_evidence.find(encoded_field),
                                 encoded_field.size());
  Check(!pose_hil::InspectPoseStatus(missing_encoded_evidence).valid,
        "missing completed evidence JPEG count rejected");
  std::string missing_process_cpu = Status(10'000'000'000, 2, 2, 0, 2, 0);
  const std::string process_cpu_field = ",\"process_cpu_time_ms\":12345";
  missing_process_cpu.erase(missing_process_cpu.find(process_cpu_field), process_cpu_field.size());
  Check(!pose_hil::InspectPoseStatus(missing_process_cpu).valid,
        "missing process CPU counter rejected");

  const pose_hil::DeviceTelemetry telemetry =
      pose_hil::InspectDeviceTelemetry("  level: 87\n  voltage: 4210\n  temperature: 296\n",
                                       "IsStatusOverride: false\nThermal Status: 1\n");
  Check(telemetry.valid, "Pixel telemetry accepted");
  Check(telemetry.thermal_status == 1, "thermal status preserved");
  Check(telemetry.battery_temperature_celsius == 29.6, "battery temperature converted");
  const pose_hil::DeviceTelemetry processor_telemetry = pose_hil::InspectDeviceTelemetry(
      "level: 87\nvoltage: 4210\ntemperature: 296\n",
      "Thermal Status: 1\nCurrent temperatures from HAL:\n"
      " Temperature{mValue=45.0, mType=0, mName=LITTLE, mStatus=0}\n"
      " Temperature{mValue=52.5, mType=0, mName=BIG, mStatus=0}\n"
      " Temperature{mValue=47.25, mType=1, mName=G3D, mStatus=0}\n"
      "Current cooling devices from HAL:\n"
      " CoolingDevice{mValue=1, mType=2, mName=thermal-cpufreq-0}\n"
      " CoolingDevice{mValue=3, mType=2, mName=thermal-cpufreq-2}\n"
      " CoolingDevice{mValue=2, mType=3, mName=thermal-gpufreq-0}\n"
      "Temperature static thresholds from HAL:\n");
  Check(processor_telemetry.valid && processor_telemetry.processor_thermal_complete,
        "CPU and GPU thermal telemetry accepted");
  Check(processor_telemetry.maximum_cpu_temperature_celsius == 52.5,
        "hottest current CPU sensor preserved");
  Check(processor_telemetry.maximum_gpu_temperature_celsius == 47.25,
        "current GPU temperature preserved");
  Check(processor_telemetry.maximum_cpu_cooling_device_value == 3 &&
            processor_telemetry.maximum_gpu_cooling_device_value == 2,
        "maximum CPU and GPU cooling values preserved");
  Check(!pose_hil::InspectDeviceTelemetry("level: 50\n", "Thermal Status: 0\n").valid,
        "incomplete telemetry rejected");

  std::cout << "pose standby HIL validation tests passed\n";
  return 0;
}
