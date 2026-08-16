#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "android/hil/android_probe_hil_support.h"
#include "capture/hil/feather_hil_controller.h"
#include "capture/hil/feather_hil_serial.h"
#include "nlohmann/json.hpp"
#include "station/station_config.h"

namespace {

using namespace std::chrono_literals;
using swing_capture::android::hil::ContinuousSoakTelemetry;
using swing_capture::android::hil::DisplayPowerStateDumps;
using swing_capture::android::hil::FinishContinuousSoakActivityArguments;
using swing_capture::android::hil::InspectContinuousSoakTelemetry;
using swing_capture::android::hil::InspectDisplayPowerState;
using swing_capture::android::hil::InspectProbeReport;
using swing_capture::android::hil::InspectRetainedManifest;
using swing_capture::android::hil::InspectRetainedSessionReport;
using swing_capture::android::hil::IsCaptureRole;
using swing_capture::android::hil::ProbeReportInspection;
using swing_capture::android::hil::ProbeReportStatus;
using swing_capture::android::hil::ProbeRequestConfiguration;
using swing_capture::android::hil::RetainedManifestInspection;
using swing_capture::android::hil::RetainedSessionPaths;
using swing_capture::android::hil::StartActivityArguments;
using swing_capture::android::hil::StartContinuousActivityArguments;

constexpr auto kHilDeadline = 15s;
constexpr auto kPollInterval = 100ms;
constexpr auto kSoakPollInterval = 1s;
constexpr auto kSoakDuration = 15min;
constexpr auto kSoakCompletionAllowance = 30s;
constexpr int kMaximumAcceptedThermalStatus = 2;
constexpr double kMinimumEncodedFramesPerSecond = 235.0;
constexpr double kMaximumEncodedFramesPerSecond = 245.0;
constexpr double kMinimumAudioFramesPerSecond = 47'500.0;
constexpr double kMaximumAudioFramesPerSecond = 48'500.0;
constexpr std::string_view kPackageName = "com.agoessling.swingcapture";
constexpr std::string_view kReportPath = "files/reports/latest.json";
constexpr std::string_view kHilSharedSessionId = "android-hil-shared-session";
constexpr auto kToneLead = 100ms;
constexpr auto kToneDuration = 20ms;
constexpr auto kQuietQualification = 1s;
constexpr auto kDisplayOffVerificationDeadline = 3s;
constexpr std::uint32_t kToneFrequencyHz = 2000;

struct CommandResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string output;
};

std::string EnvironmentValue(std::string_view name) {
  // Bazel establishes the test environment before the process starts.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *value = std::getenv(std::string(name).c_str());
  return value == nullptr ? std::string() : std::string(value);
}

std::filesystem::path OutputDirectory() {
  const std::string directory = EnvironmentValue("TEST_UNDECLARED_OUTPUTS_DIR");
  return directory.empty() ? std::filesystem::current_path() : std::filesystem::path(directory);
}

void WriteArtifact(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open Android HIL artifact " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    throw std::runtime_error("cannot write Android HIL artifact " + path.string());
  }
}

std::string DescribeCommand(const std::filesystem::path &executable,
                            const std::vector<std::string> &arguments) {
  std::string description = executable.filename().string();
  for (const std::string &argument : arguments) {
    description += " " + argument;
  }
  return description;
}

CommandResult RunCommand(const std::filesystem::path &executable,
                         const std::vector<std::string> &arguments,
                         std::chrono::steady_clock::time_point deadline) {
  int output_pipe[2] = {-1, -1};
  if (pipe2(output_pipe, O_CLOEXEC) != 0) {
    throw std::runtime_error(std::string("cannot create command output pipe: ") +
                             std::strerror(errno));
  }

  const pid_t child = fork();
  if (child < 0) {
    const int saved_errno = errno;
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw std::runtime_error(std::string("cannot fork adb: ") + std::strerror(saved_errno));
  }
  if (child == 0) {
    close(output_pipe[0]);
    if (dup2(output_pipe[1], STDOUT_FILENO) < 0 || dup2(output_pipe[1], STDERR_FILENO) < 0) {
      _exit(126);
    }
    close(output_pipe[1]);

    std::vector<char *> command;
    command.reserve(arguments.size() + 2);
    command.push_back(const_cast<char *>(executable.c_str()));
    for (const std::string &argument : arguments) {
      command.push_back(const_cast<char *>(argument.c_str()));
    }
    command.push_back(nullptr);
    execv(executable.c_str(), command.data());
    _exit(127);
  }

  close(output_pipe[1]);
  const int flags = fcntl(output_pipe[0], F_GETFL, 0);
  if (flags >= 0) {
    static_cast<void>(fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK));
  }

  CommandResult result;
  int status = 0;
  bool exited = false;
  bool end_of_output = false;
  while (!exited || !end_of_output) {
    char buffer[4096];
    while (true) {
      const ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
      if (count > 0) {
        result.output.append(buffer, static_cast<std::size_t>(count));
        continue;
      }
      if (count == 0) {
        end_of_output = true;
      }
      break;
    }

    if (!exited) {
      const pid_t waited = waitpid(child, &status, WNOHANG);
      if (waited == child) {
        exited = true;
      } else if (waited < 0 && errno != EINTR) {
        close(output_pipe[0]);
        throw std::runtime_error(std::string("cannot wait for adb: ") + std::strerror(errno));
      }
    }
    if (exited && end_of_output) {
      break;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      result.timed_out = true;
      exited = true;
      continue;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const int wait_millis = static_cast<int>(std::min(remaining, 50ms).count());
    pollfd descriptor = {.fd = output_pipe[0], .events = POLLIN, .revents = 0};
    const int polled = poll(&descriptor, 1, wait_millis);
    if (polled < 0 && errno != EINTR) {
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      close(output_pipe[0]);
      throw std::runtime_error(std::string("cannot poll adb output: ") + std::strerror(errno));
    }
  }
  close(output_pipe[0]);

  if (!result.timed_out) {
    if (WIFEXITED(status)) {
      result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      result.exit_code = 128 + WTERMSIG(status);
    }
  }
  return result;
}

