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
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "android/dual_coordination_hil/dual_coordination.h"
#include "android/dual_hil/audio_evidence_validation.h"
#include "android/dual_hil/concurrent_hil_validation.h"
#include "android/dual_hil/dual_session_validation.h"
#include "android/dual_hil/rgb_swing_analysis.h"
#include "capture/hil/feather_hil_controller.h"
#include "capture/hil/feather_hil_serial.h"
#include "capture/optical/april_tag.h"
#include "station/station_config.h"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
namespace coordination = swing_capture::android::dual_coordination_hil;
using swing_capture::android::dual_hil::AnalyzeRetainedAudioEvidence;
using swing_capture::android::dual_hil::AprilTagFrameEvidence;
using swing_capture::android::dual_hil::ArmedStatusInspection;
using swing_capture::android::dual_hil::CaptureProfileExpectation;
using swing_capture::android::dual_hil::EvaluateTimingCorrelation;
using swing_capture::android::dual_hil::NodeApiIdentity;
using swing_capture::android::dual_hil::NodeEvidence;
using swing_capture::android::dual_hil::NodeEvidenceInspection;
using swing_capture::android::dual_hil::PoseConfiguredDescriptorInspection;
using swing_capture::android::dual_hil::RetainedAudioEvidence;
using swing_capture::android::dual_hil::RetainedAudioEvidenceInspection;
using swing_capture::android::dual_hil::RgbFrameTiming;
using swing_capture::android::dual_hil::RgbSwingAnalysis;
using swing_capture::android::dual_hil::RgbSwingSequenceAnalyzer;
using swing_capture::android::dual_hil::RgbSwingSequenceConfiguration;
using swing_capture::android::dual_hil::TimingCorrelationEvidence;
using swing_capture::android::dual_hil::ValidateArmedCaptureStatus;
using swing_capture::android::dual_hil::ValidateCanonicalCoordinationReplay;
using swing_capture::android::dual_hil::ValidateDualSession;
using swing_capture::android::dual_hil::ValidateFfprobeTimeline;
using swing_capture::android::dual_hil::ValidateNodeDescriptor;
using swing_capture::android::dual_hil::ValidateNodeEvidence;
using swing_capture::android::dual_hil::ValidatePairedPoseHilReport;
using swing_capture::android::dual_hil::ValidatePoseConfiguredNodeDescriptor;
using swing_capture::android::dual_hil::ValidateRequiredAprilTagPersistence;
using swing_capture::android::dual_hil::ValidateTriggerReport;

constexpr auto kStageDeadline = 15s;
constexpr auto kPollInterval = 100ms;
constexpr auto kQuietQualification = 1s;
constexpr std::string_view kPackageName = "com.agoessling.swingcapture";
constexpr std::string_view kReportPath = "files/reports/latest.json";
constexpr std::uint32_t kMaximumAnalysisWidth = 640U;
constexpr std::uint16_t kNodeHttpPort = 8088U;
constexpr std::uint16_t kPosePeerTunnelPort = 18089U;
constexpr std::size_t kClockExchangeSampleCount = 5U;

bool SafeSessionId(std::string_view session_id);

struct RoleCaptureProfile {
  std::string_view name;
  CaptureProfileExpectation capture;
};

RoleCaptureProfile ProfileForRole(std::string_view role) {
  if (role == "down_the_line" || role == "face_on") {
    return {
        .name = "720p240",
        .capture = {.width = 1280, .height = 720, .bitrate_bits_per_second = 12000000},
    };
  }
  throw std::invalid_argument("dual Android HIL role has no qualified capture profile");
}

struct CommandResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string output;
};

struct CapturedNode {
  NodeEvidence evidence;
  std::string report;
  std::string manifest;
  std::string media;
  std::string audio_wav;
  RetainedAudioEvidence audio;
  std::int64_t maximum_media_time_residual_us = 0;
  RgbSwingAnalysis optical;
  std::array<swing_capture::optical::AprilTagDetection, 3> april_tags;
  std::array<std::size_t, 3> diagnostic_frame_indices = {};
  swing_capture::hil::FeatherSwingReceipt feather;
  std::int64_t capture_stage_milliseconds = 0;
  std::int64_t artifact_pull_stage_milliseconds = 0;
  std::int64_t ffprobe_stage_milliseconds = 0;
  std::int64_t decode_stage_milliseconds = 0;
  std::int64_t diagnostic_stage_milliseconds = 0;
  std::int64_t audio_stage_milliseconds = 0;
  std::uint32_t analysis_width = 0;
  std::uint32_t analysis_height = 0;
};

struct ExternalTools {
  std::filesystem::path ffmpeg;
  std::filesystem::path ffprobe;
  std::string ffmpeg_version;
  std::string ffprobe_version;
};

Json FeatherReceiptJson(const swing_capture::hil::FeatherSwingReceipt &receipt);

Json FeatherCalibrationJson(const swing_capture::hil::FeatherCalibrationReceipt &receipt) {
  return {
      {"request_id", receipt.request_id},
      {"accepted_device_us", receipt.accepted_device_microseconds},
      {"start_scheduled_device_us", receipt.start_scheduled_device_microseconds},
      {"start_device_us", receipt.start_device_microseconds},
      {"end_device_us", receipt.end_device_microseconds},
      {"elapsed_device_us", receipt.elapsed_device_microseconds},
      {"maximum_step_lateness_us", receipt.maximum_step_lateness_microseconds},
      {"prepare_timeout_us", receipt.prepare_timeout_microseconds},
      {"power_on_device_us", receipt.power_on_device_microseconds},
      {"prepared_until_device_us", receipt.prepared_until_device_microseconds},
      {"pixel_off", receipt.pixel_off},
      {"i2s_inactive", receipt.i2s_inactive},
      {"rail_powered", receipt.rail_powered},
      {"prepared", receipt.prepared},
      {"candidates", receipt.candidates},
  };
}

std::int64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point started) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               started)
      .count();
}

std::string EnvironmentValue(std::string_view name) {
  // Bazel establishes the test environment before the process starts.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *value = std::getenv(std::string(name).c_str());
  return value == nullptr ? std::string() : std::string(value);
}

std::string RequiredEnvironment(std::string_view name) {
  const std::string value = EnvironmentValue(name);
  if (value.empty()) {
    throw std::runtime_error("set --test_env=" + std::string(name) + "=<adb-serial>");
  }
  return value;
}

std::filesystem::path OutputDirectory() {
  const std::string directory = EnvironmentValue("TEST_UNDECLARED_OUTPUTS_DIR");
  return directory.empty() ? std::filesystem::current_path() : std::filesystem::path(directory);
}

void WriteArtifact(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open dual Android HIL artifact " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    throw std::runtime_error("cannot write dual Android HIL artifact " + path.string());
  }
}

CommandResult RunCommand(const std::filesystem::path &executable,
                         const std::vector<std::string> &arguments,
                         std::chrono::steady_clock::time_point deadline,
                         std::string_view input = {}) {
  int output_pipe[2] = {-1, -1};
  int input_pipe[2] = {-1, -1};
  if (pipe2(output_pipe, O_CLOEXEC) != 0) {
    throw std::runtime_error(std::string("cannot create command output pipe: ") +
                             std::strerror(errno));
  }
  if (pipe2(input_pipe, O_CLOEXEC) != 0) {
    const int saved_errno = errno;
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw std::runtime_error(std::string("cannot create command input pipe: ") +
                             std::strerror(saved_errno));
  }
  const pid_t child = fork();
  if (child < 0) {
    const int saved_errno = errno;
    close(output_pipe[0]);
    close(output_pipe[1]);
    close(input_pipe[0]);
    close(input_pipe[1]);
    throw std::runtime_error(std::string("cannot fork adb: ") + std::strerror(saved_errno));
  }
  if (child == 0) {
    close(output_pipe[0]);
    close(input_pipe[1]);
    if (dup2(output_pipe[1], STDOUT_FILENO) < 0 || dup2(output_pipe[1], STDERR_FILENO) < 0 ||
        dup2(input_pipe[0], STDIN_FILENO) < 0) {
      _exit(126);
    }
    close(output_pipe[1]);
    close(input_pipe[0]);
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
  close(input_pipe[0]);
  std::size_t input_offset = 0;
  while (input_offset < input.size()) {
    const ssize_t count =
        write(input_pipe[1], input.data() + input_offset, input.size() - input_offset);
    if (count > 0) {
      input_offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    close(input_pipe[1]);
    close(output_pipe[0]);
    static_cast<void>(kill(child, SIGKILL));
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
    throw std::runtime_error(std::string("cannot write command input: ") + std::strerror(errno));
  }
  close(input_pipe[1]);
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
      if (!exited) {
        static_cast<void>(kill(child, SIGKILL));
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
      }
      result.timed_out = true;
      break;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd descriptor = {.fd = output_pipe[0], .events = POLLIN, .revents = 0};
    const int polled = poll(&descriptor, 1, static_cast<int>(std::min(remaining, 50ms).count()));
    if (polled < 0 && errno != EINTR) {
      if (!exited) {
        static_cast<void>(kill(child, SIGKILL));
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
      }
      close(output_pipe[0]);
      throw std::runtime_error(std::string("cannot poll adb output: ") + std::strerror(errno));
    }
  }
  close(output_pipe[0]);
  if (!result.timed_out) {
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }
  return result;
}

std::string RunRequiredCommand(const std::filesystem::path &executable,
                               const std::vector<std::string> &arguments,
                               std::chrono::steady_clock::time_point deadline) {
  const CommandResult result = RunCommand(executable, arguments, deadline);
  if (result.timed_out) {
    throw std::runtime_error(executable.filename().string() + " exceeded its stage deadline");
  }
  if (result.exit_code != 0) {
    throw std::runtime_error(executable.filename().string() + " failed with exit " +
                             std::to_string(result.exit_code) + ": " + result.output);
  }
  return result.output;
}

std::string RunRequiredCommandWithInput(const std::filesystem::path &executable,
                                        const std::vector<std::string> &arguments,
                                        std::string_view input,
                                        std::chrono::steady_clock::time_point deadline) {
  const CommandResult result = RunCommand(executable, arguments, deadline, input);
  if (result.timed_out) {
    throw std::runtime_error(executable.filename().string() +
                             " input command exceeded its stage deadline");
  }
  if (result.exit_code != 0) {
    throw std::runtime_error(executable.filename().string() + " input command failed with exit " +
                             std::to_string(result.exit_code));
  }
  return result.output;
}

struct HttpResponse {
  int status = 0;
  std::map<std::string, std::string, std::less<>> headers;
  std::string body;
};

struct HttpRequestSpec {
  std::uint16_t port = 0;
  std::string_view method;
  std::string_view path;
  std::string_view bearer_token;
  std::string_view body;
  std::chrono::steady_clock::time_point deadline;
};

void WaitForSocket(int descriptor, short events, std::chrono::steady_clock::time_point deadline) {
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw std::runtime_error("Android node HTTP request exceeded its stage deadline");
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd polled = {.fd = descriptor, .events = events, .revents = 0};
    const int result = poll(&polled, 1, static_cast<int>(std::min(remaining, 100ms).count()));
    if (result > 0) {
      if ((polled.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
          (polled.revents & events) == 0) {
        throw std::runtime_error("Android node HTTP socket failed while waiting");
      }
      if ((polled.revents & events) != 0) {
        return;
      }
    } else if (result < 0 && errno != EINTR) {
      throw std::runtime_error(std::string("cannot poll Android node HTTP socket: ") +
                               std::strerror(errno));
    }
  }
}

int ConnectNodeHttp(std::uint16_t port, std::chrono::steady_clock::time_point deadline) {
  const int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (descriptor < 0) {
    throw std::runtime_error(std::string("cannot create Android node HTTP socket: ") +
                             std::strerror(errno));
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
    if (errno != EINPROGRESS) {
      const int saved_errno = errno;
      close(descriptor);
      throw std::runtime_error(std::string("cannot connect to Android node HTTP forward: ") +
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
      throw std::runtime_error("cannot establish Android node HTTP forward connection");
    }
  }
  return descriptor;
}

void SendHttpRequest(int descriptor, std::string_view request,
                     std::chrono::steady_clock::time_point deadline) {
  std::size_t sent = 0U;
  while (sent < request.size()) {
    const ssize_t count =
        send(descriptor, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      throw std::runtime_error(std::string("cannot write Android node HTTP request: ") +
                               std::strerror(errno));
    }
    WaitForSocket(descriptor, POLLOUT, deadline);
  }
}

std::string ReceiveHttpResponse(int descriptor, std::chrono::steady_clock::time_point deadline) {
  constexpr std::size_t kMaximumResponseBytes = 128U * 1024U;
  std::string response;
  while (true) {
    std::array<char, 4096> buffer = {};
    const ssize_t count = recv(descriptor, buffer.data(), buffer.size(), 0);
    if (count > 0) {
      response.append(buffer.data(), static_cast<std::size_t>(count));
      if (response.size() > kMaximumResponseBytes) {
        throw std::runtime_error("Android node HTTP response exceeds 128 KiB");
      }
      continue;
    }
    if (count == 0) {
      return response;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      throw std::runtime_error(std::string("cannot read Android node HTTP response: ") +
                               std::strerror(errno));
    }
    WaitForSocket(descriptor, POLLIN, deadline);
  }
}

std::string Lowercase(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

std::string_view TrimHttpValue(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1U);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1U);
  }
  return value;
}

HttpResponse ParseHttpResponse(std::string_view wire) {
  const std::size_t header_end = wire.find("\r\n\r\n");
  const std::size_t status_end = wire.find("\r\n");
  if (header_end == std::string_view::npos || status_end == std::string_view::npos ||
      status_end > header_end || !wire.starts_with("HTTP/1.")) {
    throw std::runtime_error("Android node returned a malformed HTTP response");
  }
  const std::size_t first_space = wire.find(' ');
  if (first_space == std::string_view::npos || first_space + 4U > status_end) {
    throw std::runtime_error("Android node HTTP status line is malformed");
  }
  HttpResponse response;
  const std::string_view status_text = wire.substr(first_space + 1U, 3U);
  const auto [status_pointer, status_error] =
      std::from_chars(status_text.data(), status_text.data() + status_text.size(), response.status);
  if (status_error != std::errc() || status_pointer != status_text.data() + status_text.size()) {
    throw std::runtime_error("Android node HTTP status is not numeric");
  }
  std::size_t line_start = status_end + 2U;
  while (line_start < header_end) {
    const std::size_t line_end = wire.find("\r\n", line_start);
    const std::size_t separator = wire.find(':', line_start);
    if (line_end == std::string_view::npos || line_end > header_end ||
        separator == std::string_view::npos || separator >= line_end) {
      throw std::runtime_error("Android node HTTP header is malformed");
    }
    response.headers.emplace(Lowercase(wire.substr(line_start, separator - line_start)),
                             TrimHttpValue(wire.substr(separator + 1U, line_end - separator - 1U)));
    line_start = line_end + 2U;
  }
  response.body = wire.substr(header_end + 4U);
  const auto length = response.headers.find("content-length");
  if (length == response.headers.end()) {
    throw std::runtime_error("Android node HTTP response lacks Content-Length");
  }
  std::size_t expected_bytes = 0U;
  const auto [length_pointer, length_error] = std::from_chars(
      length->second.data(), length->second.data() + length->second.size(), expected_bytes);
  if (length_error != std::errc() ||
      length_pointer != length->second.data() + length->second.size() ||
      expected_bytes != response.body.size()) {
    throw std::runtime_error("Android node HTTP Content-Length is invalid");
  }
  return response;
}

HttpResponse RequestNodeHttp(const HttpRequestSpec &specification) {
  if (specification.port == 0U || specification.method.empty() ||
      !specification.path.starts_with('/')) {
    throw std::invalid_argument("Android node HTTP request is incomplete");
  }
  std::string request = std::string(specification.method) + " " + std::string(specification.path) +
                        " HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: application/json\r\n";
  if (!specification.bearer_token.empty()) {
    request += "Authorization: Bearer " + std::string(specification.bearer_token) + "\r\n";
  }
  if (!specification.body.empty()) {
    request += "Content-Type: application/json\r\nContent-Length: " +
               std::to_string(specification.body.size()) + "\r\n";
  }
  request += "Connection: close\r\n\r\n";
  request.append(specification.body);
  const int descriptor = ConnectNodeHttp(specification.port, specification.deadline);
  try {
    SendHttpRequest(descriptor, request, specification.deadline);
    std::string response = ReceiveHttpResponse(descriptor, specification.deadline);
    close(descriptor);
    return ParseHttpResponse(response);
  } catch (...) {
    close(descriptor);
    throw;
  }
}

