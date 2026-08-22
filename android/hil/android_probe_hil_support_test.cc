#include "android/hil/android_probe_hil_support.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using swing_capture::android::hil::DisplayPowerStateDumps;
using swing_capture::android::hil::FinishContinuousSoakActivityArguments;
using swing_capture::android::hil::InspectContinuousSoakTelemetry;
using swing_capture::android::hil::InspectContinuousStartupTiming;
using swing_capture::android::hil::InspectDisplayPowerState;
using swing_capture::android::hil::InspectProbeReport;
using swing_capture::android::hil::InspectRetainedManifest;
using swing_capture::android::hil::InspectRetainedSessionReport;
using swing_capture::android::hil::InspectWarmTransitionTiming;
using swing_capture::android::hil::IsCaptureRole;
using swing_capture::android::hil::ProbeReportInspection;
using swing_capture::android::hil::ProbeRequestConfiguration;
using swing_capture::android::hil::RetainedManifestInspection;
using swing_capture::android::hil::StartActivityArguments;
using swing_capture::android::hil::StartContinuousActivityArguments;
using swing_capture::android::hil::StartWarmTransitionActivityArguments;

std::string ValidContinuousStartupReport() {
  return R"({
    "report_type":"android_continuous_capture",
    "complete":true,
    "role":"face_on",
    "passed":true,
    "startup_timing":{
      "arm_requested_elapsed_realtime_ns":"100",
      "engine_started_elapsed_realtime_ns":"120",
      "first_camera_frame_elapsed_realtime_ns":"210",
      "first_usable_encoded_frame_elapsed_realtime_ns":"260",
      "full_pre_roll_ready_elapsed_realtime_ns":"2800",
      "arm_to_engine_start_ns":"20",
      "arm_to_first_camera_frame_ns":"110",
      "arm_to_first_usable_encoded_frame_ns":"160",
      "arm_to_full_pre_roll_ready_ns":"2700",
      "first_usable_encoded_frame_to_full_pre_roll_ready_ns":"2540",
      "startup_continuity_reset_count":"0",
      "maximum_startup_continuity_gap_ns":"0"
    }
  })";
}

std::string ValidWarmTransitionReport() {
  return R"({
    "report_type":"android_warm_high_speed_transition",
    "complete":true,
    "role":"face_on",
    "passed":true,
    "standby":{
      "width":640,
      "height":360,
      "requested_interval_ms":200,
      "sensor_timestamps":{
        "count":6,
        "first":"1000000000",
        "last":"2000000000",
        "maximum_gap":"210000000"
      }
    },
    "high_speed_camera":{
      "camera_open_count":1,
      "sensor_timestamps":{"count":700},
      "transition_timing":{
        "transition_requested_elapsed_realtime_ns":"100",
        "encoder_started_elapsed_realtime_ns":"120",
        "standby_session_closed_elapsed_realtime_ns":"160",
        "high_speed_session_configured_elapsed_realtime_ns":"240",
        "first_high_speed_camera_frame_elapsed_realtime_ns":"260",
        "first_usable_encoded_frame_elapsed_realtime_ns":"310",
        "transition_to_encoder_start_ns":"20",
        "transition_to_standby_session_closed_ns":"60",
        "transition_to_high_speed_session_configured_ns":"140",
        "transition_to_first_high_speed_camera_frame_ns":"160",
        "transition_to_first_usable_encoded_frame_ns":"210"
      }
    },
    "encoder":{
      "acceptance_presentation_timestamps":{
        "count":690,
        "first":"1000000",
        "last":"3880000",
        "maximum_gap":"5000"
      }
    }
  })";
}

std::string ReplaceFieldValue(std::string report, std::string_view field,
                              std::string_view replacement) {
  const std::string marker = "\"" + std::string(field) + "\":\"";
  const std::size_t marker_position = report.find(marker);
  assert(marker_position != std::string::npos);
  const std::size_t value_position = marker_position + marker.size();
  const std::size_t value_end = report.find('"', value_position);
  assert(value_end != std::string::npos);
  report.replace(value_position, value_end - value_position, replacement);
  return report;
}

std::string UnquoteFieldValue(std::string report, std::string_view field) {
  const std::string marker = "\"" + std::string(field) + "\":\"";
  const std::size_t marker_position = report.find(marker);
  assert(marker_position != std::string::npos);
  const std::size_t value_position = marker_position + marker.size();
  const std::size_t value_end = report.find('"', value_position);
  assert(value_end != std::string::npos);
  report.erase(value_end, 1U);
  report.erase(value_position - 1U, 1U);
  return report;
}