CommandResult RunAdb(const std::filesystem::path &adb, std::vector<std::string> arguments,
                     std::chrono::steady_clock::time_point deadline) {
  return RunCommand(adb, arguments, deadline);
}

nlohmann::json CommandEvidence(const CommandResult &result) {
  return {
      {"exit_code", result.exit_code},
      {"timed_out", result.timed_out},
      {"output", result.output},
  };
}

void RequireSuccessfulCommand(const std::filesystem::path &executable,
                              const std::vector<std::string> &arguments,
                              const CommandResult &result) {
  if (result.timed_out) {
    throw std::runtime_error("Android HIL deadline expired while running: " +
                             DescribeCommand(executable, arguments));
  }
  if (result.exit_code != 0) {
    throw std::runtime_error("adb command failed (exit " + std::to_string(result.exit_code) +
                             "): " + DescribeCommand(executable, arguments) + "\n" + result.output);
  }
}

void RunRequiredAdb(const std::filesystem::path &adb, const std::vector<std::string> &arguments,
                    std::chrono::steady_clock::time_point deadline) {
  const CommandResult result = RunAdb(adb, arguments, deadline);
  RequireSuccessfulCommand(adb, arguments, result);
}

std::string RunAdbForOutput(const std::filesystem::path &adb,
                            const std::vector<std::string> &arguments,
                            std::chrono::steady_clock::time_point deadline) {
  const CommandResult result = RunAdb(adb, arguments, deadline);
  if (result.timed_out) {
    throw std::runtime_error("Android HIL deadline expired while reading: " +
                             DescribeCommand(adb, arguments));
  }
  if (result.exit_code != 0) {
    throw std::runtime_error("adb read failed (exit " + std::to_string(result.exit_code) +
                             "): " + DescribeCommand(adb, arguments) + "\n" + result.output);
  }
  return result.output;
}

std::vector<std::string> DeviceArguments(std::string_view serial,
                                         std::initializer_list<std::string_view> suffix) {
  std::vector<std::string> arguments = {"-s", std::string(serial)};
  for (const std::string_view argument : suffix) {
    arguments.emplace_back(argument);
  }
  return arguments;
}

class DeviceCleanupGuard {
 public:
  DeviceCleanupGuard(std::filesystem::path adb, std::string serial, bool wake_display,
                     nlohmann::json *lifecycle_evidence, std::filesystem::path lifecycle_artifact,
                     std::string *latest_report, std::filesystem::path latest_report_artifact)
      : adb_(std::move(adb)),
        serial_(std::move(serial)),
        wake_display_(wake_display),
        lifecycle_evidence_(lifecycle_evidence),
        lifecycle_artifact_(std::move(lifecycle_artifact)),
        latest_report_(latest_report),
        latest_report_artifact_(std::move(latest_report_artifact)) {}

  DeviceCleanupGuard(const DeviceCleanupGuard &) = delete;
  DeviceCleanupGuard &operator=(const DeviceCleanupGuard &) = delete;

