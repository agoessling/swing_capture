#include "android/hil/android_probe_hil_support.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using swing_capture::android::hil::DisplayPowerStateDumps;
using swing_capture::android::hil::FinishContinuousSoakActivityArguments;
using swing_capture::android::hil::InspectContinuousSoakTelemetry;
using swing_capture::android::hil::InspectDisplayPowerState;
using swing_capture::android::hil::InspectProbeReport;
using swing_capture::android::hil::InspectRetainedManifest;
using swing_capture::android::hil::InspectRetainedSessionReport;
using swing_capture::android::hil::IsCaptureRole;
using swing_capture::android::hil::ProbeReportInspection;
using swing_capture::android::hil::ProbeRequestConfiguration;
using swing_capture::android::hil::RetainedManifestInspection;
using swing_capture::android::hil::StartActivityArguments;
using swing_capture::android::hil::StartContinuousActivityArguments;

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

  const auto vendor_variant = InspectDisplayPowerState(DisplayPowerStateDumps{
      .power = "mInteractive=false\n",
      .display = R"(DisplayDeviceInfo{"Built-in Screen": state=OFF, committedState=OFF})",
  });
  assert(vendor_variant.confirmed_off());

  const auto awake_with_off_text = InspectDisplayPowerState(DisplayPowerStateDumps{
      .power = "mWakefulness=Awake\nDisplay Power: state=OFF\n",
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
  TestReportInspection();
  TestRetainedSessionInspection();
  TestDisplayPowerStateInspection();
}