HttpResponse RequireNodeHttp(const HttpRequestSpec &specification, int expected_status) {
  HttpResponse response = RequestNodeHttp(specification);
  if (response.status != expected_status) {
    throw std::runtime_error("Android node HTTP " + std::string(specification.method) + " " +
                             std::string(specification.path) + " returned " +
                             std::to_string(response.status) + ": " + response.body);
  }
  return response;
}

void RequireExecutable(const std::filesystem::path &path, std::string_view description) {
  if (!std::filesystem::is_regular_file(path) || access(path.c_str(), X_OK) != 0) {
    throw std::runtime_error(std::string(description) + " is not executable: " + path.string());
  }
}

ExternalTools DiscoverExternalTools() {
  const std::string configured_ffmpeg = EnvironmentValue("SWING_CAPTURE_FFMPEG");
  const std::string configured_ffprobe = EnvironmentValue("SWING_CAPTURE_FFPROBE");
  ExternalTools tools = {
      .ffmpeg = configured_ffmpeg.empty() ? "/usr/bin/ffmpeg" : configured_ffmpeg,
      .ffprobe = configured_ffprobe.empty() ? "/usr/bin/ffprobe" : configured_ffprobe,
      .ffmpeg_version = {},
      .ffprobe_version = {},
  };
  RequireExecutable(tools.ffmpeg, "local H.264 decoder");
  RequireExecutable(tools.ffprobe, "local H.264 timeline inspector");
  tools.ffmpeg_version =
      RunRequiredCommand(tools.ffmpeg, {"-version"}, std::chrono::steady_clock::now() + 2s);
  tools.ffprobe_version =
      RunRequiredCommand(tools.ffprobe, {"-version"}, std::chrono::steady_clock::now() + 2s);
  return tools;
}

struct DecodeRequest {
  std::filesystem::path ffmpeg;
  std::filesystem::path media;
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
  std::chrono::steady_clock::time_point deadline;
};

void DecodeRgbVideo(const DecodeRequest &request, RgbSwingSequenceAnalyzer *analyzer) {
  int output_pipe[2] = {-1, -1};
  if (pipe2(output_pipe, O_CLOEXEC) != 0) {
    throw std::runtime_error(std::string("cannot create decoder pipe: ") + std::strerror(errno));
  }
  const pid_t child = fork();
  if (child < 0) {
    const int saved_errno = errno;
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw std::runtime_error(std::string("cannot fork ffmpeg: ") + std::strerror(saved_errno));
  }
  if (child == 0) {
    close(output_pipe[0]);
    if (dup2(output_pipe[1], STDOUT_FILENO) < 0) {
      _exit(126);
    }
    close(output_pipe[1]);
    const std::string filter = "scale=" + std::to_string(request.output_width) + ":" +
                               std::to_string(request.output_height) + ":flags=area";
    const std::vector<std::string> arguments = {
        request.ffmpeg.string(),
        "-v",
        "error",
        "-nostdin",
        "-xerror",
        "-err_detect",
        "explode",
        "-i",
        request.media.string(),
        "-an",
        "-sn",
        "-dn",
        "-vf",
        filter,
        "-fps_mode",
        "passthrough",
        "-enc_time_base",
        "demux",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "pipe:1",
    };
    std::vector<char *> command;
    command.reserve(arguments.size() + 1U);
    for (const std::string &argument : arguments) {
      command.push_back(const_cast<char *>(argument.c_str()));
    }
    command.push_back(nullptr);
    execv(request.ffmpeg.c_str(), command.data());
    _exit(127);
  }

  close(output_pipe[1]);
  const int flags = fcntl(output_pipe[0], F_GETFL, 0);
  if (flags >= 0) {
    static_cast<void>(fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK));
  }
  if (request.output_width == 0U || request.output_height == 0U ||
      static_cast<std::size_t>(request.output_width) >
          std::numeric_limits<std::size_t>::max() /
              static_cast<std::size_t>(request.output_height) / 3U) {
    static_cast<void>(kill(child, SIGKILL));
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
    close(output_pipe[0]);
    throw std::invalid_argument("ffmpeg analysis geometry is invalid");
  }
  const std::size_t frame_bytes =
      static_cast<std::size_t>(request.output_width) * request.output_height * 3U;
  std::vector<std::uint8_t> frame(frame_bytes);
  std::size_t filled = 0U;
  bool end_of_output = false;
  while (!end_of_output) {
    const ssize_t count = read(output_pipe[0], frame.data() + filled, frame.size() - filled);
    if (count > 0) {
      filled += static_cast<std::size_t>(count);
      if (filled == frame.size()) {
        analyzer->Append(frame);
        filled = 0U;
      }
      continue;
    }
    if (count == 0) {
      end_of_output = true;
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
      close(output_pipe[0]);
      throw std::runtime_error(std::string("cannot read ffmpeg output: ") + std::strerror(errno));
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= request.deadline) {
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
      close(output_pipe[0]);
      throw std::runtime_error("ffmpeg RGB decode exceeded its 15-second stage deadline");
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(request.deadline - now);
    pollfd descriptor = {.fd = output_pipe[0], .events = POLLIN, .revents = 0};
    const int polled = poll(&descriptor, 1, static_cast<int>(std::min(remaining, 50ms).count()));
    if (polled < 0 && errno != EINTR) {
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
      close(output_pipe[0]);
      throw std::runtime_error(std::string("cannot poll ffmpeg output: ") + std::strerror(errno));
    }
  }
  close(output_pipe[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  if (filled != 0U || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error("ffmpeg did not produce a whole successful RGB24 frame stream");
  }
}

std::vector<std::string> DeviceArguments(std::string_view serial,
                                         std::initializer_list<std::string_view> suffix) {
  std::vector<std::string> arguments = {"-s", std::string(serial)};
  for (const std::string_view argument : suffix) {
    arguments.emplace_back(argument);
  }
  return arguments;
}

CommandResult RunAdb(const std::filesystem::path &adb, const std::vector<std::string> &arguments,
                     std::chrono::steady_clock::time_point deadline) {
  return RunCommand(adb, arguments, deadline);
}

std::string RunRequiredAdb(const std::filesystem::path &adb,
                           const std::vector<std::string> &arguments,
                           std::chrono::steady_clock::time_point deadline) {
  const CommandResult result = RunAdb(adb, arguments, deadline);
  if (result.timed_out) {
    throw std::runtime_error("dual Android HIL stage deadline expired in adb");
  }
  if (result.exit_code != 0) {
    throw std::runtime_error("adb failed with exit " + std::to_string(result.exit_code) + ": " +
                             result.output);
  }
  return result.output;
}

std::string ReadDeviceModel(const std::filesystem::path &adb, std::string_view serial,
                            std::chrono::steady_clock::time_point deadline) {
  std::string model = RunRequiredAdb(
      adb, DeviceArguments(serial, {"shell", "getprop", "ro.product.model"}), deadline);
  while (!model.empty() && (model.back() == '\n' || model.back() == '\r')) {
    model.pop_back();
  }
  if (model.empty()) {
    throw std::runtime_error("Android HIL device model is unavailable");
  }
  return model;
}

void StopPackage(const std::filesystem::path &adb, std::string_view serial) {
  const CommandResult result =
      RunAdb(adb, DeviceArguments(serial, {"shell", "am", "force-stop", kPackageName}),
             std::chrono::steady_clock::now() + 2s);
  if (result.timed_out || result.exit_code != 0) {
    throw std::runtime_error("cannot force-stop Android package on " + std::string(serial));
  }
}

class StopBothGuard {
 public:
  StopBothGuard(std::filesystem::path adb, std::string down_the_line_serial,
                std::string face_on_serial)
      : adb_(std::move(adb)),
        down_the_line_serial_(std::move(down_the_line_serial)),
        face_on_serial_(std::move(face_on_serial)) {}

  StopBothGuard(const StopBothGuard &) = delete;
  StopBothGuard &operator=(const StopBothGuard &) = delete;

  void RegisterForward(std::string serial, std::uint16_t host_port) {
    forwards_.emplace_back(std::move(serial), host_port);
  }

  void RegisterReverse(std::string serial, std::uint16_t device_port) {
    reverses_.emplace_back(std::move(serial), device_port);
  }

  void SnapshotNodeConfiguration(std::string serial) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    const CommandResult installed =
        RunAdb(adb_, DeviceArguments(serial, {"shell", "pm", "path", kPackageName}), deadline);
    if (installed.timed_out || installed.exit_code != 0) {
      throw std::runtime_error("cannot determine whether Android HIL package is installed");
    }
    if (!installed.output.contains("package:")) {
      configuration_snapshots_.push_back(
          {.serial = std::move(serial), .existed = false, .contents = {}});
      return;
    }
    const CommandResult exists =
        RunAdb(adb_,
               DeviceArguments(serial, {"shell", "run-as", kPackageName, "test", "-e",
                                        "shared_prefs/node_configuration.xml"}),
               deadline);
    if (exists.timed_out || (exists.exit_code != 0 && exists.exit_code != 1) ||
        (exists.exit_code == 1 && !exists.output.empty())) {
      throw std::runtime_error("cannot inspect existing private node configuration");
    }
    if (exists.exit_code == 1) {
      configuration_snapshots_.push_back(
          {.serial = std::move(serial), .existed = false, .contents = {}});
      return;
    }
    const std::string contents =
        RunRequiredAdb(adb_,
                       DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat",
                                                "shared_prefs/node_configuration.xml"}),
                       deadline);
    configuration_snapshots_.push_back(
        {.serial = std::move(serial), .existed = true, .contents = contents});
  }

  void RestoreNodeConfigurationsChecked() {
    if (configuration_snapshots_.size() != 2U) {
      throw std::runtime_error("paired HIL did not snapshot both node configurations");
    }
    StopPackage(adb_, down_the_line_serial_);
    StopPackage(adb_, face_on_serial_);
    for (const ConfigurationSnapshot &snapshot : configuration_snapshots_) {
      RestoreNodeConfiguration(snapshot);
    }
    configurations_restored_ = true;
    for (ConfigurationSnapshot &snapshot : configuration_snapshots_) {
      snapshot.contents.clear();
      snapshot.contents.shrink_to_fit();
    }
  }

  ~StopBothGuard() {
    for (const auto &[serial, device_port] : reverses_) {
      const CommandResult result = RunAdb(
          adb_,
          DeviceArguments(serial, {"reverse", "--remove", "tcp:" + std::to_string(device_port)}),
          std::chrono::steady_clock::now() + 2s);
      if (result.timed_out || result.exit_code != 0) {
        std::cerr << "Warning: could not remove Android HIL adb reverse on " << serial << '\n';
      }
    }
    for (const auto &[serial, host_port] : forwards_) {
      const CommandResult result = RunAdb(
          adb_,
          DeviceArguments(serial, {"forward", "--remove", "tcp:" + std::to_string(host_port)}),
          std::chrono::steady_clock::now() + 2s);
      if (result.timed_out || result.exit_code != 0) {
        std::cerr << "Warning: could not remove Android HIL adb forward on " << serial << '\n';
      }
    }
    for (const std::string &serial : {down_the_line_serial_, face_on_serial_}) {
      try {
        StopPackage(adb_, serial);
      } catch (const std::exception &failure) {
        std::cerr << "Warning: dual Android HIL cleanup failed: " << failure.what() << '\n';
      }
    }
    if (!configurations_restored_) {
      for (const ConfigurationSnapshot &snapshot : configuration_snapshots_) {
        try {
          RestoreNodeConfiguration(snapshot);
        } catch (const std::exception &failure) {
          std::cerr << "Warning: could not restore Android HIL node configuration on "
                    << snapshot.serial << ": " << failure.what() << '\n';
        }
      }
    }
  }

 private:
  struct ConfigurationSnapshot {
    std::string serial;
    bool existed = false;
    std::string contents;
  };

  void RestoreNodeConfiguration(const ConfigurationSnapshot &snapshot) const {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    constexpr std::string_view staging_path = "shared_prefs/.node_configuration.xml.hil-restore";
    if (!snapshot.existed) {
      RunRequiredAdb(adb_,
                     DeviceArguments(snapshot.serial, {"shell", "run-as", kPackageName, "rm", "-f",
                                                       "shared_prefs/node_configuration.xml",
                                                       std::string(staging_path)}),
                     deadline);
      const CommandResult verification =
          RunAdb(adb_,
                 DeviceArguments(snapshot.serial, {"exec-out", "run-as", kPackageName, "cat",
                                                   "shared_prefs/node_configuration.xml"}),
                 deadline);
      if (verification.timed_out || verification.exit_code == 0) {
        throw std::runtime_error("absent node configuration was not restored exactly");
      }
      return;
    }
    RunRequiredAdb(adb_,
                   DeviceArguments(snapshot.serial,
                                   {"shell", "run-as", kPackageName, "rm", "-f", staging_path}),
                   deadline);
    try {
      // `adb exec-in` does not forward stdin with every packaged platform-tools build. A
      // non-interactive shell explicitly does, and direct dd/chmod/mv arguments avoid exposing
      // the configuration (including bearer credentials) in a shell command or process output.
      RunRequiredCommandWithInput(
          adb_,
          DeviceArguments(snapshot.serial, {"shell", "-T", "run-as", kPackageName, "dd",
                                            "of=" + std::string(staging_path), "status=none"}),
          snapshot.contents, deadline);
      RunRequiredAdb(adb_,
                     DeviceArguments(snapshot.serial, {"shell", "run-as", kPackageName, "chmod",
                                                       "0600", staging_path}),
                     deadline);
      RunRequiredAdb(
          adb_,
          DeviceArguments(snapshot.serial, {"shell", "run-as", kPackageName, "mv", "-f",
                                            staging_path, "shared_prefs/node_configuration.xml"}),
          deadline);
    } catch (...) {
      static_cast<void>(RunAdb(adb_,
                               DeviceArguments(snapshot.serial, {"shell", "run-as", kPackageName,
                                                                 "rm", "-f", staging_path}),
                               deadline));
      throw;
    }
    const std::string restored =
        RunRequiredAdb(adb_,
                       DeviceArguments(snapshot.serial, {"exec-out", "run-as", kPackageName, "cat",
                                                         "shared_prefs/node_configuration.xml"}),
                       deadline);
    if (restored != snapshot.contents) {
      throw std::runtime_error("node configuration restore verification failed");
    }
    const CommandResult staging =
        RunAdb(adb_,
               DeviceArguments(snapshot.serial,
                               {"shell", "run-as", kPackageName, "test", "-e", staging_path}),
               deadline);
    if (staging.timed_out || staging.exit_code != 1 || !staging.output.empty()) {
      throw std::runtime_error("node configuration restore staging was not removed");
    }
  }

  std::filesystem::path adb_;
  std::string down_the_line_serial_;
  std::string face_on_serial_;
  std::vector<std::pair<std::string, std::uint16_t>> forwards_;
  std::vector<std::pair<std::string, std::uint16_t>> reverses_;
  std::vector<ConfigurationSnapshot> configuration_snapshots_;
  bool configurations_restored_ = false;
};

struct StartRequest {
  std::string_view serial;
  std::string_view role;
  std::string_view shared_session_id;
  RoleCaptureProfile profile;
};

std::vector<std::string> StartArguments(const StartRequest &request) {
  return {
      "-s",
      std::string(request.serial),
      "shell",
      "am",
      "start",
      "-W",
      "--activity-single-top",
      "-n",
      "com.agoessling.swingcapture/.MainActivity",
      "--es",
      "role",
      std::string(request.role),
      "--es",
      "capture_profile",
      std::string(request.profile.name),
      "--es",
      "shared_session_id",
      std::string(request.shared_session_id),
      "--ez",
      "run_audio_hil",
      "true",
  };
}