  ~DeviceCleanupGuard() {
    if (wake_display_) {
      RunCleanupCommand("wake_display",
                        DeviceArguments(serial_, {"shell", "input", "keyevent", "224"}));
    }
    RunCleanupCommand("force_stop",
                      DeviceArguments(serial_, {"shell", "am", "force-stop", kPackageName}));
    try {
      if (latest_report_ != nullptr && !latest_report_->empty()) {
        WriteArtifact(latest_report_artifact_, *latest_report_);
      }
      if (lifecycle_evidence_ != nullptr) {
        (*lifecycle_evidence_)["cleanup_complete"] = true;
        WriteArtifact(lifecycle_artifact_, lifecycle_evidence_->dump(2) + "\n");
      }
    } catch (const std::exception &failure) {
      std::cerr << "Warning: could not preserve Android HIL lifecycle evidence: " << failure.what()
                << '\n';
    }
  }

 private:
  void RunCleanupCommand(std::string_view name, const std::vector<std::string> &arguments) {
    try {
      const CommandResult result = RunAdb(adb_, arguments, std::chrono::steady_clock::now() + 2s);
      if (lifecycle_evidence_ != nullptr) {
        (*lifecycle_evidence_)["cleanup"][name] = CommandEvidence(result);
      }
      if (result.timed_out || result.exit_code != 0) {
        std::cerr << "Warning: Android HIL cleanup command failed: " << name << '\n';
      }
    } catch (const std::exception &failure) {
      if (lifecycle_evidence_ != nullptr) {
        try {
          (*lifecycle_evidence_)["cleanup"][name] = {
              {"exception", failure.what()},
          };
        } catch (const std::exception &) {
        }
      }
      std::cerr << "Warning: Android HIL cleanup command " << name << " failed: " << failure.what()
                << '\n';
    }
  }

  std::filesystem::path adb_;
  std::string serial_;
  bool wake_display_;
  nlohmann::json *lifecycle_evidence_;
  std::filesystem::path lifecycle_artifact_;
  std::string *latest_report_;
  std::filesystem::path latest_report_artifact_;
};

void RequireExecutable(const std::filesystem::path &path, std::string_view description) {
  if (!std::filesystem::is_regular_file(path) || access(path.c_str(), X_OK) != 0) {
    throw std::runtime_error(std::string(description) + " is not executable: " + path.string());
  }
}

std::string RequiredSerial() {
  std::string serial = EnvironmentValue("SWING_CAPTURE_ANDROID_SERIAL");
  if (serial.empty()) {
    serial = EnvironmentValue("ANDROID_SERIAL");
  }
  if (serial.empty()) {
    throw std::runtime_error(
        "set the phone serial with "
        "--test_env=SWING_CAPTURE_ANDROID_SERIAL=<adb-serial> (or ANDROID_SERIAL)");
  }
  return serial;
}

std::string RequestedRole() {
  std::string role = EnvironmentValue("SWING_CAPTURE_ANDROID_ROLE");
  if (role.empty()) {
    role = "down_the_line";
  }
  if (!IsCaptureRole(role)) {
    throw std::runtime_error("SWING_CAPTURE_ANDROID_ROLE must be down_the_line or face_on, not " +
                             role);
  }
  return role;
}

ProbeRequestConfiguration RequestedProbeConfiguration() {
  const std::string profile = EnvironmentValue("SWING_CAPTURE_ANDROID_PROFILE");
  if (profile.empty() || profile == "720p240") {
    return {};
  }
  if (profile == "1080p240") {
    return {
        .profile = "1080p240",
        .width = 1920,
        .height = 1080,
        .frames_per_second = 240,
        .duration_millis = 3000,
        .bitrate_bits_per_second = 24000000,
        .mime = "video/avc",
    };
  }
  throw std::runtime_error("SWING_CAPTURE_ANDROID_PROFILE must be 1080p240 or 720p240, not " +
                           profile);
}

bool IsArmedWaitingForAudio(std::string_view report) {
  try {
    const auto parsed = nlohmann::json::parse(report);
    return parsed.value("report_type", "") == "android_continuous_capture" &&
           !parsed.value("complete", true) && parsed.value("state", "") == "armed_waiting_audio";
  } catch (const std::exception &) {
    return false;
  }
}

