#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "android/hil/pose_replay_report_validation.h"
#include "android/pose_hil/pose_replay_hil_support.h"
#include "nlohmann/json.hpp"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
namespace replay_report = swing_capture::android::hil;
namespace pose_hil = swing_capture::android::pose_hil;

constexpr auto kTotalDeadline = 30s;
constexpr auto kPollInterval = 100ms;
constexpr std::size_t kMaximumCommandOutputBytes =
    replay_report::kPoseReplayMaximumReportBytes + 64U * 1024U;
constexpr std::uintmax_t kMaximumClipBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::string_view kPackageName = "com.agoessling.swingcapture";
constexpr std::string_view kActivityName = "com.agoessling.swingcapture/.MainActivity";

struct CommandResult {
  int exit_code = -1;
  bool timed_out = false;
  bool output_exceeded = false;
  std::string output;
};

std::string EnvironmentValue(std::string_view name) {
  // Bazel establishes the environment before this single-threaded test starts.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *const value = std::getenv(std::string(name).c_str());
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
    throw std::runtime_error("cannot open pose replay HIL artifact " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    throw std::runtime_error("cannot write pose replay HIL artifact " + path.string());
  }
}

CommandResult RunCommand(const std::filesystem::path &executable,
                         const std::vector<std::string> &arguments,
                         std::chrono::steady_clock::time_point deadline) {
  int output_pipe[2] = {-1, -1};
  if (pipe2(output_pipe, O_CLOEXEC) != 0) {
    throw std::runtime_error(std::string("cannot create adb output pipe: ") + std::strerror(errno));
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
    command.reserve(arguments.size() + 2U);
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
    std::array<char, 4096> buffer = {};
    while (true) {
      const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        result.output.append(buffer.data(), static_cast<std::size_t>(count));
        if (result.output.size() > kMaximumCommandOutputBytes) {
          result.output_exceeded = true;
          static_cast<void>(kill(child, SIGKILL));
        }
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
    if (std::chrono::steady_clock::now() >= deadline || result.output_exceeded) {
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      result.timed_out = !result.output_exceeded;
      exited = true;
      continue;
    }
    pollfd descriptor = {.fd = output_pipe[0], .events = POLLIN, .revents = 0};
    const int polled = poll(&descriptor, 1, 50);
    if (polled < 0 && errno != EINTR) {
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      close(output_pipe[0]);
      throw std::runtime_error(std::string("cannot poll adb output: ") + std::strerror(errno));
    }
  }
  close(output_pipe[0]);
  if (!result.timed_out && !result.output_exceeded) {
    if (WIFEXITED(status)) {
      result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      result.exit_code = 128 + WTERMSIG(status);
    }
  }
  return result;
}

std::vector<std::string> DeviceArguments(std::string_view serial,
                                         std::initializer_list<std::string_view> suffix) {
  std::vector<std::string> arguments = {"-s", std::string(serial)};
  for (const std::string_view argument : suffix) {
    arguments.emplace_back(argument);
  }
  return arguments;
}

std::string RunRequiredAdb(const std::filesystem::path &adb,
                           const std::vector<std::string> &arguments,
                           std::chrono::steady_clock::time_point deadline) {
  const CommandResult result = RunCommand(adb, arguments, deadline);
  if (result.timed_out) {
    throw std::runtime_error("pose replay HIL exceeded its 30 second deadline in adb");
  }
  if (result.output_exceeded) {
    throw std::runtime_error("adb output exceeded the pose replay HIL bound");
  }
  if (result.exit_code != 0) {
    throw std::runtime_error("adb failed with exit " + std::to_string(result.exit_code) + ": " +
                             result.output);
  }
  return result.output;
}

std::vector<std::string> ActivityArguments(std::string_view serial,
                                           const pose_hil::PoseReplayHilInputs &inputs) {
  std::vector<std::string> arguments = {
      "-s", std::string(serial),        "shell", "am", "start", "-W", "--activity-single-top",
      "-n", std::string(kActivityName),
  };
  std::vector<std::string> extras = pose_hil::PoseReplayActivityExtras(inputs);
  arguments.insert(arguments.end(), std::make_move_iterator(extras.begin()),
                   std::make_move_iterator(extras.end()));
  return arguments;
}

void PersistDriverReport(const std::filesystem::path &path, const Json &report) {
  WriteArtifact(path, report.dump(2) + "\n");
}

std::string WaitForDeviceReport(const std::filesystem::path &adb, std::string_view serial,
                                std::chrono::steady_clock::time_point deadline) {
  const std::vector<std::string> arguments =
      DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "sh", "-c",
                               pose_hil::PoseReplayReportReadShellCommand()});
  while (std::chrono::steady_clock::now() < deadline) {
    const auto attempt_deadline =
        std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    const CommandResult result = RunCommand(adb, arguments, attempt_deadline);
    if (!result.timed_out && !result.output_exceeded && result.exit_code == 0 &&
        !result.output.empty()) {
      return result.output;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("phone did not publish its pose replay report before the deadline");
}

void BestEffortCleanup(const std::filesystem::path &adb, std::string_view serial,
                       std::string_view staging_path, Json &report,
                       const std::filesystem::path &report_path) noexcept {
  Json cleanup = {
      {"device_clip_removed", false},
      {"staging_clip_removed", false},
      {"force_stop_succeeded", false},
  };
  try {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    const CommandResult clip_removed = RunCommand(
        adb,
        DeviceArguments(
            serial, {"shell", "run-as", kPackageName, "rm", "-f", pose_hil::kPoseReplayDeviceClip}),
        deadline);
    cleanup["device_clip_removed"] =
        !clip_removed.timed_out && !clip_removed.output_exceeded && clip_removed.exit_code == 0;
    if (!staging_path.empty()) {
      const CommandResult staging_removed =
          RunCommand(adb, DeviceArguments(serial, {"shell", "rm", "-f", staging_path}), deadline);
      cleanup["staging_clip_removed"] = !staging_removed.timed_out &&
                                        !staging_removed.output_exceeded &&
                                        staging_removed.exit_code == 0;
    }
    const CommandResult stopped = RunCommand(
        adb, DeviceArguments(serial, {"shell", "am", "force-stop", kPackageName}), deadline);
    cleanup["force_stop_succeeded"] =
        !stopped.timed_out && !stopped.output_exceeded && stopped.exit_code == 0;
  } catch (const std::exception &failure) {
    cleanup["error"] = failure.what();
  }
  report["cleanup"] = std::move(cleanup);
  try {
    PersistDriverReport(report_path, report);
  } catch (const std::exception &failure) {
    std::cerr << "Warning: cannot preserve pose replay cleanup evidence: " << failure.what()
              << '\n';
  }
}

int Run(int argument_count, char **arguments) {
  if (argument_count != 3) {
    throw std::invalid_argument("usage: pose_replay_hil_test <adb> <signed-apk>");
  }
  const std::filesystem::path adb(arguments[1]);
  const std::filesystem::path apk(arguments[2]);
  if (!std::filesystem::is_regular_file(adb) || access(adb.c_str(), X_OK) != 0) {
    throw std::runtime_error("Bazel hermetic adb is not executable");
  }
  if (!std::filesystem::is_regular_file(apk)) {
    throw std::runtime_error("Bazel signed Android APK is unavailable");
  }
  const std::string serial = EnvironmentValue("SWING_CAPTURE_ANDROID_SERIAL");
  if (serial.empty()) {
    throw std::runtime_error(
        "set --test_env=SWING_CAPTURE_ANDROID_SERIAL=<adb-serial>; the test never guesses "
        "between attached phones");
  }
  const std::string clip_text = EnvironmentValue("SWING_CAPTURE_POSE_REPLAY_CLIP");
  if (clip_text.empty()) {
    throw std::runtime_error(
        "set --test_env=SWING_CAPTURE_POSE_REPLAY_CLIP=/absolute/app-private-source.mp4");
  }
  const std::filesystem::path clip = std::filesystem::weakly_canonical(clip_text);
  if (!clip.is_absolute() || !std::filesystem::is_regular_file(clip)) {
    throw std::runtime_error("SWING_CAPTURE_POSE_REPLAY_CLIP must name a regular absolute file");
  }
  const std::uintmax_t clip_bytes = std::filesystem::file_size(clip);
  if (clip_bytes == 0U || clip_bytes > kMaximumClipBytes || clip.extension() != ".mp4") {
    throw std::runtime_error("pose replay clip must be a nonempty MP4 no larger than 512 MiB");
  }
  const pose_hil::PoseReplayHilInputs inputs =
      pose_hil::ResolvePoseReplayHilInputs(EnvironmentValue("SWING_CAPTURE_ANDROID_ROLE"),
                                           EnvironmentValue("SWING_CAPTURE_POSE_PROJECTION"),
                                           EnvironmentValue("SWING_CAPTURE_POSE_HITTING_REGION"),
                                           EnvironmentValue("SWING_CAPTURE_POSE_DELEGATE"),
                                           EnvironmentValue("SWING_CAPTURE_POSE_EXPECTATION"),
                                           EnvironmentValue("SWING_CAPTURE_POSE_MAXIMUM_FRAMES"));

  const auto deadline = std::chrono::steady_clock::now() + kTotalDeadline;
  const std::filesystem::path report_path = OutputDirectory() / "report.json";
  const std::filesystem::path device_report_path = OutputDirectory() / "pose_replay_report.json";
  Json report = {
      {"schema_version", 1},
      {"report_type", "android_pose_replay_hil"},
      {"complete", false},
      {"passed", false},
      {"serial", serial},
      {"input",
       {{"file_name", clip.filename().string()},
        {"bytes", clip_bytes},
        {"role", inputs.role},
        {"projection", inputs.projection},
        {"hitting_region", inputs.hitting_region},
        {"delegate_policy", inputs.delegate_policy},
        {"expectation", inputs.expectation},
        {"maximum_frames", inputs.maximum_frames}}},
      {"device_report_artifact", device_report_path.filename().string()},
  };
  PersistDriverReport(report_path, report);
  const std::string staging_path =
      "/data/local/tmp/swing_capture_pose_replay_" + std::to_string(getpid()) + ".mp4";
  try {
    report["device"] = {
        {"model",
         RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "getprop", "ro.product.model"}),
                        deadline)},
        {"api_level",
         RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "getprop", "ro.build.version.sdk"}),
                        deadline)},
    };
    RunRequiredAdb(adb,
                   DeviceArguments(serial, {"install", "--no-streaming", "-r", "-t", apk.string()}),
                   deadline);
    RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "am", "force-stop", kPackageName}),
                   deadline);
    RunRequiredAdb(adb, DeviceArguments(serial, {"push", clip.string(), staging_path}), deadline);
    RunRequiredAdb(adb,
                   DeviceArguments(serial, {"shell", "run-as", kPackageName, "mkdir", "-p",
                                            pose_hil::kPoseReplayDeviceDirectory}),
                   deadline);
    RunRequiredAdb(adb,
                   DeviceArguments(serial, {"shell", "run-as", kPackageName, "cp", staging_path,
                                            pose_hil::kPoseReplayDeviceClip}),
                   deadline);
    RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "rm", "-f", staging_path}), deadline);
    RunRequiredAdb(adb,
                   DeviceArguments(serial, {"shell", "run-as", kPackageName, "rm", "-f",
                                            pose_hil::kPoseReplayDeviceReport,
                                            "files/reports/latest.json.tmp"}),
                   deadline);
    PersistDriverReport(report_path, report);

    RunRequiredAdb(adb, ActivityArguments(serial, inputs), deadline);
    const std::string device_report = WaitForDeviceReport(adb, serial, deadline);
    WriteArtifact(device_report_path, device_report);
    const replay_report::PoseReplayReportInspection inspection =
        replay_report::InspectPoseReplayReport(device_report);
    report["device_report"] = {
        {"valid", inspection.valid},
        {"passed", inspection.passed},
        {"diagnostic", inspection.diagnostic},
        {"outcome", inspection.outcome},
        {"failure_code", inspection.failure_code},
        {"role", inspection.role},
        {"projection", inspection.projection},
        {"actual_delegate", inspection.actual_delegate.has_value()
                                ? Json(*inspection.actual_delegate)
                                : Json(nullptr)},
        {"frame_count", inspection.frame_count},
        {"arm_request_count", inspection.arm_request_count},
    };
    if (!inspection.valid) {
      throw std::runtime_error("phone pose replay report is invalid: " + inspection.diagnostic);
    }
    if (!inspection.passed) {
      throw std::runtime_error("phone pose replay did not satisfy its expectation: " +
                               inspection.failure_code);
    }
    report["complete"] = true;
    report["passed"] = true;
    PersistDriverReport(report_path, report);
  } catch (const std::exception &failure) {
    report["error"] = failure.what();
    BestEffortCleanup(adb, serial, staging_path, report, report_path);
    throw;
  }
  BestEffortCleanup(adb, serial, staging_path, report, report_path);
  return 0;
}

}  // namespace

int main(int argument_count, char **arguments) {
  try {
    return Run(argument_count, arguments);
  } catch (const std::exception &failure) {
    std::cerr << "Android pose replay HIL failed: " << failure.what() << '\n';
    return 1;
  }
}
