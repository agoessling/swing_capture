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
                   std::uint64_t encoded_evidence_frames = 0) {
  return "{\"schema_version\":2,\"state\":\"armed\",\"armed\":true,"
         "\"server_elapsed_realtime_ns\":\"" +
         std::to_string(elapsed_ns) +
         "\",\"pose\":{\"mode\":\"shadow\","
         "\"configured_delegate\":\"gpu_preferred\",\"phase\":\"" +
         phase +
         "\",\"metrics\":{\"delegate\":\"gpu\",\"offered_images\":" + std::to_string(offered) +
         " ,\"scheduled_images\":" + std::to_string(scheduled) +
         " ,\"dropped_images\":" + std::to_string(dropped) +
         " ,\"successful_inferences\":" + std::to_string(successful) +
         " ,\"failed_inferences\":" + std::to_string(failed) +
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

  const pose_hil::DeviceTelemetry telemetry =
      pose_hil::InspectDeviceTelemetry("  level: 87\n  voltage: 4210\n  temperature: 296\n",
                                       "IsStatusOverride: false\nThermal Status: 1\n");
  Check(telemetry.valid, "Pixel telemetry accepted");
  Check(telemetry.thermal_status == 1, "thermal status preserved");
  Check(telemetry.battery_temperature_celsius == 29.6, "battery temperature converted");
  Check(!pose_hil::InspectDeviceTelemetry("level: 50\n", "Thermal Status: 0\n").valid,
        "incomplete telemetry rejected");

  std::cout << "pose standby HIL validation tests passed\n";
  return 0;
}
