#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "android/hil/android_probe_hil_support.h"
#include "android/pose_hil/pose_standby_validation.h"
#include "android/pose_hil/standby_missed_validation.h"
#include "android/pose_hil/warm_retained_validation.h"
#include "capture/hil/feather_hil_controller.h"
#include "capture/hil/feather_hil_serial.h"
#include "nlohmann/json.hpp"
#include "station/station_config.h"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
namespace pose_hil = swing_capture::android::pose_hil;
namespace android_hil = swing_capture::android::hil;

constexpr auto kTotalDeadline = 15s;
constexpr auto kPollInterval = 200ms;
constexpr auto kScreenOffVerificationTimeout = 2s;
constexpr auto kMissedShotPostScreenOffReserve = 6s;
constexpr auto kMissedShotPostTagReserve = 5s;
constexpr auto kWarmPostArmReserve = 8'500ms;
constexpr auto kWarmArmRequestBudget = 250ms;
constexpr std::uint16_t kPhoneHttpPort = 8088U;
constexpr auto kToneLead = 100ms;
constexpr auto kToneDuration = 20ms;
constexpr std::uint32_t kToneFrequencyHz = 2000;
constexpr std::string_view kPackageName = "com.agoessling.swingcapture";
constexpr std::string_view kActivityName = "com.agoessling.swingcapture/.MainActivity";
constexpr std::string_view kPreferencesPath = "shared_prefs/node_configuration.xml";

struct CommandResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string output;
};

struct HttpResponse {
  int status = 0;
  std::map<std::string, std::string, std::less<>> headers;
  std::string body;
};

std::string EnvironmentValue(std::string_view name) {
  // Bazel establishes the test environment before this single-threaded process starts.
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
    throw std::runtime_error("cannot open pose standby HIL artifact " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    throw std::runtime_error("cannot write pose standby HIL artifact " + path.string());
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
    pollfd descriptor = {
        .fd = output_pipe[0],
        .events = POLLIN,
        .revents = 0,
    };
    const int polled = poll(&descriptor, 1, static_cast<int>(std::min(remaining, 50ms).count()));
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

std::vector<std::string> DeviceArguments(std::string_view serial,
                                         std::initializer_list<std::string_view> suffix) {
  std::vector<std::string> arguments = {"-s", std::string(serial)};
  for (std::string_view argument : suffix) {
    arguments.emplace_back(argument);
  }
  return arguments;
}

std::string RunRequiredAdb(const std::filesystem::path &adb,
                           const std::vector<std::string> &arguments,
                           std::chrono::steady_clock::time_point deadline) {
  const CommandResult result = RunCommand(adb, arguments, deadline);
  if (result.timed_out) {
    throw std::runtime_error("pose standby HIL exceeded its 15 second deadline in adb");
  }
  if (result.exit_code != 0) {
    throw std::runtime_error("adb failed with exit " + std::to_string(result.exit_code) + ": " +
                             result.output);
  }
  return result.output;
}

std::string Trim(std::string value) {
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' ||
                            value.back() == '\t')) {
    value.pop_back();
  }
  const auto first = std::ranges::find_if(value, [](char character) {
    return character != ' ' && character != '\t' && character != '\r' && character != '\n';
  });
  value.erase(value.begin(), first);
  return value;
}

void WaitForSocket(int descriptor, short events, std::chrono::steady_clock::time_point deadline) {
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw std::runtime_error("pose standby HTTP request exceeded its deadline");
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd polled = {.fd = descriptor, .events = events, .revents = 0};
    const int result = poll(&polled, 1, static_cast<int>(std::min(remaining, 100ms).count()));
    if (result > 0) {
      if ((polled.revents & events) != 0) {
        return;
      }
      if ((polled.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error("pose standby HTTP socket failed");
      }
    } else if (result < 0 && errno != EINTR) {
      throw std::runtime_error(std::string("cannot poll pose standby HTTP socket: ") +
                               std::strerror(errno));
    }
  }
}

int ConnectHttp(std::uint16_t port, std::chrono::steady_clock::time_point deadline) {
  const int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (descriptor < 0) {
    throw std::runtime_error(std::string("cannot create HTTP socket: ") + std::strerror(errno));
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
    if (errno != EINPROGRESS) {
      const int saved_errno = errno;
      close(descriptor);
      throw std::runtime_error(std::string("cannot connect to phone HTTP forward: ") +
                               std::strerror(saved_errno));
    }
    try {
      WaitForSocket(descriptor, POLLOUT, deadline);
    } catch (...) {
      close(descriptor);
      throw;
    }
    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0 ||
        socket_error != 0) {
      close(descriptor);
      throw std::runtime_error("cannot establish phone HTTP forward connection");
    }
  }
  return descriptor;
}

void SendAll(int descriptor, std::string_view request,
             std::chrono::steady_clock::time_point deadline) {
  std::size_t sent = 0;
  while (sent < request.size()) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const ssize_t count =
        send(descriptor, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
    } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      throw std::runtime_error(std::string("cannot write phone HTTP request: ") +
                               std::strerror(errno));
    } else {
      WaitForSocket(descriptor, POLLOUT, deadline);
    }
  }
}

std::string ReceiveAll(int descriptor, std::chrono::steady_clock::time_point deadline,
                       std::size_t maximum_response_bytes) {
  std::string response;
  while (true) {
    std::array<char, 4096> buffer = {};
    const ssize_t count = recv(descriptor, buffer.data(), buffer.size(), 0);
    if (count > 0) {
      response.append(buffer.data(), static_cast<std::size_t>(count));
      if (response.size() > maximum_response_bytes) {
        throw std::runtime_error("phone HTTP response exceeds its configured byte bound");
      }
    } else if (count == 0) {
      return response;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      throw std::runtime_error(std::string("cannot read phone HTTP response: ") +
                               std::strerror(errno));
    } else {
      WaitForSocket(descriptor, POLLIN, deadline);
    }
  }
}