std::vector<std::string> ConfigureArguments(std::string_view serial, std::string_view role,
                                            const RoleCaptureProfile &profile,
                                            bool enable_pose_arm_hil = false) {
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
      std::string(profile.name),
  };
  if (enable_pose_arm_hil) {
    arguments.insert(arguments.end(), {"--ez", "enable_pose_arm_hil", "true"});
  }
  return arguments;
}

std::uint16_t EstablishForward(const std::filesystem::path &adb, std::string_view serial,
                               std::chrono::steady_clock::time_point deadline) {
  std::string output = RunRequiredAdb(
      adb, DeviceArguments(serial, {"forward", "tcp:0", "tcp:" + std::to_string(kNodeHttpPort)}),
      deadline);
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
  unsigned int port = 0U;
  const auto [pointer, error] = std::from_chars(output.data(), output.data() + output.size(), port);
  if (error != std::errc() || pointer != output.data() + output.size() || port == 0U ||
      port > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("adb did not return a valid ephemeral HTTP forward port");
  }
  return static_cast<std::uint16_t>(port);
}

void EstablishReverse(const std::filesystem::path &adb, std::string_view serial,
                      std::uint16_t device_port, std::uint16_t host_port,
                      std::chrono::steady_clock::time_point deadline) {
  RunRequiredAdb(adb,
                 DeviceArguments(serial, {"reverse", "tcp:" + std::to_string(device_port),
                                          "tcp:" + std::to_string(host_port)}),
                 deadline);
}

std::string ReadControlToken(const std::filesystem::path &adb, std::string_view serial,
                             std::chrono::steady_clock::time_point deadline) {
  const std::string preferences =
      RunRequiredAdb(adb,
                     DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat",
                                              "shared_prefs/node_configuration.xml"}),
                     deadline);
  constexpr std::string_view kPrefix = "<string name=\"control_token\">";
  constexpr std::string_view kSuffix = "</string>";
  const std::size_t start = preferences.find(kPrefix);
  const std::size_t end =
      start == std::string::npos ? std::string::npos : preferences.find(kSuffix, start);
  if (start == std::string::npos || end == std::string::npos) {
    throw std::runtime_error("Android node did not persist its bearer control token");
  }
  const std::size_t token_start = start + kPrefix.size();
  const std::string token = preferences.substr(token_start, end - token_start);
  if (token.size() != 32U || !std::ranges::all_of(token, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-';
      })) {
    throw std::runtime_error("Android node bearer control token has an invalid format");
  }
  return token;
}

bool IsArmedWaitingForAudio(std::string_view report) {
  try {
    const Json parsed = Json::parse(report);
    return parsed.value("report_type", "") == "android_continuous_capture" &&
           !parsed.value("complete", true) && parsed.value("state", "") == "armed_waiting_audio";
  } catch (const std::exception &) {
    return false;
  }
}

swing_capture::hil::FeatherSwingReceipt RunFeatherSwing(
    const std::filesystem::path &evidence_directory) {
  const auto config_path = swing_capture::station::StationConfigPathFromEnvironment();
  if (!config_path.has_value()) {
    throw std::runtime_error("SWING_CAPTURE_STATION_CONFIG is required for dual Android HIL");
  }
  const auto station = swing_capture::station::LoadStationConfig(*config_path);
  swing_capture::hil::FeatherHilSerial serial(station.feather_serial_path);
  swing_capture::hil::FeatherHilController feather(serial);
  const auto info = feather.QueryInfo();
  if (info.calibration_candidates.empty()) {
    throw std::runtime_error("Feather advertised no safe synthetic-swing brightness");
  }
  Json transaction = {
      {"calibration_issued", true},
      {"calibration_complete", false},
      {"swing_issued", false},
      {"swing_complete", false},
      {"advertised_calibration_candidates", info.calibration_candidates},
  };
  WriteArtifact(evidence_directory / "feather-transaction.json", transaction.dump(2) + "\n");
  const auto calibration = feather.CalibrateSwingBrightness();
  transaction["calibration_complete"] = true;
  transaction["calibration"] = FeatherCalibrationJson(calibration);
  WriteArtifact(evidence_directory / "feather-transaction.json", transaction.dump(2) + "\n");
  if (!calibration.prepared || !calibration.rail_powered || !calibration.pixel_off ||
      !calibration.i2s_inactive || calibration.candidates != info.calibration_candidates) {
    throw std::runtime_error("Feather did not enter the prepared synthetic-swing state");
  }
  const std::uint32_t brightness = calibration.candidates.back();
  transaction["swing_issued"] = true;
  transaction["selected_brightness"] = brightness;
  WriteArtifact(evidence_directory / "feather-transaction.json", transaction.dump(2) + "\n");
  auto receipt = feather.RunSyntheticSwing(brightness);
  transaction["swing_complete"] = true;
  transaction["swing"] = FeatherReceiptJson(receipt);
  WriteArtifact(evidence_directory / "feather-transaction.json", transaction.dump(2) + "\n");
  WriteArtifact(evidence_directory / "feather.json", FeatherReceiptJson(receipt).dump(2) + "\n");
  if (receipt.impact.command_delta_microseconds > info.swing_maximum_impact_delta_microseconds) {
    throw std::runtime_error("Feather white/tone impact command delta exceeded its contract");
  }
  return receipt;
}

struct DecodedLuminanceImage {
  std::span<const std::byte> pixels;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

swing_capture::optical::AprilTagDetection DetectPrintedTag(const DecodedLuminanceImage &luminance) {
  const swing_capture::image::Raw8ImageView image = {
      .pixels = luminance.pixels,
      .width = luminance.width,
      .height = luminance.height,
      .row_stride_bytes = luminance.width,
  };
  swing_capture::optical::AprilTagDetector detector(
      {.family = swing_capture::optical::AprilTagFamily::kTag36h11});
  auto presence = swing_capture::optical::VerifyAprilTagPresence(
      image, detector, {.expected_id = 0, .maximum_hamming = 1, .minimum_decision_margin = 10.0});
  if (presence.present) {
    return *presence.accepted_detection;
  }
  throw std::runtime_error("retained H.264 lacks required tag36h11 ID 0: " + presence.diagnostic);
}

std::size_t PostDiagnosticFrame(const RgbSwingAnalysis &optical, std::size_t frame_count) {
  return std::min(optical.last_white_frame_index + 5U, frame_count - 1U);
}

const std::vector<std::byte> &FindDiagnosticLuminance(const RgbSwingAnalysis &optical,
                                                      std::size_t frame_index) {
  const auto found =
      std::ranges::find(optical.diagnostic_luminance_frames, frame_index,
                        &swing_capture::android::dual_hil::RetainedLuminanceFrame::frame_index);
  if (found == optical.diagnostic_luminance_frames.end()) {
    throw std::runtime_error("decoded RGB analyzer did not retain a diagnostic luminance frame");
  }
  return found->pixels;
}

void ExtractDiagnosticPngs(const ExternalTools &tools, const std::filesystem::path &media_path,
                           const std::filesystem::path &node_output,
                           const RgbSwingAnalysis &optical, std::size_t frame_count) {
  const std::size_t post_frame = PostDiagnosticFrame(optical, frame_count);
  const std::string selection = "select=eq(n\\," +
                                std::to_string(optical.representative_frame_index) + ")+eq(n\\," +
                                std::to_string(optical.first_white_frame_index) + ")+eq(n\\," +
                                std::to_string(post_frame) + ")";
  const std::filesystem::path output_pattern = node_output / "diagnostic-%02d.png";
  static_cast<void>(
      RunRequiredCommand(tools.ffmpeg,
                         {"-v", "error", "-nostdin", "-xerror", "-err_detect", "explode", "-y",
                          "-i", media_path.string(), "-vf", selection, "-fps_mode", "passthrough",
                          "-frames:v", "3", output_pattern.string()},
                         std::chrono::steady_clock::now() + kStageDeadline));
  for (int index = 1; index <= 3; ++index) {
    const std::filesystem::path diagnostic =
        node_output / ("diagnostic-0" + std::to_string(index) + ".png");
    if (!std::filesystem::is_regular_file(diagnostic) ||
        std::filesystem::file_size(diagnostic) == 0U) {
      throw std::runtime_error("ffmpeg did not publish all three diagnostic PNG frames");
    }
  }
}

void AnalyzeNodeMedia(CapturedNode *captured, const std::filesystem::path &node_output,
                      const ExternalTools &tools) {
  const std::filesystem::path media_path = node_output / (captured->evidence.role + ".mp4");
  captured->analysis_width =
      std::min(kMaximumAnalysisWidth, static_cast<std::uint32_t>(captured->evidence.width));
  captured->analysis_height = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(captured->evidence.height) * captured->analysis_width /
      static_cast<std::uint32_t>(captured->evidence.width));
  if (captured->analysis_width == 0U || captured->analysis_height == 0U) {
    throw std::runtime_error("manifest geometry cannot produce a decoded analysis image");
  }
  auto stage_started = std::chrono::steady_clock::now();
  const std::string ffprobe = RunRequiredCommand(
      tools.ffprobe,
      {"-v", "error", "-select_streams", "v:0", "-show_entries",
       "stream=width,height,codec_name,nb_frames:frame=best_effort_timestamp_time", "-of", "json",
       media_path.string()},
      std::chrono::steady_clock::now() + kStageDeadline);
  WriteArtifact(node_output / "ffprobe.json", ffprobe);
  captured->maximum_media_time_residual_us = ValidateFfprobeTimeline(captured->evidence, ffprobe);
  captured->ffprobe_stage_milliseconds = ElapsedMilliseconds(stage_started);

  std::vector<RgbFrameTiming> timings;
  timings.reserve(captured->evidence.frame_count);
  for (std::size_t index = 0; index < captured->evidence.frame_count; ++index) {
    timings.push_back({
        .media_time_us = captured->evidence.media_times_us[index],
        .time_from_impact_us = captured->evidence.times_from_impact_us[index],
    });
  }
  RgbSwingSequenceAnalyzer analyzer(RgbSwingSequenceConfiguration{
      .width = captured->analysis_width,
      .height = captured->analysis_height,
      .timings = timings,
  });
  stage_started = std::chrono::steady_clock::now();
  DecodeRgbVideo({.ffmpeg = tools.ffmpeg,
                  .media = media_path,
                  .output_width = captured->analysis_width,
                  .output_height = captured->analysis_height,
                  .deadline = std::chrono::steady_clock::now() + kStageDeadline},
                 &analyzer);
  captured->optical = analyzer.Finish();
  captured->decode_stage_milliseconds = ElapsedMilliseconds(stage_started);
  stage_started = std::chrono::steady_clock::now();
  ExtractDiagnosticPngs(tools, media_path, node_output, captured->optical,
                        captured->evidence.frame_count);
  captured->diagnostic_frame_indices = {
      captured->optical.representative_frame_index,
      captured->optical.first_white_frame_index,
      PostDiagnosticFrame(captured->optical, captured->evidence.frame_count),
  };
  std::array<AprilTagFrameEvidence, 3> tag_evidence;
  for (std::size_t index = 0; index < captured->diagnostic_frame_indices.size(); ++index) {
    const std::size_t frame_index = captured->diagnostic_frame_indices[index];
    captured->april_tags[index] = DetectPrintedTag({
        .pixels = FindDiagnosticLuminance(captured->optical, frame_index),
        .width = captured->analysis_width,
        .height = captured->analysis_height,
    });
    const auto &tag = captured->april_tags[index];
    tag_evidence[index] = {
        .frame_index = frame_index,
        .family = tag.family,
        .id = tag.id,
        .hamming = tag.hamming,
        .decision_margin = tag.decision_margin,
    };
  }
  ValidateRequiredAprilTagPersistence(tag_evidence);
  captured->diagnostic_stage_milliseconds = ElapsedMilliseconds(stage_started);
  if (!captured->optical.detected) {
    throw std::runtime_error("retained H.264 Feather LED/audio correlation failed: " +
                             captured->optical.diagnostic);
  }
}

bool SafeSessionId(std::string_view session_id) {
  if (session_id.empty() || session_id.contains("..")) {
    return false;
  }
  return std::ranges::all_of(session_id, [](const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '_' ||
           character == '.';
  });
}

std::string GenerateSharedSessionId() {
  const auto epoch_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
  std::random_device entropy;
  const std::uint32_t nonce = entropy();
  const std::string session_id = "android-hil-" + std::to_string(epoch_microseconds) + "-" +
                                 std::to_string(getpid()) + "-" + std::to_string(nonce);
  if (!SafeSessionId(session_id)) {
    throw std::runtime_error("generated Android HIL shared session ID is unsafe");
  }
  return session_id;
}