void TestCaptureRoles() {
  assert(IsCaptureRole("down_the_line"));
  assert(IsCaptureRole("face_on"));
  assert(!IsCaptureRole("unassigned"));
  assert(!IsCaptureRole("dtl"));
}

void TestActivityArguments() {
  const std::vector<std::string> arguments = StartActivityArguments("example-serial", "face_on");
  const std::vector<std::string> expected = {
      "-s",
      "example-serial",
      "shell",
      "am",
      "start",
      "-W",
      "-n",
      "com.agoessling.swingcapture/.MainActivity",
      "--es",
      "role",
      "face_on",
      "--es",
      "capture_profile",
      "720p240",
      "--ez",
      "run_probe",
      "true",
      "--ei",
      "probe_width",
      "1280",
      "--ei",
      "probe_height",
      "720",
      "--ei",
      "probe_fps",
      "240",
      "--ei",
      "probe_duration_ms",
      "3000",
      "--ei",
      "probe_bitrate",
      "12000000",
      "--es",
      "probe_mime",
      "video/avc",
  };
  assert(arguments == expected);

  const std::vector<std::string> retained_arguments =
      StartActivityArguments("example-serial", "face_on", true);
  assert(retained_arguments.size() == expected.size() + 3U);
  assert(retained_arguments[retained_arguments.size() - 3U] == "--ez");
  assert(retained_arguments[retained_arguments.size() - 2U] == "retain_clip");
  assert(retained_arguments.back() == "true");

  ProbeRequestConfiguration full_hd;
  full_hd.profile = "1080p240";
  full_hd.width = 1920;
  full_hd.height = 1080;
  full_hd.bitrate_bits_per_second = 24000000;
  const std::vector<std::string> full_hd_arguments =
      StartActivityArguments("example-serial", "face_on", false, full_hd);
  assert(std::ranges::find(full_hd_arguments, "1920") != full_hd_arguments.end());
  assert(std::ranges::find(full_hd_arguments, "24000000") != full_hd_arguments.end());

  const std::vector<std::string> continuous_arguments =
      StartContinuousActivityArguments("example-serial", "down_the_line", {}, true);
  const auto shared_key = std::ranges::find(continuous_arguments, "shared_session_id");
  assert(shared_key != continuous_arguments.end());
  assert(std::next(shared_key) != continuous_arguments.end());
  assert(*std::next(shared_key) == "android-hil-shared-session");
  assert(std::ranges::find(continuous_arguments, "run_audio_hil") != continuous_arguments.end());

  const std::vector<std::string> soak_arguments =
      StartContinuousActivityArguments("example-serial", "down_the_line", {}, true, true);
  assert(std::ranges::find(soak_arguments, "run_audio_soak_hil") != soak_arguments.end());

  const std::vector<std::string> finish_arguments =
      FinishContinuousSoakActivityArguments("example-serial");
  assert(std::ranges::find(finish_arguments, "--activity-single-top") != finish_arguments.end());
  assert(std::ranges::find(finish_arguments, "finish_audio_soak_hil") != finish_arguments.end());

  const std::vector<std::string> warm_arguments =
      StartWarmTransitionActivityArguments("example-serial", "face_on");
  assert(std::ranges::find(warm_arguments, "run_warm_transition_hil") != warm_arguments.end());
  assert(std::ranges::find(warm_arguments, "run_probe") == warm_arguments.end());
}

void TestWarmTransitionTiming() {
  const std::string valid_report = ValidWarmTransitionReport();
  const auto valid = InspectWarmTransitionTiming(valid_report);
  assert(valid.valid);
  assert(valid.transition_to_first_camera_frame_ns == 160U);
  assert(valid.transition_to_first_encoded_frame_ns == 210U);

  assert(!InspectWarmTransitionTiming(
              ReplaceFieldValue(valid_report, "encoder_started_elapsed_realtime_ns", "99"))
              .valid);
  assert(!InspectWarmTransitionTiming(
              ReplaceFieldValue(valid_report, "transition_to_first_usable_encoded_frame_ns", "211"))
              .valid);
  assert(!InspectWarmTransitionTiming(
              ReplaceFieldValue(valid_report, "first_usable_encoded_frame_elapsed_realtime_ns",
                                "+310"))
              .valid);

  std::string wrong_open_count = valid_report;
  const std::string marker = "\"camera_open_count\":1";
  const std::size_t marker_position = wrong_open_count.find(marker);
  assert(marker_position != std::string::npos);
  wrong_open_count.replace(marker_position, marker.size(), "\"camera_open_count\":2");
  assert(!InspectWarmTransitionTiming(wrong_open_count).valid);

  assert(!InspectWarmTransitionTiming(ReplaceFieldValue(valid_report, "maximum_gap", "300000001"))
              .valid);
}