std::string Lowercase(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

HttpResponse ParseHttpResponse(std::string_view wire) {
  const std::size_t header_end = wire.find("\r\n\r\n");
  const std::size_t status_end = wire.find("\r\n");
  if (header_end == std::string_view::npos || status_end == std::string_view::npos ||
      !wire.starts_with("HTTP/1.")) {
    throw std::runtime_error("phone returned a malformed HTTP response");
  }
  const std::size_t first_space = wire.find(' ');
  if (first_space == std::string_view::npos || first_space + 4U > status_end) {
    throw std::runtime_error("phone returned a malformed HTTP status");
  }
  HttpResponse response;
  const std::string_view status_text = wire.substr(first_space + 1U, 3U);
  const auto [status_end_pointer, status_error] =
      std::from_chars(status_text.data(), status_text.data() + status_text.size(),
                      response.status);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (status_error != std::errc() ||
      status_end_pointer != status_text.data() + status_text.size()) {  // NOLINT
    throw std::runtime_error("phone HTTP status is not numeric");
  }
  std::size_t line_start = status_end + 2U;
  while (line_start < header_end) {
    const std::size_t line_end = wire.find("\r\n", line_start);
    const std::size_t separator = wire.find(':', line_start);
    if (line_end == std::string_view::npos || separator == std::string_view::npos ||
        separator >= line_end) {
      throw std::runtime_error("phone returned a malformed HTTP header");
    }
    response.headers.emplace(
        Lowercase(wire.substr(line_start, separator - line_start)),
        Trim(std::string(wire.substr(separator + 1U, line_end - separator - 1U))));
    line_start = line_end + 2U;
  }
  response.body = wire.substr(header_end + 4U);
  const auto length = response.headers.find("content-length");
  if (length == response.headers.end()) {
    throw std::runtime_error("phone HTTP response lacks Content-Length");
  }
  std::size_t expected_bytes = 0;
  const auto [length_end, length_error] =
      std::from_chars(length->second.data(),
                      length->second.data() + length->second.size(),  // NOLINT
                      expected_bytes);
  if (length_error != std::errc() ||
      length_end != length->second.data() + length->second.size() ||  // NOLINT
      expected_bytes != response.body.size()) {
    throw std::runtime_error("phone HTTP Content-Length is invalid");
  }
  return response;
}

HttpResponse HttpRequest(std::uint16_t port, std::string_view method, std::string_view path,
                         std::string_view bearer_token, std::string_view body,
                         std::chrono::steady_clock::time_point deadline,
                         std::size_t maximum_response_bytes = 128ULL * 1024ULL) {
  std::string request = std::string(method) + " " + std::string(path) +
                        " HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: application/json\r\n";
  if (!bearer_token.empty()) {
    request += "Authorization: Bearer " + std::string(bearer_token) + "\r\n";
  }
  if (!body.empty()) {
    request +=
        "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  }
  request += "Connection: close\r\n\r\n";
  request.append(body);
  const int descriptor = ConnectHttp(port, deadline);
  try {
    SendAll(descriptor, request, deadline);
    std::string wire = ReceiveAll(descriptor, deadline, maximum_response_bytes);
    close(descriptor);
    return ParseHttpResponse(wire);
  } catch (...) {
    close(descriptor);
    throw;
  }
}

std::uint16_t EstablishForward(const std::filesystem::path &adb, std::string_view serial,
                               std::chrono::steady_clock::time_point deadline) {
  const std::string phone_port = "tcp:" + std::to_string(kPhoneHttpPort);
  const std::string output = Trim(
      RunRequiredAdb(adb, DeviceArguments(serial, {"forward", "tcp:0", phone_port}), deadline));
  unsigned int parsed = 0;
  const auto [end, error] = std::from_chars(output.data(), output.data() + output.size(),
                                            parsed);                   // NOLINT
  if (error != std::errc() || end != output.data() + output.size() ||  // NOLINT
      parsed == 0 || parsed > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("adb did not return a valid ephemeral forward port");
  }
  return static_cast<std::uint16_t>(parsed);
}

std::vector<std::string> StartArguments(std::string_view serial, std::string_view role,
                                        bool debug_evidence) {
  return {
      "-s",
      std::string(serial),
      "shell",
      "am",
      "start",
      "-W",
      "--activity-single-top",
      "-n",
      std::string(kActivityName),
      "--es",
      "role",
      std::string(role),
      "--es",
      "capture_profile",
      "720p240",
      "--es",
      "pose_mode",
      "shadow",
      "--es",
      "pose_delegate",
      "gpu_preferred",
      "--ez",
      "pose_debug_evidence",
      debug_evidence ? "true" : "false",
      "--ez",
      "run_pose_standby_hil",
      "true",
  };
}

Json TelemetryJson(const pose_hil::DeviceTelemetry &telemetry) {
  return {
      {"valid", telemetry.valid},
      {"diagnostic", telemetry.diagnostic},
      {"thermal_status", telemetry.thermal_status},
      {"battery_level_percent", telemetry.battery_level_percent},
      {"battery_temperature_celsius", telemetry.battery_temperature_celsius},
      {"battery_voltage_millivolts", telemetry.battery_voltage_millivolts},
  };
}

pose_hil::DeviceTelemetry CollectTelemetry(const std::filesystem::path &adb,
                                           std::string_view serial,
                                           std::chrono::steady_clock::time_point deadline) {
  const std::string battery =
      RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "dumpsys", "battery"}), deadline);
  const std::string thermal = RunRequiredAdb(
      adb, DeviceArguments(serial, {"shell", "dumpsys", "thermalservice"}), deadline);
  return pose_hil::InspectDeviceTelemetry(battery, thermal);
}

std::string ReadControlToken(const std::filesystem::path &adb, std::string_view serial,
                             std::chrono::steady_clock::time_point deadline) {
  const std::string preferences = RunRequiredAdb(
      adb, DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat", kPreferencesPath}),
      deadline);
  constexpr std::string_view kPrefix = "<string name=\"control_token\">";
  constexpr std::string_view kSuffix = "</string>";
  const std::size_t start = preferences.find(kPrefix);
  const std::size_t end =
      start == std::string::npos ? std::string::npos : preferences.find(kSuffix, start);
  if (start == std::string::npos || end == std::string::npos) {
    throw std::runtime_error("phone did not persist its bearer control token");
  }
  const std::string token =
      preferences.substr(start + kPrefix.size(), end - start - kPrefix.size());
  if (token.size() != 32U || !std::ranges::all_of(token, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-';
      })) {
    throw std::runtime_error("phone bearer control token is malformed");
  }
  return token;
}

void PersistReport(const std::filesystem::path &path, const Json &report) {
  WriteArtifact(path, report.dump(2) + "\n");
}

void BestEffortCleanup(const std::filesystem::path &adb, std::string_view serial,
                       std::optional<std::uint16_t> host_port, std::string_view control_token,
                       Json &report, const std::filesystem::path &report_path,
                       bool preserve_screen_off = false) noexcept {
  Json cleanup = {
      {"disarm_accepted", false},
      {"force_stop_succeeded", false},
      {"forward_removed", false},
  };
  try {
    const auto cleanup_deadline = std::chrono::steady_clock::now() + 3s;
    if (host_port.has_value() && !control_token.empty()) {
      const HttpResponse disarm = HttpRequest(*host_port, "POST", "/api/v1/capture/arm",
                                              control_token, "{\"armed\":false}", cleanup_deadline);
      cleanup["disarm_http_status"] = disarm.status;
      cleanup["disarm_accepted"] = disarm.status == 202;
    }
    const CommandResult stopped =
        RunCommand(adb, DeviceArguments(serial, {"shell", "am", "force-stop", kPackageName}),
                   cleanup_deadline);
    cleanup["force_stop_succeeded"] = !stopped.timed_out && stopped.exit_code == 0;
    if (host_port.has_value()) {
      const std::string forward = "tcp:" + std::to_string(*host_port);
      const CommandResult removed = RunCommand(
          adb, DeviceArguments(serial, {"forward", "--remove", forward}), cleanup_deadline);
      cleanup["forward_removed"] = !removed.timed_out && removed.exit_code == 0;
    }
    if (preserve_screen_off) {
      const CommandResult slept =
          RunCommand(adb, DeviceArguments(serial, {"shell", "input", "keyevent", "KEYCODE_SLEEP"}),
                     cleanup_deadline);
      cleanup["screen_sleep_command_succeeded"] = !slept.timed_out && slept.exit_code == 0;
      const CommandResult power =
          RunCommand(adb, DeviceArguments(serial, {"shell", "dumpsys", "power"}), cleanup_deadline);
      if (!power.timed_out && power.exit_code == 0) {
        WriteArtifact(OutputDirectory() / "cleanup_screen_off_power.txt", power.output);
      }
      const CommandResult display = RunCommand(
          adb, DeviceArguments(serial, {"shell", "dumpsys", "display"}), cleanup_deadline);
      if (!display.timed_out && display.exit_code == 0) {
        WriteArtifact(OutputDirectory() / "cleanup_screen_off_display.txt", display.output);
      }
      if (!power.timed_out && power.exit_code == 0 && !display.timed_out &&
          display.exit_code == 0) {
        const android_hil::DisplayPowerStateInspection screen =
            android_hil::InspectDisplayPowerState({power.output, display.output});
        cleanup["screen_off_preserved"] = screen.confirmed_off();
        cleanup["screen_non_interactive"] = screen.non_interactive;
        cleanup["screen_display_off"] = screen.display_off;
        cleanup["screen_diagnostic"] = screen.diagnostic;
      } else {
        cleanup["screen_off_preserved"] = false;
        cleanup["screen_diagnostic"] = "cleanup dumpsys command failed or timed out";
      }
    }
  } catch (const std::exception &failure) {
    cleanup["error"] = failure.what();
  }
  report["cleanup"] = std::move(cleanup);
  try {
    PersistReport(report_path, report);
  } catch (const std::exception &failure) {
    std::cerr << "Warning: cannot preserve cleanup evidence: " << failure.what() << '\n';
  }
}

