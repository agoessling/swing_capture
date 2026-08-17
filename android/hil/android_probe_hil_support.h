#ifndef SWING_CAPTURE_ANDROID_HIL_ANDROID_PROBE_HIL_SUPPORT_H_
#define SWING_CAPTURE_ANDROID_HIL_ANDROID_PROBE_HIL_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::android::hil {

struct ProbeRequestConfiguration {
  std::string profile = "720p240";
  int width = 1280;
  int height = 720;
  int frames_per_second = 240;
  int duration_millis = 3000;
  int bitrate_bits_per_second = 12000000;
  std::string mime = "video/avc";

  bool operator==(const ProbeRequestConfiguration &) const = default;
};

struct ProbeReportStatus {
  bool complete = false;
  bool passed = false;
  std::string diagnostic;
};

struct ProbeReportInspection {
  std::string_view report;
  std::string_view expected_role;
  std::optional<ProbeRequestConfiguration> expected_request;
  std::string_view expected_report_type = "android_high_speed_probe";
};

struct RetainedSessionPaths {
  bool valid = false;
  std::string session_id;
  std::string manifest_path;
  std::string media_path;
  std::size_t encoded_bytes = 0;
  std::string diagnostic;
};

struct RetainedManifestInspection {
  std::string_view manifest;
  std::string_view expected_session_id;
  std::string_view expected_role;
  std::string_view expected_trigger_source;
  std::string_view expected_shared_session_id;
};

struct DisplayPowerStateInspection {
  bool non_interactive = false;
  bool display_off = false;
  std::string diagnostic;

  [[nodiscard]] bool confirmed_off() const noexcept { return non_interactive && display_off; }
};

struct DisplayPowerStateDumps {
  std::string_view power;
  std::string_view display;
};

struct ContinuousSoakTelemetry {
  bool valid = false;
  std::uint64_t elapsed_realtime_ns = 0;
  int thermal_status = -1;
  std::uint64_t video_frames = 0;
  std::uint64_t audio_frames = 0;
  std::uint64_t ring_bytes = 0;
  std::uint64_t ring_duration_us = 0;
  std::string diagnostic;
};

struct ContinuousStartupTimingInspection {
  bool valid = false;
  std::string diagnostic;
};

struct WarmTransitionTimingInspection {
  bool valid = false;
  std::uint64_t transition_to_first_camera_frame_ns = 0;
  std::uint64_t transition_to_first_encoded_frame_ns = 0;
  std::string diagnostic;
};

[[nodiscard]] bool IsCaptureRole(std::string_view role) noexcept;

[[nodiscard]] std::vector<std::string> StartActivityArguments(
    std::string_view serial, std::string_view role, bool retain_session = false,
    const ProbeRequestConfiguration &request = {});

[[nodiscard]] std::vector<std::string> StartContinuousActivityArguments(
    std::string_view serial, std::string_view role, const ProbeRequestConfiguration &request = {},
    bool audio_trigger = false, bool soak = false);

[[nodiscard]] std::vector<std::string> StartWarmTransitionActivityArguments(
    std::string_view serial, std::string_view role, const ProbeRequestConfiguration &request = {});

[[nodiscard]] std::vector<std::string> FinishContinuousSoakActivityArguments(
    std::string_view serial);

[[nodiscard]] ContinuousSoakTelemetry InspectContinuousSoakTelemetry(std::string_view report);

[[nodiscard]] ContinuousStartupTimingInspection InspectContinuousStartupTiming(
    std::string_view report);

[[nodiscard]] WarmTransitionTimingInspection InspectWarmTransitionTiming(std::string_view report);

[[nodiscard]] ProbeReportStatus InspectProbeReport(const ProbeReportInspection &inspection);

[[nodiscard]] RetainedSessionPaths InspectRetainedSessionReport(
    const ProbeReportInspection &inspection);

[[nodiscard]] ProbeReportStatus InspectRetainedManifest(
    const RetainedManifestInspection &inspection);

[[nodiscard]] DisplayPowerStateInspection InspectDisplayPowerState(
    const DisplayPowerStateDumps &dumps);

}  // namespace swing_capture::android::hil

#endif  // SWING_CAPTURE_ANDROID_HIL_ANDROID_PROBE_HIL_SUPPORT_H_