void TestContinuousSoakTelemetry() {
  constexpr std::string_view report = R"({
    "report_type":"android_continuous_capture",
    "complete":false,
    "state":"armed_waiting_audio",
    "runtime_telemetry":{
      "elapsed_realtime_ns":"123456789",
      "thermal_status":2,
      "video_frames":7200,
      "audio_frames":1440000,
      "ring_bytes":50331648,
      "ring_duration_us":3990000
    }
  })";
  const auto telemetry = InspectContinuousSoakTelemetry(report);
  assert(telemetry.valid);
  assert(telemetry.elapsed_realtime_ns == 123456789U);
  assert(telemetry.thermal_status == 2);
  assert(telemetry.video_frames == 7200U);
  assert(telemetry.audio_frames == 1440000U);

  constexpr std::string_view missing_ring = R"({
    "report_type":"android_continuous_capture",
    "complete":false,
    "state":"armed_waiting_audio",
    "runtime_telemetry":{
      "elapsed_realtime_ns":"123456789",
      "thermal_status":0,
      "video_frames":7200,
      "audio_frames":1440000,
      "ring_bytes":50331648,
      "ring_duration_us":1000000
    }
  })";
  assert(!InspectContinuousSoakTelemetry(missing_ring).valid);
}

void TestContinuousStartupTiming() {
  const std::string valid_report = ValidContinuousStartupReport();
  const auto valid = InspectContinuousStartupTiming(valid_report);
  assert(valid.valid);

  std::string reset_report = ReplaceFieldValue(valid_report, "startup_continuity_reset_count", "2");
  reset_report = ReplaceFieldValue(reset_report, "maximum_startup_continuity_gap_ns", "20000001");
  assert(InspectContinuousStartupTiming(reset_report).valid);

  const auto missing = InspectContinuousStartupTiming(
      R"({"report_type":"android_continuous_capture","passed":true})");
  assert(!missing.valid);

  constexpr std::array<std::string_view, 5> absolute_fields = {
      "arm_requested_elapsed_realtime_ns",       "engine_started_elapsed_realtime_ns",
      "first_camera_frame_elapsed_realtime_ns",  "first_usable_encoded_frame_elapsed_realtime_ns",
      "full_pre_roll_ready_elapsed_realtime_ns",
  };
  for (const std::string_view field : absolute_fields) {
    assert(
        !InspectContinuousStartupTiming(ReplaceFieldValue(valid_report, field, "invalid")).valid);
  }
  assert(!InspectContinuousStartupTiming(
              UnquoteFieldValue(valid_report, "arm_requested_elapsed_realtime_ns"))
              .valid);
  assert(!InspectContinuousStartupTiming(
              ReplaceFieldValue(valid_report, "arm_requested_elapsed_realtime_ns", "+100"))
              .valid);
  assert(!InspectContinuousStartupTiming(
              ReplaceFieldValue(valid_report, "arm_requested_elapsed_realtime_ns", "0100"))
              .valid);
  assert(!InspectContinuousStartupTiming(
              ReplaceFieldValue(valid_report, "arm_requested_elapsed_realtime_ns", "-1"))
              .valid);
  assert(!InspectContinuousStartupTiming(ReplaceFieldValue(valid_report,
                                                           "arm_requested_elapsed_realtime_ns",
                                                           "9223372036854775808"))
              .valid);

  assert(!InspectContinuousStartupTiming(
              ReplaceFieldValue(valid_report, "engine_started_elapsed_realtime_ns", "99"))
              .valid);
  assert(!InspectContinuousStartupTiming(
              ReplaceFieldValue(valid_report, "first_camera_frame_elapsed_realtime_ns", "119"))
              .valid);
  assert(
      !InspectContinuousStartupTiming(
           ReplaceFieldValue(valid_report, "first_usable_encoded_frame_elapsed_realtime_ns", "209"))
           .valid);
  assert(!InspectContinuousStartupTiming(
              ReplaceFieldValue(valid_report, "full_pre_roll_ready_elapsed_realtime_ns", "259"))
              .valid);

  constexpr std::array<std::string_view, 5> duration_fields = {
      "arm_to_engine_start_ns",
      "arm_to_first_camera_frame_ns",
      "arm_to_first_usable_encoded_frame_ns",
      "arm_to_full_pre_roll_ready_ns",
      "first_usable_encoded_frame_to_full_pre_roll_ready_ns",
  };
  for (const std::string_view field : duration_fields) {
    assert(!InspectContinuousStartupTiming(ReplaceFieldValue(valid_report, field, "1")).valid);
  }
  assert(!InspectContinuousStartupTiming(UnquoteFieldValue(valid_report, "arm_to_engine_start_ns"))
              .valid);

  assert(!InspectContinuousStartupTiming(
              ReplaceFieldValue(valid_report, "startup_continuity_reset_count", "1"))
              .valid);
  assert(!InspectContinuousStartupTiming(
              ReplaceFieldValue(valid_report, "maximum_startup_continuity_gap_ns", "1"))
              .valid);
  assert(!InspectContinuousStartupTiming(
              ReplaceFieldValue(valid_report, "startup_continuity_reset_count", "-1"))
              .valid);
  assert(!InspectContinuousStartupTiming(
              ReplaceFieldValue(valid_report, "maximum_startup_continuity_gap_ns", "1.0"))
              .valid);
  assert(!InspectContinuousStartupTiming(
              UnquoteFieldValue(valid_report, "startup_continuity_reset_count"))
              .valid);
}