void AppendStatusEvidence(Json &report, std::string_view body,
                          std::chrono::steady_clock::time_point started,
                          const std::filesystem::path &report_path) {
  Json evidence = Json::parse(body);
  evidence["host_elapsed_millis"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count();
  report["status_samples"].push_back(std::move(evidence));
  PersistReport(report_path, report);
}

std::string ReadAppFile(const std::filesystem::path &adb, std::string_view serial,
                        std::string_view path, std::chrono::steady_clock::time_point deadline) {
  return RunRequiredAdb(
      adb, DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat", path}), deadline);
}

bool SafeFileSegment(std::string_view value) {
  return !value.empty() && value.size() <= 128U && std::ranges::all_of(value, [](char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '-' ||
           character == '_';
  });
}

Json WaitForMissedShotStandby(std::uint16_t host_port, std::uint64_t minimum_audio_end,
                              std::uint64_t minimum_successful_inferences,
                              std::uint64_t minimum_encoded_evidence_frames, Json &report,
                              const std::filesystem::path &report_path,
                              std::chrono::steady_clock::time_point started,
                              std::chrono::steady_clock::time_point deadline) {
  std::string latest_diagnostic = "joint pose and standby-audio status has not appeared";
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      const HttpResponse response =
          HttpRequest(host_port, "GET", "/api/v1/capture/status", "", "", deadline);
      if (response.status != 200) {
        latest_diagnostic = "capture status returned HTTP " + std::to_string(response.status);
      } else {
        AppendStatusEvidence(report, response.body, started, report_path);
        Json status = Json::parse(response.body);
        const pose_hil::PoseStatusSample sample = pose_hil::InspectPoseStatus(response.body);
        latest_diagnostic = sample.diagnostic;
        if (status.value("state", "") == "error") {
          throw std::runtime_error("standby entered error state: " +
                                   status.value("error", "unknown error"));
        }
        if (sample.valid &&
            (sample.failed_inferences != 0 || sample.standby_audio_dropped_events != 0 ||
             sample.standby_audio_discontinuities != 0 ||
             sample.standby_audio_timestamp_rejections > 2 ||
             !sample.standby_audio_last_error.empty())) {
          throw std::runtime_error("joint pose/audio standby reported a runtime failure");
        }
        const Json &pose = status.at("pose");
        if (sample.valid && sample.state == "armed" && sample.armed &&
            sample.phase == "monitoring" && sample.mode == "shadow" &&
            sample.configured_delegate == "gpu_preferred" &&
            pose.value("debug_evidence_enabled", false) && sample.standby_audio_ready &&
            sample.successful_inferences >= minimum_successful_inferences &&
            pose_hil::HasMinimumEncodedPoseEvidence(sample, minimum_encoded_evidence_frames) &&
            sample.standby_audio_end_frame_position >= minimum_audio_end) {
          return status;
        }
      }
    } catch (const nlohmann::json::exception &malformed) {
      latest_diagnostic = malformed.what();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("joint pose/audio standby was not ready: " + latest_diagnostic);
}

void VerifyConfiguredNode(std::uint16_t host_port, std::string_view role, Json &report,
                          const std::filesystem::path &report_path,
                          std::chrono::steady_clock::time_point deadline) {
  const HttpResponse response = HttpRequest(host_port, "GET", "/api/v1/node", "", "", deadline);
  if (response.status != 200) {
    throw std::runtime_error("node configuration returned HTTP " + std::to_string(response.status));
  }
  const Json node = Json::parse(response.body);
  const Json &pose = node.at("pose");
  if (node.value("schema_version", 0) != 1 || node.value("role", "") != role ||
      node.value("capture_profile", "") != "720p240" || pose.value("mode", "") != "shadow" ||
      pose.value("delegate", "") != "gpu_preferred" ||
      !pose.value("debug_evidence_enabled", false)) {
    throw std::runtime_error("phone did not apply the missed-shot HIL station configuration");
  }
  report["node_configuration"] = node;
  PersistReport(report_path, report);
}

void SleepAndVerifyDisplayOff(const std::filesystem::path &adb, std::string_view serial,
                              Json &report, const std::filesystem::path &report_path,
                              std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  const auto latest_verification_deadline = deadline - kMissedShotPostScreenOffReserve;
  if (now >= latest_verification_deadline) {
    throw std::runtime_error("insufficient HIL time remains to verify screen-off before post-roll");
  }
  const auto verification_deadline =
      std::min(latest_verification_deadline, now + kScreenOffVerificationTimeout);
  RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "input", "keyevent", "KEYCODE_SLEEP"}),
                 verification_deadline);
  std::string power;
  std::string display;
  std::string latest_diagnostic = "screen-off state has not been sampled";
  while (std::chrono::steady_clock::now() < verification_deadline) {
    power = RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "dumpsys", "power"}),
                           verification_deadline);
    WriteArtifact(OutputDirectory() / "screen_off_power.txt", power);
    display = RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "dumpsys", "display"}),
                             verification_deadline);
    WriteArtifact(OutputDirectory() / "screen_off_display.txt", display);
    const android_hil::DisplayPowerStateInspection screen =
        android_hil::InspectDisplayPowerState({power, display});
    latest_diagnostic = screen.diagnostic;
    if (screen.confirmed_off()) {
      report["screen_off"] = {
          {"sleep_key_sent", true},
          {"noninteractive", screen.non_interactive},
          {"display_off", screen.display_off},
          {"diagnostic", screen.diagnostic},
      };
      PersistReport(report_path, report);
      return;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        verification_deadline - std::chrono::steady_clock::now());
    if (remaining > 0s) {
      std::this_thread::sleep_for(std::min(100ms, remaining));
    }
  }
  throw std::runtime_error("phone did not become noninteractive with its display off within " +
                           std::to_string(kScreenOffVerificationTimeout.count()) +
                           " seconds: " + latest_diagnostic);
}