void TurnDisplayOffAndVerify(const std::filesystem::path &adb, std::string_view serial,
                             std::chrono::steady_clock::time_point hil_deadline,
                             nlohmann::json &lifecycle_evidence) {
  const std::vector<std::string> sleep_arguments =
      DeviceArguments(serial, {"shell", "input", "keyevent", "223"});
  const CommandResult sleep_result =
      RunAdb(adb, sleep_arguments, std::min(hil_deadline, std::chrono::steady_clock::now() + 2s));
  lifecycle_evidence["screen_off"]["sleep_command"] = CommandEvidence(sleep_result);
  RequireSuccessfulCommand(adb, sleep_arguments, sleep_result);

  const auto verification_deadline =
      std::min(hil_deadline, std::chrono::steady_clock::now() + kDisplayOffVerificationDeadline);
  std::string latest_diagnostic = "display state has not been inspected";
  while (std::chrono::steady_clock::now() < verification_deadline) {
    const std::vector<std::string> power_arguments =
        DeviceArguments(serial, {"shell", "dumpsys", "power"});
    const CommandResult power_result = RunAdb(adb, power_arguments, verification_deadline);
    lifecycle_evidence["screen_off"]["power_command"] = CommandEvidence(power_result);
    RequireSuccessfulCommand(adb, power_arguments, power_result);

    const std::vector<std::string> display_arguments =
        DeviceArguments(serial, {"shell", "dumpsys", "display"});
    const CommandResult display_result = RunAdb(adb, display_arguments, verification_deadline);
    lifecycle_evidence["screen_off"]["display_command"] = CommandEvidence(display_result);
    RequireSuccessfulCommand(adb, display_arguments, display_result);

    const auto inspection = InspectDisplayPowerState(DisplayPowerStateDumps{
        .power = power_result.output,
        .display = display_result.output,
    });
    lifecycle_evidence["screen_off"]["state"] = {
        {"non_interactive", inspection.non_interactive},
        {"display_off", inspection.display_off},
        {"confirmed_off", inspection.confirmed_off()},
        {"diagnostic", inspection.diagnostic},
    };
    latest_diagnostic = inspection.diagnostic;
    if (inspection.confirmed_off()) {
      std::cout << "Confirmed that the Android display is off and non-interactive\n";
      return;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("could not confirm screen-off state: " + latest_diagnostic);
}

swing_capture::hil::FeatherStimulusReceipt PlayFeatherImpactTone() {
  const auto config_path = swing_capture::station::StationConfigPathFromEnvironment();
  if (!config_path.has_value()) {
    throw std::runtime_error("SWING_CAPTURE_STATION_CONFIG is required for audio-trigger HIL");
  }
  const auto station = swing_capture::station::LoadStationConfig(*config_path);
  swing_capture::hil::FeatherHilSerial serial(station.feather_serial_path);
  swing_capture::hil::FeatherHilController feather(serial);
  const auto info = feather.QueryInfo();
  const std::uint32_t level = info.tone_maximum_level_permille;
  if (level == 0U) {
    throw std::runtime_error("Feather did not advertise a maximum tone level");
  }
  const auto receipt = feather.PlayTone(kToneLead, kToneDuration, kToneFrequencyHz, level);
  std::cout << "Played Feather impact tone at " << receipt.frequency_hz << " Hz and "
            << receipt.level_permille << " permille\n";
  return receipt;
}

int Run(int argument_count, char **arguments) {
  if (argument_count != 3 && argument_count != 4) {
    throw std::runtime_error(
        "expected Bazel runfile arguments: <adb> <APK> "
        "[--retain|--continuous|--continuous-audio|--continuous-audio-screen-off|"
        "--continuous-audio-soak-15m]");
  }
  const std::string_view mode = argument_count == 4 ? std::string_view(arguments[3]) : "";
  const bool continuous_audio_soak = mode == "--continuous-audio-soak-15m";
  const bool continuous_audio_screen_off = mode == "--continuous-audio-screen-off";
  const bool continuous_audio =
      mode == "--continuous-audio" || continuous_audio_screen_off || continuous_audio_soak;
  const bool continuous = mode == "--continuous" || continuous_audio;
  const bool retain_session = mode == "--retain" || continuous;
  if (!mode.empty() && !retain_session) {
    throw std::runtime_error(
        "the optional runner argument must be --retain, --continuous, --continuous-audio, "
        "--continuous-audio-screen-off, or --continuous-audio-soak-15m");
  }
  const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
  const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
  RequireExecutable(adb, "hermetic adb");
  if (!std::filesystem::is_regular_file(apk)) {
    throw std::runtime_error("Android APK runfile is missing: " + apk.string());
  }

  const std::string serial = RequiredSerial();
  const std::string role = RequestedRole();
  const ProbeRequestConfiguration request = RequestedProbeConfiguration();
  const auto deadline =
      std::chrono::steady_clock::now() +
      (continuous_audio_soak ? kSoakDuration + kSoakCompletionAllowance : kHilDeadline);
  std::string latest_output;
  const std::string lifecycle_artifact_stem =
      (continuous_audio_soak ? "android-continuous-15-minute-soak-"
                             : "android-audio-trigger-screen-off-") +
      role;
  nlohmann::json lifecycle_evidence = {
      {"schema_version", 1},
      {"report_type", continuous_audio_soak ? "android_continuous_15_minute_soak"
                                            : "android_audio_trigger_screen_off_lifecycle"},
      {"serial", serial},
      {"role", role},
      {"armed_with_full_pre_roll", false},
      {"quiet_interval_complete", false},
      {"quiet_interval_millis",
       std::chrono::duration_cast<std::chrono::milliseconds>(kQuietQualification).count()},
      {"stimulus_sent", false},
      {"successful_local_audio_report", false},
      {"cleanup_complete", false},
  };
  if (continuous_audio_soak) {
    lifecycle_evidence["target_duration_seconds"] =
        std::chrono::duration_cast<std::chrono::seconds>(kSoakDuration).count();
    lifecycle_evidence["telemetry_interval_seconds"] = 30;
    lifecycle_evidence["acceptance"] = {
        {"maximum_thermal_status", kMaximumAcceptedThermalStatus},
        {"minimum_encoded_frames_per_second", kMinimumEncodedFramesPerSecond},
        {"maximum_encoded_frames_per_second", kMaximumEncodedFramesPerSecond},
        {"minimum_audio_frames_per_second", kMinimumAudioFramesPerSecond},
        {"maximum_audio_frames_per_second", kMaximumAudioFramesPerSecond},
    };
    lifecycle_evidence["telemetry_samples"] = nlohmann::json::array();
  }
  const std::filesystem::path lifecycle_artifact =
      OutputDirectory() / (lifecycle_artifact_stem + "-lifecycle.json");
  const std::filesystem::path latest_report_artifact =
      OutputDirectory() / (lifecycle_artifact_stem + "-latest-report.txt");
  const bool preserve_lifecycle = continuous_audio_screen_off || continuous_audio_soak;
  const DeviceCleanupGuard cleanup(
      adb, serial, continuous_audio_screen_off, preserve_lifecycle ? &lifecycle_evidence : nullptr,
      lifecycle_artifact, preserve_lifecycle ? &latest_output : nullptr, latest_report_artifact);

  std::cout << "Running Android "
            << (continuous_audio_soak
                    ? "15-minute continuous 240 fps audio-trigger soak"
                    : (continuous_audio_screen_off
                           ? "continuous audio-trigger capture with the screen off"
                           : (continuous_audio
                                  ? "continuous audio-trigger capture"
                                  : (continuous ? "continuous retained capture"
                                                : (retain_session ? "bounded retained capture"
                                                                  : "bounded probe")))))
            << " on " << serial << " as " << role << " at " << request.width << 'x'
            << request.height << 'p' << request.frames_per_second << std::endl;
  RunRequiredAdb(adb,
                 DeviceArguments(serial, {"install", "--no-streaming", "-r", "-t", apk.string()}),
                 deadline);
  RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "am", "force-stop", kPackageName}),
                 deadline);
  RunRequiredAdb(
      adb, DeviceArguments(serial, {"shell", "run-as", kPackageName, "rm", "-f", kReportPath}),
      deadline);
  for (const std::string_view permission : {
           "android.permission.CAMERA",
           "android.permission.RECORD_AUDIO",
           "android.permission.POST_NOTIFICATIONS",
       }) {
    RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "pm", "grant", kPackageName, permission}),
                   deadline);
  }
  RunRequiredAdb(adb,
                 continuous ? StartContinuousActivityArguments(
                                  serial, role, request, continuous_audio, continuous_audio_soak)
                            : StartActivityArguments(serial, role, retain_session, request),
                 deadline);

  std::string latest_diagnostic = "probe report has not appeared";
  bool audio_stimulus_sent = false;
  std::optional<std::chrono::steady_clock::time_point> audio_ready_at;
  std::optional<ContinuousSoakTelemetry> first_soak_telemetry;
  std::optional<ContinuousSoakTelemetry> last_soak_telemetry;
  std::vector<std::string> soak_acceptance_failures;
  int maximum_thermal_status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const CommandResult result = RunAdb(
        adb, DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat", kReportPath}),
        deadline);
    if (!result.output.empty()) {
      latest_output = result.output;
    }
    if (result.exit_code == 0) {
      if (continuous_audio && !audio_stimulus_sent && IsArmedWaitingForAudio(result.output)) {
        if (!audio_ready_at.has_value()) {
          audio_ready_at = std::chrono::steady_clock::now();
          if (preserve_lifecycle) {
            lifecycle_evidence["armed_with_full_pre_roll"] = true;
          }
        }
        if (continuous_audio_soak) {
          const ContinuousSoakTelemetry telemetry = InspectContinuousSoakTelemetry(result.output);
          if (!telemetry.valid) {
            throw std::runtime_error(telemetry.diagnostic);
          }
          if (!last_soak_telemetry.has_value() ||
              last_soak_telemetry->elapsed_realtime_ns != telemetry.elapsed_realtime_ns) {
            if (!first_soak_telemetry.has_value()) {
              first_soak_telemetry = telemetry;
            } else if (telemetry.elapsed_realtime_ns < first_soak_telemetry->elapsed_realtime_ns) {
              throw std::runtime_error("soak telemetry elapsed time moved backwards");
            }
            nlohmann::json sample = {
                {"elapsed_realtime_ns", std::to_string(telemetry.elapsed_realtime_ns)},
                {"thermal_status", telemetry.thermal_status},
                {"video_frames", telemetry.video_frames},
                {"audio_frames", telemetry.audio_frames},
                {"ring_bytes", telemetry.ring_bytes},
                {"ring_duration_us", telemetry.ring_duration_us},
                {"soak_elapsed_seconds",
                 static_cast<double>(telemetry.elapsed_realtime_ns -
                                     first_soak_telemetry->elapsed_realtime_ns) /
                     1'000'000'000.0},
            };
            if (last_soak_telemetry.has_value()) {
              if (telemetry.elapsed_realtime_ns <= last_soak_telemetry->elapsed_realtime_ns ||
                  telemetry.video_frames <= last_soak_telemetry->video_frames ||
                  telemetry.audio_frames <= last_soak_telemetry->audio_frames) {
                soak_acceptance_failures.emplace_back(
                    "runtime telemetry counters did not advance monotonically");
              } else {
                const std::uint64_t elapsed_delta =
                    telemetry.elapsed_realtime_ns - last_soak_telemetry->elapsed_realtime_ns;
                const double interval_seconds =
                    static_cast<double>(elapsed_delta) / 1'000'000'000.0;
                const double video_rate = static_cast<double>(telemetry.video_frames -
                                                              last_soak_telemetry->video_frames) /
                                          interval_seconds;
                const double audio_rate = static_cast<double>(telemetry.audio_frames -
                                                              last_soak_telemetry->audio_frames) /
                                          interval_seconds;
                sample["interval_encoded_frames_per_second"] = video_rate;
                sample["interval_audio_frames_per_second"] = audio_rate;
                if (video_rate < kMinimumEncodedFramesPerSecond ||
                    video_rate > kMaximumEncodedFramesPerSecond) {
                  soak_acceptance_failures.emplace_back(
                      "an encoded-frame telemetry interval was outside 235-245 fps");
                }
                if (interval_seconds >= 20.0 && (audio_rate < kMinimumAudioFramesPerSecond ||
                                                 audio_rate > kMaximumAudioFramesPerSecond)) {
                  soak_acceptance_failures.emplace_back(
                      "an audio-frame telemetry interval was outside 47.5-48.5 kHz");
                }
              }
            }
            maximum_thermal_status = std::max(maximum_thermal_status, telemetry.thermal_status);
            const nlohmann::json report_json = nlohmann::json::parse(result.output);
            sample["device"] = report_json.at("runtime_telemetry");
            if (report_json.contains("soak_diagnostics")) {
              sample["soak_diagnostics"] = report_json.at("soak_diagnostics");
              lifecycle_evidence["incidental_trigger_count"] =
                  report_json.at("soak_diagnostics").value("incidental_trigger_count", 0);
            }
            lifecycle_evidence["telemetry_samples"].push_back(sample);
            lifecycle_evidence["maximum_thermal_status"] = maximum_thermal_status;
            lifecycle_evidence["acceptance_failures"] = soak_acceptance_failures;
            WriteArtifact(lifecycle_artifact, lifecycle_evidence.dump(2) + "\n");

            const nlohmann::json &device = sample["device"];
            const double thermal_headroom =
                device.contains("thermal_headroom") && device["thermal_headroom"].is_number()
                    ? device["thermal_headroom"].get<double>()
                    : -1.0;
            std::cout << "SOAK t=" << sample["soak_elapsed_seconds"].get<double>()
                      << "s thermal_status=" << telemetry.thermal_status
                      << " headroom=" << thermal_headroom
                      << " battery=" << device.value("battery_temperature_celsius", -1.0)
                      << "C video_frames=" << telemetry.video_frames;
            if (sample.contains("interval_encoded_frames_per_second")) {
              std::cout << " video_fps="
                        << sample["interval_encoded_frames_per_second"].get<double>()
                        << " audio_hz=" << sample["interval_audio_frames_per_second"].get<double>();
            }
            std::cout << " ring_MiB="
                      << static_cast<double>(telemetry.ring_bytes) / (1024.0 * 1024.0)
                      << " java_heap_MiB="
                      << static_cast<double>(
                             device.value<std::uint64_t>("java_heap_used_bytes", 0U)) /
                             (1024.0 * 1024.0)
                      << " native_heap_MiB="
                      << static_cast<double>(
                             device.value<std::uint64_t>("native_heap_allocated_bytes", 0U)) /
                             (1024.0 * 1024.0)
                      << std::endl;
            last_soak_telemetry = telemetry;
          }
          const std::uint64_t required_soak_nanos =
              std::chrono::duration_cast<std::chrono::nanoseconds>(kSoakDuration).count();
          if (first_soak_telemetry.has_value() && last_soak_telemetry.has_value() &&
              last_soak_telemetry->elapsed_realtime_ns -
                      first_soak_telemetry->elapsed_realtime_ns >=
                  required_soak_nanos) {
            RunRequiredAdb(adb, FinishContinuousSoakActivityArguments(serial), deadline);
            audio_stimulus_sent = true;
            lifecycle_evidence["stimulus_sent"] = true;
            lifecycle_evidence["stimulus"] = {
                {"source", "explicit_soak_completion"},
                {"action", "finish_audio_soak_hil"},
            };
            WriteArtifact(lifecycle_artifact, lifecycle_evidence.dump(2) + "\n");
          }
        } else if (std::chrono::steady_clock::now() - *audio_ready_at >= kQuietQualification) {
          if (continuous_audio_screen_off) {
            lifecycle_evidence["quiet_interval_complete"] = true;
            TurnDisplayOffAndVerify(adb, serial, deadline, lifecycle_evidence);

            const std::vector<std::string> report_arguments =
                DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat", kReportPath});
            const CommandResult post_sleep_report = RunAdb(adb, report_arguments, deadline);
            lifecycle_evidence["post_screen_off_report_command"] =
                CommandEvidence(post_sleep_report);
            RequireSuccessfulCommand(adb, report_arguments, post_sleep_report);
            latest_output = post_sleep_report.output;
            if (!IsArmedWaitingForAudio(post_sleep_report.output)) {
              throw std::runtime_error(
                  "audio detector left the armed state before the Feather stimulus");
            }
          }
          const auto stimulus_receipt = PlayFeatherImpactTone();
          audio_stimulus_sent = true;
          if (preserve_lifecycle) {
            lifecycle_evidence["stimulus_sent"] = true;
            lifecycle_evidence["stimulus"] = {
                {"request_id", stimulus_receipt.request_id},
                {"accepted_device_us", stimulus_receipt.accepted_device_microseconds},
                {"scheduled_device_us", stimulus_receipt.scheduled_device_microseconds},
                {"start_device_us", stimulus_receipt.start_device_microseconds},
                {"end_device_us", stimulus_receipt.end_device_microseconds},
                {"frequency_hz", stimulus_receipt.frequency_hz},
                {"level_permille", stimulus_receipt.level_permille},
                {"sample_rate_hz", stimulus_receipt.sample_rate_hz},
                {"sample_count", stimulus_receipt.sample_count},
                {"lead_millis", kToneLead.count()},
                {"duration_millis", kToneDuration.count()},
            };
          }
        }
        continue;
      }
      const auto status = InspectProbeReport(ProbeReportInspection{
          .report = result.output,
          .expected_role = role,
          .expected_request = request,
          .expected_report_type =
              continuous ? "android_continuous_capture" : "android_high_speed_probe",
      });
      latest_diagnostic = status.diagnostic;
      if (status.complete) {
        if (continuous_audio && !audio_stimulus_sent && status.passed) {
          throw std::runtime_error("audio detector triggered before the HIL stimulus");
        }
        const std::string artifact_stem =
            continuous_audio_soak
                ? "android-continuous-15-minute-soak-"
                : (continuous_audio_screen_off
                       ? "android-audio-trigger-screen-off-"
                       : (continuous_audio ? "android-audio-trigger-"
                                           : (continuous ? "android-continuous-capture-"
                                                         : "android-high-speed-probe-")));
        const std::filesystem::path artifact = OutputDirectory() / (artifact_stem + role + ".json");
        WriteArtifact(artifact, result.output);
        if (preserve_lifecycle) {
          lifecycle_evidence["app_report_artifact"] = artifact.filename().string();
        }
        std::cout << "Published probe report to " << artifact << '\n';
        if (!status.passed) {
          throw std::runtime_error(status.diagnostic);
        }
        if (retain_session) {
          const RetainedSessionPaths paths = InspectRetainedSessionReport(ProbeReportInspection{
              .report = result.output,
              .expected_role = role,
              .expected_request = request,
              .expected_report_type =
                  continuous ? "android_continuous_capture" : "android_high_speed_probe",
          });
          if (!paths.valid) {
            throw std::runtime_error(paths.diagnostic);
          }
          const std::string manifest =
              RunAdbForOutput(adb,
                              DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat",
                                                       "files/" + paths.manifest_path}),
                              deadline);
          const ProbeReportStatus manifest_status =
              InspectRetainedManifest(RetainedManifestInspection{
                  .manifest = manifest,
                  .expected_session_id = paths.session_id,
                  .expected_role = role,
                  .expected_trigger_source = continuous_audio_soak
                                                 ? "manual_or_local_audio"
                                                 : (continuous_audio ? "local_audio" : "manual"),
                  .expected_shared_session_id = continuous ? kHilSharedSessionId : "",
              });
          if (!manifest_status.complete || !manifest_status.passed) {
            throw std::runtime_error(manifest_status.diagnostic);
          }
          const std::string media = RunAdbForOutput(
              adb,
              DeviceArguments(
                  serial, {"exec-out", "run-as", kPackageName, "cat", "files/" + paths.media_path}),
              deadline);
          if (media.size() != paths.encoded_bytes || media.size() < 12U ||
              media.compare(4U, 4U, "ftyp") != 0) {
            throw std::runtime_error("retained MP4 size or file signature is invalid");
          }
          WriteArtifact(OutputDirectory() / ("android-retained-" + role + "-manifest.json"),
                        manifest);
          WriteArtifact(OutputDirectory() / ("android-retained-" + role + ".mp4"), media);
          std::cout << "Published retained manifest and MP4 for " << paths.session_id << '\n';
        }
        if (preserve_lifecycle) {
          lifecycle_evidence[continuous_audio_soak ? "successful_retained_report"
                                                   : "successful_local_audio_report"] = true;
        }
        if (continuous_audio_soak) {
          if (!first_soak_telemetry.has_value() || !last_soak_telemetry.has_value()) {
            soak_acceptance_failures.emplace_back("soak did not produce runtime telemetry");
          } else {
            const std::uint64_t observed_nanos = last_soak_telemetry->elapsed_realtime_ns -
                                                 first_soak_telemetry->elapsed_realtime_ns;
            const std::uint64_t required_nanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(kSoakDuration).count();
            if (observed_nanos < required_nanos) {
              soak_acceptance_failures.emplace_back(
                  "device monotonic telemetry did not cover the requested 15 minutes");
            }
            if (observed_nanos > 0U) {
              const double observed_seconds = static_cast<double>(observed_nanos) / 1'000'000'000.0;
              const double aggregate_video_rate =
                  static_cast<double>(last_soak_telemetry->video_frames -
                                      first_soak_telemetry->video_frames) /
                  observed_seconds;
              const double aggregate_audio_rate =
                  static_cast<double>(last_soak_telemetry->audio_frames -
                                      first_soak_telemetry->audio_frames) /
                  observed_seconds;
              lifecycle_evidence["aggregate_encoded_frames_per_second"] = aggregate_video_rate;
              lifecycle_evidence["aggregate_audio_frames_per_second"] = aggregate_audio_rate;
              if (aggregate_video_rate < kMinimumEncodedFramesPerSecond ||
                  aggregate_video_rate > kMaximumEncodedFramesPerSecond) {
                soak_acceptance_failures.emplace_back(
                    "aggregate encoded-frame rate was outside 235-245 fps");
              }
              if (aggregate_audio_rate < kMinimumAudioFramesPerSecond ||
                  aggregate_audio_rate > kMaximumAudioFramesPerSecond) {
                soak_acceptance_failures.emplace_back(
                    "aggregate audio-frame rate was outside 47.5-48.5 kHz");
              }
            }
          }
          if (lifecycle_evidence["telemetry_samples"].size() < 30U) {
            soak_acceptance_failures.emplace_back(
                "fewer than 30 periodic telemetry samples were retained");
          }
          if (maximum_thermal_status > kMaximumAcceptedThermalStatus) {
            soak_acceptance_failures.emplace_back(
                "Android thermal status exceeded MODERATE during the soak");
          }
          std::ranges::sort(soak_acceptance_failures);
          soak_acceptance_failures.erase(
              std::unique(soak_acceptance_failures.begin(), soak_acceptance_failures.end()),
              soak_acceptance_failures.end());
          lifecycle_evidence["acceptance_failures"] = soak_acceptance_failures;
          lifecycle_evidence["passed"] = soak_acceptance_failures.empty();
          WriteArtifact(lifecycle_artifact, lifecycle_evidence.dump(2) + "\n");
          if (!soak_acceptance_failures.empty()) {
            std::string diagnostic = "15-minute soak acceptance failed";
            for (const std::string &failure : soak_acceptance_failures) {
              diagnostic += "; " + failure;
            }
            throw std::runtime_error(diagnostic);
          }
        }
        return 0;
      }
    }
    std::this_thread::sleep_for(continuous_audio_soak ? kSoakPollInterval : kPollInterval);
  }

  if (!latest_output.empty()) {
    WriteArtifact(OutputDirectory() / ("android-latest-report-" + role + ".txt"), latest_output);
  }
  throw std::runtime_error("Android HIL deadline expired: " + latest_diagnostic);
}

}  // namespace

int main(int argument_count, char **arguments) {
  try {
    return Run(argument_count, arguments);
  } catch (const std::exception &failure) {
    std::cerr << "Android high-speed probe HIL failed: " << failure.what() << '\n';
    return 1;
  }
}