void TestRetainedSessionInspection() {
  constexpr std::string_view report = R"({
    "report_type":"android_high_speed_probe",
    "role":"down_the_line",
    "passed":true,
    "retain_session_requested":true,
    "retained_session":{
      "session_id":"android-123-abcd",
      "manifest":"sessions/android-123-abcd/manifest.json",
      "media":"sessions/android-123-abcd/down_the_line.mp4",
      "encoded_bytes":4096
    }
  })";
  const auto paths = InspectRetainedSessionReport(ProbeReportInspection{
      .report = report,
      .expected_role = "down_the_line",
      .expected_request = std::nullopt,
  });
  assert(paths.valid);
  assert(paths.session_id == "android-123-abcd");
  assert(paths.encoded_bytes == 4096U);

  constexpr std::string_view manifest = R"({
    "schema_version":1,
    "session_id":"android-123-abcd",
    "trigger":{"source":"local_audio"},
    "views":[{
      "role":"down_the_line",
      "frame_count":2,
      "impact_frame_index":1,
      "source":{"pixel_format":"camera2_private"},
      "media":{
        "mime_type":"video/mp4",
        "all_frames_keyframes":false,
        "encoded_bytes":4096
      },
      "frames":[{},{}]
    }]
  })";
  const auto status = InspectRetainedManifest(RetainedManifestInspection{
      .manifest = manifest,
      .expected_session_id = paths.session_id,
      .expected_role = "down_the_line",
      .expected_trigger_source = "",
      .expected_shared_session_id = "",
  });
  assert(status.complete);
  assert(status.passed);

  const auto manual_or_audio = InspectRetainedManifest(RetainedManifestInspection{
      .manifest = manifest,
      .expected_session_id = paths.session_id,
      .expected_role = "down_the_line",
      .expected_trigger_source = "manual_or_local_audio",
      .expected_shared_session_id = "",
  });
  assert(manual_or_audio.complete);
  assert(manual_or_audio.passed);

  const auto wrong_role = InspectRetainedManifest(RetainedManifestInspection{
      .manifest = manifest,
      .expected_session_id = paths.session_id,
      .expected_role = "face_on",
      .expected_trigger_source = "",
      .expected_shared_session_id = "",
  });
  assert(wrong_role.complete);
  assert(!wrong_role.passed);
}