pose_hil::StandbyMissedEvidence CollectStandbyDiagnosticEvidence(
    const std::filesystem::path &adb, std::string_view serial, std::uint16_t host_port,
    std::string_view control_token, std::string_view session_id,
    pose_hil::StandbyDiagnosticExpectation expectation, Json &report,
    const std::filesystem::path &report_path, std::chrono::steady_clock::time_point deadline) {
  const std::string app_root = "files/sessions/" + std::string(session_id) + "/";
  const std::string manifest = ReadAppFile(adb, serial, app_root + "manifest.json", deadline);
  const std::string wav = ReadAppFile(adb, serial, app_root + "diagnostic_audio.wav", deadline);
  const std::string incident =
      ReadAppFile(adb, serial, app_root + "diagnostic_incident.json", deadline);
  const std::string preview =
      ReadAppFile(adb, serial, app_root + "pose_diagnostics/preview_frames.mjpeg", deadline);
  const std::string trace =
      ReadAppFile(adb, serial, app_root + "pose_diagnostics/pose_trace.ndjson", deadline);
  const HttpResponse archive = HttpRequest(
      host_port, "GET", "/api/v1/sessions/" + std::string(session_id) + "/diagnostics.zip",
      control_token, "", deadline, 128ULL * 1024ULL * 1024ULL + 64ULL * 1024ULL);
  if (archive.status != 200 || !archive.headers.contains("content-type") ||
      archive.headers.at("content-type") != "application/zip") {
    throw std::runtime_error("authenticated diagnostics ZIP download failed");
  }

  const std::filesystem::path artifact_root = OutputDirectory() / "session";
  WriteArtifact(artifact_root / "manifest.json", manifest);
  WriteArtifact(artifact_root / "diagnostic_audio.wav", wav);
  WriteArtifact(artifact_root / "diagnostic_incident.json", incident);
  WriteArtifact(artifact_root / "pose_diagnostics" / "preview_frames.mjpeg", preview);
  WriteArtifact(artifact_root / "pose_diagnostics" / "pose_trace.ndjson", trace);
  WriteArtifact(artifact_root / "diagnostics.zip", archive.body);

  const pose_hil::StandbyMissedEvidence evidence = pose_hil::ValidateStandbyMissedEvidence({
      .manifest = manifest,
      .diagnostic_audio_wav = wav,
      .diagnostic_incident = incident,
      .preview_mjpeg = preview,
      .pose_trace_ndjson = trace,
      .diagnostics_zip = archive.body,
      .expected_session_id = session_id,
      .expectation = expectation,
  });
  const std::string report_key =
      expectation == pose_hil::StandbyDiagnosticExpectation::kOperatorMissedShot
          ? "standby_missed_evidence"
          : "standby_impact_evidence";
  report[report_key] = {
      {"valid", evidence.valid},
      {"diagnostic", evidence.diagnostic},
      {"wav_bytes", evidence.wav_bytes},
      {"sample_count", evidence.sample_count},
      {"pre_roll_frames", evidence.pre_roll_frames},
      {"post_roll_frames", evidence.post_roll_frames},
      {"startup_short_pre_roll", evidence.startup_short_pre_roll},
      {"preview_frame_count", evidence.preview_frame_count},
      {"zip_bytes", evidence.zip_bytes},
      {"zip_entry_count", evidence.zip_entry_count},
  };
  PersistReport(report_path, report);
  if (!evidence.valid) {
    throw std::runtime_error(evidence.diagnostic);
  }
  return evidence;
}