std::int64_t SteadyNanoseconds(std::chrono::steady_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

std::int64_t SignedFeatherWhiteToneDeltaUs(const swing_capture::hil::FeatherSwingReceipt &receipt) {
  return static_cast<std::int64_t>(receipt.impact.white_command_device_microseconds) -
         static_cast<std::int64_t>(receipt.impact.tone_command_device_microseconds);
}

Json FeatherReceiptJson(const swing_capture::hil::FeatherSwingReceipt &receipt) {
  return {
      {"request_id", receipt.request_id},
      {"brightness", receipt.brightness},
      {"impact_scheduled_device_us", receipt.impact_scheduled_device_microseconds},
      {"white_command_device_us", receipt.impact.white_command_device_microseconds},
      {"tone_command_device_us", receipt.impact.tone_command_device_microseconds},
      {"command_delta_us", receipt.impact.command_delta_microseconds},
      {"signed_white_minus_tone_command_delta_us", SignedFeatherWhiteToneDeltaUs(receipt)},
      {"white_end_device_us", receipt.impact.white_end_device_microseconds},
      {"tone_end_device_us", receipt.impact.tone_end_device_microseconds},
      {"tone_sample_rate_hz", receipt.tone_sample_rate_hz},
      {"tone_sample_count", receipt.tone_sample_count},
      {"host_command_sent_ns", SteadyNanoseconds(receipt.host_command_sent)},
      {"host_acknowledgement_received_ns",
       SteadyNanoseconds(receipt.host_acknowledgement_received)},
      {"host_done_received_ns", SteadyNanoseconds(receipt.host_done_received)},
      {"outputs_inactive_at_completion", receipt.outputs_inactive_at_completion},
      {"pixel_off_at_completion", receipt.pixel_off_at_completion},
      {"i2s_inactive_at_completion", receipt.i2s_inactive_at_completion},
  };
}

Json StageJson(std::int64_t elapsed_milliseconds) {
  constexpr auto kDeadlineMilliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count();
  return {
      {"passed", elapsed_milliseconds <= kDeadlineMilliseconds},
      {"elapsed_ms", elapsed_milliseconds},
      {"deadline_ms", kDeadlineMilliseconds},
  };
}

TimingCorrelationEvidence CorrelationBudget(const CapturedNode &captured) {
  const auto &measurements = captured.audio.evaluation.measurements;
  if (!measurements.detected_onset_sample_offset.has_value()) {
    TimingCorrelationEvidence missing_onset = EvaluateTimingCorrelation({
        .optical_onset_lower_bound_us = captured.optical.optical_onset_lower_bound_us,
        .optical_onset_upper_bound_us = captured.optical.optical_onset_upper_bound_us,
        .audio_trigger_uncertainty_ns = captured.evidence.trigger_timestamp_uncertainty_ns,
        .media_pts_residual_us = captured.maximum_media_time_residual_us,
    });
    missing_onset.passed = false;
    return missing_onset;
  }
  const auto onset_sample = static_cast<std::int64_t>(*measurements.detected_onset_sample_offset);
  const auto strike_sample = static_cast<std::int64_t>(captured.audio.strike_sample_index);
  const std::int64_t tone_onset_relative_to_strike_us = static_cast<std::int64_t>(
      std::llround(static_cast<double>(onset_sample - strike_sample) * 1000000.0 /
                   static_cast<double>(captured.evidence.audio_sample_rate_hz)));
  return EvaluateTimingCorrelation({
      .optical_onset_lower_bound_us =
          captured.optical.optical_onset_lower_bound_us - tone_onset_relative_to_strike_us,
      .optical_onset_upper_bound_us =
          captured.optical.optical_onset_upper_bound_us - tone_onset_relative_to_strike_us,
      .audio_trigger_uncertainty_ns = captured.evidence.trigger_timestamp_uncertainty_ns,
      .media_pts_residual_us = captured.maximum_media_time_residual_us,
  });
}

Json ToneOnsetRelativeToStrikeJson(const CapturedNode &captured) {
  const auto detected = captured.audio.evaluation.measurements.detected_onset_sample_offset;
  if (!detected.has_value()) {
    return nullptr;
  }
  const auto onset_sample = static_cast<std::int64_t>(*detected);
  const auto strike_sample = static_cast<std::int64_t>(captured.audio.strike_sample_index);
  return static_cast<std::int64_t>(
      std::llround(static_cast<double>(onset_sample - strike_sample) * 1000000.0 /
                   static_cast<double>(captured.evidence.audio_sample_rate_hz)));
}

void ValidateCompletedNode(const CapturedNode &captured) {
  constexpr auto kDeadlineMilliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count();
  const bool stages_passed = captured.capture_stage_milliseconds <= kDeadlineMilliseconds &&
                             captured.artifact_pull_stage_milliseconds <= kDeadlineMilliseconds &&
                             captured.ffprobe_stage_milliseconds <= kDeadlineMilliseconds &&
                             captured.decode_stage_milliseconds <= kDeadlineMilliseconds &&
                             captured.diagnostic_stage_milliseconds <= kDeadlineMilliseconds &&
                             captured.audio_stage_milliseconds <= kDeadlineMilliseconds;
  if (!stages_passed) {
    throw std::runtime_error("dual Android HIL evidence stage exceeded 15 seconds");
  }
  if (captured.optical.decoded_frame_count != captured.evidence.frame_count) {
    throw std::runtime_error("decoded video frame count differs from retained manifest");
  }
  if (!captured.audio.evaluation.passed) {
    throw std::runtime_error("retained PCM does not contain the commanded 2 kHz Feather tone");
  }
  const TimingCorrelationEvidence timing = CorrelationBudget(captured);
  if (!timing.passed) {
    std::ostringstream message;
    message << "LED/audio timing bound exceeds " << timing.acceptance_limit_us
            << " us: point_residual_us=" << captured.optical.optical_to_audio_offset_us
            << " tone_onset_relative_to_strike_us=" << ToneOnsetRelativeToStrikeJson(captured)
            << " optical_onset_interval_us=[" << timing.optical_onset_lower_bound_us << ','
            << timing.optical_onset_upper_bound_us << ']'
            << " audio_trigger_uncertainty_us=" << timing.audio_trigger_uncertainty_us
            << " signed_feather_white_minus_tone_command_delta_us="
            << SignedFeatherWhiteToneDeltaUs(captured.feather)
            << " media_pts_residual_us=" << timing.media_pts_residual_us
            << " accounted_uncertainty_us=" << timing.accounted_uncertainty_us
            << " expanded_residual_interval_us=[" << timing.minimum_residual_us << ','
            << timing.maximum_residual_us << ']' << " total_bound_us=" << timing.total_bound_us;
    throw std::runtime_error(message.str());
  }
}

struct DeviceCaptureRequest {
  std::string_view serial;
  std::string_view role;
  std::string_view shared_session_id;
};

CapturedNode CaptureNode(const std::filesystem::path &adb, const std::filesystem::path &apk,
                         const DeviceCaptureRequest &request) {
  const auto stage_started = std::chrono::steady_clock::now();
  const auto deadline = std::chrono::steady_clock::now() + kStageDeadline;
  const std::string_view serial = request.serial;
  const std::string_view role = request.role;
  const RoleCaptureProfile profile = ProfileForRole(role);
  std::cout << "Starting sequential dual Android HIL stage " << role << " on " << serial << '\n';
  RunRequiredAdb(adb,
                 DeviceArguments(serial, {"install", "--no-streaming", "-r", "-t", apk.string()}),
                 deadline);
  StopPackage(adb, serial);
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
                 StartArguments({.serial = serial,
                                 .role = role,
                                 .shared_session_id = request.shared_session_id,
                                 .profile = profile}),
                 deadline);

  std::string report_text;
  std::optional<std::chrono::steady_clock::time_point> armed_at;
  std::optional<swing_capture::hil::FeatherSwingReceipt> feather_receipt;
  while (std::chrono::steady_clock::now() < deadline) {
    const CommandResult result = RunAdb(
        adb, DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat", kReportPath}),
        deadline);
    if (result.exit_code == 0) {
      report_text = result.output;
      WriteArtifact(OutputDirectory() / std::string(role) / "report.json", report_text);
      try {
        const Json report = Json::parse(report_text);
        if (report.value("report_type", "") == "android_continuous_capture" &&
            report.value("complete", false)) {
          WriteArtifact(OutputDirectory() / std::string(role) / "report.json", report_text);
          if (!report.value("passed", false)) {
            throw std::runtime_error("phone capture failed before retained publication: " +
                                     report.value("error", "unknown Android capture failure"));
          }
          if (!feather_receipt.has_value()) {
            throw std::runtime_error("phone triggered before the commanded Feather swing");
          }
          break;
        }
      } catch (const nlohmann::json::exception &) {
        // The atomic report may not have appeared yet.
      }
      if (!feather_receipt.has_value() && IsArmedWaitingForAudio(report_text)) {
        if (!armed_at.has_value()) {
          armed_at = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - *armed_at >= kQuietQualification) {
          feather_receipt = RunFeatherSwing(OutputDirectory() / std::string(role));
        }
      }
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (report_text.empty()) {
    throw std::runtime_error("dual Android HIL stage produced no report for " + std::string(role));
  }
  const Json report = Json::parse(report_text);
  if (!report.value("complete", false)) {
    throw std::runtime_error("dual Android HIL stage deadline expired before completion for " +
                             std::string(role));
  }
  const auto retained = report.find("retained_session");
  if (retained == report.end() || !retained->is_object()) {
    throw std::runtime_error("dual Android HIL report has no retained session");
  }
  const std::string local_session_id = retained->value("session_id", "");
  const std::string manifest_path = retained->value("manifest", "");
  const std::string media_path = retained->value("media", "");
  const std::string prefix = "sessions/" + local_session_id + "/";
  const auto report_audio = retained->find("audio_evidence");
  const std::string audio_path = report_audio != retained->end() && report_audio->is_object()
                                     ? report_audio->value("path", "")
                                     : std::string();
  if (!SafeSessionId(local_session_id) || manifest_path != prefix + "manifest.json" ||
      media_path != prefix + std::string(role) + ".mp4" ||
      audio_path != prefix + "audio_evidence.wav") {
    throw std::runtime_error("dual Android HIL report contains unsafe retained paths");
  }
  CapturedNode captured;
  if (!feather_receipt.has_value()) {
    throw std::runtime_error("dual Android HIL did not command a Feather synthetic swing");
  }
  captured.feather = *feather_receipt;
  captured.report = std::move(report_text);
  const auto artifact_pull_started = std::chrono::steady_clock::now();
  captured.manifest = RunRequiredAdb(adb,
                                     DeviceArguments(serial, {"exec-out", "run-as", kPackageName,
                                                              "cat", "files/" + manifest_path}),
                                     deadline);
  captured.media = RunRequiredAdb(
      adb,
      DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat", "files/" + media_path}),
      deadline);
  captured.audio_wav = RunRequiredAdb(
      adb,
      DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat", "files/" + audio_path}),
      deadline);
  StopPackage(adb, serial);

  const std::filesystem::path node_output = OutputDirectory() / std::string(role);
  WriteArtifact(node_output / "report.json", captured.report);
  WriteArtifact(node_output / "manifest.json", captured.manifest);
  WriteArtifact(node_output / (std::string(role) + ".mp4"), captured.media);
  WriteArtifact(node_output / "audio_evidence.wav", captured.audio_wav);
  WriteArtifact(node_output / "feather.json", FeatherReceiptJson(captured.feather).dump(2) + "\n");
  captured.evidence = ValidateNodeEvidence(NodeEvidenceInspection{
      .report = captured.report,
      .manifest = captured.manifest,
      .media = captured.media,
      .expected_role = role,
      .expected_shared_session_id = request.shared_session_id,
      .expected_profile = profile.capture,
  });
  captured.artifact_pull_stage_milliseconds = ElapsedMilliseconds(artifact_pull_started);
  captured.capture_stage_milliseconds = ElapsedMilliseconds(stage_started);
  const auto audio_stage_started = std::chrono::steady_clock::now();
  captured.audio = AnalyzeRetainedAudioEvidence(RetainedAudioEvidenceInspection{
      .report = captured.report,
      .manifest = captured.manifest,
      .wav = captured.audio_wav,
      .feather_accepted_device_microseconds = captured.feather.accepted_device_microseconds,
      .feather_impact_scheduled_device_microseconds =
          captured.feather.impact_scheduled_device_microseconds,
  });
  captured.audio_stage_milliseconds = ElapsedMilliseconds(audio_stage_started);
  return captured;
}

struct ConcurrentNode {
  std::string serial;
  std::string role;
  RoleCaptureProfile profile;
  std::uint16_t host_port = 0;
  std::string control_token;
  NodeApiIdentity identity;
  std::vector<coordination::FourTimestampClockExchange> clock_exchanges;
  coordination::ClockEstimateResult clock_estimate;
  std::int64_t setup_stage_milliseconds = 0;
  std::int64_t arm_dispatch_milliseconds = 0;
};

std::int64_t JsonDecimalString(const Json &object, std::string_view name) {
  const auto field = object.find(name);
  if (field == object.end() || !field->is_string()) {
    throw std::runtime_error("Android node HTTP timestamp must be a decimal string");
  }
  const std::string text = field->get<std::string>();
  std::int64_t value = 0;
  const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || pointer != text.data() + text.size() || value < 0) {
    throw std::runtime_error("Android node HTTP timestamp is outside its supported range");
  }
  return value;
}

void GrantCapturePermissions(const std::filesystem::path &adb, std::string_view serial,
                             std::chrono::steady_clock::time_point deadline) {
  for (const std::string_view permission : {
           "android.permission.CAMERA",
           "android.permission.RECORD_AUDIO",
           "android.permission.POST_NOTIFICATIONS",
       }) {
    RunRequiredAdb(adb, DeviceArguments(serial, {"shell", "pm", "grant", kPackageName, permission}),
                   deadline);
  }
}

ConcurrentNode ConfigureConcurrentNode(const std::filesystem::path &adb,
                                       const std::filesystem::path &apk, std::string serial,
                                       std::string role, StopBothGuard *cleanup,
                                       bool enable_pose_arm_hil = false) {
  const auto stage_started = std::chrono::steady_clock::now();
  const auto deadline = stage_started + kStageDeadline;
  const RoleCaptureProfile profile = ProfileForRole(role);
  ConcurrentNode node{
      .serial = std::move(serial),
      .role = std::move(role),
      .profile = profile,
      .host_port = 0,
      .control_token = {},
      .identity = {},
      .clock_exchanges = {},
      .clock_estimate = {},
      .setup_stage_milliseconds = 0,
      .arm_dispatch_milliseconds = 0,
  };
  StopPackage(adb, node.serial);
  RunRequiredAdb(adb,
                 DeviceArguments(node.serial, {"shell", "input", "keyevent", "KEYCODE_WAKEUP"}),
                 deadline);
  RunRequiredAdb(
      adb, DeviceArguments(node.serial, {"install", "--no-streaming", "-r", "-t", apk.string()}),
      deadline);
  RunRequiredAdb(
      adb, DeviceArguments(node.serial, {"shell", "run-as", kPackageName, "rm", "-f", kReportPath}),
      deadline);
  GrantCapturePermissions(adb, node.serial, deadline);
  RunRequiredAdb(adb, ConfigureArguments(node.serial, node.role, node.profile, enable_pose_arm_hil),
                 deadline);
  node.host_port = EstablishForward(adb, node.serial, deadline);
  cleanup->RegisterForward(node.serial, node.host_port);

  std::string latest_error = "node HTTP API did not start";
  std::string current_check = "GET /api/v1/node";
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      current_check = "GET /api/v1/node";
      const HttpResponse descriptor = RequireNodeHttp({.port = node.host_port,
                                                       .method = "GET",
                                                       .path = "/api/v1/node",
                                                       .bearer_token = {},
                                                       .body = {},
                                                       .deadline = deadline},
                                                      200);
      node.identity = ValidateNodeDescriptor(descriptor.body,
                                             node.role == "down_the_line"
                                                 ? coordination::CaptureRole::kDownTheLine
                                                 : coordination::CaptureRole::kFaceOn,
                                             node.profile.name);
      current_check = "GET /";
      const HttpResponse hosted_root = RequireNodeHttp({.port = node.host_port,
                                                        .method = "GET",
                                                        .path = "/",
                                                        .bearer_token = {},
                                                        .body = {},
                                                        .deadline = deadline},
                                                       200);
      current_check = "GET /app.css";
      const HttpResponse hosted_css = RequireNodeHttp({.port = node.host_port,
                                                       .method = "GET",
                                                       .path = "/app.css",
                                                       .bearer_token = {},
                                                       .body = {},
                                                       .deadline = deadline},
                                                      200);
      if (!hosted_root.body.starts_with("<!doctype html>") ||
          !hosted_root.body.contains("/app.js") || !hosted_root.body.contains("/app.css") ||
          hosted_css.body.empty()) {
        throw std::runtime_error("phone-hosted review root/static asset smoke failed");
      }
      node.control_token = ReadControlToken(adb, node.serial, deadline);
      WriteArtifact(
          OutputDirectory() / node.role /
              (enable_pose_arm_hil ? "node-descriptor-initial.json" : "node-descriptor.json"),
          descriptor.body);
      WriteArtifact(OutputDirectory() / node.role / "hosted-root.html", hosted_root.body);
      node.setup_stage_milliseconds = ElapsedMilliseconds(stage_started);
      return node;
    } catch (const std::exception &failure) {
      latest_error = current_check + ": " + failure.what();
      std::this_thread::sleep_for(kPollInterval);
    }
  }
  throw std::runtime_error("concurrent Android HIL node setup exceeded 15 seconds: " +
                           latest_error);
}

void ConfigurePoseMode(const ConcurrentNode &node, std::string_view mode,
                       std::optional<std::string_view> peer_origin,
                       std::optional<std::string_view> peer_token,
                       std::chrono::steady_clock::time_point deadline) {
  if (peer_origin.has_value() != peer_token.has_value()) {
    throw std::invalid_argument("paired pose HIL peer origin and token must be supplied together");
  }
  const HttpResponse current = RequireNodeHttp({.port = node.host_port,
                                                .method = "GET",
                                                .path = "/api/v1/setup",
                                                .bearer_token = node.control_token,
                                                .body = {},
                                                .deadline = deadline},
                                               200);
  const Json existing = Json::parse(current.body);
  const Json peer_update =
      peer_origin.has_value()
          ? Json{{"operation", "replace"}, {"origin", *peer_origin}, {"control_token", *peer_token}}
          : Json{{"operation", "clear"}};
  const Json request = {
      {"schema_version", 1},
      {"expected_revision", existing.at("revision")},
      {"configuration",
       {{"role", node.role},
        {"capture_profile", node.profile.name},
        {"pose",
         {{"mode", mode},
          {"inference_delegate", "gpu_preferred"},
          {"debug_evidence_enabled", true},
          {"hitting_region", {{"left", 0.15}, {"top", 0.30}, {"right", 0.85}, {"bottom", 1.0}}},
          {"peer_update", peer_update}}}}},
  };
  const HttpResponse updated = RequireNodeHttp({.port = node.host_port,
                                                .method = "PUT",
                                                .path = "/api/v1/setup",
                                                .bearer_token = node.control_token,
                                                .body = request.dump(),
                                                .deadline = deadline},
                                               200);
  const Json response = Json::parse(updated.body);
  const Json &pose = response.at("configuration").at("pose");
  if (response.value("schema_version", 0) != 1 || pose.value("mode", "") != mode ||
      updated.body.contains("control_token") ||
      (peer_token.has_value() && updated.body.contains(*peer_token))) {
    throw std::runtime_error("paired pose HIL setup response is invalid or exposes a token");
  }
  if (peer_origin.has_value()) {
    if (!pose.at("peer").is_object() || pose.at("peer").value("origin", "") != *peer_origin) {
      throw std::runtime_error("paired pose leader did not persist its redacted peer origin");
    }
  } else if (!pose.at("peer").is_null()) {
    throw std::runtime_error("paired pose shadow unexpectedly retained a peer credential");
  }
  WriteArtifact(OutputDirectory() / node.role / "pose-setup.json", updated.body);
}

