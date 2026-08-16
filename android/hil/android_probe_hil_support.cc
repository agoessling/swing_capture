#include "android/hil/android_probe_hil_support.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swing_capture::android::hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

RetainedSessionPaths InvalidRetainedSession(std::string diagnostic) {
  RetainedSessionPaths paths;
  paths.diagnostic = std::move(diagnostic);
  return paths;
}

bool IsSafeSessionId(std::string_view value) {
  if (value.empty() || value.contains("..")) {
    return false;
  }
  return std::ranges::all_of(value, [](const char character) {
    const bool alpha_numeric = (character >= 'a' && character <= 'z') ||
                               (character >= 'A' && character <= 'Z') ||
                               (character >= '0' && character <= '9');
    return alpha_numeric || character == '_' || character == '.' || character == '-';
  });
}

std::string CompactLowercase(std::string_view text) {
  std::string compact;
  compact.reserve(text.size());
  for (const unsigned char character : text) {
    if (std::isspace(character) == 0) {
      compact.push_back(static_cast<char>(std::tolower(character)));
    }
  }
  return compact;
}

bool DisplayDumpReportsOff(std::string_view display_dump) {
  while (!display_dump.empty()) {
    const std::size_t newline = display_dump.find('\n');
    const std::string compact = CompactLowercase(display_dump.substr(0, newline));
    if (compact == "mstate=off" || compact == "screenstate=0" ||
        ((compact.contains("built-inscreen") || compact.contains("primarydisplay")) &&
         compact.contains("state=off"))) {
      return true;
    }
    if (newline == std::string_view::npos) {
      break;
    }
    display_dump.remove_prefix(newline + 1U);
  }
  return false;
}

}  // namespace

bool IsCaptureRole(std::string_view role) noexcept {
  return role == "down_the_line" || role == "face_on";
}

std::vector<std::string> StartActivityArguments(std::string_view serial, std::string_view role,
                                                bool retain_session,
                                                const ProbeRequestConfiguration &request) {
  std::vector<std::string> arguments = {
      "-s",
      std::string(serial),
      "shell",
      "am",
      "start",
      "-W",
      "-n",
      "com.agoessling.swingcapture/.MainActivity",
      "--es",
      "role",
      std::string(role),
      "--es",
      "capture_profile",
      request.profile,
      "--ez",
      "run_probe",
      "true",
      "--ei",
      "probe_width",
      std::to_string(request.width),
      "--ei",
      "probe_height",
      std::to_string(request.height),
      "--ei",
      "probe_fps",
      std::to_string(request.frames_per_second),
      "--ei",
      "probe_duration_ms",
      std::to_string(request.duration_millis),
      "--ei",
      "probe_bitrate",
      std::to_string(request.bitrate_bits_per_second),
      "--es",
      "probe_mime",
      request.mime,
  };
  if (retain_session) {
    arguments.insert(arguments.end(), {"--ez", "retain_clip", "true"});
  }
  return arguments;
}

std::vector<std::string> StartContinuousActivityArguments(std::string_view serial,
                                                          std::string_view role,
                                                          const ProbeRequestConfiguration &request,
                                                          bool audio_trigger, bool soak) {
  std::string_view action = "run_continuous_hil";
  if (soak) {
    action = "run_audio_soak_hil";
  } else if (audio_trigger) {
    action = "run_audio_hil";
  }
  std::vector<std::string> arguments = {
      "-s",
      std::string(serial),
      "shell",
      "am",
      "start",
      "-W",
      "-n",
      "com.agoessling.swingcapture/.MainActivity",
      "--es",
      "role",
      std::string(role),
      "--es",
      "capture_profile",
      request.profile,
      "--es",
      "shared_session_id",
      "android-hil-shared-session",
      "--ez",
      std::string(action),
      "true",
  };
  return arguments;
}

std::vector<std::string> FinishContinuousSoakActivityArguments(std::string_view serial) {
  return {
      "-s",
      std::string(serial),
      "shell",
      "am",
      "start",
      "-W",
      "--activity-single-top",
      "-n",
      "com.agoessling.swingcapture/.MainActivity",
      "--ez",
      "finish_audio_soak_hil",
      "true",
  };
}