void TestReportInspection() {
  const auto passed = InspectProbeReport(ProbeReportInspection{
      .report = R"({"report_type":"android_high_speed_probe","role":"face_on","passed":true})",
      .expected_role = "face_on",
      .expected_request = std::nullopt,
  });
  assert(passed.complete);
  assert(passed.passed);

  const auto failed = InspectProbeReport(ProbeReportInspection{
      .report = R"({"report_type":"android_high_speed_probe","role":"face_on","passed":false})",
      .expected_role = "face_on",
      .expected_request = std::nullopt,
  });
  assert(failed.complete);
  assert(!failed.passed);

  const auto wrong_role = InspectProbeReport(ProbeReportInspection{
      .report =
          R"({"report_type":"android_high_speed_probe","role":"down_the_line","passed":true})",
      .expected_role = "face_on",
      .expected_request = std::nullopt,
  });
  assert(wrong_role.complete);
  assert(!wrong_role.passed);

  const auto inventory = InspectProbeReport(ProbeReportInspection{
      .report = R"({"report_type":"android_capability_inventory"})",
      .expected_role = "face_on",
      .expected_request = std::nullopt,
  });
  assert(!inventory.complete);

  const auto invalid = InspectProbeReport(ProbeReportInspection{
      .report = "not JSON",
      .expected_role = "face_on",
      .expected_request = std::nullopt,
  });
  assert(!invalid.complete);

  ProbeRequestConfiguration compact;
  compact.width = 1280;
  compact.height = 720;
  compact.bitrate_bits_per_second = 12000000;
  const auto matching_request = InspectProbeReport(ProbeReportInspection{
      .report = R"({
        "report_type":"android_high_speed_probe",
        "role":"face_on",
        "passed":true,
        "request":{
          "width":1280,
          "height":720,
          "frames_per_second":240,
          "duration_ms":3000,
          "bitrate_bits_per_second":12000000,
          "mime":"video/avc"
        }
      })",
      .expected_role = "face_on",
      .expected_request = compact,
  });
  assert(matching_request.complete);
  assert(matching_request.passed);

  compact.width = 1920;
  const auto mismatched_request = InspectProbeReport(ProbeReportInspection{
      .report = matching_request.passed ? R"({
                        "report_type":"android_high_speed_probe",
                        "role":"face_on",
                        "passed":true,
                        "request":{
                          "width":1280,
                          "height":720,
                          "frames_per_second":240,
                          "duration_ms":3000,
                          "bitrate_bits_per_second":12000000,
                          "mime":"video/avc"
                        }
                      })"
                                        : "",
      .expected_role = "face_on",
      .expected_request = compact,
  });
  assert(mismatched_request.complete);
  assert(!mismatched_request.passed);
}

void TestDisplayPowerStateInspection() {
  const auto asleep = InspectDisplayPowerState(DisplayPowerStateDumps{
      .power = R"(
        Power Manager State:
          mWakefulness=Asleep
          mWakefulnessChanging=false
        Display Power: state=OFF
      )",
      .display = "",
  });
  assert(asleep.confirmed_off());
  assert(asleep.non_interactive);
  assert(asleep.display_off);

  const auto android_16_dozing = InspectDisplayPowerState(DisplayPowerStateDumps{
      .power = "mWakefulness=Dozing\nmInteractive=false\n",
      .display = "mScreenState=OFF\n",
  });
  assert(android_16_dozing.confirmed_off());
  assert(android_16_dozing.non_interactive);
  assert(android_16_dozing.display_off);

  const auto dozing_with_display_on = InspectDisplayPowerState(DisplayPowerStateDumps{
      .power = "mWakefulness=Dozing\nmInteractive=false\n",
      .display = "mScreenState=ON\n",
  });
  assert(!dozing_with_display_on.confirmed_off());
  assert(dozing_with_display_on.non_interactive);
  assert(!dozing_with_display_on.display_off);

  const auto dozing_with_display_doze = InspectDisplayPowerState(DisplayPowerStateDumps{
      .power = "mWakefulness=Dozing\nmInteractive=false\nDisplay Power: state=OFF\n",
      .display = "mScreenState=DOZE\n",
  });
  assert(!dozing_with_display_doze.confirmed_off());

  const auto vendor_variant = InspectDisplayPowerState(DisplayPowerStateDumps{
      .power = "mInteractive=false\n",
      .display = R"(DisplayDeviceInfo{"Built-in Screen": state=OFF, committedState=OFF})",
  });
  assert(vendor_variant.confirmed_off());

  const auto awake_with_off_text = InspectDisplayPowerState(DisplayPowerStateDumps{
      .power = "mWakefulness=Awake\nmInteractive=false\nDisplay Power: state=OFF\n",
      .display = "",
  });
  assert(!awake_with_off_text.confirmed_off());
  assert(!awake_with_off_text.non_interactive);
  assert(awake_with_off_text.display_off);

  const auto asleep_with_display_on = InspectDisplayPowerState(DisplayPowerStateDumps{
      .power = "mWakefulness=Asleep\nDisplay Power: state=ON\n",
      .display = "state=ON",
  });
  assert(!asleep_with_display_on.confirmed_off());
  assert(asleep_with_display_on.non_interactive);
  assert(!asleep_with_display_on.display_off);

  const auto misleading = InspectDisplayPowerState(DisplayPowerStateDumps{
      .power = "mWakefulness=Awake\nmWakefulnessChanging=false\n",
      .display = "mDisplayReadyLocked=false\n",
  });
  assert(!misleading.confirmed_off());
  assert(!misleading.non_interactive);
  assert(!misleading.display_off);
}

}  // namespace

int main() {
  TestCaptureRoles();
  TestActivityArguments();
  TestContinuousSoakTelemetry();
  TestContinuousStartupTiming();
  TestWarmTransitionTiming();
  TestReportInspection();
  TestRetainedSessionInspection();
  TestDisplayPowerStateInspection();
}