void PreservePoseConfiguredNodeDescriptor(const ConcurrentNode &node, std::string_view mode,
                                          bool peer_configured,
                                          std::chrono::steady_clock::time_point deadline) {
  const HttpResponse descriptor = RequireNodeHttp({.port = node.host_port,
                                                   .method = "GET",
                                                   .path = "/api/v1/node",
                                                   .bearer_token = {},
                                                   .body = {},
                                                   .deadline = deadline},
                                                  200);
  ValidatePoseConfiguredNodeDescriptor(PoseConfiguredDescriptorInspection{
      .descriptor_json = descriptor.body,
      .identity = node.identity,
      .expected_mode = mode,
      .expected_peer_configured = peer_configured,
  });
  WriteArtifact(OutputDirectory() / node.role / "node-descriptor-pose-configured.json",
                descriptor.body);
}

std::set<std::string, std::less<>> ReadyCaptureSessionIds(
    const ConcurrentNode &node, std::chrono::steady_clock::time_point deadline) {
  const HttpResponse response = RequireNodeHttp({.port = node.host_port,
                                                 .method = "GET",
                                                 .path = "/api/v1/sessions",
                                                 .bearer_token = {},
                                                 .body = {},
                                                 .deadline = deadline},
                                                200);
  std::set<std::string, std::less<>> ids;
  for (const Json &session : Json::parse(response.body).at("sessions")) {
    if (session.value("state", "") == "ready" && session.value("session_kind", "") == "capture") {
      const std::string id = session.value("session_id", "");
      if (!SafeSessionId(id) || !ids.insert(id).second) {
        throw std::runtime_error("paired pose HIL session list is unsafe or duplicated");
      }
    }
  }
  return ids;
}

void ArmPoseStandby(const ConcurrentNode &node, std::chrono::steady_clock::time_point deadline) {
  const Json request = {{"armed", true}};
  static_cast<void>(RequireNodeHttp({.port = node.host_port,
                                     .method = "POST",
                                     .path = "/api/v1/capture/arm",
                                     .bearer_token = node.control_token,
                                     .body = request.dump(),
                                     .deadline = deadline},
                                    202));
}