ContinuousSoakTelemetry InspectContinuousSoakTelemetry(std::string_view report) {
  ContinuousSoakTelemetry telemetry;
  try {
    const Json parsed = Json::parse(report);
    if (parsed.value("report_type", "") != "android_continuous_capture" ||
        parsed.value("complete", true) || parsed.value("state", "") != "armed_waiting_audio") {
      telemetry.diagnostic = "soak report is not an incomplete armed continuous capture";
      return telemetry;
    }
    const auto runtime = parsed.find("runtime_telemetry");
    if (runtime == parsed.end() || !runtime->is_object()) {
      telemetry.diagnostic = "soak report is missing runtime_telemetry";
      return telemetry;
    }
    const auto elapsed = runtime->find("elapsed_realtime_ns");
    if (elapsed == runtime->end() || !elapsed->is_string()) {
      telemetry.diagnostic = "soak telemetry elapsed_realtime_ns must be a decimal string";
      return telemetry;
    }
    const std::string elapsed_text = elapsed->get<std::string>();
    std::size_t consumed = 0;
    telemetry.elapsed_realtime_ns = std::stoull(elapsed_text, &consumed);
    if (consumed != elapsed_text.size() || telemetry.elapsed_realtime_ns == 0U) {
      telemetry.diagnostic = "soak telemetry elapsed_realtime_ns is invalid";
      return telemetry;
    }
    telemetry.thermal_status = runtime->value("thermal_status", -1);
    telemetry.video_frames = runtime->value<std::uint64_t>("video_frames", 0U);
    telemetry.audio_frames = runtime->value<std::uint64_t>("audio_frames", 0U);
    telemetry.ring_bytes = runtime->value<std::uint64_t>("ring_bytes", 0U);
    telemetry.ring_duration_us = runtime->value<std::uint64_t>("ring_duration_us", 0U);
    if (telemetry.thermal_status < 0 || telemetry.thermal_status > 6 ||
        telemetry.video_frames == 0U || telemetry.audio_frames == 0U ||
        telemetry.ring_bytes == 0U || telemetry.ring_duration_us < 2'450'000U) {
      telemetry.diagnostic =
          "soak telemetry counters, ring coverage, or thermal status are invalid";
      return telemetry;
    }
    telemetry.valid = true;
    telemetry.diagnostic = "continuous soak telemetry is valid";
    return telemetry;
  } catch (const std::exception &failure) {
    telemetry.diagnostic =
        std::string("cannot inspect continuous soak telemetry: ") + failure.what();
    return telemetry;
  }
}

RetainedSessionPaths InspectRetainedSessionReport(const ProbeReportInspection &inspection) {
  const ProbeReportStatus probe = InspectProbeReport(inspection);
  if (!probe.complete || !probe.passed) {
    return InvalidRetainedSession(probe.diagnostic);
  }
  try {
    const Json parsed = Json::parse(inspection.report);
    if (!parsed.value("retain_session_requested", false)) {
      return InvalidRetainedSession("probe report did not request retained publication");
    }
    const auto retained = parsed.find("retained_session");
    if (retained == parsed.end() || !retained->is_object()) {
      return InvalidRetainedSession("probe report is missing retained_session");
    }
    const std::string session_id = retained->value("session_id", "");
    const std::string manifest = retained->value("manifest", "");
    const std::string media = retained->value("media", "");
    const auto encoded_bytes = retained->value<std::size_t>("encoded_bytes", 0U);
    if (!IsSafeSessionId(session_id)) {
      return InvalidRetainedSession("retained session ID is unsafe");
    }
    const std::string prefix = "sessions/" + session_id + "/";
    if (manifest != prefix + "manifest.json" || !media.starts_with(prefix) ||
        media.find('/', prefix.size()) != std::string::npos || !media.ends_with(".mp4") ||
        encoded_bytes == 0U) {
      return InvalidRetainedSession("retained session artifact paths are invalid");
    }
    return {
        .valid = true,
        .session_id = session_id,
        .manifest_path = manifest,
        .media_path = media,
        .encoded_bytes = encoded_bytes,
        .diagnostic = "retained session paths are valid",
    };
  } catch (const std::exception &failure) {
    return InvalidRetainedSession(std::string("cannot inspect retained session: ") +
                                  failure.what());
  }
}