pose_hil::StandbyMissedEvidence RunStandbyMissedShotWorkflow(
    const std::filesystem::path &adb, std::string_view serial, std::string_view role,
    std::uint16_t host_port, std::string &control_token, Json &report,
    const std::filesystem::path &report_path, std::chrono::steady_clock::time_point started,
    std::chrono::steady_clock::time_point deadline) {
  VerifyConfiguredNode(host_port, role, report, report_path, deadline);
  const Json before_sleep =
      WaitForMissedShotStandby(host_port, 24'000, 3, 1, report, report_path, started, deadline);
  const pose_hil::PoseStatusSample before_sample = pose_hil::InspectPoseStatus(before_sleep.dump());
  if (!before_sample.valid) {
    throw std::runtime_error(before_sample.diagnostic);
  }
  report["actual_delegate"] = before_sample.actual_delegate;
  control_token = ReadControlToken(adb, serial, deadline);
  SleepAndVerifyDisplayOff(adb, serial, report, report_path, deadline);

  const auto tag_readiness_deadline = deadline - kMissedShotPostTagReserve;
  if (std::chrono::steady_clock::now() >= tag_readiness_deadline) {
    throw std::runtime_error(
        "insufficient HIL time remains to collect three standby JPEGs before post-roll");
  }
  const Json screen_off_status =
      WaitForMissedShotStandby(host_port, before_sample.standby_audio_end_frame_position + 24'000,
                               before_sample.successful_inferences + 1,
                               std::max<std::uint64_t>(3, before_sample.encoded_evidence_frames),
                               report, report_path, started, tag_readiness_deadline);
  const pose_hil::PoseStatusSample screen_off_sample =
      pose_hil::InspectPoseStatus(screen_off_status.dump());
  if (!screen_off_sample.valid ||
      screen_off_sample.encoded_evidence_frames < before_sample.encoded_evidence_frames ||
      screen_off_sample.actual_delegate != before_sample.actual_delegate) {
    throw std::runtime_error("pose/audio standby did not remain stable after screen-off");
  }
  report["screen_off"]["pose_and_audio_advanced"] = true;
  report["screen_off"]["audio_frames_advanced"] =
      screen_off_sample.standby_audio_end_frame_position -
      before_sample.standby_audio_end_frame_position;
  report["screen_off"]["successful_inferences_advanced"] =
      screen_off_sample.successful_inferences - before_sample.successful_inferences;
  report["screen_off"]["encoded_evidence_frames"] = screen_off_sample.encoded_evidence_frames;
  report["screen_off"]["encoded_evidence_frames_advanced"] =
      screen_off_sample.encoded_evidence_frames - before_sample.encoded_evidence_frames;
  PersistReport(report_path, report);

  const HttpResponse tagged =
      HttpRequest(host_port, "POST", "/api/v1/capture/missed-shot", control_token, "", deadline);
  report["missed_shot_http_status"] = tagged.status;
  if (tagged.status != 202) {
    throw std::runtime_error("missed-shot endpoint returned HTTP " + std::to_string(tagged.status) +
                             ": " + tagged.body);
  }
  const std::string session_id = Json::parse(tagged.body).at("session_id").get<std::string>();
  if (!SafeFileSegment(session_id)) {
    throw std::runtime_error("missed-shot endpoint returned an unsafe session ID");
  }
  report["session_id"] = session_id;
  PersistReport(report_path, report);

  bool published = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const HttpResponse sessions =
        HttpRequest(host_port, "GET", "/api/v1/sessions", "", "", deadline);
    if (sessions.status == 200) {
      for (const Json &session : Json::parse(sessions.body).at("sessions")) {
        if (session.value("session_id", "") == session_id &&
            session.value("state", "") == "ready" &&
            session.value("session_kind", "") == "standby_diagnostic") {
          published = true;
          break;
        }
      }
    }
    if (published) {
      break;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (!published) {
    throw std::runtime_error("standby missed-shot session was not published after post-roll");
  }
  return CollectStandbyDiagnosticEvidence(
      adb, serial, host_port, control_token, session_id,
      pose_hil::StandbyDiagnosticExpectation::kOperatorMissedShot, report, report_path, deadline);
}

std::set<std::string, std::less<>> PublishedStandbyDiagnosticIds(
    std::uint16_t host_port, std::chrono::steady_clock::time_point deadline) {
  const HttpResponse sessions = HttpRequest(host_port, "GET", "/api/v1/sessions", "", "", deadline);
  if (sessions.status != 200) {
    throw std::runtime_error("session list returned HTTP " + std::to_string(sessions.status));
  }
  std::set<std::string, std::less<>> ids;
  for (const Json &session : Json::parse(sessions.body).at("sessions")) {
    if (session.value("state", "") != "ready" ||
        session.value("session_kind", "") != "standby_diagnostic") {
      continue;
    }
    const std::string id = session.at("session_id").get<std::string>();
    if (!SafeFileSegment(id) || !ids.insert(id).second) {
      throw std::runtime_error("session list contains an unsafe or duplicate diagnostic ID");
    }
  }
  return ids;
}

swing_capture::hil::FeatherStimulusReceipt PlayStandbyImpactTone() {
  const auto config_path = swing_capture::station::StationConfigPathFromEnvironment();
  if (!config_path.has_value()) {
    throw std::runtime_error("SWING_CAPTURE_STATION_CONFIG is required for Feather impact HIL");
  }
  const auto station = swing_capture::station::LoadStationConfig(*config_path);
  swing_capture::hil::FeatherHilSerial serial(station.feather_serial_path);
  swing_capture::hil::FeatherHilController feather(serial);
  const auto info = feather.QueryInfo();
  if (info.tone_maximum_level_permille == 0U) {
    throw std::runtime_error("Feather did not advertise a usable tone level");
  }
  return feather.PlayTone(kToneLead, kToneDuration, kToneFrequencyHz,
                          info.tone_maximum_level_permille);
}

pose_hil::StandbyMissedEvidence RunStandbyImpactWorkflow(
    const std::filesystem::path &adb, std::string_view serial, std::string_view role,
    std::uint16_t host_port, std::string &control_token, Json &report,
    const std::filesystem::path &report_path, std::chrono::steady_clock::time_point started,
    std::chrono::steady_clock::time_point deadline) {
  VerifyConfiguredNode(host_port, role, report, report_path, deadline);
  const Json ready =
      WaitForMissedShotStandby(host_port, 48'000, 3, 3, report, report_path, started, deadline);
  const pose_hil::PoseStatusSample ready_sample = pose_hil::InspectPoseStatus(ready.dump());
  const Json &ready_audio = ready.at("pose").at("standby_audio");
  if (!ready_sample.valid || ready_audio.value("pending_events", -1) != 0) {
    throw std::runtime_error("standby was not quiet before the Feather impact stimulus");
  }
  const std::uint64_t baseline_detected = ready_audio.at("detected_events").get<std::uint64_t>();
  const std::uint64_t baseline_operator_tags = ready_audio.at("operator_tags").get<std::uint64_t>();
  const std::uint64_t baseline_published =
      ready_audio.at("published_sessions").get<std::uint64_t>();
  const std::set<std::string, std::less<>> baseline_ids =
      PublishedStandbyDiagnosticIds(host_port, deadline);
  control_token = ReadControlToken(adb, serial, deadline);
  report["actual_delegate"] = ready_sample.actual_delegate;
  report["standby_impact_baseline"] = {
      {"detected_events", baseline_detected},
      {"operator_tags", baseline_operator_tags},
      {"published_sessions", baseline_published},
      {"published_session_count", baseline_ids.size()},
  };
  PersistReport(report_path, report);

  const swing_capture::hil::FeatherStimulusReceipt stimulus = PlayStandbyImpactTone();
  report["feather_stimulus"] = {
      {"request_id", stimulus.request_id},
      {"accepted_device_us", stimulus.accepted_device_microseconds},
      {"scheduled_device_us", stimulus.scheduled_device_microseconds},
      {"start_device_us", stimulus.start_device_microseconds},
      {"end_device_us", stimulus.end_device_microseconds},
      {"frequency_hz", stimulus.frequency_hz},
      {"level_permille", stimulus.level_permille},
      {"sample_rate_hz", stimulus.sample_rate_hz},
      {"sample_count", stimulus.sample_count},
      {"lead_millis", kToneLead.count()},
      {"duration_millis", kToneDuration.count()},
  };
  PersistReport(report_path, report);

  std::string published_session_id;
  while (std::chrono::steady_clock::now() < deadline) {
    const HttpResponse status_response =
        HttpRequest(host_port, "GET", "/api/v1/capture/status", "", "", deadline);
    if (status_response.status == 200) {
      AppendStatusEvidence(report, status_response.body, started, report_path);
      const Json status = Json::parse(status_response.body);
      const pose_hil::PoseStatusSample sample = pose_hil::InspectPoseStatus(status_response.body);
      const Json &audio = status.at("pose").at("standby_audio");
      const std::uint64_t detected = audio.at("detected_events").get<std::uint64_t>();
      const std::uint64_t operator_tags = audio.at("operator_tags").get<std::uint64_t>();
      const std::uint64_t published = audio.at("published_sessions").get<std::uint64_t>();
      if (!sample.valid || sample.failed_inferences != 0 ||
          sample.standby_audio_dropped_events != 0 || sample.standby_audio_discontinuities != 0 ||
          !sample.standby_audio_last_error.empty() || detected > baseline_detected + 1U ||
          operator_tags != baseline_operator_tags || published > baseline_published + 1U) {
        throw std::runtime_error("standby impact counters or joint pose/audio health are invalid");
      }
      const auto current_ids = PublishedStandbyDiagnosticIds(host_port, deadline);
      std::vector<std::string> new_ids;
      std::ranges::set_difference(current_ids, baseline_ids, std::back_inserter(new_ids));
      if (new_ids.size() > 1U) {
        throw std::runtime_error("Feather stimulus produced ambiguous diagnostic sessions");
      }
      if (new_ids.size() == 1U && detected == baseline_detected + 1U &&
          published == baseline_published + 1U) {
        published_session_id = std::move(new_ids.front());
        report["standby_impact_final_counters"] = {
            {"detected_events", detected},
            {"operator_tags", operator_tags},
            {"published_sessions", published},
        };
        break;
      }
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (published_session_id.empty()) {
    throw std::runtime_error("Feather tone did not publish one automatic standby impact session");
  }
  report["session_id"] = published_session_id;
  PersistReport(report_path, report);
  return CollectStandbyDiagnosticEvidence(
      adb, serial, host_port, control_token, published_session_id,
      pose_hil::StandbyDiagnosticExpectation::kAutomaticImpact, report, report_path, deadline);
}

Json WaitForDebugPoseStandby(std::uint16_t host_port, Json &report,
                             const std::filesystem::path &report_path,
                             std::chrono::steady_clock::time_point started,
                             std::chrono::steady_clock::time_point deadline) {
  std::string latest_diagnostic = "pose standby status has not appeared";
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      const HttpResponse response =
          HttpRequest(host_port, "GET", "/api/v1/capture/status", "", "", deadline);
      if (response.status != 200) {
        latest_diagnostic = "capture status returned HTTP " + std::to_string(response.status);
      } else {
        AppendStatusEvidence(report, response.body, started, report_path);
        Json status = Json::parse(response.body);
        const Json &pose = status.at("pose");
        const pose_hil::PoseStatusSample sample = pose_hil::InspectPoseStatus(response.body);
        latest_diagnostic = sample.diagnostic;
        if (sample.valid && sample.failed_inferences > 0) {
          throw std::runtime_error("pose inference failed before the warm transition");
        }
        if (status.value("state", "") == "error") {
          throw std::runtime_error("pose standby failed before the warm transition: " +
                                   status.value("error", "unknown error"));
        }
        if (sample.valid && sample.state == "armed" && sample.armed &&
            sample.phase == "monitoring" && sample.mode == "shadow" &&
            sample.configured_delegate == "gpu_preferred" &&
            pose.value("debug_evidence_enabled", false) &&
            pose_hil::HasMinimumEncodedPoseEvidence(sample, 3)) {
          return status;
        }
        if (sample.valid) {
          latest_diagnostic = "pose standby has completed " +
                              std::to_string(sample.encoded_evidence_frames) +
                              " of three required debug-preview JPEGs";
        }
      }
    } catch (const nlohmann::json::exception &malformed) {
      latest_diagnostic = malformed.what();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("debug pose standby was not ready: " + latest_diagnostic);
}

void WaitForHighSpeedReady(std::uint16_t host_port, Json &report,
                           const std::filesystem::path &report_path,
                           std::chrono::steady_clock::time_point started,
                           std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      const HttpResponse response =
          HttpRequest(host_port, "GET", "/api/v1/capture/status", "", "", deadline);
      if (response.status == 200) {
        AppendStatusEvidence(report, response.body, started, report_path);
        const Json status = Json::parse(response.body);
        const Json &pose = status.at("pose");
        if (status.value("state", "") == "error") {
          throw std::runtime_error("high-speed transition failed: " +
                                   status.value("error", "unknown error"));
        }
        if (status.value("state", "") == "armed" && status.value("armed", false) &&
            pose.value("phase", "") == "high_speed" &&
            status.value("ring_duration_us", 0L) >= 1'300'000L) {
          return;
        }
      }
    } catch (const nlohmann::json::exception &) {
      // Startup status can be observed between service state publications.
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("high-speed capture did not fill its pre-roll");
}

pose_hil::WarmRetainedEvidence RunWarmRetainedWorkflow(
    const std::filesystem::path &adb, std::string_view serial, std::string_view role,
    std::uint16_t host_port, std::string &control_token, Json &report,
    const std::filesystem::path &report_path, std::chrono::steady_clock::time_point started,
    std::chrono::steady_clock::time_point deadline) {
  const auto pose_arm_acceptance_deadline = deadline - kWarmPostArmReserve;
  const auto standby_readiness_deadline = pose_arm_acceptance_deadline - kWarmArmRequestBudget;
  if (std::chrono::steady_clock::now() >= standby_readiness_deadline) {
    throw std::runtime_error(
        "insufficient HIL time remains to collect three warm-preview JPEGs before capture");
  }
  control_token = ReadControlToken(adb, serial, standby_readiness_deadline);
  const Json monitoring_status =
      WaitForDebugPoseStandby(host_port, report, report_path, started, standby_readiness_deadline);
  const pose_hil::PoseStatusSample monitoring_sample =
      pose_hil::InspectPoseStatus(monitoring_status.dump());
  if (!pose_hil::HasMinimumEncodedPoseEvidence(monitoring_sample, 3)) {
    throw std::runtime_error("warm pose standby returned without three completed JPEGs");
  }
  report["actual_delegate"] = monitoring_sample.actual_delegate;
  report["warm_standby_readiness"] = {
      {"successful_inferences", monitoring_sample.successful_inferences},
      {"encoded_evidence_frames", monitoring_sample.encoded_evidence_frames},
      {"post_arm_reserve_millis",
       std::chrono::duration_cast<std::chrono::milliseconds>(kWarmPostArmReserve).count()},
  };
  const std::string shared_session_id =
      "warm-hil-" + monitoring_status.at("server_elapsed_realtime_ns").get<std::string>();
  report["shared_session_id"] = shared_session_id;
  const Json pose_arm_request = {
      {"schema_version", 1},
      {"shared_session_id", shared_session_id},
      {"leader_node_id", "pose-warm-hil-controller"},
      {"candidate_elapsed_realtime_ns", monitoring_status.at("server_elapsed_realtime_ns")},
      {"person_confidence", 1.0},
      {"address_confidence", 1.0},
  };
  const HttpResponse pose_arm =
      HttpRequest(host_port, "POST", "/api/v1/capture/pose-arm", control_token,
                  pose_arm_request.dump(), pose_arm_acceptance_deadline);
  report["pose_arm_http_status"] = pose_arm.status;
  const auto remaining_after_pose_arm = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  report["warm_standby_readiness"]["remaining_after_pose_arm_millis"] =
      remaining_after_pose_arm.count();
  PersistReport(report_path, report);
  if (pose_arm.status != 202) {
    throw std::runtime_error("pose-arm endpoint rejected warm transition with HTTP " +
                             std::to_string(pose_arm.status) + ": " + pose_arm.body);
  }
  if (remaining_after_pose_arm < kWarmPostArmReserve) {
    throw std::runtime_error("pose-arm was not accepted with the required 8.5 second reserve");
  }
  WaitForHighSpeedReady(host_port, report, report_path, started, deadline);
  report["high_speed_armed"] = true;
  PersistReport(report_path, report);

  const HttpResponse manual =
      HttpRequest(host_port, "POST", "/api/v1/capture/manual", control_token, "", deadline);
  report["manual_trigger_http_status"] = manual.status;
  if (manual.status != 202) {
    throw std::runtime_error("manual retained trigger returned HTTP " +
                             std::to_string(manual.status) + ": " + manual.body);
  }
  const std::string session_id = Json::parse(manual.body).at("session_id").get<std::string>();
  if (!SafeFileSegment(session_id)) {
    throw std::runtime_error("manual trigger returned an unsafe retained session ID");
  }
  report["session_id"] = session_id;
  PersistReport(report_path, report);

  bool published = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const HttpResponse sessions =
        HttpRequest(host_port, "GET", "/api/v1/sessions", "", "", deadline);
    if (sessions.status == 200) {
      for (const Json &session : Json::parse(sessions.body).at("sessions")) {
        if (session.value("session_id", "") == session_id &&
            session.value("state", "") == "ready") {
          published = true;
          break;
        }
      }
    }
    if (published) {
      break;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (!published) {
    throw std::runtime_error("retained warm-transition session was not published");
  }

  const std::string app_session_root = "files/sessions/" + session_id + "/";
  const std::string media_name = std::string(role) + ".mp4";
  const std::string manifest =
      ReadAppFile(adb, serial, app_session_root + "manifest.json", deadline);
  const std::string media = ReadAppFile(adb, serial, app_session_root + media_name, deadline);
  const std::string preview = ReadAppFile(
      adb, serial, app_session_root + "pose_diagnostics/preview_frames.mjpeg", deadline);
  const std::string trace =
      ReadAppFile(adb, serial, app_session_root + "pose_diagnostics/pose_trace.ndjson", deadline);

  const std::filesystem::path artifact_root = OutputDirectory() / "session";
  WriteArtifact(artifact_root / "manifest.json", manifest);
  WriteArtifact(artifact_root / media_name, media);
  WriteArtifact(artifact_root / "pose_diagnostics" / "preview_frames.mjpeg", preview);
  WriteArtifact(artifact_root / "pose_diagnostics" / "pose_trace.ndjson", trace);

  pose_hil::WarmRetainedEvidence evidence = pose_hil::ValidateWarmRetainedEvidence({
      .manifest = manifest,
      .media = media,
      .preview_mjpeg = preview,
      .pose_trace_ndjson = trace,
      .expected_session_id = session_id,
      .expected_shared_session_id = shared_session_id,
      .expected_role = role,
  });
  report["retained_evidence"] = {
      {"valid", evidence.valid},
      {"diagnostic", evidence.diagnostic},
      {"media_path", evidence.media_path},
      {"media_bytes", evidence.media_bytes},
      {"frame_count", evidence.frame_count},
      {"preview_frame_count", evidence.preview_frame_count},
      {"camera_to_encoder_ordinal_shift", evidence.camera_to_encoder_ordinal_shift},
      {"encoder_to_sensor_offset_ns", std::to_string(evidence.encoder_to_sensor_offset_ns)},
      {"expected_encoder_to_sensor_offset_ns",
       std::to_string(evidence.expected_encoder_to_sensor_offset_ns)},
      {"encoder_to_sensor_offset_residual_ns", evidence.encoder_to_sensor_offset_residual_ns},
      {"timestamp_offset_span_ns", evidence.timestamp_offset_span_ns},
      {"timestamp_pair_count", evidence.timestamp_pair_count},
      {"actual_pre_roll_us", evidence.actual_pre_roll_us},
      {"actual_post_roll_us", evidence.actual_post_roll_us},
  };
  PersistReport(report_path, report);
  if (!evidence.valid) {
    throw std::runtime_error(evidence.diagnostic);
  }
  return evidence;
}

int Run(int argument_count, char **arguments) {
  const bool warm_retained =
      argument_count == 4 && std::string_view(arguments[3]) == "--warm-retained";
  const bool standby_missed_shot =
      argument_count == 4 && std::string_view(arguments[3]) == "--standby-missed-shot";
  const bool standby_impact =
      argument_count == 4 && std::string_view(arguments[3]) == "--standby-impact";
  if (argument_count != 3 && !warm_retained && !standby_missed_shot && !standby_impact) {
    throw std::invalid_argument(
        "usage: pose_standby_hil_test <adb> <signed-apk> "
        "[--warm-retained|--standby-missed-shot|--standby-impact]");
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
  std::string role = EnvironmentValue("SWING_CAPTURE_ANDROID_ROLE");
  if (role.empty()) {
    role = "down_the_line";
  }
  if (role != "down_the_line" && role != "face_on") {
    throw std::runtime_error("SWING_CAPTURE_ANDROID_ROLE must be down_the_line or face_on");
  }

  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + kTotalDeadline;
  const std::filesystem::path report_path = OutputDirectory() / "report.json";
  Json report = {
      {"schema_version", 1},
      {"report_type",
       warm_retained
           ? "android_pose_warm_retained_hil"
           : (standby_missed_shot ? "android_pose_standby_missed_shot_hil"
                                  : (standby_impact ? "android_pose_standby_impact_diagnostic_hil"
                                                    : "android_pose_standby_hil"))},
      {"complete", false},
      {"passed", false},
      {"serial", serial},
      {"role", role},
      {"capture_profile", "720p240"},
      {"pose_mode", "shadow"},
      {"configured_delegate", "gpu_preferred"},
      {"debug_evidence_enabled", warm_retained || standby_missed_shot || standby_impact},
      {"acceptance",
       {{"minimum_interval_seconds", 2.0},
        {"minimum_successful_inferences_per_second", 4.0},
        {"maximum_dropped_fraction_after_two_drops", 0.20},
        {"maximum_failed_inferences", 0},
        {"require_advancing_standby_audio", true},
        {"maximum_standby_audio_dropped_events", 0},
        {"maximum_standby_audio_discontinuities", 0},
        {"maximum_startup_audio_timestamp_rejections", 2}}},
      {"status_samples", Json::array()},
  };
  if (warm_retained) {
    report["acceptance"] = {
        {"maximum_duration_millis", 15'000},
        {"minimum_video_frame_count", 450},
        {"minimum_preview_frame_count", 3},
        {"minimum_encoded_evidence_frames_before_arm", 3},
        {"minimum_post_arm_reserve_millis", 8'500},
        {"maximum_pose_arm_request_budget_millis", 250},
        {"maximum_absolute_camera_to_encoder_ordinal_shift", 32},
        {"maximum_encoder_to_sensor_offset_residual_ns", 1'000'000},
        {"maximum_timestamp_offset_span_ns", 100'000},
        {"minimum_timestamp_pair_count", 16},
        {"minimum_actual_pre_roll_us", 1'300'000},
        {"minimum_actual_post_roll_us", 450'000},
    };
  } else if (standby_missed_shot) {
    report["acceptance"] = {
        {"maximum_duration_millis", 15'000},
        {"sample_rate_hz", 48'000},
        {"full_pre_roll_frames", 480'000},
        {"minimum_startup_short_pre_roll_frames", 24'000},
        {"exact_post_roll_frames", 96'000},
        {"minimum_preview_frame_count", 3},
        {"diagnostics_zip_entry_count", 6},
        {"maximum_startup_audio_timestamp_rejections", 2},
        {"require_screen_off_pose_audio_progress", true},
    };
  } else if (standby_impact) {
    report["acceptance"] = {
        {"maximum_duration_millis", 15'000},
        {"sample_rate_hz", 48'000},
        {"minimum_audio_before_stimulus_frames", 48'000},
        {"full_pre_roll_frames", 480'000},
        {"minimum_startup_short_pre_roll_frames", 24'000},
        {"exact_post_roll_frames", 96'000},
        {"minimum_preview_frame_count", 3},
        {"diagnostics_zip_entry_count", 6},
        {"maximum_startup_audio_timestamp_rejections", 2},
        {"expected_event_kind", "detected_impact"},
        {"expected_incident_classification", "impact_while_not_armed"},
        {"expected_new_session_count", 1},
    };
  }
  PersistReport(report_path, report);
  std::optional<std::uint16_t> host_port;
  std::string control_token;
  try {
    report["device"] = {
        {"model",
         Trim(RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "getprop", "ro.product.model"}),
                             deadline))},
        {"android_release",
         Trim(RunRequiredAdb(
             adb, DeviceArguments(serial, {"shell", "getprop", "ro.build.version.release"}),
             deadline))},
        {"api_level",
         Trim(RunRequiredAdb(adb,
                             DeviceArguments(serial, {"shell", "getprop", "ro.build.version.sdk"}),
                             deadline))},
    };
    RunRequiredAdb(adb,
                   DeviceArguments(serial, {"install", "--no-streaming", "-r", "-t", apk.string()}),
                   deadline);
    RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "am", "force-stop", kPackageName}),
                   deadline);
    for (std::string_view permission : {
             "android.permission.CAMERA",
             "android.permission.RECORD_AUDIO",
             "android.permission.POST_NOTIFICATIONS",
         }) {
      RunRequiredAdb(adb,
                     DeviceArguments(serial, {"shell", "pm", "grant", kPackageName, permission}),
                     deadline);
    }
    if (!warm_retained && !standby_missed_shot && !standby_impact) {
      const pose_hil::DeviceTelemetry before = CollectTelemetry(adb, serial, deadline);
      report["telemetry_before"] = TelemetryJson(before);
      if (!before.valid) {
        throw std::runtime_error("pre-run telemetry is invalid: " + before.diagnostic);
      }
    }
    host_port = EstablishForward(adb, serial, deadline);
    RunRequiredAdb(
        adb, StartArguments(serial, role, warm_retained || standby_missed_shot || standby_impact),
        deadline);

    if (warm_retained) {
      const pose_hil::WarmRetainedEvidence evidence = RunWarmRetainedWorkflow(
          adb, serial, role, *host_port, control_token, report, report_path, started, deadline);
      const auto duration_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
      if (duration_millis >
          report["acceptance"].at("maximum_duration_millis").get<std::int64_t>()) {
        throw std::runtime_error("warm retained HIL exceeded its 15 second main-stage bound");
      }
      report["complete"] = true;
      report["passed"] = true;
      report["duration_millis"] = duration_millis;
      PersistReport(report_path, report);
      BestEffortCleanup(adb, serial, host_port, control_token, report, report_path);
      if (!report["cleanup"].value("disarm_accepted", false) ||
          !report["cleanup"].value("force_stop_succeeded", false) ||
          !report["cleanup"].value("forward_removed", false)) {
        throw std::runtime_error("warm retained HIL cleanup was incomplete");
      }
      std::cout << "Warm pose transition published " << evidence.frame_count
                << " high-speed frames and " << evidence.preview_frame_count
                << " preview frames on " << report["device"]["model"].get<std::string>() << " ("
                << serial << ")\nEvidence: " << report_path << '\n';
      return 0;
    }

    if (standby_missed_shot) {
      const pose_hil::StandbyMissedEvidence evidence = RunStandbyMissedShotWorkflow(
          adb, serial, role, *host_port, control_token, report, report_path, started, deadline);
      const auto duration_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
      if (duration_millis >
          report["acceptance"].at("maximum_duration_millis").get<std::int64_t>()) {
        throw std::runtime_error("standby missed-shot HIL exceeded its 15 second bound");
      }
      report["complete"] = true;
      report["passed"] = true;
      report["duration_millis"] = duration_millis;
      PersistReport(report_path, report);
      BestEffortCleanup(adb, serial, host_port, control_token, report, report_path, true);
      if (!report["cleanup"].value("disarm_accepted", false) ||
          !report["cleanup"].value("force_stop_succeeded", false) ||
          !report["cleanup"].value("screen_sleep_command_succeeded", false) ||
          !report["cleanup"].value("screen_off_preserved", false) ||
          !report["cleanup"].value("forward_removed", false)) {
        throw std::runtime_error("standby missed-shot HIL cleanup was incomplete");
      }
      std::cout << "Screen-off standby missed-shot published " << evidence.sample_count
                << " audio samples, " << evidence.preview_frame_count
                << " preview frames, and a validated diagnostics ZIP on "
                << report["device"]["model"].get<std::string>() << " (" << serial
                << ")\nEvidence: " << report_path << '\n';
      return 0;
    }

    if (standby_impact) {
      const pose_hil::StandbyMissedEvidence evidence = RunStandbyImpactWorkflow(
          adb, serial, role, *host_port, control_token, report, report_path, started, deadline);
      const auto duration_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
      if (duration_millis >
          report["acceptance"].at("maximum_duration_millis").get<std::int64_t>()) {
        throw std::runtime_error("standby impact HIL exceeded its 15 second bound");
      }
      report["complete"] = true;
      report["passed"] = true;
      report["duration_millis"] = duration_millis;
      PersistReport(report_path, report);
      BestEffortCleanup(adb, serial, host_port, control_token, report, report_path);
      if (!report["cleanup"].value("disarm_accepted", false) ||
          !report["cleanup"].value("force_stop_succeeded", false) ||
          !report["cleanup"].value("forward_removed", false)) {
        throw std::runtime_error("standby impact HIL cleanup was incomplete");
      }
      std::cout << "Feather standby impact published " << evidence.sample_count
                << " audio samples, " << evidence.preview_frame_count
                << " preview frames, and a validated diagnostics ZIP on "
                << report["device"]["model"].get<std::string>() << " (" << serial
                << ")\nEvidence: " << report_path << '\n';
      return 0;
    }

    std::optional<pose_hil::PoseStatusSample> first;
    std::optional<pose_hil::PoseCadenceAcceptance> acceptance;
    std::string latest_diagnostic = "pose status endpoint has not answered";
    while (std::chrono::steady_clock::now() < deadline) {
      try {
        const HttpResponse response =
            HttpRequest(*host_port, "GET", "/api/v1/capture/status", "", "", deadline);
        if (response.status != 200) {
          latest_diagnostic = "capture status returned HTTP " + std::to_string(response.status);
        } else {
          const pose_hil::PoseStatusSample sample = pose_hil::InspectPoseStatus(response.body);
          latest_diagnostic = sample.diagnostic;
          Json evidence = Json::parse(response.body);
          evidence["host_elapsed_millis"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - started)
                                                .count();
          report["status_samples"].push_back(std::move(evidence));
          PersistReport(report_path, report);
          if (sample.valid &&
              (sample.failed_inferences != 0 || sample.standby_audio_dropped_events != 0 ||
               sample.standby_audio_discontinuities != 0 ||
               sample.standby_audio_timestamp_rejections > 2 ||
               !sample.standby_audio_last_error.empty())) {
            latest_diagnostic = "pose or standby audio failed before the cadence interval";
            break;
          }
          if (sample.valid && sample.phase == "monitoring" && sample.mode == "shadow" &&
              sample.configured_delegate == "gpu_preferred" && sample.armed &&
              sample.successful_inferences > 0 && sample.standby_audio_ready &&
              sample.standby_audio_end_frame_position > 0) {
            if (!first.has_value()) {
              first = sample;
            } else if (sample.server_elapsed_realtime_ns - first->server_elapsed_realtime_ns >=
                       2'000'000'000ULL) {
              acceptance = pose_hil::EvaluatePoseCadence(*first, sample);
              break;
            }
          }
        }
      } catch (const std::exception &startup) {
        latest_diagnostic = startup.what();
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining > 0s) {
        std::this_thread::sleep_for(std::min(kPollInterval, remaining));
      }
    }
    if (!acceptance.has_value()) {
      throw std::runtime_error("pose cadence interval was not completed: " + latest_diagnostic);
    }
    report["cadence"] = {
        {"passed", acceptance->passed},
        {"diagnostic", acceptance->diagnostic},
        {"interval_seconds", acceptance->interval_seconds},
        {"successful_inferences_per_second", acceptance->successful_inferences_per_second},
        {"offered_images", acceptance->offered_images},
        {"scheduled_images", acceptance->scheduled_images},
        {"dropped_images", acceptance->dropped_images},
        {"dropped_fraction", acceptance->dropped_fraction},
        {"successful_inferences", acceptance->successful_inferences},
        {"failed_inferences", acceptance->failed_inferences},
    };
    if (!acceptance->passed) {
      throw std::runtime_error(acceptance->diagnostic);
    }
    const Json &last_status = report["status_samples"].back();
    report["actual_delegate"] = last_status.at("pose").at("metrics").at("delegate");

    const pose_hil::DeviceTelemetry after = CollectTelemetry(adb, serial, deadline);
    report["telemetry_after"] = TelemetryJson(after);
    if (!after.valid) {
      throw std::runtime_error("post-run telemetry is invalid: " + after.diagnostic);
    }
    control_token = ReadControlToken(adb, serial, deadline);
    report["complete"] = true;
    report["passed"] = true;
    report["duration_millis"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
    PersistReport(report_path, report);
    BestEffortCleanup(adb, serial, host_port, control_token, report, report_path);
    if (!report["cleanup"].value("disarm_accepted", false) ||
        !report["cleanup"].value("force_stop_succeeded", false) ||
        !report["cleanup"].value("forward_removed", false)) {
      throw std::runtime_error("pose standby HIL cleanup was incomplete");
    }
    std::cout << "Pose standby sustained " << acceptance->successful_inferences_per_second
              << " Hz using " << report["actual_delegate"].get<std::string>() << " on "
              << report["device"]["model"].get<std::string>() << " (" << serial << ")\n"
              << "Evidence: " << report_path << '\n';
    return 0;
  } catch (const std::exception &failure) {
    report["complete"] = true;
    report["passed"] = false;
    report["error"] = failure.what();
    report["duration_millis"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
    BestEffortCleanup(adb, serial, host_port, control_token, report, report_path,
                      standby_missed_shot);
    throw;
  }
}

}  // namespace

int main(int argument_count, char **arguments) {
  try {
    return Run(argument_count, arguments);
  } catch (const std::exception &failure) {
    std::cerr << "Android pose standby HIL failed: " << failure.what() << '\n';
    return 1;
  }
}