void WaitForPairedPosePhase(const ConcurrentNode &leader, const ConcurrentNode &shadow,
                            std::string_view phase,
                            std::chrono::steady_clock::time_point deadline) {
  if (phase != "monitoring" && phase != "high_speed") {
    throw std::invalid_argument("paired pose HIL phase does not have an evidence contract");
  }
  const std::string latest_filename = "pose-status-" + std::string(phase) + "-latest.json";
  const std::string accepted_filename = "pose-status-" + std::string(phase) + ".json";
  std::optional<std::chrono::steady_clock::time_point> stable_since;
  while (std::chrono::steady_clock::now() < deadline) {
    const HttpResponse leader_response = RequireNodeHttp({.port = leader.host_port,
                                                          .method = "GET",
                                                          .path = "/api/v1/capture/status",
                                                          .bearer_token = {},
                                                          .body = {},
                                                          .deadline = deadline},
                                                         200);
    const HttpResponse shadow_response = RequireNodeHttp({.port = shadow.host_port,
                                                          .method = "GET",
                                                          .path = "/api/v1/capture/status",
                                                          .bearer_token = {},
                                                          .body = {},
                                                          .deadline = deadline},
                                                         200);
    WriteArtifact(OutputDirectory() / leader.role / latest_filename, leader_response.body);
    WriteArtifact(OutputDirectory() / shadow.role / latest_filename, shadow_response.body);
    const Json leader_status = Json::parse(leader_response.body);
    const Json shadow_status = Json::parse(shadow_response.body);
    if (leader_status.value("state", "") == "error" ||
        shadow_status.value("state", "") == "error") {
      throw std::runtime_error("paired pose HIL node entered ERROR");
    }
    const Json &leader_pose = leader_status.at("pose");
    const Json &shadow_pose = shadow_status.at("pose");
    const bool phase_ready =
        leader_status.value("state", "") == "armed" && leader_status.value("armed", false) &&
        shadow_status.value("state", "") == "armed" && shadow_status.value("armed", false) &&
        leader_pose.value("phase", "") == phase && shadow_pose.value("phase", "") == phase &&
        leader_pose.value("mode", "") == "leader" && shadow_pose.value("mode", "") == "shadow";
    bool ready = phase_ready;
    if (phase == "monitoring") {
      ready = ready && leader_pose.at("metrics").value("successful_inferences", 0L) >= 2L &&
              shadow_pose.at("metrics").value("successful_inferences", 0L) >= 2L &&
              leader_pose.value("hil_pose_arm_enabled", false) &&
              leader_pose.at("standby_audio").value("ready", false) &&
              shadow_pose.at("standby_audio").value("ready", false);
    } else {
      ready = ready && leader_status.value("ring_duration_us", 0L) >= 1'300'000L &&
              shadow_status.value("ring_duration_us", 0L) >= 1'300'000L &&
              leader_pose.at("peer_arm").value("state", "") == "accepted" &&
              shadow_pose.at("peer_arm").value("state", "") == "inbound_accepted";
    }
    if (ready) {
      if (!stable_since.has_value()) {
        stable_since = std::chrono::steady_clock::now();
      } else if (std::chrono::steady_clock::now() - *stable_since >= 300ms) {
        WriteArtifact(OutputDirectory() / leader.role / accepted_filename, leader_response.body);
        WriteArtifact(OutputDirectory() / shadow.role / accepted_filename, shadow_response.body);
        return;
      }
    } else {
      stable_since.reset();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("paired pose HIL nodes did not reach phase " + std::string(phase));
}

std::array<std::string, 2> WaitForNewPoseSessions(
    const ConcurrentNode &leader, const ConcurrentNode &shadow,
    const std::set<std::string, std::less<>> &leader_baseline,
    const std::set<std::string, std::less<>> &shadow_baseline,
    std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const auto leader_current = ReadyCaptureSessionIds(leader, deadline);
    const auto shadow_current = ReadyCaptureSessionIds(shadow, deadline);
    std::vector<std::string> leader_new;
    std::vector<std::string> shadow_new;
    std::ranges::set_difference(leader_current, leader_baseline, std::back_inserter(leader_new));
    std::ranges::set_difference(shadow_current, shadow_baseline, std::back_inserter(shadow_new));
    if (leader_new.size() > 1U || shadow_new.size() > 1U) {
      throw std::runtime_error("paired pose HIL published ambiguous capture sessions");
    }
    if (leader_new.size() == 1U && shadow_new.size() == 1U) {
      return {leader_new.front(), shadow_new.front()};
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("paired pose HIL did not publish both Feather-triggered sessions");
}

std::string BuildPoseCaptureReport(const std::filesystem::path &adb, const ConcurrentNode &node,
                                   std::string_view session_id,
                                   std::chrono::steady_clock::time_point deadline) {
  const std::string manifest_text = RunRequiredAdb(
      adb,
      DeviceArguments(node.serial,
                      {"exec-out", "run-as", kPackageName, "cat",
                       "files/sessions/" + std::string(session_id) + "/manifest.json"}),
      deadline);
  const Json manifest = Json::parse(manifest_text);
  const Json &media = manifest.at("views").at(0).at("media");
  Json audio = manifest.at("android_capture").at("audio_evidence");
  const std::string audio_relative_path = audio.value("path", "");
  if (audio_relative_path != "audio_evidence.wav") {
    throw std::runtime_error("paired pose HIL manifest lacks exact retained audio evidence");
  }
  audio["path"] = "sessions/" + std::string(session_id) + "/" + audio_relative_path;
  const Json report = {
      {"schema_version", 1},
      {"report_type", "android_continuous_capture"},
      {"complete", true},
      {"passed", true},
      {"node_id", node.identity.node_id},
      {"role", node.role},
      {"retain_session_requested", true},
      {"request",
       {{"width", node.profile.capture.width},
        {"height", node.profile.capture.height},
        {"frames_per_second", 240},
        {"duration_ms", 3000},
        {"bitrate_bits_per_second", node.profile.capture.bitrate_bits_per_second},
        {"mime", "video/avc"}}},
      {"retained_session",
       {{"session_id", session_id},
        {"manifest", "sessions/" + std::string(session_id) + "/manifest.json"},
        {"media", "sessions/" + std::string(session_id) + "/" + node.role + ".mp4"},
        {"encoded_bytes", media.at("encoded_bytes")},
        {"audio_evidence", std::move(audio)}}},
  };
  return report.dump(2) + "\n";
}

Json ValidatePersistedPeerArm(std::string_view manifest_text, std::string_view expected_state,
                              std::string_view shared_session_id) {
  const Json manifest = Json::parse(manifest_text);
  const Json &peer_arm = manifest.at("android_capture").at("peer_arm");
  if (peer_arm.value("state", "") != expected_state ||
      peer_arm.value("shared_session_id", "") != shared_session_id ||
      !peer_arm.at("failure_type").is_null()) {
    throw std::runtime_error("retained manifest peer-arm evidence is not nominal");
  }
  if (expected_state == "accepted") {
    if (peer_arm.value("http_status", 0) != 202) {
      throw std::runtime_error("leader manifest lacks the accepted peer HTTP response");
    }
  } else if (!peer_arm.at("http_status").is_null()) {
    throw std::runtime_error("shadow manifest unexpectedly reports outbound peer HTTP status");
  }
  return peer_arm;
}

coordination::FourTimestampClockExchange ReadClockExchange(
    const ConcurrentNode &node, std::chrono::steady_clock::time_point deadline) {
  coordination::FourTimestampClockExchange exchange;
  exchange.coordinator_send_ns = SteadyNanoseconds(std::chrono::steady_clock::now());
  const HttpResponse response = RequireNodeHttp({.port = node.host_port,
                                                 .method = "GET",
                                                 .path = "/api/v1/clock",
                                                 .bearer_token = {},
                                                 .body = {},
                                                 .deadline = deadline},
                                                200);
  exchange.coordinator_receive_ns = SteadyNanoseconds(std::chrono::steady_clock::now());
  const Json clock = Json::parse(response.body);
  if (clock.value("schema_version", 0) != 1 ||
      clock.value("node_id", "") != node.identity.node_id) {
    throw std::runtime_error("Android clock response belongs to another node");
  }
  exchange.node_receive_ns = JsonDecimalString(clock, "request_received_elapsed_realtime_ns");
  exchange.node_send_ns = JsonDecimalString(clock, "response_prepared_elapsed_realtime_ns");
  return exchange;
}

void CollectClockExchanges(ConcurrentNode *down_the_line, ConcurrentNode *face_on,
                           std::chrono::steady_clock::time_point deadline) {
  down_the_line->clock_exchanges.reserve(kClockExchangeSampleCount);
  face_on->clock_exchanges.reserve(kClockExchangeSampleCount);
  for (std::size_t index = 0; index < kClockExchangeSampleCount; ++index) {
    down_the_line->clock_exchanges.push_back(ReadClockExchange(*down_the_line, deadline));
    face_on->clock_exchanges.push_back(ReadClockExchange(*face_on, deadline));
  }
  down_the_line->clock_estimate = coordination::EstimateClockOffset(down_the_line->identity.node_id,
                                                                    down_the_line->clock_exchanges);
  face_on->clock_estimate =
      coordination::EstimateClockOffset(face_on->identity.node_id, face_on->clock_exchanges);
  if (down_the_line->clock_estimate.status != coordination::ClockEstimateStatus::kReady ||
      face_on->clock_estimate.status != coordination::ClockEstimateStatus::kReady) {
    throw std::runtime_error("concurrent Android HIL could not establish bounded node clocks");
  }
}

std::optional<std::string> ReadAndPreserveLatestReport(
    const std::filesystem::path &adb, const ConcurrentNode &node,
    std::chrono::steady_clock::time_point deadline) {
  const CommandResult result = RunAdb(
      adb, DeviceArguments(node.serial, {"exec-out", "run-as", kPackageName, "cat", kReportPath}),
      deadline);
  if (result.exit_code != 0) {
    return std::nullopt;
  }
  WriteArtifact(OutputDirectory() / node.role / "report.json", result.output);
  return result.output;
}

bool ReportIsComplete(std::string_view report_text) {
  try {
    const Json report = Json::parse(report_text);
    if (report.value("complete", false) && !report.value("passed", false)) {
      throw std::runtime_error("Android capture failed: " + report.value("error", "unknown"));
    }
    return report.value("complete", false) && report.value("passed", false);
  } catch (const nlohmann::json::exception &) {
    return false;
  }
}

void RequireBothArmed(const std::filesystem::path &adb, const ConcurrentNode &down_the_line,
                      const ConcurrentNode &face_on, std::string_view shared_session_id,
                      std::chrono::steady_clock::time_point deadline) {
  std::optional<std::chrono::steady_clock::time_point> both_armed_at;
  while (std::chrono::steady_clock::now() < deadline) {
    const HttpResponse down_status = RequireNodeHttp({.port = down_the_line.host_port,
                                                      .method = "GET",
                                                      .path = "/api/v1/capture/status",
                                                      .bearer_token = {},
                                                      .body = {},
                                                      .deadline = deadline},
                                                     200);
    const HttpResponse face_status = RequireNodeHttp({.port = face_on.host_port,
                                                      .method = "GET",
                                                      .path = "/api/v1/capture/status",
                                                      .bearer_token = {},
                                                      .body = {},
                                                      .deadline = deadline},
                                                     200);
    WriteArtifact(OutputDirectory() / down_the_line.role / "capture-status-latest.json",
                  down_status.body);
    WriteArtifact(OutputDirectory() / face_on.role / "capture-status-latest.json",
                  face_status.body);
    const auto down_report = ReadAndPreserveLatestReport(adb, down_the_line, deadline);
    const auto face_report = ReadAndPreserveLatestReport(adb, face_on, deadline);
    if ((down_report.has_value() && ReportIsComplete(*down_report)) ||
        (face_report.has_value() && ReportIsComplete(*face_report))) {
      throw std::runtime_error("Android node triggered before the single commanded Feather swing");
    }
    try {
      ValidateArmedCaptureStatus(ArmedStatusInspection{.status_json = down_status.body,
                                                       .shared_session_id = shared_session_id});
      ValidateArmedCaptureStatus(ArmedStatusInspection{.status_json = face_status.body,
                                                       .shared_session_id = shared_session_id});
      if (!both_armed_at.has_value()) {
        both_armed_at = std::chrono::steady_clock::now();
      } else if (std::chrono::steady_clock::now() - *both_armed_at >= kQuietQualification) {
        return;
      }
    } catch (const std::runtime_error &) {
      both_armed_at.reset();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("both Android nodes did not remain ARMED for the quiet qualification");
}

std::array<std::string, 2> WaitForConcurrentReports(
    const std::filesystem::path &adb, const ConcurrentNode &down_the_line,
    const ConcurrentNode &face_on, std::chrono::steady_clock::time_point deadline) {
  std::array<std::string, 2> reports;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto down = ReadAndPreserveLatestReport(adb, down_the_line, deadline);
    const auto face = ReadAndPreserveLatestReport(adb, face_on, deadline);
    if (down.has_value()) {
      reports[0] = *down;
    }
    if (face.has_value()) {
      reports[1] = *face;
    }
    if (!reports[0].empty() && !reports[1].empty() && ReportIsComplete(reports[0]) &&
        ReportIsComplete(reports[1])) {
      return reports;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("concurrent Android HIL timed out before both reports completed");
}

CapturedNode PullConcurrentNode(const std::filesystem::path &adb, const ConcurrentNode &node,
                                std::string_view shared_session_id, std::string report_text,
                                const swing_capture::hil::FeatherSwingReceipt &feather,
                                std::int64_t capture_stage_milliseconds) {
  const auto deadline = std::chrono::steady_clock::now() + kStageDeadline;
  const Json report = Json::parse(report_text);
  const Json &retained = report.at("retained_session");
  const std::string local_session_id = retained.value("session_id", "");
  const std::string prefix = "sessions/" + local_session_id + "/";
  const std::string manifest_path = retained.value("manifest", "");
  const std::string media_path = retained.value("media", "");
  const std::string audio_path = retained.at("audio_evidence").value("path", "");
  if (!SafeSessionId(local_session_id) || manifest_path != prefix + "manifest.json" ||
      media_path != prefix + node.role + ".mp4" || audio_path != prefix + "audio_evidence.wav") {
    throw std::runtime_error("concurrent Android HIL report contains unsafe retained paths");
  }
  CapturedNode captured;
  captured.feather = feather;
  captured.report = std::move(report_text);
  const auto artifact_pull_started = std::chrono::steady_clock::now();
  captured.manifest = RunRequiredAdb(
      adb,
      DeviceArguments(node.serial,
                      {"exec-out", "run-as", kPackageName, "cat", "files/" + manifest_path}),
      deadline);
  captured.media = RunRequiredAdb(adb,
                                  DeviceArguments(node.serial, {"exec-out", "run-as", kPackageName,
                                                                "cat", "files/" + media_path}),
                                  deadline);
  captured.audio_wav = RunRequiredAdb(
      adb,
      DeviceArguments(node.serial,
                      {"exec-out", "run-as", kPackageName, "cat", "files/" + audio_path}),
      deadline);
  const std::filesystem::path output = OutputDirectory() / node.role;
  WriteArtifact(output / "report.json", captured.report);
  WriteArtifact(output / "manifest.json", captured.manifest);
  WriteArtifact(output / (node.role + ".mp4"), captured.media);
  WriteArtifact(output / "audio_evidence.wav", captured.audio_wav);
  captured.evidence = ValidateNodeEvidence(NodeEvidenceInspection{
      .report = captured.report,
      .manifest = captured.manifest,
      .media = captured.media,
      .expected_role = node.role,
      .expected_shared_session_id = shared_session_id,
      .expected_profile = node.profile.capture,
  });
  captured.artifact_pull_stage_milliseconds = ElapsedMilliseconds(artifact_pull_started);
  captured.capture_stage_milliseconds = capture_stage_milliseconds;
  const auto audio_started = std::chrono::steady_clock::now();
  captured.audio = AnalyzeRetainedAudioEvidence({
      .report = captured.report,
      .manifest = captured.manifest,
      .wav = captured.audio_wav,
      .feather_accepted_device_microseconds = feather.accepted_device_microseconds,
      .feather_impact_scheduled_device_microseconds = feather.impact_scheduled_device_microseconds,
  });
  captured.audio_stage_milliseconds = ElapsedMilliseconds(audio_started);
  return captured;
}

Json ClockEvidenceJson(const ConcurrentNode &node) {
  Json exchanges = Json::array();
  for (const auto &exchange : node.clock_exchanges) {
    exchanges.push_back({
        {"coordinator_send_ns", std::to_string(exchange.coordinator_send_ns)},
        {"node_receive_ns", std::to_string(exchange.node_receive_ns)},
        {"node_send_ns", std::to_string(exchange.node_send_ns)},
        {"coordinator_receive_ns", std::to_string(exchange.coordinator_receive_ns)},
    });
  }
  const auto &estimate = *node.clock_estimate.estimate;
  return {
      {"passed", node.clock_estimate.status == coordination::ClockEstimateStatus::kReady},
      {"node_id", node.identity.node_id},
      {"sample_count", estimate.sample_count},
      {"offset_ns", std::to_string(estimate.offset_ns)},
      {"uncertainty_ns", std::to_string(estimate.uncertainty_ns)},
      {"minimum_round_trip_ns", std::to_string(estimate.minimum_round_trip_ns)},
      {"maximum_round_trip_ns", std::to_string(estimate.maximum_round_trip_ns)},
      {"exchanges", std::move(exchanges)},
  };
}

std::string RequiredHttpHeader(const HttpResponse &response, std::string_view name) {
  const auto value = response.headers.find(name);
  if (value == response.headers.end() || value->second.empty()) {
    throw std::runtime_error("Android coordination response lacks required header " +
                             std::string(name));
  }
  return value->second;
}

Json PersistCoordinationRecord(const ConcurrentNode &node, std::string_view shared_session_id,
                               std::string_view canonical_json,
                               std::chrono::steady_clock::time_point deadline) {
  const std::string path = "/api/v1/coordination/" + std::string(shared_session_id);
  const HttpResponse posted = RequireNodeHttp({.port = node.host_port,
                                               .method = "POST",
                                               .path = path,
                                               .bearer_token = node.control_token,
                                               .body = canonical_json,
                                               .deadline = deadline},
                                              201);
  ValidateCanonicalCoordinationReplay(posted.body, canonical_json);
  const std::string post_status = RequiredHttpHeader(posted, "x-swing-capture-coordination-status");
  const std::string post_revision =
      RequiredHttpHeader(posted, "x-swing-capture-coordination-revision");
  if (post_status != "stored" || post_revision != "1") {
    throw std::runtime_error("Android node did not create coordination revision 1");
  }
  WriteArtifact(OutputDirectory() / node.role / "coordination-post-response.json", posted.body);

  const HttpResponse replayed = RequireNodeHttp({.port = node.host_port,
                                                 .method = "GET",
                                                 .path = path,
                                                 .bearer_token = node.control_token,
                                                 .body = {},
                                                 .deadline = deadline},
                                                200);
  ValidateCanonicalCoordinationReplay(replayed.body, canonical_json);
  const std::string get_status =
      RequiredHttpHeader(replayed, "x-swing-capture-coordination-status");
  const std::string get_revision =
      RequiredHttpHeader(replayed, "x-swing-capture-coordination-revision");
  if (get_status != "present" || get_revision != "1") {
    throw std::runtime_error("Android node did not replay coordination revision 1");
  }
  WriteArtifact(OutputDirectory() / node.role / "coordination-get-response.json", replayed.body);
  return {
      {"passed", true},
      {"role", node.role},
      {"post_status_code", posted.status},
      {"post_store_status", post_status},
      {"post_revision", post_revision},
      {"get_status_code", replayed.status},
      {"get_store_status", get_status},
      {"get_revision", get_revision},
      {"post_response", node.role + "/coordination-post-response.json"},
      {"get_response", node.role + "/coordination-get-response.json"},
  };
}

struct CoordinationRunEvidence {
  coordination::PairedCoordinationRecord record;
  Json report;
  std::int64_t stage_milliseconds = 0;
};

CoordinationRunEvidence BuildAndPersistCoordination(const ConcurrentNode &down_node,
                                                    const ConcurrentNode &face_node,
                                                    const CapturedNode &down_capture,
                                                    const CapturedNode &face_capture,
                                                    std::string_view shared_session_id) {
  const auto stage_started = std::chrono::steady_clock::now();
  const auto deadline = stage_started + kStageDeadline;
  const HttpResponse down_report_response =
      RequireNodeHttp({.port = down_node.host_port,
                       .method = "GET",
                       .path = "/api/v1/capture/trigger-report",
                       .bearer_token = {},
                       .body = {},
                       .deadline = deadline},
                      200);
  const HttpResponse face_report_response =
      RequireNodeHttp({.port = face_node.host_port,
                       .method = "GET",
                       .path = "/api/v1/capture/trigger-report",
                       .bearer_token = {},
                       .body = {},
                       .deadline = deadline},
                      200);
  WriteArtifact(OutputDirectory() / down_node.role / "trigger-report.json",
                down_report_response.body);
  WriteArtifact(OutputDirectory() / face_node.role / "trigger-report.json",
                face_report_response.body);
  const coordination::TriggerReport down_report =
      ValidateTriggerReport(down_report_response.body, down_node.identity, shared_session_id,
                            down_capture.evidence.local_session_id);
  const coordination::TriggerReport face_report =
      ValidateTriggerReport(face_report_response.body, face_node.identity, shared_session_id,
                            face_capture.evidence.local_session_id);
  const std::array<coordination::TriggerWithClock, 2> inputs = {
      coordination::TriggerWithClock{.report = down_report, .clock = down_node.clock_estimate},
      coordination::TriggerWithClock{.report = face_report, .clock = face_node.clock_estimate},
  };
  const auto recorded_at_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
  const coordination::PairingResult paired =
      coordination::BuildPairedCoordinationRecord(inputs, recorded_at_epoch_ms);
  if (paired.status != coordination::PairingStatus::kPaired || !paired.record.has_value()) {
    throw std::runtime_error("dual-node trigger pairing failed: " + paired.detail);
  }
  const std::string canonical = coordination::ToCanonicalJson(*paired.record);
  WriteArtifact(OutputDirectory() / "coordination.json", canonical + "\n");
  Json persistence = Json::array();
  persistence.push_back(
      PersistCoordinationRecord(down_node, shared_session_id, canonical, deadline));
  persistence.push_back(
      PersistCoordinationRecord(face_node, shared_session_id, canonical, deadline));
  const auto &record = *paired.record;
  CoordinationRunEvidence result{
      .record = record,
      .report =
          {
              {"passed", true},
              {"record", Json::parse(canonical)},
              {"canonical_record", "coordination.json"},
              {"limits_ns",
               {
                   {"maximum_per_report_uncertainty", coordination::kMaximumReportUncertaintyNs},
                   {"maximum_pair_uncertainty", coordination::kMaximumPairUncertaintyNs},
                   {"maximum_trigger_separation", coordination::kMaximumTriggerSeparationNs},
               }},
              {"measured",
               {
                   {"down_the_line_mapped_uncertainty_ns",
                    record.down_the_line.mapped_coordinator_uncertainty_ns},
                   {"face_on_mapped_uncertainty_ns",
                    record.face_on.mapped_coordinator_uncertainty_ns},
                   {"combined_pair_uncertainty_ns",
                    record.down_the_line.mapped_coordinator_uncertainty_ns +
                        record.face_on.mapped_coordinator_uncertainty_ns},
                   {"minimum_trigger_separation_ns", record.minimum_trigger_separation_ns},
                   {"maximum_trigger_separation_ns", record.maximum_trigger_separation_ns},
               }},
              {"persistence", std::move(persistence)},
          },
      .stage_milliseconds = ElapsedMilliseconds(stage_started),
  };
  return result;
}

Json OptionalSampleOffset(const std::optional<std::uint64_t> &offset) {
  return offset.has_value() ? Json(*offset) : Json(nullptr);
}

Json CommandedToneJson(const CapturedNode &captured) {
  const RetainedAudioEvidence &audio = captured.audio;
  const auto &measurements = audio.evaluation.measurements;
  Json checks = Json::array();
  for (const auto &check : audio.evaluation.checks) {
    checks.push_back({
        {"name", check.name},
        {"passed", check.passed},
        {"message", check.message},
    });
  }
  return {
      {"passed", audio.evaluation.passed},
      {"path", captured.evidence.role + "/audio_evidence.wav"},
      {"bytes", captured.audio_wav.size()},
      {"sample_rate_hz", measurements.sample_rate_hz},
      {"first_frame_position", std::to_string(audio.first_frame_position)},
      {"last_frame_position", std::to_string(audio.last_frame_position)},
      {"strike_frame_position", std::to_string(audio.strike_frame_position)},
      {"sample_count", audio.sample_count},
      {"strike_sample_index", audio.strike_sample_index},
      {"command_inference",
       {
           {"feather_accepted_device_us", captured.feather.accepted_device_microseconds},
           {"feather_impact_scheduled_device_us",
            captured.feather.impact_scheduled_device_microseconds},
           {"command_to_impact_us", audio.feather_command_to_impact_microseconds},
           {"inferred_command_sample_offset", audio.inferred_command_sample_offset},
       }},
      {"window_contract",
       {
           {"passed", true},
           {"guarded_background_duration_ms", audio.thresholds.background_duration.count()},
           {"background_guard_ms", audio.thresholds.background_guard.count()},
           {"guarded_background_required_samples", audio.guarded_background_required_samples},
           {"guarded_background_available_samples", audio.inferred_command_sample_offset},
           {"required_end_sample_offset", audio.required_end_sample_offset},
           {"captured_samples", audio.sample_count},
       }},
      {"stimulus",
       {
           {"lead_us", audio.stimulus.lead.count()},
           {"duration_us", audio.stimulus.duration.count()},
           {"frequency_hz", audio.stimulus.frequency_hz},
       }},
      {"thresholds",
       {
           {"onset_early_tolerance_ms", audio.thresholds.onset_early_tolerance.count()},
           {"maximum_additional_latency_ms", audio.thresholds.maximum_additional_latency.count()},
           {"spectral_frame_duration_ms", audio.thresholds.spectral_frame_duration.count()},
           {"spectral_hop_duration_ms", audio.thresholds.spectral_hop_duration.count()},
           {"minimum_event_rms_normalized_amplitude",
            audio.thresholds.minimum_event_rms_normalized_amplitude},
           {"minimum_signal_to_noise_decibels", audio.thresholds.minimum_signal_to_noise_decibels},
           {"minimum_fundamental_power_fraction",
            audio.thresholds.minimum_fundamental_power_fraction},
           {"maximum_frequency_error_hz", audio.thresholds.maximum_frequency_error_hz},
           {"minimum_active_duration_ms", audio.thresholds.minimum_active_duration.count()},
           {"maximum_active_duration_ms", audio.thresholds.maximum_active_duration.count()},
           {"maximum_event_clipped_fraction", audio.thresholds.maximum_event_clipped_fraction},
       }},
      {"measurements",
       {
           {"sample_rate_hz", measurements.sample_rate_hz},
           {"captured_samples", measurements.captured_samples},
           {"commanded_sample_offset", measurements.commanded_sample_offset},
           {"background_start_sample_offset", measurements.background_start_sample_offset},
           {"background_end_sample_offset", measurements.background_end_sample_offset},
           {"onset_search_start_sample_offset", measurements.onset_search_start_sample_offset},
           {"onset_search_end_sample_offset", measurements.onset_search_end_sample_offset},
           {"detected_onset_sample_offset",
            OptionalSampleOffset(measurements.detected_onset_sample_offset)},
           {"detected_end_sample_offset",
            OptionalSampleOffset(measurements.detected_end_sample_offset)},
           {"detected_onset_delay_samples", measurements.detected_onset_delay_samples},
           {"detected_onset_delay_seconds", measurements.detected_onset_delay_seconds},
           {"measured_active_duration_samples", measurements.measured_active_duration_samples},
           {"measured_active_duration_seconds", measurements.measured_active_duration_seconds},
           {"analysis_start_sample_offset", measurements.analysis_start_sample_offset},
           {"analysis_end_sample_offset", measurements.analysis_end_sample_offset},
           {"peak_normalized_amplitude", measurements.peak_normalized_amplitude},
           {"event_rms_normalized_amplitude", measurements.event_rms_normalized_amplitude},
           {"background_rms_normalized_amplitude",
            measurements.background_rms_normalized_amplitude},
           {"signal_rms_normalized_amplitude", measurements.signal_rms_normalized_amplitude},
           {"signal_to_noise_decibels", measurements.signal_to_noise_decibels},
           {"fundamental_power_fraction", measurements.fundamental_power_fraction},
           {"estimated_frequency_hz", measurements.estimated_frequency_hz},
           {"frequency_error_hz", measurements.frequency_error_hz},
           {"active_spectral_frames", measurements.active_spectral_frames},
           {"event_clipped_samples", measurements.event_clipped_samples},
           {"event_clipped_fraction", measurements.event_clipped_fraction},
       }},
      {"checks", std::move(checks)},
  };
}

Json EvidenceJson(const CapturedNode &captured) {
  const NodeEvidence &evidence = captured.evidence;
  const TimingCorrelationEvidence timing = CorrelationBudget(captured);
  constexpr auto kDeadlineMilliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count();
  const bool stages_passed = captured.capture_stage_milliseconds <= kDeadlineMilliseconds &&
                             captured.artifact_pull_stage_milliseconds <= kDeadlineMilliseconds &&
                             captured.ffprobe_stage_milliseconds <= kDeadlineMilliseconds &&
                             captured.decode_stage_milliseconds <= kDeadlineMilliseconds &&
                             captured.diagnostic_stage_milliseconds <= kDeadlineMilliseconds &&
                             captured.audio_stage_milliseconds <= kDeadlineMilliseconds;
  const bool decoded_video_passed =
      captured.optical.decoded_frame_count == captured.evidence.frame_count;
  const bool passed = stages_passed && decoded_video_passed && captured.audio.evaluation.passed &&
                      captured.optical.detected && timing.passed;
  Json april_tag_frames = Json::array();
  for (std::size_t index = 0; index < captured.april_tags.size(); ++index) {
    const auto &tag = captured.april_tags[index];
    april_tag_frames.push_back({
        {"frame_index", captured.diagnostic_frame_indices[index]},
        {"family", tag.family},
        {"id", tag.id},
        {"hamming", tag.hamming},
        {"decision_margin", tag.decision_margin},
    });
  }
  return {
      {"passed", passed},
      {"node_id", evidence.node_id},
      {"role", evidence.role},
      {"local_session_id", evidence.local_session_id},
      {"shared_session_id", evidence.shared_session_id},
      {"frame_count", evidence.frame_count},
      {"measured_sensor_fps", evidence.measured_sensor_fps},
      {"actual_pre_roll_us", evidence.actual_pre_roll_us},
      {"actual_post_roll_us", evidence.actual_post_roll_us},
      {"profile",
       {
           {"width", evidence.width},
           {"height", evidence.height},
           {"frames_per_second", 240},
           {"bitrate_bits_per_second", evidence.bitrate_bits_per_second},
       }},
      {"local_nearest_frame_residual_us", evidence.local_nearest_frame_residual_us},
      {"trigger_timestamp_uncertainty_ns", evidence.trigger_timestamp_uncertainty_ns},
      {"audio",
       {
           {"passed", captured.audio.evaluation.passed},
           {"source", "local_audio"},
           {"sample_rate_hz", evidence.audio_sample_rate_hz},
           {"peak_amplitude", evidence.audio_peak_amplitude},
           {"noise_floor", evidence.audio_noise_floor},
           {"threshold", evidence.audio_threshold},
           {"commanded_tone", CommandedToneJson(captured)},
       }},
      {"encoded_bytes", evidence.media_bytes},
      {"decoded_video",
       {
           {"passed", decoded_video_passed},
           {"exact_frame_count", decoded_video_passed},
           {"decoded_frame_count", captured.optical.decoded_frame_count},
           {"manifest_frame_count", evidence.frame_count},
           {"display_order_pts_valid", true},
           {"maximum_media_time_residual_us", captured.maximum_media_time_residual_us},
           {"analysis_width", captured.analysis_width},
           {"analysis_height", captured.analysis_height},
       }},
      {"optical",
       {
           {"passed", captured.optical.detected},
           {"detected", captured.optical.detected},
           {"diagnostic", captured.optical.diagnostic},
           {"peak_frame_index", captured.optical.peak_frame_index},
           {"first_white_frame_index", captured.optical.first_white_frame_index},
           {"last_white_frame_index", captured.optical.last_white_frame_index},
           {"maximum_white_delta", captured.optical.maximum_white_delta},
           {"localized_response_tile_count", captured.optical.localized_response_tile_count},
           {"post_sequence_baseline_shift", captured.optical.post_sequence_baseline_shift},
           {"white_duration_us", captured.optical.white_duration_us},
           {"optical_to_audio_offset_us", captured.optical.optical_to_audio_offset_us},
           {"optical_onset_lower_bound_us", captured.optical.optical_onset_lower_bound_us},
           {"optical_onset_upper_bound_us", captured.optical.optical_onset_upper_bound_us},
           {"timing_correlation",
            {
                {"passed", timing.passed},
                {"signed_led_onset_to_audio_strike_residual_us",
                 captured.optical.optical_to_audio_offset_us},
                {"tone_onset_relative_to_strike_us", ToneOnsetRelativeToStrikeJson(captured)},
                {"acceptance_limit_us", timing.acceptance_limit_us},
                {"optical_onset_lower_bound_us", timing.optical_onset_lower_bound_us},
                {"optical_onset_upper_bound_us", timing.optical_onset_upper_bound_us},
                {"optical_interval_width_us", timing.optical_interval_width_us},
                {"audio_trigger_uncertainty_us", timing.audio_trigger_uncertainty_us},
                {"signed_feather_white_minus_tone_command_delta_us",
                 SignedFeatherWhiteToneDeltaUs(captured.feather)},
                {"media_pts_residual_us", timing.media_pts_residual_us},
                {"accounted_uncertainty_us", timing.accounted_uncertainty_us},
                {"minimum_residual_us", timing.minimum_residual_us},
                {"maximum_residual_us", timing.maximum_residual_us},
                {"total_bound_us", timing.total_bound_us},
            }},
       }},
      {"april_tag",
       {
           {"passed", true},
           {"required_family", "tag36h11"},
           {"required_id", 0},
           {"persistent_pre_impact_post", true},
           {"frames", std::move(april_tag_frames)},
       }},
      {"feather",
       {
           {"passed", true},
           {"receipt", FeatherReceiptJson(captured.feather)},
       }},
      {"stages",
       {
           {"capture", StageJson(captured.capture_stage_milliseconds)},
           {"artifact_pull", StageJson(captured.artifact_pull_stage_milliseconds)},
           {"ffprobe", StageJson(captured.ffprobe_stage_milliseconds)},
           {"decode", StageJson(captured.decode_stage_milliseconds)},
           {"diagnostic", StageJson(captured.diagnostic_stage_milliseconds)},
           {"audio_analysis", StageJson(captured.audio_stage_milliseconds)},
       }},
      {"report", evidence.role + "/report.json"},
      {"manifest", evidence.role + "/manifest.json"},
      {"media", evidence.role + "/" + evidence.role + ".mp4"},
      {"audio_evidence", evidence.role + "/audio_evidence.wav"},
      {"ffprobe", evidence.role + "/ffprobe.json"},
      {"diagnostic_frames",
       {
           {"pre",
            {{"frame_index", captured.diagnostic_frame_indices[0]},
             {"path", evidence.role + "/diagnostic-01.png"}}},
           {"impact",
            {{"frame_index", captured.diagnostic_frame_indices[1]},
             {"path", evidence.role + "/diagnostic-02.png"}}},
           {"post",
            {{"frame_index", captured.diagnostic_frame_indices[2]},
             {"path", evidence.role + "/diagnostic-03.png"}}},
       }},
  };
}

std::string FirstLine(std::string_view text) {
  return std::string(text.substr(0U, text.find('\n')));
}

int RunPairedPose(int argument_count, char **arguments) {
  if (argument_count != 4 || std::string_view(arguments[3]) != "paired-pose") {
    throw std::runtime_error("expected Bazel runfiles: <adb> <APK> paired-pose");
  }
  const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
  const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
  if (!std::filesystem::is_regular_file(adb) || access(adb.c_str(), X_OK) != 0 ||
      !std::filesystem::is_regular_file(apk)) {
    throw std::runtime_error("paired pose-arm dual Android HIL runfiles are missing");
  }
  const std::string leader_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_SERIAL");
  const std::string shadow_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_SERIAL");
  if (leader_serial == shadow_serial) {
    throw std::runtime_error("paired pose-arm HIL requires two distinct devices");
  }

  StopBothGuard cleanup(adb, leader_serial, shadow_serial);
  StopPackage(adb, leader_serial);
  StopPackage(adb, shadow_serial);
  cleanup.SnapshotNodeConfiguration(leader_serial);
  cleanup.SnapshotNodeConfiguration(shadow_serial);
  const auto identity_deadline = std::chrono::steady_clock::now() + 3s;
  const std::string leader_model = ReadDeviceModel(adb, leader_serial, identity_deadline);
  const std::string shadow_model = ReadDeviceModel(adb, shadow_serial, identity_deadline);
  if (!leader_model.starts_with("Pixel 6") || shadow_model != "Pixel 5a") {
    throw std::runtime_error(
        "paired pose-arm HIL requires Pixel 6-family leader and Pixel 5a shadow");
  }
  const ExternalTools external_tools = DiscoverExternalTools();
  ConcurrentNode leader =
      ConfigureConcurrentNode(adb, apk, leader_serial, "down_the_line", &cleanup, true);
  ConcurrentNode shadow =
      ConfigureConcurrentNode(adb, apk, shadow_serial, "face_on", &cleanup, true);
  if (leader.identity.node_id == shadow.identity.node_id) {
    throw std::runtime_error("paired pose-arm HIL nodes reuse one persistent identity");
  }

  const auto association_started = std::chrono::steady_clock::now();
  const auto association_deadline = association_started + kStageDeadline;
  EstablishReverse(adb, leader.serial, kPosePeerTunnelPort, shadow.host_port, association_deadline);
  cleanup.RegisterReverse(leader.serial, kPosePeerTunnelPort);
  ConfigurePoseMode(shadow, "shadow", std::nullopt, std::nullopt, association_deadline);
  const std::string peer_origin = "http://127.0.0.1:" + std::to_string(kPosePeerTunnelPort);
  ConfigurePoseMode(leader, "leader", peer_origin, shadow.control_token, association_deadline);
  PreservePoseConfiguredNodeDescriptor(shadow, "shadow", false, association_deadline);
  PreservePoseConfiguredNodeDescriptor(leader, "leader", true, association_deadline);
  CollectClockExchanges(&leader, &shadow, association_deadline);
  WriteArtifact(OutputDirectory() / leader.role / "clock.json",
                ClockEvidenceJson(leader).dump(2) + "\n");
  WriteArtifact(OutputDirectory() / shadow.role / "clock.json",
                ClockEvidenceJson(shadow).dump(2) + "\n");
  const std::int64_t association_stage_milliseconds = ElapsedMilliseconds(association_started);

  const auto capture_started = std::chrono::steady_clock::now();
  const auto capture_deadline = capture_started + kStageDeadline;
  const auto leader_baseline = ReadyCaptureSessionIds(leader, capture_deadline);
  const auto shadow_baseline = ReadyCaptureSessionIds(shadow, capture_deadline);
  ArmPoseStandby(shadow, capture_deadline);
  ArmPoseStandby(leader, capture_deadline);
  WaitForPairedPosePhase(leader, shadow, "monitoring", capture_deadline);

  const HttpResponse transition = RequireNodeHttp({.port = leader.host_port,
                                                   .method = "POST",
                                                   .path = "/api/v1/hil/pose-arm",
                                                   .bearer_token = leader.control_token,
                                                   .body = {},
                                                   .deadline = capture_deadline},
                                                  202);
  WriteArtifact(OutputDirectory() / leader.role / "pose-arm-response.json", transition.body);
  const Json transition_json = Json::parse(transition.body);
  const std::string shared_session_id = transition_json.value("shared_session_id", "");
  if (transition_json.value("schema_version", 0) != 1 ||
      transition_json.value("state", "") != "transitioning_to_high_speed" ||
      !SafeSessionId(shared_session_id)) {
    throw std::runtime_error("leader deterministic pose-arm response is invalid");
  }
  WaitForPairedPosePhase(leader, shadow, "high_speed", capture_deadline);
  const swing_capture::hil::FeatherSwingReceipt feather = RunFeatherSwing(OutputDirectory());
  const std::array<std::string, 2> session_ids =
      WaitForNewPoseSessions(leader, shadow, leader_baseline, shadow_baseline, capture_deadline);
  const std::int64_t capture_stage_milliseconds = ElapsedMilliseconds(capture_started);
  if (capture_stage_milliseconds >
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count()) {
    throw std::runtime_error("paired pose-arm capture exceeded 15 seconds");
  }

  const auto report_deadline = std::chrono::steady_clock::now() + kStageDeadline;
  std::array<std::string, 2> reports = {
      BuildPoseCaptureReport(adb, leader, session_ids[0], report_deadline),
      BuildPoseCaptureReport(adb, shadow, session_ids[1], report_deadline),
  };
  CapturedNode leader_capture = PullConcurrentNode(
      adb, leader, shared_session_id, std::move(reports[0]), feather, capture_stage_milliseconds);
  CapturedNode shadow_capture = PullConcurrentNode(
      adb, shadow, shared_session_id, std::move(reports[1]), feather, capture_stage_milliseconds);
  AnalyzeNodeMedia(&leader_capture, OutputDirectory() / leader.role, external_tools);
  WriteArtifact(OutputDirectory() / leader.role / "evidence.json",
                EvidenceJson(leader_capture).dump(2) + "\n");
  ValidateCompletedNode(leader_capture);
  AnalyzeNodeMedia(&shadow_capture, OutputDirectory() / shadow.role, external_tools);
  WriteArtifact(OutputDirectory() / shadow.role / "evidence.json",
                EvidenceJson(shadow_capture).dump(2) + "\n");
  ValidateCompletedNode(shadow_capture);
  ValidateDualSession(leader_capture.evidence, shadow_capture.evidence);
  const Json leader_peer_manifest =
      ValidatePersistedPeerArm(leader_capture.manifest, "accepted", shared_session_id);
  const Json shadow_peer_manifest =
      ValidatePersistedPeerArm(shadow_capture.manifest, "inbound_accepted", shared_session_id);
  if (leader_capture.evidence.node_id != leader.identity.node_id ||
      shadow_capture.evidence.node_id != shadow.identity.node_id ||
      leader_capture.april_tags.front().family != shadow_capture.april_tags.front().family ||
      leader_capture.april_tags.front().id != shadow_capture.april_tags.front().id) {
    throw std::runtime_error("paired pose-arm evidence identity cross-check failed");
  }
  CoordinationRunEvidence coordination_evidence = BuildAndPersistCoordination(
      leader, shadow, leader_capture, shadow_capture, shared_session_id);
  if (coordination_evidence.stage_milliseconds >
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count()) {
    throw std::runtime_error("paired pose-arm coordination persistence exceeded 15 seconds");
  }

  Json aggregate = {
      {"schema_version", 1},
      {"report_type", "android_dual_phone_paired_pose_arm_hil"},
      {"passed", true},
      {"camera_jobs_concurrent", true},
      {"single_feather_swing_count", 1},
      {"shared_session_id", shared_session_id},
      {"pose_transition",
       {
           {"passed", true},
           {"leader_role", leader.role},
           {"shadow_role", shadow.role},
           {"leader_device_model", leader_model},
           {"shadow_device_model", shadow_model},
           {"leader_candidate_source", "explicit_hil_endpoint"},
           {"peer_dispatch", "production_pose_peer_arm_client"},
           {"peer_transport", "adb_reverse_to_shadow_http_api"},
           {"standby_inference", "real_5hz_on_device"},
           {"high_speed_profile", "720p240"},
           {"endpoint_hil_launch_gated", true},
           {"persisted_peer_arm",
            {{"leader", leader_peer_manifest}, {"shadow", shadow_peer_manifest}}},
       }},
      {"cross_node_checks",
       {
           {"passed", true},
           {"distinct_node_identities", true},
           {"roles_complete", true},
           {"shared_session_id_matches", true},
           {"distinct_local_session_ids", true},
           {"same_april_tag_identity", true},
       }},
      {"clock_exchanges",
       {
           {"down_the_line", ClockEvidenceJson(leader)},
           {"face_on", ClockEvidenceJson(shadow)},
       }},
      {"coordination", std::move(coordination_evidence.report)},
      {"stages",
       {
           {"down_the_line_setup", StageJson(leader.setup_stage_milliseconds)},
           {"face_on_setup", StageJson(shadow.setup_stage_milliseconds)},
           {"peer_association", StageJson(association_stage_milliseconds)},
           {"standby_transition_and_capture", StageJson(capture_stage_milliseconds)},
           {"coordination", StageJson(coordination_evidence.stage_milliseconds)},
       }},
      {"decoder",
       {
           {"ownership", "local_nonhermetic_manual_hil"},
           {"ffmpeg", FirstLine(external_tools.ffmpeg_version)},
           {"ffprobe", FirstLine(external_tools.ffprobe_version)},
       }},
      {"artifacts",
       {
           {"report", "report.json"},
           {"feather_receipt", "feather.json"},
           {"feather_transaction", "feather-transaction.json"},
           {"coordination_record", "coordination.json"},
           {"leader_pose_arm_response", "down_the_line/pose-arm-response.json"},
           {"down_the_line",
            {
                {"initial_node_descriptor", "down_the_line/node-descriptor-initial.json"},
                {"pose_configured_node_descriptor",
                 "down_the_line/node-descriptor-pose-configured.json"},
                {"pose_setup", "down_the_line/pose-setup.json"},
                {"pose_status_monitoring", "down_the_line/pose-status-monitoring.json"},
                {"pose_status_high_speed", "down_the_line/pose-status-high-speed.json"},
                {"clock", "down_the_line/clock.json"},
                {"trigger_report", "down_the_line/trigger-report.json"},
                {"evidence", "down_the_line/evidence.json"},
            }},
           {"face_on",
            {
                {"initial_node_descriptor", "face_on/node-descriptor-initial.json"},
                {"pose_configured_node_descriptor", "face_on/node-descriptor-pose-configured.json"},
                {"pose_setup", "face_on/pose-setup.json"},
                {"pose_status_monitoring", "face_on/pose-status-monitoring.json"},
                {"pose_status_high_speed", "face_on/pose-status-high-speed.json"},
                {"clock", "face_on/clock.json"},
                {"trigger_report", "face_on/trigger-report.json"},
                {"evidence", "face_on/evidence.json"},
            }},
       }},
      {"nodes", Json::array({EvidenceJson(leader_capture), EvidenceJson(shadow_capture)})},
  };
  cleanup.RestoreNodeConfigurationsChecked();
  aggregate["configuration_restore"] = {
      {"passed", true},
      {"scope", "complete_private_node_configuration_generation"},
      {"temporary_peer_configuration_removed", true},
      {"secret_material_preserved_in_artifacts", false},
  };
  ValidatePairedPoseHilReport(aggregate.dump());
  WriteArtifact(OutputDirectory() / "report.json", aggregate.dump(2) + "\n");
  std::cout << "Published paired pose-arm dual-phone Android HIL evidence\n";
  return 0;
}

int RunConcurrent(int argument_count, char **arguments) {
  if (argument_count != 4 || std::string_view(arguments[3]) != "concurrent") {
    throw std::runtime_error("expected Bazel runfiles: <adb> <APK> concurrent");
  }
  const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
  const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
  if (!std::filesystem::is_regular_file(adb) || access(adb.c_str(), X_OK) != 0 ||
      !std::filesystem::is_regular_file(apk)) {
    throw std::runtime_error("concurrent dual Android HIL runfiles are missing");
  }
  const std::string down_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_SERIAL");
  const std::string face_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_SERIAL");
  if (down_serial == face_serial) {
    throw std::runtime_error("concurrent dual Android HIL requires two distinct devices");
  }
  StopBothGuard cleanup(adb, down_serial, face_serial);
  StopPackage(adb, down_serial);
  StopPackage(adb, face_serial);
  const ExternalTools external_tools = DiscoverExternalTools();
  const std::string shared_session_id = GenerateSharedSessionId();
  ConcurrentNode down_node =
      ConfigureConcurrentNode(adb, apk, down_serial, "down_the_line", &cleanup);
  ConcurrentNode face_node = ConfigureConcurrentNode(adb, apk, face_serial, "face_on", &cleanup);
  if (down_node.identity.node_id == face_node.identity.node_id) {
    throw std::runtime_error("concurrent dual Android HIL nodes reuse one persistent identity");
  }

  const auto capture_started = std::chrono::steady_clock::now();
  const auto capture_deadline = capture_started + kStageDeadline;
  RunRequiredAdb(adb,
                 StartArguments({.serial = down_node.serial,
                                 .role = down_node.role,
                                 .shared_session_id = shared_session_id,
                                 .profile = down_node.profile}),
                 capture_deadline);
  const auto second_dispatch_started = std::chrono::steady_clock::now();
  RunRequiredAdb(adb,
                 StartArguments({.serial = face_node.serial,
                                 .role = face_node.role,
                                 .shared_session_id = shared_session_id,
                                 .profile = face_node.profile}),
                 capture_deadline);
  down_node.arm_dispatch_milliseconds = ElapsedMilliseconds(capture_started);
  face_node.arm_dispatch_milliseconds = ElapsedMilliseconds(second_dispatch_started);
  if (down_node.arm_dispatch_milliseconds > 1000L) {
    throw std::runtime_error("dual Android HIL arm dispatches were not concurrent within 1 second");
  }
  CollectClockExchanges(&down_node, &face_node, capture_deadline);
  WriteArtifact(OutputDirectory() / down_node.role / "clock.json",
                ClockEvidenceJson(down_node).dump(2) + "\n");
  WriteArtifact(OutputDirectory() / face_node.role / "clock.json",
                ClockEvidenceJson(face_node).dump(2) + "\n");
  RequireBothArmed(adb, down_node, face_node, shared_session_id, capture_deadline);
  const swing_capture::hil::FeatherSwingReceipt feather = RunFeatherSwing(OutputDirectory());
  const std::array<std::string, 2> reports =
      WaitForConcurrentReports(adb, down_node, face_node, capture_deadline);
  const std::int64_t capture_stage_milliseconds = ElapsedMilliseconds(capture_started);
  if (capture_stage_milliseconds >
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count()) {
    throw std::runtime_error("concurrent dual Android HIL capture exceeded 15 seconds");
  }

  CapturedNode down_capture = PullConcurrentNode(adb, down_node, shared_session_id, reports[0],
                                                 feather, capture_stage_milliseconds);
  CapturedNode face_capture = PullConcurrentNode(adb, face_node, shared_session_id, reports[1],
                                                 feather, capture_stage_milliseconds);
  AnalyzeNodeMedia(&down_capture, OutputDirectory() / down_node.role, external_tools);
  WriteArtifact(OutputDirectory() / down_node.role / "evidence.json",
                EvidenceJson(down_capture).dump(2) + "\n");
  ValidateCompletedNode(down_capture);
  AnalyzeNodeMedia(&face_capture, OutputDirectory() / face_node.role, external_tools);
  WriteArtifact(OutputDirectory() / face_node.role / "evidence.json",
                EvidenceJson(face_capture).dump(2) + "\n");
  ValidateCompletedNode(face_capture);
  ValidateDualSession(down_capture.evidence, face_capture.evidence);
  if (down_capture.evidence.node_id != down_node.identity.node_id ||
      face_capture.evidence.node_id != face_node.identity.node_id ||
      down_capture.april_tags.front().family != face_capture.april_tags.front().family ||
      down_capture.april_tags.front().id != face_capture.april_tags.front().id) {
    throw std::runtime_error("concurrent dual Android evidence identity cross-check failed");
  }
  CoordinationRunEvidence coordination_evidence = BuildAndPersistCoordination(
      down_node, face_node, down_capture, face_capture, shared_session_id);
  if (coordination_evidence.stage_milliseconds >
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count()) {
    throw std::runtime_error("dual Android coordination persistence exceeded 15 seconds");
  }
  const Json aggregate = {
      {"schema_version", 1},
      {"report_type", "android_dual_phone_concurrent_hil"},
      {"passed", true},
      {"camera_jobs_concurrent", true},
      {"single_feather_swing_count", 1},
      {"shared_session_id", shared_session_id},
      {"coordination_scope", "one_event_dual_phone_concurrent"},
      {"concurrent_capture_validated", true},
      {"cross_node_checks",
       {
           {"passed", true},
           {"distinct_node_identities", true},
           {"roles_complete", true},
           {"shared_session_id_matches", true},
           {"distinct_local_session_ids", true},
           {"same_april_tag_identity", true},
       }},
      {"clock_exchanges",
       {
           {"down_the_line", ClockEvidenceJson(down_node)},
           {"face_on", ClockEvidenceJson(face_node)},
       }},
      {"arm_dispatch",
       {
           {"passed", down_node.arm_dispatch_milliseconds <= 1000L},
           {"first_to_second_dispatch_complete_ms", down_node.arm_dispatch_milliseconds},
           {"second_dispatch_command_ms", face_node.arm_dispatch_milliseconds},
           {"maximum_dispatch_span_ms", 1000},
       }},
      {"coordination", std::move(coordination_evidence.report)},
      {"stages",
       {
           {"down_the_line_setup", StageJson(down_node.setup_stage_milliseconds)},
           {"face_on_setup", StageJson(face_node.setup_stage_milliseconds)},
           {"arm_and_capture", StageJson(capture_stage_milliseconds)},
           {"coordination", StageJson(coordination_evidence.stage_milliseconds)},
       }},
      {"decoder",
       {
           {"ownership", "local_nonhermetic_manual_hil"},
           {"ffmpeg", FirstLine(external_tools.ffmpeg_version)},
           {"ffprobe", FirstLine(external_tools.ffprobe_version)},
       }},
      {"artifacts",
       {
           {"report", "report.json"},
           {"feather_receipt", "feather.json"},
           {"feather_transaction", "feather-transaction.json"},
           {"coordination_record", "coordination.json"},
           {"down_the_line",
            {
                {"node_descriptor", "down_the_line/node-descriptor.json"},
                {"clock", "down_the_line/clock.json"},
                {"trigger_report", "down_the_line/trigger-report.json"},
                {"evidence", "down_the_line/evidence.json"},
            }},
           {"face_on",
            {
                {"node_descriptor", "face_on/node-descriptor.json"},
                {"clock", "face_on/clock.json"},
                {"trigger_report", "face_on/trigger-report.json"},
                {"evidence", "face_on/evidence.json"},
            }},
       }},
      {"nodes", Json::array({EvidenceJson(down_capture), EvidenceJson(face_capture)})},
  };
  WriteArtifact(OutputDirectory() / "report.json", aggregate.dump(2) + "\n");
  std::cout << "Published concurrent one-event dual-phone Android HIL evidence\n";
  return 0;
}

int Run(int argument_count, char **arguments) {
  if (argument_count != 3) {
    throw std::runtime_error("expected Bazel runfiles: <adb> <APK>");
  }
  const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
  const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
  if (!std::filesystem::is_regular_file(adb) || access(adb.c_str(), X_OK) != 0 ||
      !std::filesystem::is_regular_file(apk)) {
    throw std::runtime_error("dual Android HIL runfiles are missing");
  }
  const std::string down_the_line_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_SERIAL");
  const std::string face_on_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_SERIAL");
  if (down_the_line_serial == face_on_serial) {
    throw std::runtime_error("dual Android HIL requires two distinct device serials");
  }
  const StopBothGuard cleanup(adb, down_the_line_serial, face_on_serial);
  StopPackage(adb, down_the_line_serial);
  StopPackage(adb, face_on_serial);
  const ExternalTools external_tools = DiscoverExternalTools();
  const std::string shared_session_id = GenerateSharedSessionId();

  CapturedNode down_the_line = CaptureNode(adb, apk,
                                           {.serial = down_the_line_serial,
                                            .role = "down_the_line",
                                            .shared_session_id = shared_session_id});
  AnalyzeNodeMedia(&down_the_line, OutputDirectory() / "down_the_line", external_tools);
  WriteArtifact(OutputDirectory() / "down_the_line" / "evidence.json",
                EvidenceJson(down_the_line).dump(2) + "\n");
  ValidateCompletedNode(down_the_line);
  CapturedNode face_on = CaptureNode(
      adb, apk,
      {.serial = face_on_serial, .role = "face_on", .shared_session_id = shared_session_id});
  AnalyzeNodeMedia(&face_on, OutputDirectory() / "face_on", external_tools);
  WriteArtifact(OutputDirectory() / "face_on" / "evidence.json",
                EvidenceJson(face_on).dump(2) + "\n");
  ValidateCompletedNode(face_on);
  ValidateDualSession(down_the_line.evidence, face_on.evidence);
  if (down_the_line.april_tags.front().family != face_on.april_tags.front().family ||
      down_the_line.april_tags.front().id != face_on.april_tags.front().id) {
    throw std::runtime_error("dual retained videos did not decode the same printed AprilTag");
  }
  const Json aggregate = {
      {"schema_version", 1},
      {"report_type", "android_dual_phone_sequential_hil"},
      {"passed", true},
      {"camera_jobs_concurrent", false},
      {"shared_session_id", shared_session_id},
      {"coordination_scope", "per_phone_local_sequential_gate"},
      {"concurrent_capture_validated", false},
      {"cross_node_checks",
       {
           {"passed", true},
           {"distinct_node_identities", true},
           {"roles_complete", true},
           {"shared_session_id_matches", true},
           {"same_april_tag_identity", true},
       }},
      {"decoder",
       {
           {"ownership", "local_nonhermetic_manual_hil"},
           {"ffmpeg", FirstLine(external_tools.ffmpeg_version)},
           {"ffprobe", FirstLine(external_tools.ffprobe_version)},
       }},
      {"nodes", Json::array({EvidenceJson(down_the_line), EvidenceJson(face_on)})},
  };
  WriteArtifact(OutputDirectory() / "report.json", aggregate.dump(2) + "\n");
  std::cout << "Published sequential dual-phone Android HIL evidence\n";
  return 0;
}

Json LatestNodeDiagnostics(std::string_view role) {
  const std::filesystem::path path = OutputDirectory() / role / "report.json";
  if (!std::filesystem::is_regular_file(path)) {
    return {{"present", false}, {"report", std::string(role) + "/report.json"}};
  }
  std::ifstream input(path, std::ios::binary);
  const std::uintmax_t size = std::filesystem::file_size(path);
  std::string contents(static_cast<std::size_t>(size), '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!input) {
    return {{"present", true},
            {"report", std::string(role) + "/report.json"},
            {"parse_error", "cannot read preserved report"}};
  }
  try {
    const Json report = Json::parse(contents);
    const Json audio = report.value("audio_diagnostics", Json::object());
    return {
        {"present", true},
        {"report", std::string(role) + "/report.json"},
        {"complete", report.value("complete", false)},
        {"passed", report.value("passed", false)},
        {"state", report.value("state", "")},
        {"error", report.value("error", "")},
        {"audio",
         {
             {"maximum_peak_amplitude", audio.value("maximum_peak_amplitude", 0.0)},
             {"noise_floor", audio.value("noise_floor", 0.0)},
             {"threshold", audio.value("threshold", 0.0)},
         }},
    };
  } catch (const nlohmann::json::exception &failure) {
    return {{"present", true},
            {"report", std::string(role) + "/report.json"},
            {"parse_error", failure.what()}};
  }
}

}  // namespace

int main(int argument_count, char **arguments) {
  std::signal(SIGPIPE, SIG_IGN);
  const bool concurrent = argument_count == 4 && std::string_view(arguments[3]) == "concurrent";
  const bool paired_pose = argument_count == 4 && std::string_view(arguments[3]) == "paired-pose";
  try {
    if (paired_pose) {
      return RunPairedPose(argument_count, arguments);
    }
    return concurrent ? RunConcurrent(argument_count, arguments) : Run(argument_count, arguments);
  } catch (const std::exception &failure) {
    try {
      const std::string_view report_type = paired_pose
                                               ? "android_dual_phone_paired_pose_arm_hil"
                                               : (concurrent ? "android_dual_phone_concurrent_hil"
                                                             : "android_dual_phone_sequential_hil");
      const Json report = {
          {"schema_version", 1},
          {"report_type", report_type},
          {"passed", false},
          {"camera_jobs_concurrent", concurrent || paired_pose},
          {"diagnostic", failure.what()},
          {"last_node_diagnostics",
           {
               {"down_the_line", LatestNodeDiagnostics("down_the_line")},
               {"face_on", LatestNodeDiagnostics("face_on")},
           }},
      };
      WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
    } catch (const std::exception &write_failure) {
      std::cerr << "Could not publish failing dual-phone report: " << write_failure.what() << '\n';
    }
    std::cerr << (paired_pose ? "Paired pose-arm" : (concurrent ? "Concurrent" : "Sequential"))
              << " dual-phone Android HIL failed: " << failure.what() << '\n';
    return 1;
  }
}