ProbeReportStatus InspectRetainedManifest(const RetainedManifestInspection &inspection) {
  try {
    const Json parsed = Json::parse(inspection.manifest);
    if (parsed.value("schema_version", 0) != 1 ||
        parsed.value("session_id", "") != inspection.expected_session_id) {
      return {.complete = true, .diagnostic = "retained manifest identity is invalid"};
    }
    const auto views = parsed.find("views");
    if (views == parsed.end() || !views->is_array() || views->size() != 1U) {
      return {.complete = true, .diagnostic = "retained manifest must contain one view"};
    }
    const Json &view = views->at(0);
    const auto trigger = parsed.find("trigger");
    const auto frame_count = view.value<std::size_t>("frame_count", 0U);
    const auto impact_index = view.value<std::size_t>("impact_frame_index", frame_count);
    const auto frames = view.find("frames");
    const auto source = view.find("source");
    const auto media = view.find("media");
    const auto android_capture = parsed.find("android_capture");
    const std::string actual_trigger_source =
        trigger != parsed.end() && trigger->is_object() ? trigger->value("source", "") : "";
    const bool trigger_source_matches =
        inspection.expected_trigger_source.empty() ||
        actual_trigger_source == inspection.expected_trigger_source ||
        (inspection.expected_trigger_source == "manual_or_local_audio" &&
         (actual_trigger_source == "manual" || actual_trigger_source == "local_audio"));
    if (!trigger_source_matches || view.value("role", "") != inspection.expected_role ||
        frame_count < 2U || impact_index >= frame_count || frames == view.end() ||
        !frames->is_array() || frames->size() != frame_count || source == view.end() ||
        !source->is_object() || source->value("pixel_format", "") != "camera2_private" ||
        media == view.end() || !media->is_object() ||
        media->value("mime_type", "") != "video/mp4" ||
        media->value("all_frames_keyframes", true) ||
        media->value<std::size_t>("encoded_bytes", 0U) == 0U ||
        (!inspection.expected_shared_session_id.empty() &&
         (android_capture == parsed.end() || !android_capture->is_object() ||
          android_capture->value("shared_session_id", "") !=
              inspection.expected_shared_session_id))) {
      return {.complete = true, .diagnostic = "retained manifest track is invalid"};
    }
    return {.complete = true, .passed = true, .diagnostic = "retained manifest is valid"};
  } catch (const std::exception &failure) {
    return {.diagnostic = std::string("retained manifest is not valid JSON: ") + failure.what()};
  }
}

ProbeReportStatus InspectProbeReport(const ProbeReportInspection &inspection) {
  try {
    const Json parsed = Json::parse(inspection.report);
    const auto report_type = parsed.find("report_type");
    if (report_type == parsed.end() || !report_type->is_string() ||
        report_type->get<std::string_view>() != inspection.expected_report_type) {
      return {.diagnostic = "latest report does not have the expected Android report type"};
    }
    const auto passed = parsed.find("passed");
    if (passed == parsed.end() || !passed->is_boolean()) {
      return {.diagnostic = "probe report does not contain a Boolean passed field"};
    }
    if (!parsed.value("complete", true)) {
      return {.diagnostic = "probe report is an incomplete intermediate state"};
    }
    const auto role = parsed.find("role");
    if (role == parsed.end() || !role->is_string() ||
        role->get<std::string_view>() != inspection.expected_role) {
      return {
          .complete = true,
          .diagnostic = "probe report role does not match the requested role",
      };
    }
    if (inspection.expected_request.has_value()) {
      const auto request = parsed.find("request");
      const ProbeRequestConfiguration &expected = *inspection.expected_request;
      if (request == parsed.end() || !request->is_object() ||
          request->value("width", 0) != expected.width ||
          request->value("height", 0) != expected.height ||
          request->value("frames_per_second", 0) != expected.frames_per_second ||
          request->value("duration_ms", 0) != expected.duration_millis ||
          request->value("bitrate_bits_per_second", 0) != expected.bitrate_bits_per_second ||
          request->value("mime", "") != expected.mime) {
        return {
            .complete = true,
            .diagnostic = "probe report request does not match the requested capture profile",
        };
      }
    }
    return {
        .complete = true,
        .passed = passed->get<bool>(),
        .diagnostic = passed->get<bool>() ? "probe passed" : "probe reported passed=false",
    };
  } catch (const std::exception &failure) {
    return {.diagnostic = std::string("latest report is not valid JSON: ") + failure.what()};
  }
}

DisplayPowerStateInspection InspectDisplayPowerState(const DisplayPowerStateDumps &dumps) {
  const std::string power = CompactLowercase(dumps.power);
  const bool non_interactive =
      power.contains("mwakefulness=asleep") || power.contains("mwakefulness=dozing") ||
      power.contains("minteractive=false") || power.contains("mawake=false");
  const bool display_off = power.contains("displaypower:state=off") ||
                           power.contains("mscreenon=false") ||
                           DisplayDumpReportsOff(dumps.display);

  std::string diagnostic;
  if (non_interactive && display_off) {
    diagnostic = "device is non-interactive and its display is off";
  } else if (!non_interactive && !display_off) {
    diagnostic = "device is still interactive and its display is not off";
  } else if (!non_interactive) {
    diagnostic = "display is off but device is still interactive";
  } else {
    diagnostic = "device is non-interactive but display is not off";
  }
  return {
      .non_interactive = non_interactive,
      .display_off = display_off,
      .diagnostic = std::move(diagnostic),
  };
}

}  // namespace swing_capture::android::hil
