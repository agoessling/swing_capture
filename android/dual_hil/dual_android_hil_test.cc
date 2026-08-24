#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
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
#include "android/dual_hil/android_apk_install.h"
#include "android/dual_hil/audio_evidence_validation.h"
#include "android/dual_hil/autonomous_recovery_validation.h"
#include "android/dual_hil/concurrent_hil_validation.h"
#include "android/dual_hil/configuration_recovery_journal.h"
#include "android/dual_hil/dual_android_hil_runner.h"
#include "android/dual_hil/dual_session_validation.h"
#include "android/dual_hil/hil_artifact_validation.h"
#include "android/dual_hil/hil_cleanup_evidence.h"
#include "android/dual_hil/hil_command.h"
#include "android/dual_hil/paired_pose_qualification_policy.h"
#include "android/dual_hil/pcm_replay_case.h"
#include "android/dual_hil/retained_media_analysis.h"
#include "android/dual_hil/rgb_swing_analysis.h"
#include "android/hil/android_probe_hil_support.h"
#include "android/pose_hil/pose_qualification_performance.h"
#include "android/pose_hil/pose_standby_validation.h"
#include "capture/audio/pcm_wav.h"
#include "capture/hil/feather_hil_controller.h"
#include "capture/hil/feather_hil_serial.h"
#include "capture/optical/april_tag.h"
#include "station/station_config.h"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
namespace coordination = swing_capture::android::dual_coordination_hil;
using swing_capture::android::dual_hil::AnalyzeRetainedAudioEvidence;
using swing_capture::android::dual_hil::AndroidApkInstallEvidence;
using swing_capture::android::dual_hil::ArmedStatusInspection;
using swing_capture::android::dual_hil::AutonomousLocalOnlyRecoveryInspection;
using swing_capture::android::dual_hil::AutonomousRecoveredStationInspection;
using swing_capture::android::dual_hil::BuildConfigurationBackupRetirementPlan;
using swing_capture::android::dual_hil::BuildConfigurationRestorePlan;
using swing_capture::android::dual_hil::CaptureProfileExpectation;
using swing_capture::android::dual_hil::CaptureStartupTimingEvidence;
using swing_capture::android::dual_hil::ClassifyPcmReplayObservationDecision;
using swing_capture::android::dual_hil::ClassifyPcmReplayPairOutcome;
using swing_capture::android::dual_hil::ClassifyPcmReplayRun;
using swing_capture::android::dual_hil::CombineHilPrimaryAndCleanup;
using swing_capture::android::dual_hil::ConfigurationRecoveryCommand;
using swing_capture::android::dual_hil::ConfigurationRecoveryJournal;
using swing_capture::android::dual_hil::ConfigurationRecoveryState;
using swing_capture::android::dual_hil::ConfigurationSnapshotReference;
using swing_capture::android::dual_hil::DurablePairingState;
using swing_capture::android::dual_hil::EvaluateTimingCorrelation;
using swing_capture::android::dual_hil::ExtractedPcmReplayCase;
using swing_capture::android::dual_hil::ExtractPcmReplayCase;
using swing_capture::android::dual_hil::FindPcmReplayCase;
using swing_capture::android::dual_hil::HasPairedAutomaticImpactEvidence;
using swing_capture::android::dual_hil::HilCleanupEvidence;
using swing_capture::android::dual_hil::HilCommandResult;
using swing_capture::android::dual_hil::HilPrimaryOutcome;
using swing_capture::android::dual_hil::InspectDiscoveryPairingFixture;
using swing_capture::android::dual_hil::InspectPairNetworkHealthForArm;
using swing_capture::android::dual_hil::InstallAndroidApk;
using swing_capture::android::dual_hil::IsPairedPoseQualificationMode;
using swing_capture::android::dual_hil::LanEndpointInspection;
using swing_capture::android::dual_hil::MappedPeerImpactStatusInspection;
using swing_capture::android::dual_hil::NodeApiIdentity;
using swing_capture::android::dual_hil::NodeEvidence;
using swing_capture::android::dual_hil::NodeEvidenceInspection;
using swing_capture::android::dual_hil::PairedPoseClockStatusInspection;
using swing_capture::android::dual_hil::PairedPoseQualificationPolicy;
using swing_capture::android::dual_hil::PairedPoseQualificationPolicyForMode;
using swing_capture::android::dual_hil::PairedPoseSessionStateInspection;
using swing_capture::android::dual_hil::PairNetworkHealthArmDecision;
using swing_capture::android::dual_hil::PairNetworkHealthArmRequestBody;
using swing_capture::android::dual_hil::PairNetworkHealthStatusInspection;
using swing_capture::android::dual_hil::ParsePcmReplayManifest;
using swing_capture::android::dual_hil::PcmReplayExpectation;
using swing_capture::android::dual_hil::PcmReplayObservation;
using swing_capture::android::dual_hil::PcmReplayObservationDecision;
using swing_capture::android::dual_hil::PcmReplayPairOutcome;
using swing_capture::android::dual_hil::PcmReplayPairOutcomeName;
using swing_capture::android::dual_hil::PcmReplayPeerImpactQuiescenceWindow;
using swing_capture::android::dual_hil::PcmReplayRunDisposition;
using swing_capture::android::dual_hil::PoseConfiguredDescriptorInspection;
using swing_capture::android::dual_hil::ReadConfigurationRecoveryJournal;
using swing_capture::android::dual_hil::RetainedAudioEvidence;
using swing_capture::android::dual_hil::RetainedAudioEvidenceInspection;
using swing_capture::android::dual_hil::RetainedMediaAnalysis;
using swing_capture::android::dual_hil::RgbSwingAnalysis;
using swing_capture::android::dual_hil::SerializeHilCleanupEvidence;
using swing_capture::android::dual_hil::TimingCorrelationEvidence;
using swing_capture::android::dual_hil::ValidateArmedCaptureStatus;
using swing_capture::android::dual_hil::ValidateAutonomousLocalOnlyRecovery;
using swing_capture::android::dual_hil::ValidateAutonomousPeerUnavailable;
using swing_capture::android::dual_hil::ValidateAutonomousRecoveredStation;
using swing_capture::android::dual_hil::ValidateCanonicalCoordinationReplay;
using swing_capture::android::dual_hil::ValidateDualSession;
using swing_capture::android::dual_hil::ValidateHilReportArtifacts;
using swing_capture::android::dual_hil::ValidateLanEndpoint;
using swing_capture::android::dual_hil::ValidateMappedPeerImpactStatus;
using swing_capture::android::dual_hil::ValidateNodeDescriptor;
using swing_capture::android::dual_hil::ValidateNodeEvidence;
using swing_capture::android::dual_hil::ValidatePairedPoseClockStatus;
using swing_capture::android::dual_hil::ValidatePairedPoseHilReport;
using swing_capture::android::dual_hil::ValidatePairedPoseQualificationReport;
using swing_capture::android::dual_hil::ValidatePairedPoseSessionState;
using swing_capture::android::dual_hil::ValidatePoseConfiguredNodeDescriptor;
using swing_capture::android::dual_hil::ValidateShortPoseLatency;
using swing_capture::android::dual_hil::ValidateTriggerReport;
using swing_capture::android::dual_hil::WriteConfigurationRecoveryJournalAtomically;
using swing_capture::android::hil::DisplayPowerStateDumps;
using swing_capture::android::hil::DisplayPowerStateInspection;
using swing_capture::android::hil::InspectDisplayPowerState;
using swing_capture::android::pose_hil::BuildPoseQualificationPerformanceSummary;
using swing_capture::android::pose_hil::DeviceTelemetry;
using swing_capture::android::pose_hil::InspectDeviceTelemetry;
using swing_capture::android::pose_hil::InspectPoseStatus;
using swing_capture::android::pose_hil::PoseStatusSample;

constexpr auto kStageDeadline = 15s;
constexpr auto kPollInterval = 100ms;
constexpr auto kQuietQualification = 1s;
constexpr auto kPcmInitialTriggerObservation = 750ms;
constexpr auto kPcmPeerImpactQuiescence =
    PcmReplayPeerImpactQuiescenceWindow(1500ms, 1500ms, 3U, 50ms, 1s);
static_assert(kPcmPeerImpactQuiescence < kStageDeadline);
constexpr std::string_view kPackageName = "com.agoessling.swingcapture";
constexpr std::string_view kReportPath = "files/reports/latest.json";
constexpr std::uint16_t kNodeHttpPort = 8088U;
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

using CommandResult = HilCommandResult;

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
  std::optional<swing_capture::hil::FeatherPcmPlaybackReceipt> pcm_playback;
  std::int64_t capture_stage_milliseconds = 0;
  std::int64_t artifact_pull_stage_milliseconds = 0;
  std::int64_t ffprobe_stage_milliseconds = 0;
  std::int64_t decode_stage_milliseconds = 0;
  std::int64_t diagnostic_stage_milliseconds = 0;
  std::int64_t audio_stage_milliseconds = 0;
  std::uint32_t analysis_width = 0;
  std::uint32_t analysis_height = 0;
};

struct PcmFailureSalvageNode {
  std::string session_id;
  std::string trigger_source;
  std::string manifest;
  std::string media;
  std::string diagnostic_audio_wav;
};

struct ExternalTools {
  std::filesystem::path ffmpeg;
  std::filesystem::path ffprobe;
  std::string ffmpeg_version;
  std::string ffprobe_version;
};

Json FeatherReceiptJson(const swing_capture::hil::FeatherSwingReceipt &receipt);
Json FeatherPcmPlaybackJson(const swing_capture::hil::FeatherPcmPlaybackReceipt &receipt);

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
    const std::string_view example = name.ends_with("_SERIAL") ? "<adb-serial>" : "<value>";
    throw std::runtime_error("set --test_env=" + std::string(name) + "=" + std::string(example));
  }
  return value;
}

bool SkipExactApkInstall() {
  const std::string value = EnvironmentValue("SWING_CAPTURE_ANDROID_SKIP_EXACT_APK_INSTALL");
  if (value.empty() || value == "false" || value == "0") {
    return false;
  }
  if (value == "true" || value == "1") {
    return true;
  }
  throw std::runtime_error(
      "SWING_CAPTURE_ANDROID_SKIP_EXACT_APK_INSTALL must be true, false, 1, or 0");
}

AndroidApkInstallEvidence InstallConfiguredApk(const std::filesystem::path &adb,
                                               const std::filesystem::path &apk,
                                               std::string_view serial,
                                               std::chrono::steady_clock::time_point deadline) {
  const std::string installer = EnvironmentValue("SWING_CAPTURE_ANDROID_APK_INSTALLER");
  if (installer.empty()) {
    throw std::runtime_error("Android HIL APK installer runfile is missing");
  }
  return InstallAndroidApk(installer, adb, apk, serial, SkipExactApkInstall(), deadline);
}

Json ApkInstallJson(const AndroidApkInstallEvidence &evidence) {
  return {
      {"installed", evidence.installed},
      {"reason", evidence.reason},
      {"sha256", evidence.sha256},
      {"installed_before_sha256", evidence.installed_before_sha256.has_value()
                                      ? Json(*evidence.installed_before_sha256)
                                      : Json(nullptr)},
      {"elapsed_ms", evidence.elapsed_ms},
  };
}

struct LanOrigin {
  std::string origin;
  std::string ipv4_address;
  std::uint16_t port = 0;
};

LanOrigin ParseLanOrigin(std::string origin, std::string_view environment_name) {
  constexpr std::string_view kPrefix = "http://";
  if (!origin.starts_with(kPrefix)) {
    throw std::invalid_argument(std::string(environment_name) +
                                " must be a path-free HTTP IPv4 origin");
  }
  const std::string_view authority(origin.data() + kPrefix.size(), origin.size() - kPrefix.size());
  if (authority.contains('/') || authority.contains('?') || authority.contains('#')) {
    throw std::invalid_argument(std::string(environment_name) +
                                " must be a path-free HTTP IPv4 origin");
  }
  const std::size_t separator = authority.rfind(':');
  if (separator == std::string_view::npos) {
    throw std::invalid_argument(std::string(environment_name) + " must include port 8088");
  }
  const std::string address(authority.substr(0U, separator));
  const std::string_view port_text = authority.substr(separator + 1U);
  std::uint16_t port = 0U;
  const auto [port_end, port_error] =
      std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
  in_addr parsed = {};
  std::array<char, INET_ADDRSTRLEN> canonical = {};
  if (port_error != std::errc() || port_end != port_text.data() + port_text.size() ||
      port != kNodeHttpPort || inet_pton(AF_INET, address.c_str(), &parsed) != 1 ||
      inet_ntop(AF_INET, &parsed, canonical.data(), canonical.size()) == nullptr ||
      address != canonical.data() || (ntohl(parsed.s_addr) >> 24U) == 127U ||
      parsed.s_addr == htonl(INADDR_ANY)) {
    throw std::invalid_argument(std::string(environment_name) +
                                " must be a canonical non-loopback IPv4 origin on port 8088");
  }
  return {.origin = std::move(origin), .ipv4_address = address, .port = port};
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

std::vector<std::byte> ReadBoundedFile(const std::filesystem::path &path,
                                       std::uintmax_t maximum_bytes) {
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error("required HIL input is not a regular file: " + path.string());
  }
  const std::uintmax_t size = std::filesystem::file_size(path);
  if (size == 0U || size > maximum_bytes || size > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("required HIL input has an invalid size: " + path.string());
  }
  std::vector<std::byte> contents(static_cast<std::size_t>(size));
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char *>(contents.data()),
             static_cast<std::streamsize>(contents.size()));
  if (!input) {
    throw std::runtime_error("cannot read required HIL input: " + path.string());
  }
  return contents;
}

CommandResult RunCommand(const std::filesystem::path &executable,
                         const std::vector<std::string> &arguments,
                         std::chrono::steady_clock::time_point deadline,
                         std::string_view input = {}) {
  return swing_capture::android::dual_hil::RunHilCommand(executable, arguments, deadline, input);
}

std::string RunRequiredCommand(const std::filesystem::path &executable,
                               const std::vector<std::string> &arguments,
                               std::chrono::steady_clock::time_point deadline) {
  return swing_capture::android::dual_hil::RunRequiredHilCommand(executable, arguments, deadline);
}

struct HttpResponse {
  int status = 0;
  std::map<std::string, std::string, std::less<>> headers;
  std::string body;
};

struct HttpRequestSpec {
  std::string_view host = "127.0.0.1";
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

int ConnectNodeHttp(std::string_view host, std::uint16_t port,
                    std::chrono::steady_clock::time_point deadline) {
  const int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (descriptor < 0) {
    throw std::runtime_error(std::string("cannot create Android node HTTP socket: ") +
                             std::strerror(errno));
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  const std::string host_text(host);
  if (inet_pton(AF_INET, host_text.c_str(), &address.sin_addr) != 1) {
    close(descriptor);
    throw std::invalid_argument("Android node HTTP host must be a canonical IPv4 address");
  }
  if (connect(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
    if (errno != EINPROGRESS) {
      const int saved_errno = errno;
      close(descriptor);
      throw std::runtime_error(std::string("cannot connect to Android node HTTP endpoint: ") +
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
      throw std::runtime_error("cannot establish Android node HTTP connection");
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
  // Setup previews are production-bounded to 512 KiB; leave room for headers while retaining a
  // hard ceiling on every HIL response.
  constexpr std::size_t kMaximumResponseBytes = 1U * 1024U * 1024U;
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
  if (specification.host.empty() || specification.port == 0U || specification.method.empty() ||
      !specification.path.starts_with('/')) {
    throw std::invalid_argument("Android node HTTP request is incomplete");
  }
  std::string request = std::string(specification.method) + " " + std::string(specification.path) +
                        " HTTP/1.1\r\nHost: " + std::string(specification.host) +
                        "\r\nAccept: application/json\r\n";
  if (!specification.bearer_token.empty()) {
    request += "Authorization: Bearer " + std::string(specification.bearer_token) + "\r\n";
  }
  if (!specification.body.empty()) {
    request += "Content-Type: application/json\r\nContent-Length: " +
               std::to_string(specification.body.size()) + "\r\n";
  }
  request += "Connection: close\r\n\r\n";
  request.append(specification.body);
  const int descriptor =
      ConnectNodeHttp(specification.host, specification.port, specification.deadline);
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

std::uint64_t CurrentEpochMilliseconds() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

std::string GenerateRecoveryTransactionId() {
  std::random_device random;
  std::uniform_int_distribution<std::uint32_t> distribution;
  std::array<char, 96> text{};
  const int length = std::snprintf(text.data(), text.size(), "dual_hil_%llu_%ld_%08x",
                                   static_cast<unsigned long long>(CurrentEpochMilliseconds()),
                                   static_cast<long>(getpid()), distribution(random));
  if (length <= 0 || static_cast<std::size_t>(length) >= text.size()) {
    throw std::runtime_error("cannot generate Android HIL recovery transaction ID");
  }
  return std::string(text.data(), static_cast<std::size_t>(length));
}

std::filesystem::path ConfigurationRecoveryDirectory() {
  const char *configured = std::getenv("SWING_CAPTURE_ANDROID_HIL_RECOVERY_DIR");
  const std::filesystem::path directory =
      configured != nullptr && *configured != '\0'
          ? std::filesystem::path(configured)
          : std::filesystem::path("/var/tmp") /
                ("swing-capture-android-hil-recovery-" + std::to_string(geteuid()));
  if (!directory.is_absolute() || directory == directory.root_path()) {
    throw std::invalid_argument("Android HIL recovery directory must be a bounded absolute path");
  }
  for (const auto &component : directory) {
    if (component == "..") {
      throw std::invalid_argument("Android HIL recovery directory must not contain '..'");
    }
  }
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    if (error || !std::filesystem::create_directory(directory, error) || error) {
      throw std::runtime_error("cannot create Android HIL recovery directory: " + error.message());
    }
  }
  struct stat metadata{};
  if (lstat(directory.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
      metadata.st_uid != geteuid()) {
    throw std::runtime_error("Android HIL recovery directory is not an owned real directory");
  }
  if (chmod(directory.c_str(), S_IRWXU) != 0) {
    throw std::runtime_error("cannot protect Android HIL recovery directory");
  }
  return directory;
}

class StopBothGuard {
 public:
  StopBothGuard(std::filesystem::path adb, std::string down_the_line_serial,
                std::string face_on_serial)
      : adb_(std::move(adb)),
        down_the_line_serial_(std::move(down_the_line_serial)),
        face_on_serial_(std::move(face_on_serial)),
        recovery_directory_(ConfigurationRecoveryDirectory()),
        recovery_journal_path_(recovery_directory_ / "pending.json"),
        recovery_transaction_id_(GenerateRecoveryTransactionId()) {
    recovery_lock_descriptor_ = open((recovery_directory_ / "hardware.lock").c_str(),
                                     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (recovery_lock_descriptor_ < 0 ||
        fchmod(recovery_lock_descriptor_, S_IRUSR | S_IWUSR) != 0 ||
        flock(recovery_lock_descriptor_, LOCK_EX | LOCK_NB) != 0) {
      if (recovery_lock_descriptor_ >= 0) {
        static_cast<void>(close(recovery_lock_descriptor_));
        recovery_lock_descriptor_ = -1;
      }
      throw std::runtime_error("another Android HIL owns the durable configuration-recovery lock");
    }
    try {
      RecoverPendingConfigurationJournal();
    } catch (...) {
      static_cast<void>(flock(recovery_lock_descriptor_, LOCK_UN));
      static_cast<void>(close(recovery_lock_descriptor_));
      recovery_lock_descriptor_ = -1;
      throw;
    }
    cleanup_evidence_.Register("package_stop", "down_the_line");
    cleanup_evidence_.Register("package_stop", "face_on");
    PublishCleanupEvidence();
  }

  StopBothGuard(const StopBothGuard &) = delete;
  StopBothGuard &operator=(const StopBothGuard &) = delete;

  void RegisterForward(std::string serial, std::uint16_t host_port) {
    forwards_.emplace_back(serial, host_port);
    cleanup_evidence_.Register("adb_forward_remove",
                               CleanupTarget(serial) + ":tcp:" + std::to_string(host_port));
    PublishCleanupEvidence();
  }

  void RegisterReverse(std::string serial, std::uint16_t device_port) {
    reverses_.emplace_back(serial, device_port);
    cleanup_evidence_.Register("adb_reverse_remove",
                               CleanupTarget(serial) + ":tcp:" + std::to_string(device_port));
    PublishCleanupEvidence();
  }

  void RegisterPrivateFileCleanup(std::string serial, std::string path) {
    if (!path.starts_with("files/") || path.contains("..") || path.ends_with('/')) {
      throw std::invalid_argument("Android HIL private cleanup path is unsafe");
    }
    cleanup_evidence_.Register("private_file_remove", CleanupTarget(serial) + ":" + path);
    private_file_cleanups_.emplace_back(std::move(serial), std::move(path));
    PublishCleanupEvidence();
  }

  void PreserveRegisteredPrivateFile(std::string_view serial, std::string_view path) {
    const auto registered = std::ranges::find_if(private_file_cleanups_, [&](const auto &entry) {
      return entry.first == serial && entry.second == path;
    });
    if (registered == private_file_cleanups_.end()) {
      throw std::logic_error("Android HIL attempted to preserve an unregistered private file");
    }
    cleanup_evidence_.Cancel("private_file_remove",
                             CleanupTarget(serial) + ":" + std::string(path));
    private_file_cleanups_.erase(registered);
    PublishCleanupEvidence();
  }

  void RegisterExternalCleanup(std::string operation, std::string target) {
    cleanup_evidence_.Register(std::move(operation), std::move(target));
    PublishCleanupEvidence();
  }

  void RecordExternalCleanupRestored(std::string_view operation, std::string_view target) {
    cleanup_evidence_.RecordRestored(operation, target);
    PublishCleanupEvidence();
  }

  void RecordExternalCleanupFailed(std::string_view operation, std::string_view target,
                                   std::string diagnostic) {
    cleanup_evidence_.RecordFailed(operation, target, std::move(diagnostic));
    PublishCleanupEvidence();
  }

  void SnapshotNodeConfigurations() {
    if (configuration_journal_.has_value()) {
      throw std::logic_error("Android HIL node configurations were already snapshotted");
    }
    std::vector<ConfigurationSnapshotReference> snapshots;
    try {
      snapshots.push_back(SnapshotNodeConfiguration(down_the_line_serial_, "down_the_line"));
      snapshots.push_back(SnapshotNodeConfiguration(face_on_serial_, "face_on"));
    } catch (...) {
      for (const ConfigurationSnapshotReference &snapshot : snapshots) {
        if (snapshot.original_existed) {
          static_cast<void>(
              RunAdb(adb_,
                     DeviceArguments(snapshot.serial, {"shell", "run-as", kPackageName, "rm", "-f",
                                                       snapshot.backup_relative_path}),
                     std::chrono::steady_clock::now() + 2s));
        }
      }
      throw;
    }
    ConfigurationRecoveryJournal journal{
        .transaction_id = recovery_transaction_id_,
        .created_at_epoch_ms = CurrentEpochMilliseconds(),
        .package_name = std::string(kPackageName),
        .state = ConfigurationRecoveryState::kPrepared,
        .snapshots = std::move(snapshots),
    };
    try {
      WriteConfigurationRecoveryJournalAtomically(recovery_journal_path_, journal);
    } catch (...) {
      // The node configuration has not been mutated yet. If the host cannot durably record the
      // recovery plan, retire the otherwise-orphaned phone-private snapshots before failing.
      for (const ConfigurationSnapshotReference &snapshot : journal.snapshots) {
        if (snapshot.original_existed) {
          static_cast<void>(
              RunAdb(adb_,
                     DeviceArguments(snapshot.serial, {"shell", "run-as", kPackageName, "rm", "-f",
                                                       snapshot.backup_relative_path}),
                     std::chrono::steady_clock::now() + 2s));
        }
      }
      throw;
    }
    configuration_journal_ = journal;
    for (const ConfigurationSnapshotReference &snapshot : journal.snapshots) {
      cleanup_evidence_.Register("configuration_restore", CleanupTarget(snapshot.serial));
    }
    PublishCleanupEvidence();
  }

  void RestoreNodeConfigurationsChecked() {
    if (!configuration_journal_.has_value() || configuration_journal_->snapshots.size() != 2U) {
      throw std::runtime_error("paired HIL did not snapshot both node configurations");
    }
    try {
      RestoreConfigurationJournal(configuration_journal_.value());
    } catch (const std::exception &failure) {
      for (const ConfigurationSnapshotReference &snapshot : configuration_journal_->snapshots) {
        try {
          cleanup_evidence_.RecordFailed("configuration_restore", CleanupTarget(snapshot.serial),
                                         failure.what());
        } catch (const std::exception &) {
        }
      }
      PublishCleanupEvidence();
      throw;
    }
    cleanup_evidence_.RecordRestored("package_stop", "down_the_line");
    cleanup_evidence_.RecordRestored("package_stop", "face_on");
    for (const ConfigurationSnapshotReference &snapshot : configuration_journal_->snapshots) {
      cleanup_evidence_.RecordRestored("configuration_restore", CleanupTarget(snapshot.serial));
    }
    PublishCleanupEvidence();
    configurations_restored_ = true;
    configuration_journal_.reset();
  }

  ~StopBothGuard() {
    for (const auto &[serial, device_port] : reverses_) {
      const std::string target = CleanupTarget(serial) + ":tcp:" + std::to_string(device_port);
      AttemptCleanupNoThrow("adb_reverse_remove", target, [&] {
        const CommandResult result = RunAdb(
            adb_,
            DeviceArguments(serial, {"reverse", "--remove", "tcp:" + std::to_string(device_port)}),
            std::chrono::steady_clock::now() + 2s);
        if (result.timed_out || result.exit_code != 0) {
          throw std::runtime_error("adb reverse removal failed");
        }
      });
    }
    for (const auto &[serial, host_port] : forwards_) {
      const std::string target = CleanupTarget(serial) + ":tcp:" + std::to_string(host_port);
      AttemptCleanupNoThrow("adb_forward_remove", target, [&] {
        const CommandResult result = RunAdb(
            adb_,
            DeviceArguments(serial, {"forward", "--remove", "tcp:" + std::to_string(host_port)}),
            std::chrono::steady_clock::now() + 2s);
        if (result.timed_out || result.exit_code != 0) {
          throw std::runtime_error("adb forward removal failed");
        }
      });
    }
    for (const std::string &serial : {down_the_line_serial_, face_on_serial_}) {
      AttemptCleanupNoThrow("package_stop", CleanupTarget(serial),
                            [&] { StopPackage(adb_, serial); });
    }
    for (const auto &[serial, path] : private_file_cleanups_) {
      AttemptCleanupNoThrow("private_file_remove", CleanupTarget(serial) + ":" + path, [&] {
        const CommandResult result = RunAdb(
            adb_, DeviceArguments(serial, {"shell", "run-as", kPackageName, "rm", "-f", path}),
            std::chrono::steady_clock::now() + 2s);
        if (result.timed_out || result.exit_code != 0) {
          throw std::runtime_error("registered private artifact removal failed");
        }
      });
    }
    if (!configurations_restored_ && configuration_journal_.has_value()) {
      try {
        RestoreConfigurationJournal(configuration_journal_.value());
        for (const ConfigurationSnapshotReference &snapshot : configuration_journal_->snapshots) {
          RecordCleanupNoThrow("configuration_restore", CleanupTarget(snapshot.serial), true);
        }
        configurations_restored_ = true;
        configuration_journal_.reset();
      } catch (const std::exception &failure) {
        for (const ConfigurationSnapshotReference &snapshot : configuration_journal_->snapshots) {
          RecordCleanupNoThrow("configuration_restore", CleanupTarget(snapshot.serial), false,
                               failure.what());
        }
        std::cerr << "Warning: Android HIL durable configuration recovery failed: "
                  << failure.what() << '\n';
      } catch (...) {
        for (const ConfigurationSnapshotReference &snapshot : configuration_journal_->snapshots) {
          RecordCleanupNoThrow("configuration_restore", CleanupTarget(snapshot.serial), false,
                               "unknown durable recovery failure");
        }
        std::cerr << "Warning: Android HIL durable configuration recovery failed with an unknown "
                     "exception\n";
      }
    }
    cleanup_evidence_.Finalize();
    PublishCleanupEvidenceNoThrow();
    if (recovery_lock_descriptor_ >= 0) {
      static_cast<void>(flock(recovery_lock_descriptor_, LOCK_UN));
      static_cast<void>(close(recovery_lock_descriptor_));
      recovery_lock_descriptor_ = -1;
    }
  }

 private:
  std::string CleanupTarget(std::string_view serial) const {
    if (serial == down_the_line_serial_) {
      return "down_the_line";
    }
    if (serial == face_on_serial_) {
      return "face_on";
    }
    throw std::logic_error("Android HIL cleanup target has an unknown serial");
  }

  void PublishCleanupEvidence() const {
    WriteArtifact(OutputDirectory() / "cleanup.json",
                  SerializeHilCleanupEvidence(cleanup_evidence_));
  }

  void PublishCleanupEvidenceNoThrow() const noexcept {
    try {
      PublishCleanupEvidence();
    } catch (const std::exception &failure) {
      std::cerr << "Warning: could not publish structured Android HIL cleanup evidence: "
                << failure.what() << '\n';
    }
  }

  void RecordCleanupNoThrow(std::string_view operation, std::string_view target, bool restored,
                            std::string diagnostic = {}) noexcept {
    try {
      if (restored) {
        cleanup_evidence_.RecordRestored(operation, target);
      } else {
        cleanup_evidence_.RecordFailed(operation, target, std::move(diagnostic));
      }
      PublishCleanupEvidence();
    } catch (const std::exception &failure) {
      std::cerr << "Warning: could not record structured Android HIL cleanup evidence: "
                << failure.what() << '\n';
    }
  }

  template <typename Action>
  void AttemptCleanupNoThrow(std::string_view operation, std::string_view target,
                             Action &&action) noexcept {
    try {
      std::forward<Action>(action)();
      RecordCleanupNoThrow(operation, target, true);
    } catch (const std::exception &failure) {
      RecordCleanupNoThrow(operation, target, false, failure.what());
      std::cerr << "Warning: Android HIL cleanup " << operation << " failed for " << target << ": "
                << failure.what() << '\n';
    } catch (...) {
      RecordCleanupNoThrow(operation, target, false, "unknown cleanup failure");
      std::cerr << "Warning: Android HIL cleanup " << operation << " failed for " << target
                << " with an unknown exception\n";
    }
  }

  static std::string FirstToken(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
      return {};
    }
    const std::size_t end = text.find_first_of(" \t\r\n", begin);
    return std::string(
        text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin));
  }

  void ExecuteRecoveryCommand(const ConfigurationRecoveryCommand &command) const {
    const CommandResult result =
        RunAdb(adb_, command.adb_arguments, std::chrono::steady_clock::now() + 5s);
    const bool accepted =
        !result.timed_out && std::ranges::find(command.accepted_exit_codes, result.exit_code) !=
                                 command.accepted_exit_codes.end();
    if (!accepted) {
      throw std::runtime_error("configuration recovery " + command.purpose + " failed for " +
                               command.role + " with adb exit " + std::to_string(result.exit_code));
    }
    if (command.expected_sha256.has_value() &&
        FirstToken(result.output) != command.expected_sha256.value()) {
      throw std::runtime_error("configuration recovery " + command.purpose +
                               " returned an unexpected digest for " + command.role);
    }
  }

  void RemoveRecoveryJournalDurably() const {
    std::error_code error;
    const bool removed = std::filesystem::remove(recovery_journal_path_, error);
    if (error || !removed) {
      throw std::runtime_error("cannot retire restored Android HIL recovery journal: " +
                               error.message());
    }
    const int directory_descriptor =
        open(recovery_directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_descriptor < 0) {
      throw std::runtime_error("cannot open Android HIL recovery directory for durable retirement");
    }
    const int sync_result = fsync(directory_descriptor);
    const int sync_error = errno;
    static_cast<void>(close(directory_descriptor));
    if (sync_result != 0) {
      errno = sync_error;
      throw std::runtime_error("cannot durably retire Android HIL recovery journal");
    }
  }

  void RestoreConfigurationJournal(ConfigurationRecoveryJournal journal) const {
    if (journal.state == ConfigurationRecoveryState::kPrepared) {
      for (const ConfigurationRecoveryCommand &command : BuildConfigurationRestorePlan(journal)) {
        ExecuteRecoveryCommand(command);
      }
      journal.state = ConfigurationRecoveryState::kRestored;
      WriteConfigurationRecoveryJournalAtomically(recovery_journal_path_, journal);
    }
    for (const ConfigurationRecoveryCommand &command :
         BuildConfigurationBackupRetirementPlan(journal)) {
      ExecuteRecoveryCommand(command);
    }
    RemoveRecoveryJournalDurably();
  }

  void RecoverPendingConfigurationJournal() const {
    std::error_code error;
    const bool exists = std::filesystem::exists(recovery_journal_path_, error);
    if (error) {
      throw std::runtime_error("cannot inspect durable Android HIL recovery journal: " +
                               error.message());
    }
    if (!exists) {
      return;
    }
    ConfigurationRecoveryJournal journal = ReadConfigurationRecoveryJournal(recovery_journal_path_);
    RestoreConfigurationJournal(journal);
    journal.state = ConfigurationRecoveryState::kRestored;
    WriteArtifact(
        OutputDirectory() / "preflight-recovered-configuration.json",
        swing_capture::android::dual_hil::SerializeConfigurationRecoveryJournal(journal).dump(2) +
            "\n");
  }

  ConfigurationSnapshotReference SnapshotNodeConfiguration(std::string_view serial,
                                                           std::string_view role) const {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    const CommandResult installed =
        RunAdb(adb_, DeviceArguments(serial, {"shell", "pm", "path", kPackageName}), deadline);
    if (installed.timed_out || installed.exit_code != 0) {
      throw std::runtime_error("cannot determine whether Android HIL package is installed");
    }
    if (!installed.output.contains("package:")) {
      return {
          .role = std::string(role),
          .serial = std::string(serial),
          .original_existed = false,
          .backup_relative_path = {},
          .sha256 = {},
          .byte_count = 0,
      };
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
      return {
          .role = std::string(role),
          .serial = std::string(serial),
          .original_existed = false,
          .backup_relative_path = {},
          .sha256 = {},
          .byte_count = 0,
      };
    }
    const std::string backup_directory = "files/hil_recovery/" + recovery_transaction_id_;
    const std::string backup_path = backup_directory + "/" + std::string(role) + ".xml";
    RunRequiredAdb(
        adb_,
        DeviceArguments(serial, {"shell", "run-as", kPackageName, "mkdir", "-p", backup_directory}),
        deadline);
    RunRequiredAdb(adb_,
                   DeviceArguments(serial, {"shell", "run-as", kPackageName, "chmod", "0700",
                                            backup_directory}),
                   deadline);
    RunRequiredAdb(adb_,
                   DeviceArguments(serial, {"shell", "run-as", kPackageName, "cp",
                                            "shared_prefs/node_configuration.xml", backup_path}),
                   deadline);
    RunRequiredAdb(
        adb_,
        DeviceArguments(serial, {"shell", "run-as", kPackageName, "chmod", "0600", backup_path}),
        deadline);
    RunRequiredAdb(adb_, DeviceArguments(serial, {"shell", "sync"}), deadline);
    const std::string digest = FirstToken(RunRequiredAdb(
        adb_,
        DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "sha256sum", backup_path}),
        deadline));
    const std::string bytes =
        FirstToken(RunRequiredAdb(adb_,
                                  DeviceArguments(serial, {"exec-out", "run-as", kPackageName,
                                                           "stat", "-c", "%s", backup_path}),
                                  deadline));
    std::uint64_t byte_count = 0;
    const auto [end, parse_error] =
        std::from_chars(bytes.data(), bytes.data() + bytes.size(), byte_count);
    if (digest.size() != 64U || parse_error != std::errc() || end != bytes.data() + bytes.size() ||
        byte_count == 0) {
      throw std::runtime_error("private Android HIL configuration backup metadata is invalid");
    }
    return {
        .role = std::string(role),
        .serial = std::string(serial),
        .original_existed = true,
        .backup_relative_path = backup_path,
        .sha256 = digest,
        .byte_count = byte_count,
    };
  }

  std::filesystem::path adb_;
  std::string down_the_line_serial_;
  std::string face_on_serial_;
  std::vector<std::pair<std::string, std::uint16_t>> forwards_;
  std::vector<std::pair<std::string, std::uint16_t>> reverses_;
  std::vector<std::pair<std::string, std::string>> private_file_cleanups_;
  std::filesystem::path recovery_directory_;
  std::filesystem::path recovery_journal_path_;
  std::string recovery_transaction_id_;
  std::optional<ConfigurationRecoveryJournal> configuration_journal_;
  HilCleanupEvidence cleanup_evidence_;
  bool configurations_restored_ = false;
  int recovery_lock_descriptor_ = -1;
};

class WifiEnableGuard {
 public:
  WifiEnableGuard(std::filesystem::path adb, std::string serial, std::string cleanup_target,
                  StopBothGuard *cleanup)
      : adb_(std::move(adb)),
        serial_(std::move(serial)),
        cleanup_target_(std::move(cleanup_target)),
        cleanup_(cleanup) {
    if (cleanup_ == nullptr) {
      throw std::invalid_argument("Wi-Fi cleanup evidence owner is required");
    }
  }

  WifiEnableGuard(const WifiEnableGuard &) = delete;
  WifiEnableGuard &operator=(const WifiEnableGuard &) = delete;

  ~WifiEnableGuard() {
    if (!disabled_) {
      return;
    }
    try {
      const CommandResult result =
          RunAdb(adb_, DeviceArguments(serial_, {"shell", "svc", "wifi", "enable"}),
                 std::chrono::steady_clock::now() + 3s);
      if (result.timed_out || result.exit_code != 0) {
        throw std::runtime_error("Wi-Fi enable command failed");
      }
    } catch (const std::exception &failure) {
      try {
        cleanup_->RecordExternalCleanupFailed("wifi_enable", cleanup_target_, failure.what());
      } catch (const std::exception &record_failure) {
        std::cerr << "Warning: could not record Wi-Fi restoration failure: "
                  << record_failure.what() << '\n';
      }
      std::cerr << "Warning: could not restore Android HIL Wi-Fi on " << cleanup_target_ << ": "
                << failure.what() << '\n';
      return;
    }
    disabled_ = false;
    try {
      cleanup_->RecordExternalCleanupRestored("wifi_enable", cleanup_target_);
    } catch (const std::exception &record_failure) {
      std::cerr << "Warning: could not record successful Wi-Fi restoration: "
                << record_failure.what() << '\n';
    }
  }

  void Disable(std::chrono::steady_clock::time_point deadline) {
    cleanup_->RegisterExternalCleanup("wifi_enable", cleanup_target_);
    disabled_ = true;
    RunRequiredAdb(adb_, DeviceArguments(serial_, {"shell", "svc", "wifi", "disable"}), deadline);
  }

  void Enable(std::chrono::steady_clock::time_point deadline) {
    try {
      RunRequiredAdb(adb_, DeviceArguments(serial_, {"shell", "svc", "wifi", "enable"}), deadline);
    } catch (const std::exception &failure) {
      try {
        cleanup_->RecordExternalCleanupFailed("wifi_enable", cleanup_target_, failure.what());
      } catch (const std::exception &record_failure) {
        std::cerr << "Warning: could not record checked Wi-Fi restoration failure: "
                  << record_failure.what() << '\n';
      }
      throw;
    }
    disabled_ = false;
    cleanup_->RecordExternalCleanupRestored("wifi_enable", cleanup_target_);
  }

 private:
  std::filesystem::path adb_;
  std::string serial_;
  std::string cleanup_target_;
  StopBothGuard *cleanup_;
  bool disabled_ = false;
};

void RegisterAutonomousStationCheckpointCleanup(StopBothGuard &cleanup,
                                                const std::filesystem::path &adb,
                                                std::string_view leader_serial,
                                                std::chrono::steady_clock::time_point deadline) {
  constexpr std::string_view checkpoint = "files/autonomous_pair/checkpoint.bin";
  const CommandResult existing = RunAdb(
      adb,
      DeviceArguments(leader_serial, {"shell", "run-as", kPackageName, "test", "-e", checkpoint}),
      deadline);
  if (existing.timed_out || (existing.exit_code != 0 && existing.exit_code != 1) ||
      !existing.output.empty()) {
    throw std::runtime_error("cannot safely inspect the autonomous recovery checkpoint");
  }
  if (existing.exit_code == 0) {
    throw std::runtime_error("autonomous recovery HIL refuses a pre-existing checkpoint");
  }
  cleanup.RegisterPrivateFileCleanup(std::string(leader_serial), std::string(checkpoint));
}

void RegisterAutonomousSessionCleanup(StopBothGuard &cleanup, std::string_view leader_serial,
                                      std::string_view shadow_serial,
                                      std::string_view shared_session_id) {
  if (!SafeSessionId(shared_session_id)) {
    throw std::invalid_argument("autonomous recovery cleanup session ID is unsafe");
  }
  const std::string session(shared_session_id);
  cleanup.RegisterPrivateFileCleanup(
      std::string(leader_serial), "files/autonomous_pair/replication_backlog/" + session + ".json");
  cleanup.RegisterPrivateFileCleanup(std::string(leader_serial),
                                     "files/coordination/" + session + ".json");
  cleanup.RegisterPrivateFileCleanup(std::string(shadow_serial),
                                     "files/coordination/" + session + ".json");
}

void RegisterAutonomousRecoveryCleanup(StopBothGuard &cleanup, const std::filesystem::path &adb,
                                       std::string_view leader_serial,
                                       std::string_view shadow_serial,
                                       std::string_view shared_session_id,
                                       std::chrono::steady_clock::time_point deadline) {
  RegisterAutonomousStationCheckpointCleanup(cleanup, adb, leader_serial, deadline);
  RegisterAutonomousSessionCleanup(cleanup, leader_serial, shadow_serial, shared_session_id);
}

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

struct PreparedPcmReplay {
  ExtractedPcmReplayCase replay_case;
  std::unique_ptr<swing_capture::hil::FeatherHilSerial> serial;
  std::unique_ptr<swing_capture::hil::FeatherHilController> controller;
  swing_capture::hil::FeatherPcmUploadReceipt upload;
  swing_capture::hil::FeatherCalibrationReceipt calibration;
  std::uint32_t brightness = 0;
};

PreparedPcmReplay PreparePcmReplay(const std::filesystem::path &evidence_directory) {
  const auto stage_started = std::chrono::steady_clock::now();
  const std::filesystem::path manifest_path =
      RequiredEnvironment("SWING_CAPTURE_PCM_REPLAY_MANIFEST");
  const std::filesystem::path wav_path = RequiredEnvironment("SWING_CAPTURE_PCM_REPLAY_WAV");
  const std::string case_name = RequiredEnvironment("SWING_CAPTURE_PCM_REPLAY_CASE");
  const std::vector<std::byte> manifest_bytes = ReadBoundedFile(manifest_path, 1024U * 1024U);
  const std::string manifest_text(reinterpret_cast<const char *>(manifest_bytes.data()),
                                  manifest_bytes.size());
  const auto manifest = ParsePcmReplayManifest(manifest_text);
  const std::vector<std::byte> wav =
      ReadBoundedFile(wav_path, swing_capture::kMaximumMonoPcmS16WavBytes);
  ExtractedPcmReplayCase replay_case =
      ExtractPcmReplayCase(manifest, FindPcmReplayCase(manifest, case_name), wav);

  const auto config_path = swing_capture::station::StationConfigPathFromEnvironment();
  if (!config_path.has_value()) {
    throw std::runtime_error("SWING_CAPTURE_STATION_CONFIG is required for dual Android HIL");
  }
  const auto station = swing_capture::station::LoadStationConfig(*config_path);
  auto serial = std::make_unique<swing_capture::hil::FeatherHilSerial>(station.feather_serial_path);
  auto controller = std::make_unique<swing_capture::hil::FeatherHilController>(*serial);
  const auto info = controller->QueryInfo();
  if (info.pcm_sample_rate_hz != replay_case.sample_rate_hz ||
      info.calibration_candidates.empty()) {
    throw std::runtime_error("Feather does not advertise the required PCM replay capability");
  }
  const auto upload = controller->UploadPcm16(replay_case.samples);
  if (upload.source_crc32 != replay_case.source_crc32 ||
      upload.committed_crc32 != replay_case.source_crc32 ||
      upload.sample_count != replay_case.samples.size()) {
    throw std::runtime_error("Feather PCM upload does not match the selected source window");
  }
  const auto calibration = controller->CalibrateSwingBrightness();
  if (!calibration.prepared || !calibration.rail_powered || !calibration.pixel_off ||
      !calibration.i2s_inactive || calibration.candidates != info.calibration_candidates) {
    throw std::runtime_error("Feather did not retain a prepared rail after PCM calibration");
  }
  if (ElapsedMilliseconds(stage_started) >
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count()) {
    throw std::runtime_error("PCM upload and fixture preparation exceeded 15 seconds");
  }
  const std::uint32_t brightness = calibration.candidates.back();
  Json transaction = {
      {"schema_version", 1},
      {"case_name", replay_case.definition.name},
      {"source_id", replay_case.source_id},
      {"source_start_frame", replay_case.definition.start_frame},
      {"source_sample_count", replay_case.definition.sample_count},
      {"marker_frame", replay_case.definition.marker_frame},
      {"gain_permille", replay_case.definition.gain_permille},
      {"source_crc32", replay_case.source_crc32},
      {"sample_rate_hz", replay_case.sample_rate_hz},
      {"upload",
       {{"request_id", upload.request_id},
        {"sample_count", upload.sample_count},
        {"byte_count", upload.byte_count},
        {"source_crc32", upload.source_crc32},
        {"committed_crc32", upload.committed_crc32}}},
      {"calibration", FeatherCalibrationJson(calibration)},
      {"selected_brightness", brightness},
      {"playback", nullptr},
  };
  WriteArtifact(evidence_directory / "feather-pcm-transaction.json", transaction.dump(2) + "\n");
  return {
      .replay_case = std::move(replay_case),
      .serial = std::move(serial),
      .controller = std::move(controller),
      .upload = upload,
      .calibration = calibration,
      .brightness = brightness,
  };
}

swing_capture::hil::FeatherPcmPlaybackReceipt PlayPreparedPcmReplay(
    PreparedPcmReplay *prepared, const std::filesystem::path &evidence_directory) {
  auto receipt = prepared->controller->PlayUploadedPcm(
      100ms, prepared->replay_case.definition.gain_permille, prepared->brightness,
      prepared->replay_case.definition.marker_frame);
  Json transaction = {
      {"schema_version", 1},
      {"case_name", prepared->replay_case.definition.name},
      {"source_id", prepared->replay_case.source_id},
      {"source_start_frame", prepared->replay_case.definition.start_frame},
      {"source_sample_count", prepared->replay_case.definition.sample_count},
      {"marker_frame", prepared->replay_case.definition.marker_frame},
      {"gain_permille", prepared->replay_case.definition.gain_permille},
      {"source_crc32", prepared->replay_case.source_crc32},
      {"sample_rate_hz", prepared->replay_case.sample_rate_hz},
      {"upload",
       {{"request_id", prepared->upload.request_id},
        {"sample_count", prepared->upload.sample_count},
        {"byte_count", prepared->upload.byte_count},
        {"source_crc32", prepared->upload.source_crc32},
        {"committed_crc32", prepared->upload.committed_crc32}}},
      {"calibration", FeatherCalibrationJson(prepared->calibration)},
      {"selected_brightness", prepared->brightness},
      {"playback", FeatherPcmPlaybackJson(receipt)},
  };
  WriteArtifact(evidence_directory / "feather-pcm-transaction.json", transaction.dump(2) + "\n");
  WriteArtifact(evidence_directory / "feather-pcm-playback.json",
                FeatherPcmPlaybackJson(receipt).dump(2) + "\n");

  std::vector<std::string> mismatches;
  auto require_equal = [&mismatches](std::string_view field, const auto &actual,
                                     const auto &expected) {
    if (actual != expected) {
      mismatches.push_back(std::string(field) + " actual=" + std::to_string(actual) +
                           " expected=" + std::to_string(expected));
    }
  };
  require_equal("source_crc32", receipt.source_crc32, prepared->replay_case.source_crc32);
  require_equal("played_crc32", receipt.played_crc32, receipt.expected_played_crc32);
  require_equal("sample_rate_hz", receipt.sample_rate_hz, prepared->replay_case.sample_rate_hz);
  require_equal("sample_count", receipt.sample_count, prepared->replay_case.samples.size());
  require_equal("marker_sample", receipt.marker_sample,
                prepared->replay_case.definition.marker_frame);
  require_equal("gain_permille", receipt.gain_permille,
                prepared->replay_case.definition.gain_permille);
  require_equal("brightness", receipt.brightness, prepared->brightness);
  if (receipt.prepare_source != "calibration") {
    mismatches.push_back("prepare_source actual=" + receipt.prepare_source +
                         " expected=calibration");
  }
  auto require_true = [&mismatches](std::string_view field, bool actual) {
    if (!actual) {
      mismatches.push_back(std::string(field) + " actual=false expected=true");
    }
  };
  auto require_false = [&mismatches](std::string_view field, bool actual) {
    if (actual) {
      mismatches.push_back(std::string(field) + " actual=true expected=false");
    }
  };
  require_true("outputs_inactive_at_completion", receipt.outputs_inactive_at_completion);
  require_true("pixel_off_at_completion", receipt.pixel_off_at_completion);
  require_true("i2s_inactive_at_completion", receipt.i2s_inactive_at_completion);
  require_false("rail_powered_at_completion", receipt.rail_powered_at_completion);
  require_false("prepared_at_completion", receipt.prepared_at_completion);
  if (!mismatches.empty()) {
    std::string diagnostic = "Feather PCM playback receipt mismatch:";
    for (const std::string &mismatch : mismatches) {
      diagnostic += " " + mismatch + ";";
    }
    throw std::runtime_error(diagnostic);
  }
  return receipt;
}

std::int64_t RecalibratePreparedPcmReplay(PreparedPcmReplay *prepared,
                                          const std::filesystem::path &evidence_directory) {
  const auto stage_started = std::chrono::steady_clock::now();
  prepared->calibration = prepared->controller->CalibrateSwingBrightness();
  if (!prepared->calibration.prepared || !prepared->calibration.rail_powered ||
      !prepared->calibration.pixel_off || !prepared->calibration.i2s_inactive ||
      prepared->calibration.candidates.empty()) {
    throw std::runtime_error("Feather did not refresh the prepared rail before PCM playback");
  }
  prepared->brightness = prepared->calibration.candidates.back();
  const std::int64_t elapsed_milliseconds = ElapsedMilliseconds(stage_started);
  if (elapsed_milliseconds >
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count()) {
    throw std::runtime_error("Feather PCM recalibration exceeded 15 seconds");
  }
  WriteArtifact(evidence_directory / "feather-pcm-recalibration.json",
                FeatherCalibrationJson(prepared->calibration).dump(2) + "\n");
  return elapsed_milliseconds;
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

void AnalyzeNodeMedia(CapturedNode *captured, const std::filesystem::path &node_output,
                      const ExternalTools &tools) {
  const std::filesystem::path media_path = node_output / (captured->evidence.role + ".mp4");
  RetainedMediaAnalysis analysis = swing_capture::android::dual_hil::AnalyzeRetainedMedia({
      .ffmpeg = tools.ffmpeg,
      .ffprobe = tools.ffprobe,
      .media_path = media_path,
      .node_output = node_output,
      .evidence = captured->evidence,
  });
  captured->maximum_media_time_residual_us = analysis.maximum_media_time_residual_us;
  captured->optical = std::move(analysis.optical);
  captured->april_tags = std::move(analysis.april_tags);
  captured->diagnostic_frame_indices = analysis.diagnostic_frame_indices;
  captured->ffprobe_stage_milliseconds = analysis.ffprobe_stage_milliseconds;
  captured->decode_stage_milliseconds = analysis.decode_stage_milliseconds;
  captured->diagnostic_stage_milliseconds = analysis.diagnostic_stage_milliseconds;
  captured->analysis_width = analysis.analysis_width;
  captured->analysis_height = analysis.analysis_height;
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

Json FeatherPcmPlaybackJson(const swing_capture::hil::FeatherPcmPlaybackReceipt &receipt) {
  return {
      {"request_id", receipt.request_id},
      {"accepted_device_us", receipt.accepted_device_microseconds},
      {"scheduled_audio_device_us", receipt.scheduled_audio_device_microseconds},
      {"audio_command_device_us", receipt.audio_command_device_microseconds},
      {"scheduled_marker_device_us", receipt.scheduled_marker_device_microseconds},
      {"marker_device_us", receipt.marker_device_microseconds},
      {"marker_offset_us", receipt.marker_offset_microseconds},
      {"command_delta_us", receipt.command_delta_microseconds},
      {"command_delta_error_us", receipt.command_delta_error_microseconds},
      {"sample_rate_hz", receipt.sample_rate_hz},
      {"sample_count", receipt.sample_count},
      {"marker_sample", receipt.marker_sample},
      {"gain_permille", receipt.gain_permille},
      {"brightness", receipt.brightness},
      {"source_crc32", receipt.source_crc32},
      {"expected_played_crc32", receipt.expected_played_crc32},
      {"played_crc32", receipt.played_crc32},
      {"white_us", receipt.white_microseconds},
      {"prepare_source", receipt.prepare_source},
      {"host_command_sent_ns", SteadyNanoseconds(receipt.host_command_sent)},
      {"host_acknowledgement_received_ns",
       SteadyNanoseconds(receipt.host_acknowledgement_received)},
      {"host_done_received_ns", SteadyNanoseconds(receipt.host_done_received)},
      {"outputs_inactive_at_completion", receipt.outputs_inactive_at_completion},
      {"pixel_off_at_completion", receipt.pixel_off_at_completion},
      {"i2s_inactive_at_completion", receipt.i2s_inactive_at_completion},
      {"rail_powered_at_completion", receipt.rail_powered_at_completion},
      {"prepared_at_completion", receipt.prepared_at_completion},
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

Json UncalibratedTimingClaimJson() {
  return {
      {"scope", "fixture_optical_marker_to_audio_trigger"},
      {"qualification", "operational_correlation_only"},
      {"review_marker_semantics", "audio_trigger_estimate"},
      {"absolute_ball_impact_calibrated", false},
      {"unmeasured_latency_components",
       Json::array({"camera_exposure_timestamp_semantics", "microphone_input_path_latency",
                    "speaker_amplifier_and_fixture_acoustic_latency",
                    "field_ball_to_microphone_acoustic_latency"})},
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

TimingCorrelationEvidence PcmMarkerCorrelation(const CapturedNode &captured) {
  // The recorded-field marker remains the labeled physical strike, while the production detector
  // deliberately timestamps the strongest confirmed microphone peak. Keep the 20 ms point-offset
  // gate in RgbSwingSequenceAnalyzer, but allow one additional 240 fps frame when proving the
  // conservative interval after camera phase and timestamp uncertainty are composed.
  constexpr std::int64_t kPcmConservativeAcceptanceLimitUs = 25000;
  return EvaluateTimingCorrelation({
      .acceptance_limit_us = kPcmConservativeAcceptanceLimitUs,
      .optical_onset_lower_bound_us = captured.optical.optical_onset_lower_bound_us,
      .optical_onset_upper_bound_us = captured.optical.optical_onset_upper_bound_us,
      .audio_trigger_uncertainty_ns = captured.evidence.trigger_timestamp_uncertainty_ns,
      .media_pts_residual_us = captured.maximum_media_time_residual_us,
  });
}

void ValidateCompletedPcmNode(const CapturedNode &captured) {
  constexpr auto kDeadlineMilliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count();
  if (!captured.pcm_playback.has_value() ||
      captured.capture_stage_milliseconds > kDeadlineMilliseconds ||
      captured.artifact_pull_stage_milliseconds > kDeadlineMilliseconds ||
      captured.ffprobe_stage_milliseconds > kDeadlineMilliseconds ||
      captured.decode_stage_milliseconds > kDeadlineMilliseconds ||
      captured.diagnostic_stage_milliseconds > kDeadlineMilliseconds ||
      captured.audio_stage_milliseconds > kDeadlineMilliseconds) {
    throw std::runtime_error("paired PCM HIL evidence is missing or exceeded 15 seconds");
  }
  if (captured.optical.decoded_frame_count != captured.evidence.frame_count ||
      !captured.optical.detected) {
    throw std::runtime_error("paired PCM HIL video lacks its complete white-marker response");
  }
  const TimingCorrelationEvidence timing = PcmMarkerCorrelation(captured);
  if (!timing.passed) {
    throw std::runtime_error("paired PCM white marker and production audio trigger are misaligned");
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
  const AndroidApkInstallEvidence apk_install = InstallConfiguredApk(adb, apk, serial, deadline);
  WriteArtifact(OutputDirectory() / std::string(role) / "apk-install.json",
                ApkInstallJson(apk_install).dump(2) + "\n");
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
  AndroidApkInstallEvidence apk_install;
  std::int64_t pre_install_stage_milliseconds = 0;
  std::int64_t configuration_stage_milliseconds = 0;
  std::int64_t api_ready_stage_milliseconds = 0;
  std::int64_t setup_stage_milliseconds = 0;
  std::int64_t arm_dispatch_milliseconds = 0;
};

Json NodeSetupTimingJson(const ConcurrentNode &node);

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
      .apk_install = {},
      .pre_install_stage_milliseconds = 0,
      .configuration_stage_milliseconds = 0,
      .api_ready_stage_milliseconds = 0,
      .setup_stage_milliseconds = 0,
      .arm_dispatch_milliseconds = 0,
  };
  StopPackage(adb, node.serial);
  RunRequiredAdb(adb,
                 DeviceArguments(node.serial, {"shell", "input", "keyevent", "KEYCODE_WAKEUP"}),
                 deadline);
  node.pre_install_stage_milliseconds = ElapsedMilliseconds(stage_started);
  node.apk_install = InstallConfiguredApk(adb, apk, node.serial, deadline);
  WriteArtifact(OutputDirectory() / node.role / "apk-install.json",
                ApkInstallJson(node.apk_install).dump(2) + "\n");
  const auto configuration_started = std::chrono::steady_clock::now();
  RunRequiredAdb(
      adb, DeviceArguments(node.serial, {"shell", "run-as", kPackageName, "rm", "-f", kReportPath}),
      deadline);
  GrantCapturePermissions(adb, node.serial, deadline);
  RunRequiredAdb(adb, ConfigureArguments(node.serial, node.role, node.profile, enable_pose_arm_hil),
                 deadline);
  node.host_port = EstablishForward(adb, node.serial, deadline);
  cleanup->RegisterForward(node.serial, node.host_port);
  node.control_token = ReadControlToken(adb, node.serial, deadline);
  node.configuration_stage_milliseconds = ElapsedMilliseconds(configuration_started);

  const auto api_ready_started = std::chrono::steady_clock::now();
  std::string latest_error = "node HTTP API did not start";
  std::string current_check = "GET /api/v1/node";
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      current_check = "GET /api/v1/node";
      const HttpResponse descriptor = RequireNodeHttp({.port = node.host_port,
                                                       .method = "GET",
                                                       .path = "/api/v1/node",
                                                       .bearer_token = node.control_token,
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
      WriteArtifact(
          OutputDirectory() / node.role /
              (enable_pose_arm_hil ? "node-descriptor-initial.json" : "node-descriptor.json"),
          descriptor.body);
      WriteArtifact(OutputDirectory() / node.role / "hosted-root.html", hosted_root.body);
      node.api_ready_stage_milliseconds = ElapsedMilliseconds(api_ready_started);
      node.setup_stage_milliseconds = ElapsedMilliseconds(stage_started);
      WriteArtifact(OutputDirectory() / node.role / "setup-timing.json",
                    NodeSetupTimingJson(node).dump(2) + "\n");
      return node;
    } catch (const std::exception &failure) {
      latest_error = current_check + ": " + failure.what();
      std::this_thread::sleep_for(kPollInterval);
    }
  }
  throw std::runtime_error("concurrent Android HIL node setup exceeded 15 seconds: " +
                           latest_error);
}

Json NodeSetupTimingJson(const ConcurrentNode &node) {
  return {
      {"total", StageJson(node.setup_stage_milliseconds)},
      {"stop_and_wake", StageJson(node.pre_install_stage_milliseconds)},
      {"apk_install", ApkInstallJson(node.apk_install)},
      {"configure_and_forward", StageJson(node.configuration_stage_milliseconds)},
      {"api_ready", StageJson(node.api_ready_stage_milliseconds)},
  };
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

void PrepareUnpairedDiscoveryFixture(const ConcurrentNode &node,
                                     std::chrono::steady_clock::time_point deadline) {
  const auto read_setup = [&]() {
    return RequireNodeHttp({.port = node.host_port,
                            .method = "GET",
                            .path = "/api/v1/setup",
                            .bearer_token = node.control_token,
                            .body = {},
                            .deadline = deadline},
                           200);
  };

  HttpResponse current = read_setup();
  WriteArtifact(OutputDirectory() / node.role / "discovery-field-setup.json", current.body);
  auto inspection = InspectDiscoveryPairingFixture(current.body);
  if (inspection.pose_mode != "disabled" || inspection.peer_configured ||
      inspection.pairing_state == DurablePairingState::kActive) {
    ConfigurePoseMode(node, "disabled", std::nullopt, std::nullopt, deadline);
    current = read_setup();
    inspection = InspectDiscoveryPairingFixture(current.body);
  }
  if (inspection.pairing_state == DurablePairingState::kActive) {
    throw std::runtime_error("explicit discovery fixture reset left an active peer binding");
  }
  if (inspection.pairing_state == DurablePairingState::kRevoked) {
    const Json reset_request = {
        {"schema_version", 1},
        {"expected_revision", Json::parse(current.body).at("revision")},
        {"peer_node_id", inspection.peer_node_id},
    };
    current = RequireNodeHttp({.port = node.host_port,
                               .method = "POST",
                               .path = "/api/v1/pairing/reset",
                               .bearer_token = node.control_token,
                               .body = reset_request.dump(),
                               .deadline = deadline},
                              200);
    inspection = InspectDiscoveryPairingFixture(current.body);
  }
  if (!inspection.IsUnpaired()) {
    throw std::runtime_error("discovery fixture did not reach an explicit unpaired baseline");
  }
  WriteArtifact(OutputDirectory() / node.role / "discovery-unpaired-setup.json", current.body);
}

void ConfigurePeerPoseModeWithRetries(const ConcurrentNode &node, std::string_view peer_origin,
                                      std::string_view peer_token,
                                      std::chrono::steady_clock::time_point deadline) {
  std::string latest_error = "peer setup has not been attempted";
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      ConfigurePoseMode(node, "leader", peer_origin, peer_token, deadline);
      return;
    } catch (const std::exception &failure) {
      latest_error = failure.what();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("paired pose leader setup did not reach its authenticated peer: " +
                           latest_error);
}

void PreservePoseConfiguredNodeDescriptor(const ConcurrentNode &node, std::string_view mode,
                                          bool peer_configured,
                                          std::chrono::steady_clock::time_point deadline) {
  const HttpResponse descriptor = RequireNodeHttp({.port = node.host_port,
                                                   .method = "GET",
                                                   .path = "/api/v1/node",
                                                   .bearer_token = node.control_token,
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

Json ValidateLanNodeEndpoint(const ConcurrentNode &node, const LanOrigin &lan_origin,
                             std::string_view expected_device_model,
                             std::string_view expected_pose_mode,
                             std::string_view expected_peer_origin,
                             std::chrono::steady_clock::time_point deadline) {
  const auto direct_request = [&](std::string_view method, std::string_view path,
                                  std::string_view bearer_token) {
    return RequestNodeHttp({.host = lan_origin.ipv4_address,
                            .port = lan_origin.port,
                            .method = method,
                            .path = path,
                            .bearer_token = bearer_token,
                            .body = {},
                            .deadline = deadline});
  };
  const HttpResponse unauthenticated_descriptor = direct_request("GET", "/api/v1/node", {});
  const HttpResponse descriptor = direct_request("GET", "/api/v1/node", node.control_token);
  const HttpResponse unauthenticated_setup = direct_request("GET", "/api/v1/setup", {});
  const HttpResponse authenticated_setup =
      direct_request("GET", "/api/v1/setup", node.control_token);
  const HttpResponse unauthenticated_status = direct_request("GET", "/api/v1/capture/status", {});
  const HttpResponse status = direct_request("GET", "/api/v1/capture/status", node.control_token);
  const HttpResponse clock = direct_request("GET", "/api/v1/clock", {});
  if (descriptor.status != 200 || authenticated_setup.status != 200 || status.status != 200 ||
      clock.status != 200) {
    throw std::runtime_error("direct Wi-Fi Android node endpoint did not serve its LAN API");
  }
  const auto descriptor_authentication_challenge =
      unauthenticated_descriptor.headers.find("www-authenticate");
  if (unauthenticated_descriptor.status != 401 ||
      descriptor_authentication_challenge == unauthenticated_descriptor.headers.end() ||
      descriptor_authentication_challenge->second != "Bearer") {
    throw std::runtime_error(
        "direct Wi-Fi node descriptor did not enforce general-read bearer authentication");
  }
  const auto authentication_challenge = unauthenticated_setup.headers.find("www-authenticate");
  if (unauthenticated_setup.status != 401 ||
      authentication_challenge == unauthenticated_setup.headers.end() ||
      authentication_challenge->second != "Bearer") {
    throw std::runtime_error("direct Wi-Fi setup endpoint did not enforce bearer authentication");
  }
  const auto status_authentication_challenge =
      unauthenticated_status.headers.find("www-authenticate");
  if (unauthenticated_status.status != 401 ||
      status_authentication_challenge == unauthenticated_status.headers.end() ||
      status_authentication_challenge->second != "Bearer") {
    throw std::runtime_error(
        "direct Wi-Fi capture status did not enforce general-read bearer authentication");
  }
  ValidateLanEndpoint({.descriptor_json = descriptor.body,
                       .setup_json = authenticated_setup.body,
                       .status_json = status.body,
                       .clock_json = clock.body,
                       .identity = node.identity,
                       .expected_origin = lan_origin.origin,
                       .expected_device_model = expected_device_model,
                       .expected_pose_mode = expected_pose_mode,
                       .expected_peer_origin = expected_peer_origin,
                       .unauthenticated_setup_status = unauthenticated_setup.status,
                       .authenticated_setup_status = authenticated_setup.status});
  if (unauthenticated_setup.body.contains(node.control_token) ||
      authenticated_setup.body.contains(node.control_token) ||
      descriptor.body.contains(node.control_token) || status.body.contains(node.control_token) ||
      clock.body.contains(node.control_token)) {
    throw std::runtime_error("direct Wi-Fi Android node evidence exposed a bearer token");
  }
  WriteArtifact(OutputDirectory() / node.role / "lan-node-descriptor.json", descriptor.body);
  WriteArtifact(OutputDirectory() / node.role / "lan-setup-authenticated.json",
                authenticated_setup.body);
  WriteArtifact(OutputDirectory() / node.role / "lan-setup-unauthenticated.json",
                unauthenticated_setup.body);
  WriteArtifact(OutputDirectory() / node.role / "lan-capture-status.json", status.body);
  WriteArtifact(OutputDirectory() / node.role / "lan-clock.json", clock.body);
  return {
      {"origin", lan_origin.origin},
      {"host_direct_request", true},
      {"descriptor_schema_version", Json::parse(descriptor.body).at("schema_version")},
      {"status_schema_version", Json::parse(status.body).at("schema_version")},
      {"clock_schema_version", Json::parse(clock.body).at("schema_version")},
      {"unauthenticated_descriptor_status", unauthenticated_descriptor.status},
      {"unauthenticated_setup_status", unauthenticated_setup.status},
      {"authenticated_setup_status", authenticated_setup.status},
      {"unauthenticated_capture_status", unauthenticated_status.status},
      {"authenticated_capture_status", status.status},
      {"node_id", node.identity.node_id},
      {"role", node.role},
      {"device_model", expected_device_model},
      {"pose_mode", expected_pose_mode},
      {"peer_origin", expected_peer_origin.empty() ? Json(nullptr) : Json(expected_peer_origin)},
      {"setup_readiness_issues", Json::array()},
      {"control_authentication", "bearer"},
      {"secret_material_preserved", false},
  };
}

std::set<std::string, std::less<>> ReadyCaptureSessionIds(
    const ConcurrentNode &node, std::chrono::steady_clock::time_point deadline) {
  const HttpResponse response = RequireNodeHttp({.port = node.host_port,
                                                 .method = "GET",
                                                 .path = "/api/v1/sessions",
                                                 .bearer_token = node.control_token,
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

struct PairNetworkHealthAcceptance {
  PairNetworkHealthArmDecision decision = PairNetworkHealthArmDecision::kNotReady;
  Json evidence;
};

PairNetworkHealthAcceptance WaitForPairNetworkHealth(
    const ConcurrentNode &leader, std::string_view expected_peer_origin,
    std::string_view expected_peer_node_id, std::chrono::steady_clock::time_point deadline) {
  const auto started = std::chrono::steady_clock::now();
  std::size_t poll_count = 0;
  std::string latest_state = "unavailable";
  while (std::chrono::steady_clock::now() < deadline) {
    const HttpResponse response = RequireNodeHttp({.port = leader.host_port,
                                                   .method = "GET",
                                                   .path = "/api/v1/capture/status",
                                                   .bearer_token = leader.control_token,
                                                   .body = {},
                                                   .deadline = deadline},
                                                  200);
    ++poll_count;
    WriteArtifact(OutputDirectory() / leader.role / "pair-network-health-status-latest.json",
                  response.body);
    const PairNetworkHealthArmDecision decision =
        InspectPairNetworkHealthForArm({.status_json = response.body,
                                        .expected_peer_origin = expected_peer_origin,
                                        .expected_peer_node_id = expected_peer_node_id});
    const Json status = Json::parse(response.body);
    const Json &snapshot = status.at("pair_network_health");
    latest_state = snapshot.value("state", "invalid");
    if (decision == PairNetworkHealthArmDecision::kGood ||
        decision == PairNetworkHealthArmDecision::kDegraded) {
      Json evidence = {
          {"schema_version", 1},
          {"passed", true},
          {"poll_count", static_cast<std::int64_t>(poll_count)},
          {"elapsed_milliseconds", ElapsedMilliseconds(started)},
          {"accepted_state", decision == PairNetworkHealthArmDecision::kGood ? "good" : "degraded"},
          {"degraded_override", decision == PairNetworkHealthArmDecision::kDegraded},
          {"expected_peer_origin", expected_peer_origin},
          {"expected_peer_node_id", expected_peer_node_id},
          {"snapshot", snapshot},
      };
      WriteArtifact(OutputDirectory() / leader.role / "pair-network-health-accepted.json",
                    evidence.dump(2) + "\n");
      return {.decision = decision, .evidence = std::move(evidence)};
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error(
      "paired pose leader network health did not become usable within 15 "
      "seconds (latest state=" +
      latest_state + ")");
}

void ArmPoseStandby(const ConcurrentNode &node, std::string_view request_body,
                    std::chrono::steady_clock::time_point deadline) {
  static_cast<void>(RequireNodeHttp({.port = node.host_port,
                                     .method = "POST",
                                     .path = "/api/v1/capture/arm",
                                     .bearer_token = node.control_token,
                                     .body = request_body,
                                     .deadline = deadline},
                                    202));
}

void DisarmPoseStandby(const ConcurrentNode &node, std::chrono::steady_clock::time_point deadline) {
  const Json request = {{"armed", false}};
  const HttpResponse response = RequireNodeHttp({.port = node.host_port,
                                                 .method = "POST",
                                                 .path = "/api/v1/capture/arm",
                                                 .bearer_token = node.control_token,
                                                 .body = request.dump(),
                                                 .deadline = deadline},
                                                202);
  Json status = Json::parse(response.body);
  if (status.value("schema_version", 0) != 2) {
    throw std::runtime_error("paired pose node returned an invalid disarm acknowledgement");
  }
  while (status.value("armed", true) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(kPollInterval);
    const HttpResponse latest = RequireNodeHttp({.port = node.host_port,
                                                 .method = "GET",
                                                 .path = "/api/v1/capture/status",
                                                 .bearer_token = node.control_token,
                                                 .body = {},
                                                 .deadline = deadline},
                                                200);
    status = Json::parse(latest.body);
    WriteArtifact(OutputDirectory() / node.role / "pose-status-disarmed-latest.json", latest.body);
  }
  if (status.value("schema_version", 0) != 2 || status.value("armed", true)) {
    throw std::runtime_error("paired pose node did not complete a safe disarm");
  }
}

Json ShortPoseLatencyJson(std::string_view status) {
  const auto evidence = ValidateShortPoseLatency(status);
  return {
      {"passed", true},
      {"actual_delegate", evidence.actual_delegate},
      {"successful_inferences", evidence.successful_inferences},
      {"inference_duration_p95_ns", evidence.inference_duration_p95_ns},
      {"maximum_inference_duration_ns", evidence.maximum_inference_duration_ns},
      {"inference_deadline_misses", evidence.inference_deadline_misses},
      {"inference_outliers", evidence.inference_outliers},
      {"decision_age_samples", evidence.decision_age_samples},
      {"rejected_decision_timestamps", evidence.rejected_decision_timestamps},
      {"decision_age_p95_ns", evidence.decision_age_p95_ns},
      {"maximum_decision_age_ns", evidence.maximum_decision_age_ns},
      {"offered_images", evidence.offered_images},
      {"scheduled_images", evidence.scheduled_images},
      {"dropped_images", evidence.dropped_images},
      {"maximum_warmup_duration_ns", evidence.maximum_warmup_duration_ns},
  };
}

Json WaitForPairedPosePhase(const ConcurrentNode &leader, const ConcurrentNode &shadow,
                            std::string_view phase, std::string_view expected_shared_session_id,
                            std::chrono::steady_clock::time_point deadline) {
  if (phase != "monitoring" && phase != "high_speed") {
    throw std::invalid_argument("paired pose HIL phase does not have an evidence contract");
  }
  const std::string latest_filename = "pose-status-" + std::string(phase) + "-latest.json";
  const std::string accepted_filename = "pose-status-" + std::string(phase) + ".json";
  std::optional<std::chrono::steady_clock::time_point> stable_since;
  std::string last_not_ready_reason = "status has not reached the requested phase";
  const auto read_status = [&](const ConcurrentNode &node) {
    try {
      return RequireNodeHttp({.port = node.host_port,
                              .method = "GET",
                              .path = "/api/v1/capture/status",
                              .bearer_token = node.control_token,
                              .body = {},
                              .deadline = deadline},
                             200);
    } catch (const std::runtime_error &failure) {
      throw std::runtime_error("paired pose status transport failed while waiting for " +
                               std::string(phase) + ": " + failure.what() +
                               "; last validation: " + last_not_ready_reason);
    }
  };
  while (std::chrono::steady_clock::now() < deadline) {
    const HttpResponse leader_response = read_status(leader);
    const HttpResponse shadow_response = read_status(shadow);
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
    if (ready) {
      try {
        ValidatePairedPoseSessionState(PairedPoseSessionStateInspection{
            .leader_status_json = leader_response.body,
            .shadow_status_json = shadow_response.body,
            .expected_phase = phase,
            .expected_shared_session_id = expected_shared_session_id});
        ValidatePairedPoseClockStatus({.leader_status_json = leader_response.body,
                                       .shadow_status_json = shadow_response.body,
                                       .leader_identity = leader.identity,
                                       .shadow_identity = shadow.identity});
        if (phase == "monitoring") {
          static_cast<void>(ValidateShortPoseLatency(leader_response.body));
          static_cast<void>(ValidateShortPoseLatency(shadow_response.body));
        }
      } catch (const std::runtime_error &failure) {
        last_not_ready_reason = failure.what();
        ready = false;
      }
    }
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
        return phase == "monitoring"
                   ? Json{{"face_on", ShortPoseLatencyJson(leader_response.body)},
                          {"down_the_line", ShortPoseLatencyJson(shadow_response.body)}}
                   : Json(nullptr);
      }
    } else {
      stable_since.reset();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("paired pose HIL nodes did not reach phase " + std::string(phase) +
                           ": " + last_not_ready_reason);
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

std::array<std::string, 2> ReadAndPreservePostStimulusStatus(
    const ConcurrentNode &leader, const ConcurrentNode &shadow,
    std::chrono::steady_clock::time_point deadline, std::string_view suffix) {
  const HttpResponse leader_response = RequireNodeHttp({.port = leader.host_port,
                                                        .method = "GET",
                                                        .path = "/api/v1/capture/status",
                                                        .bearer_token = leader.control_token,
                                                        .body = {},
                                                        .deadline = deadline},
                                                       200);
  const HttpResponse shadow_response = RequireNodeHttp({.port = shadow.host_port,
                                                        .method = "GET",
                                                        .path = "/api/v1/capture/status",
                                                        .bearer_token = shadow.control_token,
                                                        .body = {},
                                                        .deadline = deadline},
                                                       200);
  WriteArtifact(OutputDirectory() / leader.role /
                    ("pose-status-post-stimulus-" + std::string(suffix) + ".json"),
                leader_response.body);
  WriteArtifact(OutputDirectory() / shadow.role /
                    ("pose-status-post-stimulus-" + std::string(suffix) + ".json"),
                shadow_response.body);
  return {leader_response.body, shadow_response.body};
}

struct PcmAutomaticTriggerObservation {
  PcmReplayPairOutcome outcome = PcmReplayPairOutcome::kNeither;
  std::int64_t elapsed_milliseconds = 0;
  std::string termination_reason;
};

Json PcmAutomaticTriggerObservationJson(const PcmAutomaticTriggerObservation &observation) {
  return {
      {"outcome", PcmReplayPairOutcomeName(observation.outcome)},
      {"elapsed_ms", observation.elapsed_milliseconds},
      {"termination_reason", observation.termination_reason},
      {"initial_observation_ms", kPcmInitialTriggerObservation.count()},
      {"peer_delivery_quiescence_ms", kPcmPeerImpactQuiescence.count()},
  };
}

PcmAutomaticTriggerObservation ObserveAutomaticPcmTrigger(
    const ConcurrentNode &leader, const ConcurrentNode &shadow,
    std::chrono::steady_clock::time_point stage_deadline) {
  const auto started = std::chrono::steady_clock::now();
  const auto neither_deadline = started + kPcmInitialTriggerObservation;
  if (neither_deadline >= stage_deadline) {
    throw std::runtime_error("PCM trigger stage has insufficient initial observation time");
  }
  std::array<std::string, 2> latest;
  std::optional<PcmReplayPairOutcome> stable_partial_outcome;
  std::optional<std::chrono::steady_clock::time_point> partial_observed_at;
  while (std::chrono::steady_clock::now() < stage_deadline) {
    latest = ReadAndPreservePostStimulusStatus(leader, shadow, stage_deadline, "latest");
    const Json leader_status = Json::parse(latest[0]);
    const Json shadow_status = Json::parse(latest[1]);
    if (leader_status.value("state", "") == "error" ||
        shadow_status.value("state", "") == "error") {
      throw std::runtime_error("paired pose HIL node entered ERROR after PCM playback");
    }
    const PcmReplayPairOutcome outcome = ClassifyPcmReplayPairOutcome(
        !leader_status.at("last_trigger_elapsed_realtime_ns").is_null(),
        !shadow_status.at("last_trigger_elapsed_realtime_ns").is_null());
    const auto now = std::chrono::steady_clock::now();
    if (HasPairedAutomaticImpactEvidence(latest[0], latest[1])) {
      WriteArtifact(OutputDirectory() / leader.role / "pose-status-post-stimulus.json", latest[0]);
      WriteArtifact(OutputDirectory() / shadow.role / "pose-status-post-stimulus.json", latest[1]);
      return {.outcome = PcmReplayPairOutcome::kBoth,
              .elapsed_milliseconds = ElapsedMilliseconds(started),
              .termination_reason = "paired_terminal_evidence"};
    }
    if (outcome == PcmReplayPairOutcome::kBoth) {
      throw std::runtime_error(
          "both phones triggered but mapped peer-impact evidence is incomplete");
    }
    if (outcome == PcmReplayPairOutcome::kNeither) {
      stable_partial_outcome.reset();
      partial_observed_at.reset();
    } else if (!stable_partial_outcome.has_value() || *stable_partial_outcome != outcome) {
      stable_partial_outcome = outcome;
      partial_observed_at = now;
      if (now + kPcmPeerImpactQuiescence >= stage_deadline) {
        throw std::runtime_error(
            "PCM trigger became partial too late to prove peer-impact delivery quiescence");
      }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started);
    const std::optional<std::chrono::milliseconds> stable_partial_elapsed =
        partial_observed_at.has_value()
            ? std::optional(
                  std::chrono::duration_cast<std::chrono::milliseconds>(now - *partial_observed_at))
            : std::nullopt;
    const PcmReplayObservationDecision decision = ClassifyPcmReplayObservationDecision(
        outcome, elapsed, stable_partial_elapsed, kPcmInitialTriggerObservation,
        kPcmPeerImpactQuiescence);
    if (decision == PcmReplayObservationDecision::kNeitherQuiescent) {
      WriteArtifact(OutputDirectory() / leader.role / "pose-status-post-stimulus.json", latest[0]);
      WriteArtifact(OutputDirectory() / shadow.role / "pose-status-post-stimulus.json", latest[1]);
      return {.outcome = outcome,
              .elapsed_milliseconds = ElapsedMilliseconds(started),
              .termination_reason = "neither_quiescent"};
    }
    if (decision == PcmReplayObservationDecision::kPartialQuiescent) {
      WriteArtifact(OutputDirectory() / leader.role / "pose-status-post-stimulus.json", latest[0]);
      WriteArtifact(OutputDirectory() / shadow.role / "pose-status-post-stimulus.json", latest[1]);
      return {.outcome = outcome,
              .elapsed_milliseconds = ElapsedMilliseconds(started),
              .termination_reason = "partial_pair_quiescent"};
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("PCM trigger observation exhausted its 15-second stage deadline");
}

std::string RequestPcmFailureSalvage(const ConcurrentNode &node,
                                     std::chrono::steady_clock::time_point deadline) {
  const HttpResponse response = RequireNodeHttp({.port = node.host_port,
                                                 .method = "POST",
                                                 .path = "/api/v1/capture/missed-shot",
                                                 .bearer_token = node.control_token,
                                                 .body = {},
                                                 .deadline = deadline},
                                                202);
  const Json summary = Json::parse(response.body);
  const std::string session_id = summary.value("session_id", "");
  if (!SafeSessionId(session_id) || summary.value("session_kind", "") != "capture") {
    throw std::runtime_error("PCM failure salvage did not freeze a high-speed capture ring");
  }
  WriteArtifact(OutputDirectory() / node.role / "pcm-failure-salvage-request.json", response.body);
  return session_id;
}

void WaitForReadyPoseSessions(const ConcurrentNode &leader, const ConcurrentNode &shadow,
                              std::string_view leader_session_id,
                              std::string_view shadow_session_id,
                              std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const auto leader_sessions = ReadyCaptureSessionIds(leader, deadline);
    const auto shadow_sessions = ReadyCaptureSessionIds(shadow, deadline);
    if (leader_sessions.contains(std::string(leader_session_id)) &&
        shadow_sessions.contains(std::string(shadow_session_id))) {
      return;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("PCM failure salvage sessions did not publish before the deadline");
}

Json ReadMappedPeerImpactEvidence(const ConcurrentNode &leader, const ConcurrentNode &shadow,
                                  std::string_view expected_shared_session_id,
                                  std::chrono::steady_clock::time_point deadline) {
  const HttpResponse shadow_response = RequireNodeHttp({.port = shadow.host_port,
                                                        .method = "GET",
                                                        .path = "/api/v1/capture/status",
                                                        .bearer_token = shadow.control_token,
                                                        .body = {},
                                                        .deadline = deadline},
                                                       200);
  ValidateMappedPeerImpactStatus({.shadow_status_json = shadow_response.body,
                                  .leader_identity = leader.identity,
                                  .shadow_identity = shadow.identity,
                                  .expected_shared_session_id = expected_shared_session_id});
  WriteArtifact(OutputDirectory() / shadow.role / "pose-status-triggered.json",
                shadow_response.body);
  return Json::parse(shadow_response.body).at("pose").at("peer_impact_mapping");
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
  const Json &android_capture = manifest.at("android_capture");
  Json audio = android_capture.contains("audio_evidence")
                   ? android_capture.at("audio_evidence")
                   : android_capture.at("diagnostic_evidence").at("audio");
  const std::string audio_relative_path = audio.value("path", "");
  if (audio_relative_path != "audio_evidence.wav" &&
      audio_relative_path != "diagnostic_audio.wav") {
    throw std::runtime_error("paired pose HIL manifest lacks bounded retained audio evidence");
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
                                                      .bearer_token = down_the_line.control_token,
                                                      .body = {},
                                                      .deadline = deadline},
                                                     200);
    const HttpResponse face_status = RequireNodeHttp({.port = face_on.host_port,
                                                      .method = "GET",
                                                      .path = "/api/v1/capture/status",
                                                      .bearer_token = face_on.control_token,
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
                                std::int64_t capture_stage_milliseconds,
                                std::string_view expected_trigger_source = "local_audio") {
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
      .expected_trigger_source = expected_trigger_source,
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

CapturedNode PullConcurrentPcmNode(
    const std::filesystem::path &adb, const ConcurrentNode &node,
    std::string_view shared_session_id, std::string report_text,
    const swing_capture::hil::FeatherPcmPlaybackReceipt &playback,
    std::int64_t capture_stage_milliseconds, std::string_view expected_trigger_source,
    std::string_view expected_audio_filename = "audio_evidence.wav") {
  const auto deadline = std::chrono::steady_clock::now() + kStageDeadline;
  const Json report = Json::parse(report_text);
  const Json &retained = report.at("retained_session");
  const std::string local_session_id = retained.value("session_id", "");
  const std::string prefix = "sessions/" + local_session_id + "/";
  const std::string manifest_path = retained.value("manifest", "");
  const std::string media_path = retained.value("media", "");
  const std::string audio_path = retained.at("audio_evidence").value("path", "");
  if (!SafeSessionId(local_session_id) || manifest_path != prefix + "manifest.json" ||
      media_path != prefix + node.role + ".mp4" ||
      audio_path != prefix + std::string(expected_audio_filename)) {
    throw std::runtime_error("paired PCM HIL report contains unsafe retained paths");
  }
  CapturedNode captured;
  captured.pcm_playback = playback;
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
  WriteArtifact(output / expected_audio_filename, captured.audio_wav);
  captured.evidence = ValidateNodeEvidence(NodeEvidenceInspection{
      .report = captured.report,
      .manifest = captured.manifest,
      .media = captured.media,
      .expected_role = node.role,
      .expected_shared_session_id = shared_session_id,
      .expected_trigger_source = expected_trigger_source,
      .expected_profile = node.profile.capture,
  });
  captured.artifact_pull_stage_milliseconds = ElapsedMilliseconds(artifact_pull_started);
  captured.capture_stage_milliseconds = capture_stage_milliseconds;
  const auto audio_started = std::chrono::steady_clock::now();
  const auto wav_bytes = std::as_bytes(std::span(captured.audio_wav));
  const auto decoded = swing_capture::DecodeMonoPcmS16Wav(wav_bytes);
  if (decoded.sample_rate_hz != playback.sample_rate_hz || decoded.samples.size() < 48'000U) {
    throw std::runtime_error("paired PCM HIL retained audio evidence is incomplete");
  }
  captured.audio_stage_milliseconds = ElapsedMilliseconds(audio_started);
  return captured;
}

PcmFailureSalvageNode PullPcmFailureSalvageNode(const std::filesystem::path &adb,
                                                const ConcurrentNode &node,
                                                std::string_view shared_session_id,
                                                std::string_view session_id,
                                                std::string report_text, bool missed_shot_capture) {
  const auto deadline = std::chrono::steady_clock::now() + kStageDeadline;
  const Json report = Json::parse(report_text);
  const Json &retained = report.at("retained_session");
  const std::string prefix = "sessions/" + std::string(session_id) + "/";
  const std::string manifest_path = retained.value("manifest", "");
  const std::string media_path = retained.value("media", "");
  const std::string audio_path = retained.at("audio_evidence").value("path", "");
  const std::string expected_audio_filename =
      missed_shot_capture ? "diagnostic_audio.wav" : "audio_evidence.wav";
  if (!SafeSessionId(session_id) || retained.value("session_id", "") != session_id ||
      manifest_path != prefix + "manifest.json" || media_path != prefix + node.role + ".mp4" ||
      audio_path != prefix + expected_audio_filename) {
    throw std::runtime_error("PCM failure salvage report contains unsafe retained paths");
  }
  PcmFailureSalvageNode salvaged{
      .session_id = std::string(session_id),
      .trigger_source = {},
      .manifest = {},
      .media = {},
      .diagnostic_audio_wav = {},
  };
  salvaged.manifest = RunRequiredAdb(
      adb,
      DeviceArguments(node.serial,
                      {"exec-out", "run-as", kPackageName, "cat", "files/" + manifest_path}),
      deadline);
  salvaged.media = RunRequiredAdb(adb,
                                  DeviceArguments(node.serial, {"exec-out", "run-as", kPackageName,
                                                                "cat", "files/" + media_path}),
                                  deadline);
  salvaged.diagnostic_audio_wav = RunRequiredAdb(
      adb,
      DeviceArguments(node.serial,
                      {"exec-out", "run-as", kPackageName, "cat", "files/" + audio_path}),
      deadline);
  const Json manifest = Json::parse(salvaged.manifest);
  const std::string trigger_source = manifest.at("trigger").value("source", "");
  const std::string_view expected_trigger_source = missed_shot_capture ? "missed_shot"
                                                   : node.role == "face_on"
                                                       ? "local_audio"
                                                       : "peer_audio_clock_candidate";
  if (manifest.value("schema_version", 0) != 1 || manifest.value("session_id", "") != session_id ||
      trigger_source != expected_trigger_source ||
      manifest.at("android_capture").value("node_id", "") != node.identity.node_id ||
      manifest.at("android_capture").value("shared_session_id", "") != shared_session_id ||
      manifest.at("views").size() != 1U ||
      manifest.at("views").at(0).value("role", "") != node.role ||
      manifest.at("views").at(0).at("media").value<std::size_t>("encoded_bytes", 0U) !=
          salvaged.media.size() ||
      salvaged.media.size() < 12U || salvaged.media.substr(4U, 4U) != "ftyp") {
    throw std::runtime_error("PCM failure salvage manifest identity or media is invalid");
  }
  salvaged.trigger_source = trigger_source;
  const auto wav_bytes = std::as_bytes(std::span(salvaged.diagnostic_audio_wav));
  const auto decoded = swing_capture::DecodeMonoPcmS16Wav(wav_bytes);
  if (decoded.sample_rate_hz != 48'000U || decoded.samples.size() < 48'000U) {
    throw std::runtime_error("PCM failure salvage diagnostic audio is incomplete");
  }
  const std::filesystem::path output = OutputDirectory() / node.role;
  WriteArtifact(output / "salvage-report.json", report_text);
  WriteArtifact(output / "salvage-manifest.json", salvaged.manifest);
  WriteArtifact(output / ("salvage-" + node.role + ".mp4"), salvaged.media);
  WriteArtifact(output / ("salvage-" + expected_audio_filename), salvaged.diagnostic_audio_wav);
  return salvaged;
}

double MaximumNormalizedPcmAmplitude(std::string_view wav) {
  const auto wav_bytes = std::as_bytes(std::span(wav));
  const auto decoded = swing_capture::DecodeMonoPcmS16Wav(wav_bytes);
  std::uint16_t maximum_magnitude = 0U;
  for (const std::int16_t sample : decoded.samples) {
    const std::int32_t widened = sample;
    const std::uint16_t magnitude =
        static_cast<std::uint16_t>(widened < 0 ? std::min<std::int32_t>(-widened, 32768) : widened);
    maximum_magnitude = std::max(maximum_magnitude, magnitude);
  }
  return static_cast<double>(maximum_magnitude) / 32768.0;
}

Json PcmReplayObservationJson(const ExtractedPcmReplayCase &replay_case,
                              std::string_view leader_source, std::string_view shadow_source) {
  const PcmReplayObservation observation{
      .case_name = replay_case.definition.name,
      .source_crc32 = replay_case.source_crc32,
      .gain_permille = replay_case.definition.gain_permille,
      .leader_detected = !leader_source.empty(),
      .shadow_detected = !shadow_source.empty(),
      .leader_source = std::string(leader_source),
      .shadow_source = std::string(shadow_source),
  };
  return {
      {"schema_version", 2},
      {"source_id", replay_case.source_id},
      {"observations",
       Json::array(
           {{{"case_name", replay_case.definition.name},
             {"source_crc32", replay_case.source_crc32},
             {"gain_permille", replay_case.definition.gain_permille},
             {"pair_outcome", PcmReplayPairOutcomeName(observation.pair_outcome())},
             {"paired_automatic_trigger_observed", observation.paired_detected()},
             {"any_automatic_trigger_observed", observation.any_detected()},
             {"leader",
              {{"role", "face_on"},
               {"automatic_trigger_observed", observation.leader_detected},
               {"source", observation.leader_source.empty() ? Json(nullptr)
                                                            : Json(observation.leader_source)}}},
             {"shadow",
              {{"role", "down_the_line"},
               {"automatic_trigger_observed", observation.shadow_detected},
               {"source", observation.shadow_source.empty()
                              ? Json(nullptr)
                              : Json(observation.shadow_source)}}}}})},
  };
}

Json SalvageMissedPcmTrigger(const std::filesystem::path &adb, const ConcurrentNode &leader,
                             const ConcurrentNode &shadow, std::string_view shared_session_id,
                             const swing_capture::hil::FeatherPcmPlaybackReceipt &playback,
                             const ExtractedPcmReplayCase &replay_case,
                             const std::array<std::string, 2> &post_stimulus_status,
                             const PcmAutomaticTriggerObservation &automatic_observation) {
  const auto salvage_started = std::chrono::steady_clock::now();
  const auto salvage_deadline = salvage_started + kStageDeadline;
  const Json leader_status = Json::parse(post_stimulus_status[0]);
  const Json shadow_status = Json::parse(post_stimulus_status[1]);
  const bool leader_triggered = !leader_status.at("last_trigger_elapsed_realtime_ns").is_null();
  const bool shadow_triggered = !shadow_status.at("last_trigger_elapsed_realtime_ns").is_null();
  const std::string_view leader_source = leader_triggered ? "local_audio" : "";
  const std::string_view shadow_source = shadow_triggered ? "peer_audio_clock_candidate" : "";
  const Json scoring_input = PcmReplayObservationJson(replay_case, leader_source, shadow_source);
  const Json &observation = scoring_input.at("observations").at(0);
  Json salvage = {
      {"schema_version", 2},
      {"passed", false},
      {"qualification_eligible", false},
      {"reason", "paired automatic impact evidence was incomplete after PCM playback"},
      {"automatic_trigger_outcome", observation.at("pair_outcome")},
      {"paired_automatic_trigger_observed", observation.at("paired_automatic_trigger_observed")},
      {"any_automatic_trigger_observed", observation.at("any_automatic_trigger_observed")},
      {"case_name", replay_case.definition.name},
      {"expectation", replay_case.definition.expectation == PcmReplayExpectation::kRequiredPositive
                          ? "required_positive"
                          : "diagnostic_negative"},
      {"source_id", replay_case.source_id},
      {"source_crc32", replay_case.source_crc32},
      {"gain_permille", replay_case.definition.gain_permille},
      {"post_stimulus_status",
       {{"face_on", Json::parse(post_stimulus_status[0])},
        {"down_the_line", Json::parse(post_stimulus_status[1])}}},
      {"playback", FeatherPcmPlaybackJson(playback)},
      {"automatic_observation", PcmAutomaticTriggerObservationJson(automatic_observation)},
      {"salvage", {{"attempted", true}, {"complete", false}}},
  };
  salvage["scoring_observation"] = observation;
  WriteArtifact(OutputDirectory() / "pcm-failure-salvage.json", salvage.dump(2) + "\n");
  WriteArtifact(OutputDirectory() / "pcm-replay-observation.json", scoring_input.dump(2) + "\n");

  const auto existing_session_id = [](const Json &status, std::string_view role) {
    const std::string session_id = status.value("active_session_id", "");
    if (!SafeSessionId(session_id)) {
      throw std::runtime_error(std::string(role) +
                               " triggered capture has no active session available for salvage");
    }
    return session_id;
  };
  const std::string leader_session_id = leader_triggered
                                            ? existing_session_id(leader_status, leader.role)
                                            : RequestPcmFailureSalvage(leader, salvage_deadline);
  const std::string shadow_session_id = shadow_triggered
                                            ? existing_session_id(shadow_status, shadow.role)
                                            : RequestPcmFailureSalvage(shadow, salvage_deadline);
  salvage["salvage"]["leader_session_id"] = leader_session_id;
  salvage["salvage"]["shadow_session_id"] = shadow_session_id;
  salvage["salvage"]["face_on_capture_origin"] =
      leader_triggered ? "automatic_trigger" : "missed_shot_salvage";
  salvage["salvage"]["down_the_line_capture_origin"] =
      shadow_triggered ? "automatic_trigger" : "missed_shot_salvage";
  WriteArtifact(OutputDirectory() / "pcm-failure-salvage.json", salvage.dump(2) + "\n");
  WaitForReadyPoseSessions(leader, shadow, leader_session_id, shadow_session_id, salvage_deadline);
  const std::int64_t freeze_and_publish_milliseconds = ElapsedMilliseconds(salvage_started);
  if (freeze_and_publish_milliseconds >
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count()) {
    throw std::runtime_error("PCM failure salvage freeze-and-publish stage exceeded 15 seconds");
  }

  const auto report_deadline = std::chrono::steady_clock::now() + kStageDeadline;
  std::string leader_report =
      BuildPoseCaptureReport(adb, leader, leader_session_id, report_deadline);
  std::string shadow_report =
      BuildPoseCaptureReport(adb, shadow, shadow_session_id, report_deadline);
  PcmFailureSalvageNode leader_capture =
      PullPcmFailureSalvageNode(adb, leader, shared_session_id, leader_session_id,
                                std::move(leader_report), !leader_triggered);
  PcmFailureSalvageNode shadow_capture =
      PullPcmFailureSalvageNode(adb, shadow, shared_session_id, shadow_session_id,
                                std::move(shadow_report), !shadow_triggered);
  const Json retained_scoring_input = PcmReplayObservationJson(
      replay_case, leader_triggered ? leader_capture.trigger_source : std::string_view(),
      shadow_triggered ? shadow_capture.trigger_source : std::string_view());
  const Json &retained_observation = retained_scoring_input.at("observations").at(0);
  salvage["automatic_trigger_outcome"] = retained_observation.at("pair_outcome");
  salvage["paired_automatic_trigger_observed"] =
      retained_observation.at("paired_automatic_trigger_observed");
  salvage["any_automatic_trigger_observed"] =
      retained_observation.at("any_automatic_trigger_observed");
  salvage["scoring_observation"] = retained_observation;
  salvage["salvage"] = {
      {"attempted", true},
      {"complete", true},
      {"automatic_trigger_outcome", retained_observation.at("pair_outcome")},
      {"qualification_eligible", false},
      {"freeze_and_publish_stage", StageJson(freeze_and_publish_milliseconds)},
      {"face_on",
       {{"session_id", leader_capture.session_id},
        {"capture_origin", leader_triggered ? "automatic_trigger" : "missed_shot_salvage"},
        {"automatic_trigger_observed", leader_triggered},
        {"trigger_source", leader_capture.trigger_source},
        {"manifest", "face_on/salvage-manifest.json"},
        {"media", "face_on/salvage-face_on.mp4"},
        {"audio", leader_triggered ? "face_on/salvage-audio_evidence.wav"
                                   : "face_on/salvage-diagnostic_audio.wav"},
        {"retained_wav_maximum_normalized_amplitude",
         MaximumNormalizedPcmAmplitude(leader_capture.diagnostic_audio_wav)}}},
      {"down_the_line",
       {{"session_id", shadow_capture.session_id},
        {"capture_origin", shadow_triggered ? "automatic_trigger" : "missed_shot_salvage"},
        {"automatic_trigger_observed", shadow_triggered},
        {"trigger_source", shadow_capture.trigger_source},
        {"manifest", "down_the_line/salvage-manifest.json"},
        {"media", "down_the_line/salvage-down_the_line.mp4"},
        {"audio", shadow_triggered ? "down_the_line/salvage-audio_evidence.wav"
                                   : "down_the_line/salvage-diagnostic_audio.wav"},
        {"retained_wav_maximum_normalized_amplitude",
         MaximumNormalizedPcmAmplitude(shadow_capture.diagnostic_audio_wav)}}},
  };
  WriteArtifact(OutputDirectory() / "pcm-failure-salvage.json", salvage.dump(2) + "\n");
  WriteArtifact(OutputDirectory() / "pcm-replay-observation.json",
                retained_scoring_input.dump(2) + "\n");
  return salvage;
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

Json WaitForLiveSetupPreview(const ConcurrentNode &node,
                             std::chrono::steady_clock::time_point deadline) {
  const HttpResponse unauthenticated = RequireNodeHttp({.port = node.host_port,
                                                        .method = "GET",
                                                        .path = "/api/v1/setup/preview",
                                                        .bearer_token = {},
                                                        .body = {},
                                                        .deadline = deadline},
                                                       401);
  WriteArtifact(OutputDirectory() / node.role / "setup-preview-unauthenticated.json",
                unauthenticated.body);

  std::string latest = "setup metadata has not advertised a live preview";
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      const HttpResponse setup = RequireNodeHttp({.port = node.host_port,
                                                  .method = "GET",
                                                  .path = "/api/v1/setup",
                                                  .bearer_token = node.control_token,
                                                  .body = {},
                                                  .deadline = deadline},
                                                 200);
      const Json setup_json = Json::parse(setup.body);
      const Json &metadata = setup_json.at("preview");
      if (!metadata.value("available", false)) {
        latest = "setup preview is " + metadata.value("reason", "unavailable");
        std::this_thread::sleep_for(kPollInterval);
        continue;
      }
      const HttpResponse jpeg = RequestNodeHttp({.port = node.host_port,
                                                 .method = "GET",
                                                 .path = "/api/v1/setup/preview",
                                                 .bearer_token = node.control_token,
                                                 .body = {},
                                                 .deadline = deadline});
      if (jpeg.status == 503) {
        latest = "setup preview became unavailable before JPEG retrieval";
        std::this_thread::sleep_for(kPollInterval);
        continue;
      }
      if (jpeg.status != 200) {
        throw std::runtime_error("authenticated setup preview returned HTTP " +
                                 std::to_string(jpeg.status));
      }
      const bool jpeg_markers =
          jpeg.body.size() >= 4U && static_cast<unsigned char>(jpeg.body[0]) == 0xffU &&
          static_cast<unsigned char>(jpeg.body[1]) == 0xd8U &&
          static_cast<unsigned char>(jpeg.body[jpeg.body.size() - 2U]) == 0xffU &&
          static_cast<unsigned char>(jpeg.body.back()) == 0xd9U;
      const std::int64_t frame_age_ms = metadata.value("frame_age_ms", -1LL);
      const int rotation = metadata.value("image_rotation_degrees", -1);
      if (!jpeg_markers || RequiredHttpHeader(jpeg, "content-type") != "image/jpeg" ||
          RequiredHttpHeader(jpeg, "cache-control") != "no-store, max-age=0" ||
          RequiredHttpHeader(jpeg, "x-content-type-options") != "nosniff" ||
          metadata.value("url", "") != "/api/v1/setup/preview" ||
          metadata.value("state", "") != "available" || metadata.value("reason", "") != "none" ||
          metadata.value("generation", 0LL) < 1 || frame_age_ms < 0 || frame_age_ms > 3'000 ||
          (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)) {
        throw std::runtime_error("authenticated setup preview metadata or JPEG is invalid");
      }
      WriteArtifact(OutputDirectory() / node.role / "setup-preview.json", setup.body);
      WriteArtifact(OutputDirectory() / node.role / "setup-preview.jpg", jpeg.body);
      return {
          {"passed", true},
          {"unauthenticated_status", 401},
          {"authenticated_status", 200},
          {"content_type", "image/jpeg"},
          {"cache_control", "no-store, max-age=0"},
          {"byte_count", jpeg.body.size()},
          {"state", metadata.at("state")},
          {"reason", metadata.at("reason")},
          {"generation", metadata.at("generation")},
          {"image_rotation_degrees", rotation},
          {"frame_age_ms", frame_age_ms},
      };
    } catch (const std::exception &failure) {
      latest = failure.what();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("live authenticated setup preview exceeded 15 seconds: " + latest);
}

Json ValidateHighSpeedSetupPreviewUnavailable(const ConcurrentNode &node,
                                              std::chrono::steady_clock::time_point deadline) {
  const HttpResponse unavailable = RequireNodeHttp({.port = node.host_port,
                                                    .method = "GET",
                                                    .path = "/api/v1/setup/preview",
                                                    .bearer_token = node.control_token,
                                                    .body = {},
                                                    .deadline = deadline},
                                                   503);
  const HttpResponse setup = RequireNodeHttp({.port = node.host_port,
                                              .method = "GET",
                                              .path = "/api/v1/setup",
                                              .bearer_token = node.control_token,
                                              .body = {},
                                              .deadline = deadline},
                                             200);
  const Json setup_json = Json::parse(setup.body);
  const Json &metadata = setup_json.at("preview");
  if (RequiredHttpHeader(unavailable, "cache-control") != "no-store, max-age=0" ||
      metadata.value("available", true) || !metadata.at("url").is_null() ||
      metadata.value("state", "") != "unavailable" ||
      metadata.value("reason", "") != "high_speed_capture" ||
      !metadata.at("frame_age_ms").is_null()) {
    throw std::runtime_error("high-speed capture did not suppress the setup preview");
  }
  WriteArtifact(OutputDirectory() / node.role / "setup-preview-high-speed.json", setup.body);
  WriteArtifact(OutputDirectory() / node.role / "setup-preview-high-speed-unavailable.json",
                unavailable.body);
  return {
      {"passed", true},
      {"authenticated_status", 503},
      {"cache_control", "no-store, max-age=0"},
      {"state", metadata.at("state")},
      {"reason", metadata.at("reason")},
  };
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
                                                    std::string_view shared_session_id,
                                                    std::string_view expected_down_source,
                                                    std::string_view expected_face_source) {
  const auto stage_started = std::chrono::steady_clock::now();
  const auto deadline = stage_started + kStageDeadline;
  const HttpResponse down_report_response =
      RequireNodeHttp({.port = down_node.host_port,
                       .method = "GET",
                       .path = "/api/v1/capture/trigger-report",
                       .bearer_token = down_node.control_token,
                       .body = {},
                       .deadline = deadline},
                      200);
  const HttpResponse face_report_response =
      RequireNodeHttp({.port = face_node.host_port,
                       .method = "GET",
                       .path = "/api/v1/capture/trigger-report",
                       .bearer_token = face_node.control_token,
                       .body = {},
                       .deadline = deadline},
                      200);
  WriteArtifact(OutputDirectory() / down_node.role / "trigger-report.json",
                down_report_response.body);
  WriteArtifact(OutputDirectory() / face_node.role / "trigger-report.json",
                face_report_response.body);
  const coordination::TriggerReport down_report =
      ValidateTriggerReport(down_report_response.body, down_node.identity, shared_session_id,
                            down_capture.evidence.local_session_id, expected_down_source);
  const coordination::TriggerReport face_report =
      ValidateTriggerReport(face_report_response.body, face_node.identity, shared_session_id,
                            face_capture.evidence.local_session_id, expected_face_source);
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

Json StartupTimingJson(const CaptureStartupTimingEvidence &timing) {
  constexpr double kNanosecondsPerMillisecond = 1'000'000.0;
  return {
      {"schema_version", 1},
      {"clock", "CLOCK_BOOTTIME"},
      {"continuity_clean", timing.continuity_reset_count == 0U},
      {"startup_continuity_reset_count", std::to_string(timing.continuity_reset_count)},
      {"maximum_startup_continuity_gap_ns", std::to_string(timing.maximum_continuity_gap_ns)},
      {"milestones",
       {{"arm_requested_elapsed_realtime_ns", std::to_string(timing.arm_requested_ns)},
        {"engine_started_elapsed_realtime_ns", std::to_string(timing.engine_started_ns)},
        {"first_camera_frame_elapsed_realtime_ns", std::to_string(timing.first_camera_frame_ns)},
        {"first_usable_encoded_frame_elapsed_realtime_ns",
         std::to_string(timing.first_usable_encoded_frame_ns)},
        {"full_pre_roll_ready_elapsed_realtime_ns",
         std::to_string(timing.full_pre_roll_ready_ns)}}},
      {"durations",
       {{"arm_to_engine_start_ns", std::to_string(timing.arm_to_engine_start_ns)},
        {"engine_start_to_first_camera_frame_ns",
         std::to_string(timing.engine_start_to_first_camera_frame_ns)},
        {"arm_to_first_camera_frame_ns", std::to_string(timing.arm_to_first_camera_frame_ns)},
        {"first_camera_frame_to_first_usable_encoded_frame_ns",
         std::to_string(timing.first_camera_frame_to_first_usable_encoded_frame_ns)},
        {"arm_to_first_usable_encoded_frame_ns",
         std::to_string(timing.arm_to_first_usable_encoded_frame_ns)},
        {"first_usable_encoded_frame_to_full_pre_roll_ready_ns",
         std::to_string(timing.first_usable_encoded_frame_to_full_pre_roll_ready_ns)},
        {"arm_to_full_pre_roll_ready_ns", std::to_string(timing.arm_to_full_pre_roll_ready_ns)},
        {"arm_to_engine_start_ms",
         static_cast<double>(timing.arm_to_engine_start_ns) / kNanosecondsPerMillisecond},
        {"engine_start_to_first_camera_frame_ms",
         static_cast<double>(timing.engine_start_to_first_camera_frame_ns) /
             kNanosecondsPerMillisecond},
        {"arm_to_first_camera_frame_ms",
         static_cast<double>(timing.arm_to_first_camera_frame_ns) / kNanosecondsPerMillisecond},
        {"first_camera_frame_to_first_usable_encoded_frame_ms",
         static_cast<double>(timing.first_camera_frame_to_first_usable_encoded_frame_ns) /
             kNanosecondsPerMillisecond},
        {"arm_to_first_usable_encoded_frame_ms",
         static_cast<double>(timing.arm_to_first_usable_encoded_frame_ns) /
             kNanosecondsPerMillisecond},
        {"first_usable_encoded_frame_to_full_pre_roll_ready_ms",
         static_cast<double>(timing.first_usable_encoded_frame_to_full_pre_roll_ready_ns) /
             kNanosecondsPerMillisecond},
        {"arm_to_full_pre_roll_ready_ms",
         static_cast<double>(timing.arm_to_full_pre_roll_ready_ns) / kNanosecondsPerMillisecond}}},
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
      {"startup_timing", StartupTimingJson(evidence.startup_timing)},
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
           {"peak_tile", {{"x", captured.optical.tile_x}, {"y", captured.optical.tile_y}}},
           {"localized_response_tile_count", captured.optical.localized_response_tile_count},
           {"post_sequence_baseline_shift", captured.optical.post_sequence_baseline_shift},
           {"white_duration_us", captured.optical.white_duration_us},
           {"optical_to_audio_offset_us", captured.optical.optical_to_audio_offset_us},
           {"optical_onset_lower_bound_us", captured.optical.optical_onset_lower_bound_us},
           {"optical_onset_upper_bound_us", captured.optical.optical_onset_upper_bound_us},
           {"timing_correlation",
            {
                {"passed", timing.passed},
                {"claim_scope", "fixture_optical_marker_to_audio_trigger"},
                {"absolute_ball_impact_calibrated", false},
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

Json PcmEvidenceJson(const CapturedNode &captured, const ExtractedPcmReplayCase &replay_case,
                     std::string_view audio_filename) {
  const NodeEvidence &evidence = captured.evidence;
  const auto &playback = captured.pcm_playback.value();
  const TimingCorrelationEvidence timing = PcmMarkerCorrelation(captured);
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
      {"passed", true},
      {"node_id", evidence.node_id},
      {"role", evidence.role},
      {"local_session_id", evidence.local_session_id},
      {"shared_session_id", evidence.shared_session_id},
      {"frame_count", evidence.frame_count},
      {"measured_sensor_fps", evidence.measured_sensor_fps},
      {"actual_pre_roll_us", evidence.actual_pre_roll_us},
      {"actual_post_roll_us", evidence.actual_post_roll_us},
      {"startup_timing", StartupTimingJson(evidence.startup_timing)},
      {"local_nearest_frame_residual_us", evidence.local_nearest_frame_residual_us},
      {"trigger_timestamp_uncertainty_ns", evidence.trigger_timestamp_uncertainty_ns},
      {"audio",
       {{"sample_rate_hz", evidence.audio_sample_rate_hz},
        {"peak_amplitude", evidence.audio_peak_amplitude},
        {"noise_floor", evidence.audio_noise_floor},
        {"threshold", evidence.audio_threshold},
        {"retained_wav", evidence.role + "/" + std::string(audio_filename)}}},
      {"pcm_replay",
       {{"case_name", replay_case.definition.name},
        {"source_id", replay_case.source_id},
        {"source_start_frame", replay_case.definition.start_frame},
        {"source_sample_count", replay_case.definition.sample_count},
        {"marker_frame", replay_case.definition.marker_frame},
        {"source_crc32", replay_case.source_crc32},
        {"gain_permille", replay_case.definition.gain_permille},
        {"playback", FeatherPcmPlaybackJson(playback)}}},
      {"decoded_video",
       {{"passed", true},
        {"exact_frame_count", true},
        {"decoded_frame_count", captured.optical.decoded_frame_count},
        {"manifest_frame_count", evidence.frame_count},
        {"maximum_media_time_residual_us", captured.maximum_media_time_residual_us},
        {"analysis_width", captured.analysis_width},
        {"analysis_height", captured.analysis_height}}},
      {"optical",
       {{"passed", captured.optical.detected},
        {"diagnostic", captured.optical.diagnostic},
        {"peak_frame_index", captured.optical.peak_frame_index},
        {"first_white_frame_index", captured.optical.first_white_frame_index},
        {"last_white_frame_index", captured.optical.last_white_frame_index},
        {"maximum_white_delta", captured.optical.maximum_white_delta},
        {"peak_tile", {{"x", captured.optical.tile_x}, {"y", captured.optical.tile_y}}},
        {"localized_response_tile_count", captured.optical.localized_response_tile_count},
        {"white_duration_us", captured.optical.white_duration_us},
        {"optical_to_audio_offset_us", captured.optical.optical_to_audio_offset_us},
        {"timing_correlation",
         {{"passed", timing.passed},
          {"claim_scope", "fixture_optical_marker_to_audio_trigger"},
          {"absolute_ball_impact_calibrated", false},
          {"acceptance_limit_us", timing.acceptance_limit_us},
          {"optical_onset_lower_bound_us", timing.optical_onset_lower_bound_us},
          {"optical_onset_upper_bound_us", timing.optical_onset_upper_bound_us},
          {"optical_interval_width_us", timing.optical_interval_width_us},
          {"audio_trigger_uncertainty_us", timing.audio_trigger_uncertainty_us},
          {"media_pts_residual_us", timing.media_pts_residual_us},
          {"accounted_uncertainty_us", timing.accounted_uncertainty_us},
          {"minimum_residual_us", timing.minimum_residual_us},
          {"maximum_residual_us", timing.maximum_residual_us},
          {"total_bound_us", timing.total_bound_us}}}}},
      {"april_tag",
       {{"passed", true},
        {"required_family", "tag36h11"},
        {"required_id", 0},
        {"persistent_pre_marker_post", true},
        {"frames", std::move(april_tag_frames)}}},
      {"stages",
       {{"capture", StageJson(captured.capture_stage_milliseconds)},
        {"artifact_pull", StageJson(captured.artifact_pull_stage_milliseconds)},
        {"ffprobe", StageJson(captured.ffprobe_stage_milliseconds)},
        {"decode", StageJson(captured.decode_stage_milliseconds)},
        {"diagnostic", StageJson(captured.diagnostic_stage_milliseconds)},
        {"audio_validation", StageJson(captured.audio_stage_milliseconds)}}},
      {"report", evidence.role + "/report.json"},
      {"manifest", evidence.role + "/manifest.json"},
      {"media", evidence.role + "/" + evidence.role + ".mp4"},
      {"audio_evidence", evidence.role + "/" + std::string(audio_filename)},
      {"ffprobe", evidence.role + "/ffprobe.json"},
      {"diagnostic_frames",
       {{"pre", evidence.role + "/diagnostic-01.png"},
        {"marker", evidence.role + "/diagnostic-02.png"},
        {"post", evidence.role + "/diagnostic-03.png"}}},
  };
}

Json PcmNodeArtifactPaths(std::string_view role, std::string_view audio_filename) {
  const std::string prefix = std::string(role) + "/";
  return {
      {"capture_report", prefix + "report.json"},
      {"manifest", prefix + "manifest.json"},
      {"media", prefix + std::string(role) + ".mp4"},
      {"audio", prefix + std::string(audio_filename)},
      {"ffprobe", prefix + "ffprobe.json"},
      {"diagnostic_frames",
       {{"pre", prefix + "diagnostic-01.png"},
        {"marker", prefix + "diagnostic-02.png"},
        {"post", prefix + "diagnostic-03.png"}}},
  };
}

std::string FirstLine(std::string_view text) {
  return std::string(text.substr(0U, text.find('\n')));
}

Json WaitForUntrustedDiscovery(const ConcurrentNode &observer, std::string_view expected_node_id,
                               std::string_view expected_origin, std::string_view expected_role,
                               std::chrono::steady_clock::time_point deadline) {
  std::string latest = "no discovery response";
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      const HttpResponse response = RequireNodeHttp({.port = observer.host_port,
                                                     .method = "GET",
                                                     .path = "/api/v1/discovery",
                                                     .bearer_token = observer.control_token,
                                                     .body = {},
                                                     .deadline = deadline},
                                                    200);
      const Json discovery = Json::parse(response.body);
      if (discovery.value("schema_version", 0) != 1 ||
          !discovery.value("authentication_required_for_pairing", false) ||
          !discovery.at("observations").is_array()) {
        throw std::runtime_error("discovery response contract is invalid");
      }
      for (const Json &observation : discovery.at("observations")) {
        if (observation.value("node_id", "") == expected_node_id) {
          if (observation.value("trusted", true) ||
              observation.value("origin", "") != expected_origin ||
              observation.value("role", "") != expected_role ||
              observation.value("expires_at_epoch_ms", 0LL) <=
                  observation.value("observed_at_epoch_ms", 0LL)) {
            throw std::runtime_error("discovery observation was trusted or malformed");
          }
          return discovery;
        }
      }
      latest = "expected stable node ID has not appeared";
    } catch (const std::exception &failure) {
      latest = failure.what();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("mutual NSD discovery exceeded 15 seconds: " + latest);
}

int RunDiscoveryPairing(int argument_count, char **arguments) {
  if (argument_count != 4 || std::string_view(arguments[3]) != "discovery-pairing") {
    throw std::runtime_error("expected Bazel runfiles: <adb> <APK> discovery-pairing");
  }
  const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
  const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
  if (!std::filesystem::is_regular_file(adb) || access(adb.c_str(), X_OK) != 0 ||
      !std::filesystem::is_regular_file(apk)) {
    throw std::runtime_error("discovery/pairing HIL runfiles are missing");
  }
  const std::string leader_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_SERIAL");
  const std::string shadow_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_SERIAL");
  if (leader_serial == shadow_serial) {
    throw std::runtime_error("discovery/pairing HIL requires two distinct devices");
  }
  const LanOrigin leader_lan =
      ParseLanOrigin(RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN"),
                     "SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN");
  const LanOrigin shadow_lan =
      ParseLanOrigin(RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN"),
                     "SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN");

  StopBothGuard cleanup(adb, shadow_serial, leader_serial);
  StopPackage(adb, leader_serial);
  StopPackage(adb, shadow_serial);
  cleanup.SnapshotNodeConfigurations();
  ConcurrentNode leader =
      ConfigureConcurrentNode(adb, apk, leader_serial, "face_on", &cleanup, true);
  ConcurrentNode shadow =
      ConfigureConcurrentNode(adb, apk, shadow_serial, "down_the_line", &cleanup, true);
  if (leader.identity.node_id == shadow.identity.node_id) {
    throw std::runtime_error("discovery/pairing HIL nodes reuse one persistent identity");
  }

  const auto fixture_started = std::chrono::steady_clock::now();
  const auto fixture_deadline = fixture_started + kStageDeadline;
  PrepareUnpairedDiscoveryFixture(leader, fixture_deadline);
  PrepareUnpairedDiscoveryFixture(shadow, fixture_deadline);
  const std::int64_t fixture_milliseconds = ElapsedMilliseconds(fixture_started);

  const auto discovery_started = std::chrono::steady_clock::now();
  const auto discovery_deadline = discovery_started + kStageDeadline;
  const HttpResponse unauthenticated_discovery = RequireNodeHttp({.port = leader.host_port,
                                                                  .method = "GET",
                                                                  .path = "/api/v1/discovery",
                                                                  .bearer_token = {},
                                                                  .body = {},
                                                                  .deadline = discovery_deadline},
                                                                 401);
  static_cast<void>(unauthenticated_discovery);
  Json leader_discovery = WaitForUntrustedDiscovery(
      leader, shadow.identity.node_id, shadow_lan.origin, "down_the_line", discovery_deadline);
  Json shadow_discovery = WaitForUntrustedDiscovery(
      shadow, leader.identity.node_id, leader_lan.origin, "face_on", discovery_deadline);
  WriteArtifact(OutputDirectory() / "face_on" / "discovery.json", leader_discovery.dump(2) + "\n");
  WriteArtifact(OutputDirectory() / "down_the_line" / "discovery.json",
                shadow_discovery.dump(2) + "\n");

  RequireNodeHttp({.host = shadow_lan.ipv4_address,
                   .port = shadow_lan.port,
                   .method = "GET",
                   .path = "/api/v1/pairing/identity",
                   .bearer_token = "deliberately-invalid-control-token",
                   .body = {},
                   .deadline = discovery_deadline},
                  401);
  const HttpResponse authenticated_identity = RequireNodeHttp({.host = shadow_lan.ipv4_address,
                                                               .port = shadow_lan.port,
                                                               .method = "GET",
                                                               .path = "/api/v1/pairing/identity",
                                                               .bearer_token = shadow.control_token,
                                                               .body = {},
                                                               .deadline = discovery_deadline},
                                                              200);
  const Json identity = Json::parse(authenticated_identity.body);
  if (identity.value("node_id", "") != shadow.identity.node_id ||
      identity.value("role", "") != "down_the_line" || identity.value("label", "").empty()) {
    throw std::runtime_error("authenticated pairing identity is invalid");
  }
  WriteArtifact(OutputDirectory() / "down_the_line" / "pairing-identity.json",
                identity.dump(2) + "\n");

  const HttpResponse before = RequireNodeHttp({.port = leader.host_port,
                                               .method = "GET",
                                               .path = "/api/v1/setup",
                                               .bearer_token = leader.control_token,
                                               .body = {},
                                               .deadline = discovery_deadline},
                                              200);
  if (!InspectDiscoveryPairingFixture(before.body).IsUnpaired()) {
    throw std::runtime_error("leader acquired a binding from discovery without setup mutation");
  }
  const std::int64_t discovery_milliseconds = ElapsedMilliseconds(discovery_started);

  const auto pairing_started = std::chrono::steady_clock::now();
  const auto pairing_deadline = pairing_started + kStageDeadline;
  ConfigurePoseMode(shadow, "shadow", std::nullopt, std::nullopt, pairing_deadline);
  ConfigurePeerPoseModeWithRetries(leader, shadow_lan.origin, shadow.control_token,
                                   pairing_deadline);
  const HttpResponse paired = RequireNodeHttp({.port = leader.host_port,
                                               .method = "GET",
                                               .path = "/api/v1/setup",
                                               .bearer_token = leader.control_token,
                                               .body = {},
                                               .deadline = pairing_deadline},
                                              200);
  const Json paired_setup = Json::parse(paired.body);
  const Json &binding = paired_setup.at("pairing");
  if (binding.value("state", "") != "active" ||
      binding.value("peer_node_id", "") != shadow.identity.node_id ||
      binding.value("expected_role", "") != "down_the_line" ||
      binding.value("origin", "") != shadow_lan.origin ||
      binding.value("credential_generation", 0) < 1 || paired.body.contains(shadow.control_token)) {
    throw std::runtime_error("leader did not persist the authenticated redacted peer binding");
  }
  WriteArtifact(OutputDirectory() / "face_on" / "paired-setup.json", paired_setup.dump(2) + "\n");

  const Json rejected_request = {
      {"schema_version", 1},
      {"expected_revision", paired_setup.at("revision")},
      {"configuration",
       {{"role", "face_on"},
        {"capture_profile", leader.profile.name},
        {"pose",
         {{"mode", "leader"},
          {"inference_delegate", "gpu_preferred"},
          {"debug_evidence_enabled", true},
          {"hitting_region", {{"left", 0.0}, {"top", 0.0}, {"right", 1.0}, {"bottom", 1.0}}},
          {"peer_update",
           {{"operation", "replace"},
            {"origin", shadow_lan.origin},
            {"control_token", "deliberately-invalid-control-token"}}}}}}},
  };
  RequireNodeHttp({.port = leader.host_port,
                   .method = "PUT",
                   .path = "/api/v1/setup",
                   .bearer_token = leader.control_token,
                   .body = rejected_request.dump(),
                   .deadline = pairing_deadline},
                  400);
  const Json after_rejection = Json::parse(RequireNodeHttp({.port = leader.host_port,
                                                            .method = "GET",
                                                            .path = "/api/v1/setup",
                                                            .bearer_token = leader.control_token,
                                                            .body = {},
                                                            .deadline = pairing_deadline},
                                                           200)
                                               .body);
  if (after_rejection.at("revision") != paired_setup.at("revision") ||
      after_rejection.at("pairing") != paired_setup.at("pairing")) {
    throw std::runtime_error("rejected peer credential changed durable leader configuration");
  }
  const std::int64_t pairing_milliseconds = ElapsedMilliseconds(pairing_started);

  const auto rotation_started = std::chrono::steady_clock::now();
  const auto rotation_deadline = rotation_started + kStageDeadline;
  const std::string previous_shadow_token = shadow.control_token;
  const Json shadow_setup_before_rotation =
      Json::parse(RequireNodeHttp({.port = shadow.host_port,
                                   .method = "GET",
                                   .path = "/api/v1/setup",
                                   .bearer_token = previous_shadow_token,
                                   .body = {},
                                   .deadline = rotation_deadline},
                                  200)
                      .body);
  const Json rotation_request = {
      {"schema_version", 1},
      {"expected_revision", shadow_setup_before_rotation.at("revision")},
      {"confirmed_node_id", shadow.identity.node_id},
  };
  const HttpResponse rotation_response =
      RequireNodeHttp({.port = shadow.host_port,
                       .method = "POST",
                       .path = "/api/v1/control-credential/rotate",
                       .bearer_token = previous_shadow_token,
                       .body = rotation_request.dump(),
                       .deadline = rotation_deadline},
                      200);
  const Json rotation = Json::parse(rotation_response.body);
  shadow.control_token = rotation.value("control_token", "");
  if (rotation.value("schema_version", 0) != 1 ||
      rotation.value("node_id", "") != shadow.identity.node_id ||
      rotation.value("setup_revision", -1LL) <= shadow_setup_before_rotation.at("revision") ||
      rotation.value("control_credential_generation", 0LL) <=
          shadow_setup_before_rotation.at("node").value("control_credential_generation", 0LL) ||
      !rotation.value("remote_peer_bindings_require_re_pair", false) ||
      shadow.control_token.size() != 32U ||
      !std::ranges::all_of(shadow.control_token,
                           [](char character) {
                             return (character >= 'a' && character <= 'z') ||
                                    (character >= 'A' && character <= 'Z') ||
                                    (character >= '0' && character <= '9') || character == '_' ||
                                    character == '-';
                           }) ||
      shadow.control_token == previous_shadow_token) {
    throw std::runtime_error("shadow control credential rotation response is invalid");
  }
  RequireNodeHttp({.host = shadow_lan.ipv4_address,
                   .port = shadow_lan.port,
                   .method = "GET",
                   .path = "/api/v1/pairing/identity",
                   .bearer_token = previous_shadow_token,
                   .body = {},
                   .deadline = rotation_deadline},
                  401);
  RequireNodeHttp({.host = shadow_lan.ipv4_address,
                   .port = shadow_lan.port,
                   .method = "GET",
                   .path = "/api/v1/pairing/identity",
                   .bearer_token = shadow.control_token,
                   .body = {},
                   .deadline = rotation_deadline},
                  200);
  const Json stale_leader_setup = Json::parse(RequireNodeHttp({.port = leader.host_port,
                                                               .method = "GET",
                                                               .path = "/api/v1/setup",
                                                               .bearer_token = leader.control_token,
                                                               .body = {},
                                                               .deadline = rotation_deadline},
                                                              200)
                                                  .body);
  if (stale_leader_setup.at("pairing").value("credential_status", "") != "re_pair_required") {
    throw std::runtime_error("leader silently retained a peer credential after remote rotation");
  }
  WriteArtifact(OutputDirectory() / "face_on" / "stale-peer-setup.json",
                stale_leader_setup.dump(2) + "\n");

  ConfigurePeerPoseModeWithRetries(leader, shadow_lan.origin, shadow.control_token,
                                   rotation_deadline);
  const Json repaired_setup = Json::parse(RequireNodeHttp({.port = leader.host_port,
                                                           .method = "GET",
                                                           .path = "/api/v1/setup",
                                                           .bearer_token = leader.control_token,
                                                           .body = {},
                                                           .deadline = rotation_deadline},
                                                          200)
                                              .body);
  if (repaired_setup.at("pairing").value("credential_status", "") != "verified" ||
      repaired_setup.at("pairing").value("credential_generation", 0LL) <=
          paired_setup.at("pairing").value("credential_generation", 0LL) ||
      repaired_setup.dump().contains(shadow.control_token)) {
    throw std::runtime_error("explicit authenticated re-pair did not restore the leader binding");
  }
  WriteArtifact(OutputDirectory() / "face_on" / "repaired-peer-setup.json",
                repaired_setup.dump(2) + "\n");
  const std::int64_t rotation_milliseconds = ElapsedMilliseconds(rotation_started);
  const auto cleanup_started = std::chrono::steady_clock::now();
  cleanup.RestoreNodeConfigurationsChecked();
  const std::int64_t cleanup_milliseconds = ElapsedMilliseconds(cleanup_started);
  constexpr auto kStageDeadlineMilliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count();
  for (const std::int64_t elapsed :
       {fixture_milliseconds, discovery_milliseconds, pairing_milliseconds, rotation_milliseconds,
        cleanup_milliseconds}) {
    if (elapsed > kStageDeadlineMilliseconds) {
      throw std::runtime_error("discovery/pairing HIL stage exceeded 15 seconds");
    }
  }
  const Json report = {
      {"schema_version", 1},
      {"report_type", "android_dual_phone_discovery_pairing_hil"},
      {"passed", true},
      {"stages",
       {{"unpaired_fixture_setup", StageJson(fixture_milliseconds)},
        {"mutual_discovery_and_identity", StageJson(discovery_milliseconds)},
        {"authenticated_pairing", StageJson(pairing_milliseconds)},
        {"credential_rotation_and_re_pair", StageJson(rotation_milliseconds)},
        {"configuration_restore", StageJson(cleanup_milliseconds)}}},
      {"mutual_untrusted_discovery", true},
      {"authenticated_identity_verified", true},
      {"binding_created_only_after_setup_mutation", true},
      {"wrong_credential_rejected_without_mutation", true},
      {"own_control_credential_rotated", true},
      {"old_control_credential_rejected", true},
      {"stale_remote_binding_required_re_pair", true},
      {"authenticated_re_pair_restored_binding", true},
      {"leader_lan_origin", leader_lan.origin},
      {"shadow_lan_origin", shadow_lan.origin},
      {"artifacts",
       {{"face_on_field_setup", "face_on/discovery-field-setup.json"},
        {"face_on_unpaired_setup", "face_on/discovery-unpaired-setup.json"},
        {"down_the_line_field_setup", "down_the_line/discovery-field-setup.json"},
        {"down_the_line_unpaired_setup", "down_the_line/discovery-unpaired-setup.json"},
        {"face_on_discovery", "face_on/discovery.json"},
        {"down_the_line_discovery", "down_the_line/discovery.json"},
        {"authenticated_identity", "down_the_line/pairing-identity.json"},
        {"paired_setup", "face_on/paired-setup.json"},
        {"stale_peer_setup", "face_on/stale-peer-setup.json"},
        {"repaired_peer_setup", "face_on/repaired-peer-setup.json"}}},
      {"configuration_restore",
       {{"passed", true},
        {"scope", "complete_private_node_configuration_generation"},
        {"temporary_peer_configuration_removed", true},
        {"secret_material_preserved_in_artifacts", false}}},
  };
  WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
  std::cout << "Published bounded two-phone discovery/pairing evidence\n";
  return 0;
}

int RunAutonomousRestartRecovery(int argument_count, char **arguments) {
  if (argument_count != 4 || std::string_view(arguments[3]) != "autonomous-restart") {
    throw std::runtime_error("expected Bazel runfiles: <adb> <APK> autonomous-restart");
  }
  const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
  const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
  if (!std::filesystem::is_regular_file(adb) || access(adb.c_str(), X_OK) != 0 ||
      !std::filesystem::is_regular_file(apk)) {
    throw std::runtime_error("autonomous restart HIL runfiles are missing");
  }
  const std::string leader_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_SERIAL");
  const std::string shadow_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_SERIAL");
  if (leader_serial == shadow_serial) {
    throw std::runtime_error("autonomous restart HIL requires two distinct devices");
  }
  const LanOrigin shadow_lan =
      ParseLanOrigin(RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN"),
                     "SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN");

  StopBothGuard cleanup(adb, shadow_serial, leader_serial);
  StopPackage(adb, leader_serial);
  StopPackage(adb, shadow_serial);
  cleanup.SnapshotNodeConfigurations();
  ConcurrentNode leader =
      ConfigureConcurrentNode(adb, apk, leader_serial, "face_on", &cleanup, true);
  ConcurrentNode shadow =
      ConfigureConcurrentNode(adb, apk, shadow_serial, "down_the_line", &cleanup, true);
  if (leader.identity.node_id == shadow.identity.node_id) {
    throw std::runtime_error("autonomous restart HIL nodes reuse one persistent identity");
  }

  const auto setup_started = std::chrono::steady_clock::now();
  const auto setup_deadline = setup_started + kStageDeadline;
  ConfigurePoseMode(shadow, "shadow", std::nullopt, std::nullopt, setup_deadline);
  ConfigurePeerPoseModeWithRetries(leader, shadow_lan.origin, shadow.control_token, setup_deadline);
  const std::int64_t setup_milliseconds = ElapsedMilliseconds(setup_started);

  const std::string shared_session_id =
      "restart-hil-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
  if (!SafeSessionId(shared_session_id)) {
    throw std::runtime_error("autonomous restart HIL generated an unsafe shared session ID");
  }
  const auto seed_started = std::chrono::steady_clock::now();
  const auto seed_deadline = seed_started + kStageDeadline;
  RegisterAutonomousRecoveryCleanup(cleanup, adb, leader.serial, shadow.serial, shared_session_id,
                                    seed_deadline);
  RunRequiredAdb(
      adb,
      DeviceArguments(
          leader.serial,
          {"shell", "am", "start", "-W", "--activity-single-top", "-n",
           "com.agoessling.swingcapture/.MainActivity", "--ez", "enable_pose_arm_hil", "true",
           "--ez", "prepare_autonomous_recovery_hil", "true", "--es", "shared_session_id",
           shared_session_id, "--es", "peer_node_id", shadow.identity.node_id}),
      seed_deadline);
  Json before_restart;
  while (std::chrono::steady_clock::now() < seed_deadline) {
    const HttpResponse response = RequireNodeHttp({.port = leader.host_port,
                                                   .method = "GET",
                                                   .path = "/api/v1/capture/status",
                                                   .bearer_token = leader.control_token,
                                                   .body = {},
                                                   .deadline = seed_deadline},
                                                  200);
    before_restart = Json::parse(response.body);
    WriteArtifact(OutputDirectory() / leader.role / "autonomous-before-restart.json",
                  response.body);
    const Json &autonomous = before_restart.at("pose").at("autonomous_pair");
    if (autonomous.value("state", "") == "storing_local_record" &&
        autonomous.value("active_shared_session_id", "") == shared_session_id &&
        autonomous.value("replication_backlog_size", 0) == 1 &&
        autonomous.value("last_backlog_outcome", "") == "hil_queued_for_restart") {
      break;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (before_restart.is_null() ||
      before_restart.at("pose").at("autonomous_pair").value("active_shared_session_id", "") !=
          shared_session_id) {
    throw std::runtime_error("autonomous restart HIL checkpoint was not prepared");
  }
  const std::int64_t seed_milliseconds = ElapsedMilliseconds(seed_started);

  const auto restart_started = std::chrono::steady_clock::now();
  const auto restart_deadline = restart_started + kStageDeadline;
  StopPackage(adb, leader.serial);
  RunRequiredAdb(adb, ConfigureArguments(leader.serial, leader.role, leader.profile, true),
                 restart_deadline);

  Json recovered;
  Json shadow_status;
  bool recovery_ready = false;
  while (std::chrono::steady_clock::now() < restart_deadline) {
    try {
      const HttpResponse leader_response = RequireNodeHttp({.port = leader.host_port,
                                                            .method = "GET",
                                                            .path = "/api/v1/capture/status",
                                                            .bearer_token = leader.control_token,
                                                            .body = {},
                                                            .deadline = restart_deadline},
                                                           200);
      const HttpResponse shadow_response = RequireNodeHttp({.port = shadow.host_port,
                                                            .method = "GET",
                                                            .path = "/api/v1/capture/status",
                                                            .bearer_token = shadow.control_token,
                                                            .body = {},
                                                            .deadline = restart_deadline},
                                                           200);
      recovered = Json::parse(leader_response.body);
      shadow_status = Json::parse(shadow_response.body);
      WriteArtifact(OutputDirectory() / leader.role / "autonomous-after-restart-latest.json",
                    leader_response.body);
      WriteArtifact(OutputDirectory() / shadow.role / "autonomous-after-restart-latest.json",
                    shadow_response.body);
      const Json &autonomous = recovered.at("pose").at("autonomous_pair");
      const bool leader_ready =
          recovered.value("state", "") == "armed" && recovered.value("armed", false) &&
          recovered.at("pose").value("phase", "") == "monitoring" &&
          autonomous.value("state", "") == "monitoring" &&
          autonomous.value("recovered_from_checkpoint", false) &&
          autonomous.value("replication_backlog_size", -1) == 0 &&
          autonomous.value("last_completed_shared_session_id", "") == shared_session_id &&
          autonomous.value("last_outcome", "") == "paired" &&
          autonomous.value("last_backlog_shared_session_id", "") == shared_session_id &&
          autonomous.value("last_backlog_outcome", "") == "replicated" &&
          autonomous.at("active_shared_session_id").is_null();
      const bool shadow_ready = shadow_status.value("state", "") == "armed" &&
                                shadow_status.value("armed", false) &&
                                shadow_status.at("pose").value("phase", "") == "monitoring" &&
                                shadow_status.at("pose").value("mode", "") == "shadow";
      if (leader_ready && shadow_ready) {
        recovery_ready = true;
        WriteArtifact(OutputDirectory() / leader.role / "autonomous-after-restart.json",
                      leader_response.body);
        WriteArtifact(OutputDirectory() / shadow.role / "autonomous-after-restart.json",
                      shadow_response.body);
        break;
      }
    } catch (const std::exception &) {
      // The leader's HTTP listener is expected to disappear briefly during force-stop.
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  const Json &recovered_autonomous = recovered.at("pose").at("autonomous_pair");
  if (!recovery_ready ||
      recovered_autonomous.value("last_completed_shared_session_id", "") != shared_session_id ||
      recovered_autonomous.value("last_outcome", "") != "paired" ||
      recovered_autonomous.value("replication_backlog_size", -1) != 0) {
    throw std::runtime_error("autonomous restart recovery did not finish the existing session");
  }
  const HttpResponse peer_record =
      RequireNodeHttp({.port = shadow.host_port,
                       .method = "GET",
                       .path = "/api/v1/coordination/" + shared_session_id,
                       .bearer_token = shadow.control_token,
                       .body = {},
                       .deadline = restart_deadline},
                      200);
  const Json peer_record_json = Json::parse(peer_record.body);
  if (peer_record_json.value("shared_session_id", "") != shared_session_id) {
    throw std::runtime_error("peer replicated a coordination record for another shared session");
  }
  WriteArtifact(OutputDirectory() / shadow.role / "replicated-coordination.json", peer_record.body);
  const std::int64_t restart_milliseconds = ElapsedMilliseconds(restart_started);

  const auto cleanup_started = std::chrono::steady_clock::now();
  const auto cleanup_deadline = cleanup_started + kStageDeadline;
  DisarmPoseStandby(leader, cleanup_deadline);
  DisarmPoseStandby(shadow, cleanup_deadline);
  bool leader_stopped = false;
  while (std::chrono::steady_clock::now() < cleanup_deadline) {
    const HttpResponse status = RequireNodeHttp({.port = leader.host_port,
                                                 .method = "GET",
                                                 .path = "/api/v1/capture/status",
                                                 .bearer_token = leader.control_token,
                                                 .body = {},
                                                 .deadline = cleanup_deadline},
                                                200);
    const Json parsed = Json::parse(status.body);
    if (!parsed.value("armed", true) &&
        parsed.at("pose").at("autonomous_pair").value("state", "") == "stopped") {
      leader_stopped = true;
      break;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (!leader_stopped) {
    throw std::runtime_error("autonomous restart HIL did not durably stop before cleanup");
  }
  for (const ConcurrentNode *node : {&leader, &shadow}) {
    RunRequiredAdb(
        adb,
        DeviceArguments(node->serial, {"shell", "run-as", kPackageName, "rm", "-f",
                                       "files/coordination/" + shared_session_id + ".json"}),
        cleanup_deadline);
  }
  cleanup.RestoreNodeConfigurationsChecked();
  const std::int64_t cleanup_milliseconds = ElapsedMilliseconds(cleanup_started);

  const Json report = {
      {"schema_version", 1},
      {"report_type", "android_dual_phone_autonomous_restart_recovery_hil"},
      {"passed", true},
      {"shared_session_id", shared_session_id},
      {"same_shared_session_recovered", true},
      {"new_shared_session_synthesized", false},
      {"local_immutable_record_survived", true},
      {"replication_backlog_survived", true},
      {"peer_replication_acknowledged", true},
      {"replication_backlog_cleared", true},
      {"automatic_peer_standby_and_rearm", true},
      {"conflict_fail_closed_coverage", "hermetic_software"},
      {"stages",
       {{"peer_setup", StageJson(setup_milliseconds)},
        {"checkpoint_seed", StageJson(seed_milliseconds)},
        {"force_stop_restart_recovery", StageJson(restart_milliseconds)},
        {"safe_cleanup", StageJson(cleanup_milliseconds)}}},
      {"artifacts",
       {{"before_restart", "face_on/autonomous-before-restart.json"},
        {"after_restart", "face_on/autonomous-after-restart.json"},
        {"shadow_after_restart", "down_the_line/autonomous-after-restart.json"},
        {"peer_coordination", "down_the_line/replicated-coordination.json"}}},
  };
  WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
  std::cout << "Published bounded autonomous restart-recovery evidence\n";
  return 0;
}

int RunAutonomousDisturbance(int argument_count, char **arguments) {
  if (argument_count != 4 || std::string_view(arguments[3]) != "autonomous-disturbance") {
    throw std::runtime_error("expected Bazel runfiles: <adb> <APK> autonomous-disturbance");
  }
  const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
  const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
  if (!std::filesystem::is_regular_file(adb) || access(adb.c_str(), X_OK) != 0 ||
      !std::filesystem::is_regular_file(apk)) {
    throw std::runtime_error("autonomous disturbance HIL runfiles are missing");
  }
  const std::string leader_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_SERIAL");
  const std::string shadow_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_SERIAL");
  if (leader_serial == shadow_serial || leader_serial.contains(':') ||
      shadow_serial.contains(':')) {
    throw std::runtime_error("autonomous disturbance HIL requires two distinct USB ADB serials");
  }
  const LanOrigin shadow_lan =
      ParseLanOrigin(RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN"),
                     "SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN");

  StopBothGuard cleanup(adb, shadow_serial, leader_serial);
  WifiEnableGuard shadow_wifi(adb, shadow_serial, "down_the_line", &cleanup);
  StopPackage(adb, leader_serial);
  StopPackage(adb, shadow_serial);
  cleanup.SnapshotNodeConfigurations();
  ConcurrentNode leader =
      ConfigureConcurrentNode(adb, apk, leader_serial, "face_on", &cleanup, true);
  ConcurrentNode shadow =
      ConfigureConcurrentNode(adb, apk, shadow_serial, "down_the_line", &cleanup, true);
  if (leader.identity.node_id == shadow.identity.node_id) {
    throw std::runtime_error("autonomous disturbance HIL nodes reuse one persistent identity");
  }

  const auto setup_started = std::chrono::steady_clock::now();
  const auto setup_deadline = setup_started + kStageDeadline;
  ConfigurePoseMode(shadow, "shadow", std::nullopt, std::nullopt, setup_deadline);
  ConfigurePeerPoseModeWithRetries(leader, shadow_lan.origin, shadow.control_token, setup_deadline);
  const std::int64_t setup_milliseconds = ElapsedMilliseconds(setup_started);

  const std::string shared_session_id =
      "disturbance-hil-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
  if (!SafeSessionId(shared_session_id)) {
    throw std::runtime_error("autonomous disturbance HIL generated an unsafe shared session ID");
  }

  const auto interruption_started = std::chrono::steady_clock::now();
  const auto interruption_deadline = interruption_started + kStageDeadline;
  RegisterAutonomousRecoveryCleanup(cleanup, adb, leader.serial, shadow.serial, shared_session_id,
                                    interruption_deadline);
  shadow_wifi.Disable(interruption_deadline);
  RunRequiredAdb(
      adb,
      DeviceArguments(
          leader.serial,
          {"shell", "am", "start", "-W", "--activity-single-top", "-n",
           "com.agoessling.swingcapture/.MainActivity", "--ez", "enable_pose_arm_hil", "true",
           "--ez", "prepare_autonomous_recovery_hil", "true", "--es", "shared_session_id",
           shared_session_id, "--es", "peer_node_id", shadow.identity.node_id}),
      interruption_deadline);
  bool checkpoint_ready = false;
  while (std::chrono::steady_clock::now() < interruption_deadline) {
    const HttpResponse response = RequireNodeHttp({.port = leader.host_port,
                                                   .method = "GET",
                                                   .path = "/api/v1/capture/status",
                                                   .bearer_token = leader.control_token,
                                                   .body = {},
                                                   .deadline = interruption_deadline},
                                                  200);
    WriteArtifact(OutputDirectory() / leader.role / "disturbance-checkpoint-latest.json",
                  response.body);
    const Json status = Json::parse(response.body);
    const Json &autonomous = status.at("pose").at("autonomous_pair");
    if (autonomous.value("state", "") == "storing_local_record" &&
        autonomous.value("active_shared_session_id", "") == shared_session_id &&
        autonomous.value("replication_backlog_size", 0) == 1) {
      checkpoint_ready = true;
      WriteArtifact(OutputDirectory() / leader.role / "disturbance-checkpoint.json", response.body);
      break;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (!checkpoint_ready) {
    throw std::runtime_error("autonomous disturbance HIL checkpoint was not prepared");
  }
  const std::int64_t interruption_milliseconds = ElapsedMilliseconds(interruption_started);

  const auto local_only_started = std::chrono::steady_clock::now();
  const auto local_only_deadline = local_only_started + kStageDeadline;
  StopPackage(adb, leader.serial);
  RunRequiredAdb(adb, ConfigureArguments(leader.serial, leader.role, leader.profile, true),
                 local_only_deadline);
  bool local_only_ready = false;
  while (std::chrono::steady_clock::now() < local_only_deadline) {
    try {
      const HttpResponse response = RequireNodeHttp({.port = leader.host_port,
                                                     .method = "GET",
                                                     .path = "/api/v1/capture/status",
                                                     .bearer_token = leader.control_token,
                                                     .body = {},
                                                     .deadline = local_only_deadline},
                                                    200);
      WriteArtifact(OutputDirectory() / leader.role / "disturbance-local-only-latest.json",
                    response.body);
      ValidateAutonomousLocalOnlyRecovery(AutonomousLocalOnlyRecoveryInspection{
          .leader_status_json = response.body, .expected_shared_session_id = shared_session_id});
      local_only_ready = true;
      WriteArtifact(OutputDirectory() / leader.role / "disturbance-local-only.json", response.body);
      break;
    } catch (const std::exception &) {
      // HTTP restarts and intermediate lifecycle generations are expected in this stage.
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (!local_only_ready) {
    throw std::runtime_error(
        "Wi-Fi interruption did not produce durable local-only recovery evidence");
  }
  const std::int64_t local_only_milliseconds = ElapsedMilliseconds(local_only_started);

  const auto wifi_recovery_started = std::chrono::steady_clock::now();
  const auto wifi_recovery_deadline = wifi_recovery_started + kStageDeadline;
  shadow_wifi.Enable(wifi_recovery_deadline);
  bool wifi_recovered = false;
  while (std::chrono::steady_clock::now() < wifi_recovery_deadline) {
    try {
      const HttpResponse leader_response = RequireNodeHttp({.port = leader.host_port,
                                                            .method = "GET",
                                                            .path = "/api/v1/capture/status",
                                                            .bearer_token = leader.control_token,
                                                            .body = {},
                                                            .deadline = wifi_recovery_deadline},
                                                           200);
      const HttpResponse shadow_response = RequireNodeHttp({.port = shadow.host_port,
                                                            .method = "GET",
                                                            .path = "/api/v1/capture/status",
                                                            .bearer_token = shadow.control_token,
                                                            .body = {},
                                                            .deadline = wifi_recovery_deadline},
                                                           200);
      WriteArtifact(OutputDirectory() / leader.role / "disturbance-wifi-recovered-latest.json",
                    leader_response.body);
      WriteArtifact(OutputDirectory() / shadow.role / "disturbance-wifi-recovered-latest.json",
                    shadow_response.body);
      ValidateAutonomousRecoveredStation(
          AutonomousRecoveredStationInspection{.leader_status_json = leader_response.body,
                                               .shadow_status_json = shadow_response.body,
                                               .expected_shared_session_id = shared_session_id});
      wifi_recovered = true;
      WriteArtifact(OutputDirectory() / leader.role / "disturbance-wifi-recovered.json",
                    leader_response.body);
      WriteArtifact(OutputDirectory() / shadow.role / "disturbance-wifi-recovered.json",
                    shadow_response.body);
      break;
    } catch (const std::exception &) {
      // Wi-Fi association, peer HTTP restart, and GPU warmup are independently asynchronous.
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (!wifi_recovered) {
    throw std::runtime_error(
        "restored Wi-Fi did not replicate the local-only backlog and rearm both phones");
  }
  const HttpResponse peer_record =
      RequireNodeHttp({.port = shadow.host_port,
                       .method = "GET",
                       .path = "/api/v1/coordination/" + shared_session_id,
                       .bearer_token = shadow.control_token,
                       .body = {},
                       .deadline = wifi_recovery_deadline},
                      200);
  if (Json::parse(peer_record.body).value("shared_session_id", "") != shared_session_id) {
    throw std::runtime_error("Wi-Fi recovery replicated a different coordination record");
  }
  WriteArtifact(OutputDirectory() / shadow.role / "disturbance-replicated-coordination.json",
                peer_record.body);
  const std::int64_t wifi_recovery_milliseconds = ElapsedMilliseconds(wifi_recovery_started);

  const auto peer_outage_started = std::chrono::steady_clock::now();
  const auto peer_outage_deadline = peer_outage_started + kStageDeadline;
  StopPackage(adb, shadow.serial);
  bool peer_outage_observed = false;
  while (std::chrono::steady_clock::now() < peer_outage_deadline) {
    try {
      const HttpResponse response = RequireNodeHttp({.port = leader.host_port,
                                                     .method = "GET",
                                                     .path = "/api/v1/capture/status",
                                                     .bearer_token = leader.control_token,
                                                     .body = {},
                                                     .deadline = peer_outage_deadline},
                                                    200);
      ValidateAutonomousPeerUnavailable(response.body);
      peer_outage_observed = true;
      WriteArtifact(OutputDirectory() / leader.role / "disturbance-peer-stopped.json",
                    response.body);
      break;
    } catch (const std::exception &) {
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (!peer_outage_observed) {
    throw std::runtime_error("leader did not detect the force-stopped peer app");
  }
  const std::int64_t peer_outage_milliseconds = ElapsedMilliseconds(peer_outage_started);

  const auto peer_restart_started = std::chrono::steady_clock::now();
  const auto peer_restart_deadline = peer_restart_started + kStageDeadline;
  RunRequiredAdb(adb, ConfigureArguments(shadow.serial, shadow.role, shadow.profile, true),
                 peer_restart_deadline);
  bool peer_restart_recovered = false;
  while (std::chrono::steady_clock::now() < peer_restart_deadline) {
    try {
      const HttpResponse leader_response = RequireNodeHttp({.port = leader.host_port,
                                                            .method = "GET",
                                                            .path = "/api/v1/capture/status",
                                                            .bearer_token = leader.control_token,
                                                            .body = {},
                                                            .deadline = peer_restart_deadline},
                                                           200);
      const HttpResponse shadow_response = RequireNodeHttp({.port = shadow.host_port,
                                                            .method = "GET",
                                                            .path = "/api/v1/capture/status",
                                                            .bearer_token = shadow.control_token,
                                                            .body = {},
                                                            .deadline = peer_restart_deadline},
                                                           200);
      WriteArtifact(OutputDirectory() / leader.role / "disturbance-peer-restarted-latest.json",
                    leader_response.body);
      WriteArtifact(OutputDirectory() / shadow.role / "disturbance-peer-restarted-latest.json",
                    shadow_response.body);
      ValidateAutonomousRecoveredStation(
          AutonomousRecoveredStationInspection{.leader_status_json = leader_response.body,
                                               .shadow_status_json = shadow_response.body,
                                               .expected_shared_session_id = shared_session_id});
      peer_restart_recovered = true;
      WriteArtifact(OutputDirectory() / leader.role / "disturbance-peer-restarted.json",
                    leader_response.body);
      WriteArtifact(OutputDirectory() / shadow.role / "disturbance-peer-restarted.json",
                    shadow_response.body);
      break;
    } catch (const std::exception &) {
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (!peer_restart_recovered) {
    throw std::runtime_error("station did not recover and rearm after the peer app restart");
  }
  const std::int64_t peer_restart_milliseconds = ElapsedMilliseconds(peer_restart_started);

  const auto cleanup_started = std::chrono::steady_clock::now();
  const auto cleanup_deadline = cleanup_started + kStageDeadline;
  DisarmPoseStandby(leader, cleanup_deadline);
  DisarmPoseStandby(shadow, cleanup_deadline);
  bool stopped = false;
  while (std::chrono::steady_clock::now() < cleanup_deadline) {
    const HttpResponse response = RequireNodeHttp({.port = leader.host_port,
                                                   .method = "GET",
                                                   .path = "/api/v1/capture/status",
                                                   .bearer_token = leader.control_token,
                                                   .body = {},
                                                   .deadline = cleanup_deadline},
                                                  200);
    const Json status = Json::parse(response.body);
    if (!status.value("armed", true) &&
        status.at("pose").at("autonomous_pair").value("state", "") == "stopped") {
      stopped = true;
      break;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (!stopped) {
    throw std::runtime_error("autonomous disturbance HIL did not durably stop before cleanup");
  }
  for (const ConcurrentNode *node : {&leader, &shadow}) {
    RunRequiredAdb(
        adb,
        DeviceArguments(node->serial, {"shell", "run-as", kPackageName, "rm", "-f",
                                       "files/coordination/" + shared_session_id + ".json"}),
        cleanup_deadline);
  }
  cleanup.RestoreNodeConfigurationsChecked();
  const std::int64_t cleanup_milliseconds = ElapsedMilliseconds(cleanup_started);

  const Json report = {
      {"schema_version", 1},
      {"report_type", "android_dual_phone_autonomous_disturbance_hil"},
      {"passed", true},
      {"shared_session_id", shared_session_id},
      {"wifi_interruption", true},
      {"valid_local_evidence_preserved", true},
      {"local_only_backlog_preserved", true},
      {"authenticated_peer_recovered", true},
      {"backlog_replicated_after_wifi_restore", true},
      {"peer_app_force_stopped_and_restarted", true},
      {"automatic_rearm", true},
      {"stages",
       {{"peer_setup", StageJson(setup_milliseconds)},
        {"wifi_interruption_and_checkpoint", StageJson(interruption_milliseconds)},
        {"local_only_recovery", StageJson(local_only_milliseconds)},
        {"wifi_restore_and_replication", StageJson(wifi_recovery_milliseconds)},
        {"peer_app_outage_detection", StageJson(peer_outage_milliseconds)},
        {"peer_app_restart_recovery", StageJson(peer_restart_milliseconds)},
        {"safe_cleanup", StageJson(cleanup_milliseconds)}}},
      {"artifacts",
       {{"checkpoint", "face_on/disturbance-checkpoint.json"},
        {"local_only", "face_on/disturbance-local-only.json"},
        {"wifi_recovered_leader", "face_on/disturbance-wifi-recovered.json"},
        {"wifi_recovered_shadow", "down_the_line/disturbance-wifi-recovered.json"},
        {"peer_stopped", "face_on/disturbance-peer-stopped.json"},
        {"peer_restarted_leader", "face_on/disturbance-peer-restarted.json"},
        {"peer_restarted_shadow", "down_the_line/disturbance-peer-restarted.json"},
        {"peer_coordination", "down_the_line/disturbance-replicated-coordination.json"}}},
  };
  WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
  std::cout << "Published bounded autonomous disturbance evidence\n";
  return 0;
}

DisplayPowerStateInspection ReadQualificationDisplayState(
    const std::filesystem::path &adb, const ConcurrentNode &node,
    std::chrono::steady_clock::time_point deadline) {
  const std::string power =
      RunRequiredAdb(adb, DeviceArguments(node.serial, {"shell", "dumpsys", "power"}), deadline);
  const std::string display =
      RunRequiredAdb(adb, DeviceArguments(node.serial, {"shell", "dumpsys", "display"}), deadline);
  return InspectDisplayPowerState({.power = power, .display = display});
}

Json QualificationDisplayStateJson(const DisplayPowerStateInspection &inspection) {
  return {
      {"non_interactive", inspection.non_interactive},
      {"display_off", inspection.display_off},
      {"interaction_state_known", inspection.interaction_state_known},
      {"display_state_known", inspection.display_state_known},
      {"confirmed_off", inspection.confirmed_off()},
      {"confirmed_on", inspection.confirmed_on()},
      {"diagnostic", inspection.diagnostic},
  };
}

Json InspectQualificationDisplayOff(const std::filesystem::path &adb, const ConcurrentNode &node,
                                    std::chrono::steady_clock::time_point deadline) {
  const DisplayPowerStateInspection inspection = ReadQualificationDisplayState(adb, node, deadline);
  if (!inspection.confirmed_off()) {
    throw std::runtime_error("paired qualification display is not off on " + node.role + ": " +
                             inspection.diagnostic);
  }
  return QualificationDisplayStateJson(inspection);
}

class QualificationDisplayGuard {
 public:
  QualificationDisplayGuard(std::filesystem::path adb, StopBothGuard *cleanup)
      : adb_(std::move(adb)), cleanup_(cleanup) {
    if (cleanup_ == nullptr) {
      throw std::invalid_argument("qualification display cleanup owner is required");
    }
  }

  QualificationDisplayGuard(const QualificationDisplayGuard &) = delete;
  QualificationDisplayGuard &operator=(const QualificationDisplayGuard &) = delete;

  Json TurnOffAndVerify(const ConcurrentNode &node,
                        std::chrono::steady_clock::time_point deadline) {
    const DisplayPowerStateInspection initial = ReadQualificationDisplayState(adb_, node, deadline);
    if (initial.confirmed_off()) {
      Json evidence = QualificationDisplayStateJson(initial);
      evidence["initial_state"] = "off";
      evidence["sleep_command_sent"] = false;
      evidence["wake_restore_required"] = false;
      return evidence;
    }
    if (!initial.confirmed_on()) {
      throw std::runtime_error("qualification display initial state is inconsistent on " +
                               node.role + ": " + initial.diagnostic);
    }
    cleanup_->RegisterExternalCleanup("display_wake", node.role);
    sleeping_nodes_.push_back({node.serial, node.role});
    RunRequiredAdb(adb_, DeviceArguments(node.serial, {"shell", "input", "keyevent", "223"}),
                   deadline);
    std::string latest_diagnostic = "display has not been inspected";
    while (std::chrono::steady_clock::now() < deadline) {
      try {
        Json evidence = InspectQualificationDisplayOff(adb_, node, deadline);
        evidence["initial_state"] = "on";
        evidence["sleep_command_sent"] = true;
        evidence["wake_restore_required"] = true;
        return evidence;
      } catch (const std::runtime_error &failure) {
        latest_diagnostic = failure.what();
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    throw std::runtime_error("could not confirm qualification screen-off state for " + node.role +
                             ": " + latest_diagnostic);
  }

  void RestoreChecked() {
    std::string first_failure;
    for (const auto &[serial, role] : sleeping_nodes_) {
      try {
        RunRequiredAdb(adb_,
                       DeviceArguments(serial, {"shell", "input", "keyevent", "KEYCODE_WAKEUP"}),
                       std::chrono::steady_clock::now() + 2s);
        cleanup_->RecordExternalCleanupRestored("display_wake", role);
      } catch (const std::exception &failure) {
        cleanup_->RecordExternalCleanupFailed("display_wake", role, failure.what());
        if (first_failure.empty()) {
          first_failure = failure.what();
        }
      }
    }
    sleeping_nodes_.clear();
    if (!first_failure.empty()) {
      throw std::runtime_error("could not restore a qualification display: " + first_failure);
    }
  }

  ~QualificationDisplayGuard() {
    for (const auto &[serial, role] : sleeping_nodes_) {
      try {
        RunRequiredAdb(adb_,
                       DeviceArguments(serial, {"shell", "input", "keyevent", "KEYCODE_WAKEUP"}),
                       std::chrono::steady_clock::now() + 2s);
        cleanup_->RecordExternalCleanupRestored("display_wake", role);
      } catch (const std::exception &failure) {
        try {
          cleanup_->RecordExternalCleanupFailed("display_wake", role, failure.what());
        } catch (...) {
        }
      } catch (...) {
        try {
          cleanup_->RecordExternalCleanupFailed("display_wake", role,
                                                "unknown display wake failure");
        } catch (...) {
        }
      }
    }
  }

 private:
  std::filesystem::path adb_;
  StopBothGuard *cleanup_;
  std::vector<std::pair<std::string, std::string>> sleeping_nodes_;
};

std::uint64_t QualificationCounterDelta(std::uint64_t previous, std::uint64_t current,
                                        std::string_view name) {
  if (current < previous) {
    throw std::runtime_error("qualification pose counter moved backwards: " + std::string(name));
  }
  return current - previous;
}

struct QualificationNodeTelemetry {
  PoseStatusSample pose;
  Json report;
};

QualificationNodeTelemetry ReadQualificationNodeTelemetry(
    const std::filesystem::path &adb, const ConcurrentNode &node,
    const std::optional<PoseStatusSample> &previous, bool cadence_eligible,
    std::chrono::steady_clock::time_point deadline) {
  const HttpResponse status_response = RequireNodeHttp({.port = node.host_port,
                                                        .method = "GET",
                                                        .path = "/api/v1/capture/status",
                                                        .bearer_token = node.control_token,
                                                        .body = {},
                                                        .deadline = deadline},
                                                       200);
  const PoseStatusSample pose = InspectPoseStatus(status_response.body);
  const std::string expected_mode = node.role == "face_on" ? "leader" : "shadow";
  if (!pose.valid || pose.state != "armed" || !pose.armed || pose.phase != "monitoring" ||
      pose.mode != expected_mode || !pose.standby_audio_ready) {
    throw std::runtime_error("qualification pose status is not armed monitoring for " + node.role +
                             ": " + pose.diagnostic);
  }
  const std::string battery =
      RunRequiredAdb(adb, DeviceArguments(node.serial, {"shell", "dumpsys", "battery"}), deadline);
  const std::string thermal = RunRequiredAdb(
      adb, DeviceArguments(node.serial, {"shell", "dumpsys", "thermalservice"}), deadline);
  const DeviceTelemetry device = InspectDeviceTelemetry(battery, thermal);
  if (!device.valid || !device.processor_thermal_complete) {
    throw std::runtime_error("qualification device telemetry is invalid for " + node.role + ": " +
                             device.diagnostic);
  }
  const Json screen = InspectQualificationDisplayOff(adb, node, deadline);

  std::uint64_t offered_delta = 0;
  std::uint64_t successful_delta = 0;
  std::uint64_t failed_delta = 0;
  std::uint64_t dropped_delta = 0;
  std::uint64_t deadline_miss_delta = 0;
  std::uint64_t outlier_delta = 0;
  std::uint64_t decision_age_samples_delta = 0;
  std::uint64_t rejected_decision_timestamps_delta = 0;
  std::uint64_t process_cpu_time_delta_ms = 0;
  double interval_seconds = 0.0;
  const bool metrics_generation_reset = previous.has_value() && !cadence_eligible;
  if (previous.has_value()) {
    if (pose.server_elapsed_realtime_ns <= previous->server_elapsed_realtime_ns) {
      throw std::runtime_error("qualification device monotonic clock did not advance");
    }
    interval_seconds = static_cast<double>(pose.server_elapsed_realtime_ns -
                                           previous->server_elapsed_realtime_ns) /
                       1'000'000'000.0;
    const auto generation_delta = [&](std::uint64_t prior, std::uint64_t current,
                                      std::string_view name) {
      return metrics_generation_reset ? current : QualificationCounterDelta(prior, current, name);
    };
    offered_delta =
        generation_delta(previous->offered_images, pose.offered_images, "offered_images");
    successful_delta = generation_delta(previous->successful_inferences, pose.successful_inferences,
                                        "successful_inferences");
    failed_delta =
        generation_delta(previous->failed_inferences, pose.failed_inferences, "failed_inferences");
    dropped_delta =
        generation_delta(previous->dropped_images, pose.dropped_images, "dropped_images");
    deadline_miss_delta =
        generation_delta(previous->inference_deadline_misses, pose.inference_deadline_misses,
                         "inference_deadline_misses");
    outlier_delta = generation_delta(previous->inference_outliers, pose.inference_outliers,
                                     "inference_outliers");
    decision_age_samples_delta = generation_delta(
        previous->decision_age_samples, pose.decision_age_samples, "decision_age_samples");
    rejected_decision_timestamps_delta =
        generation_delta(previous->rejected_decision_timestamps, pose.rejected_decision_timestamps,
                         "rejected_decision_timestamps");
    process_cpu_time_delta_ms = QualificationCounterDelta(
        previous->process_cpu_time_ms, pose.process_cpu_time_ms, "process_cpu_time_ms");
  }
  const double successful_rate =
      interval_seconds == 0.0 ? 0.0 : static_cast<double>(successful_delta) / interval_seconds;
  const double dropped_fraction =
      offered_delta == 0 ? 0.0 : static_cast<double>(dropped_delta) / offered_delta;
  const double process_cpu_utilization_percent =
      interval_seconds == 0.0
          ? 0.0
          : (static_cast<double>(process_cpu_time_delta_ms) / 1000.0) / interval_seconds * 100.0;
  Json report = {
      {"screen_off", screen.at("confirmed_off")},
      {"screen_state", std::move(screen)},
      {"phase", pose.phase},
      {"state", pose.state},
      {"actual_delegate", pose.actual_delegate},
      {"configured_delegate", pose.configured_delegate},
      {"thermal_status", device.thermal_status},
      {"battery_level_percent", device.battery_level_percent},
      {"battery_temperature_celsius", device.battery_temperature_celsius},
      {"battery_voltage_millivolts", device.battery_voltage_millivolts},
      {"processor_thermal_complete", device.processor_thermal_complete},
      {"maximum_cpu_temperature_celsius", device.maximum_cpu_temperature_celsius},
      {"maximum_gpu_temperature_celsius", device.maximum_gpu_temperature_celsius},
      {"maximum_cpu_cooling_device_value", device.maximum_cpu_cooling_device_value},
      {"maximum_gpu_cooling_device_value", device.maximum_gpu_cooling_device_value},
      {"server_elapsed_realtime_ns", std::to_string(pose.server_elapsed_realtime_ns)},
      {"process_cpu_time_ms", pose.process_cpu_time_ms},
      {"process_cpu_time_delta_ms", process_cpu_time_delta_ms},
      {"process_cpu_utilization_percent", process_cpu_utilization_percent},
      {"interval_seconds", interval_seconds},
      {"offered_images_delta", offered_delta},
      {"successful_inferences_delta", successful_delta},
      {"failed_inferences_delta", failed_delta},
      {"dropped_images_delta", dropped_delta},
      {"inference_deadline_misses_delta", deadline_miss_delta},
      {"inference_outliers_delta", outlier_delta},
      {"decision_age_samples_delta", decision_age_samples_delta},
      {"rejected_decision_timestamps_delta", rejected_decision_timestamps_delta},
      {"successful_inferences_per_second", successful_rate},
      {"dropped_fraction", dropped_fraction},
      {"cadence_eligible", previous.has_value() && cadence_eligible},
      {"metrics_generation_reset", metrics_generation_reset},
      {"inference_duration_p95_ns", pose.inference_duration_p95_ns},
      {"maximum_inference_duration_ns", pose.maximum_inference_duration_ns},
      {"decision_age_p95_ns", pose.decision_age_p95_ns},
      {"maximum_decision_age_ns", pose.maximum_decision_age_ns},
      {"standby_audio_end_frame_position", std::to_string(pose.standby_audio_end_frame_position)},
      {"standby_audio_dropped_events", pose.standby_audio_dropped_events},
      {"standby_audio_timestamp_rejections", pose.standby_audio_timestamp_rejections},
      {"standby_audio_discontinuities", pose.standby_audio_discontinuities},
      {"operational_health", Json::parse(status_response.body).at("operational_health")},
  };
  return {.pose = pose, .report = std::move(report)};
}

Json QualificationNodeCaptureJson(CapturedNode *captured,
                                  const std::filesystem::path &cycle_directory,
                                  const std::filesystem::path &cycle_relative_directory,
                                  std::string_view audio_filename, const ExternalTools &tools) {
  const std::filesystem::path node_directory = cycle_directory / captured->evidence.role;
  const std::filesystem::path relative_node_directory =
      cycle_relative_directory / captured->evidence.role;
  const std::filesystem::path media_filename = captured->evidence.role + ".mp4";
  WriteArtifact(node_directory / "report.json", captured->report);
  WriteArtifact(node_directory / "manifest.json", captured->manifest);
  WriteArtifact(node_directory / media_filename, captured->media);
  WriteArtifact(node_directory / std::string(audio_filename), captured->audio_wav);
  AnalyzeNodeMedia(captured, node_directory, tools);
  ValidateCompletedPcmNode(*captured);
  const TimingCorrelationEvidence timing = PcmMarkerCorrelation(*captured);
  return {
      {"passed", true},
      {"node_id", captured->evidence.node_id},
      {"role", captured->evidence.role},
      {"local_session_id", captured->evidence.local_session_id},
      {"shared_session_id", captured->evidence.shared_session_id},
      {"frame_count", captured->evidence.frame_count},
      {"measured_sensor_fps", captured->evidence.measured_sensor_fps},
      {"actual_pre_roll_us", captured->evidence.actual_pre_roll_us},
      {"actual_post_roll_us", captured->evidence.actual_post_roll_us},
      {"startup_timing", StartupTimingJson(captured->evidence.startup_timing)},
      {"encoded_bytes", captured->evidence.media_bytes},
      {"audio_sample_rate_hz", captured->evidence.audio_sample_rate_hz},
      {"audio_peak_amplitude", captured->evidence.audio_peak_amplitude},
      {"decoded_video",
       {{"passed", true},
        {"exact_frame_count", true},
        {"decoded_frame_count", captured->optical.decoded_frame_count},
        {"manifest_frame_count", captured->evidence.frame_count},
        {"maximum_media_time_residual_us", captured->maximum_media_time_residual_us},
        {"analysis_width", captured->analysis_width},
        {"analysis_height", captured->analysis_height}}},
      {"optical",
       {{"passed", captured->optical.detected},
        {"timing_correlation_passed", timing.passed},
        {"peak_frame_index", captured->optical.peak_frame_index},
        {"maximum_white_delta", captured->optical.maximum_white_delta},
        {"peak_tile", {{"x", captured->optical.tile_x}, {"y", captured->optical.tile_y}}},
        {"localized_response_tile_count", captured->optical.localized_response_tile_count}}},
      {"april_tag", {{"passed", true}, {"persistent_pre_marker_post", true}}},
      {"report", (relative_node_directory / "report.json").generic_string()},
      {"manifest", (relative_node_directory / "manifest.json").generic_string()},
      {"media", (relative_node_directory / media_filename).generic_string()},
      {"audio", (relative_node_directory / std::string(audio_filename)).generic_string()},
      {"ffprobe", (relative_node_directory / "ffprobe.json").generic_string()},
      {"diagnostic_frames",
       {{"pre", (relative_node_directory / "diagnostic-01.png").generic_string()},
        {"marker", (relative_node_directory / "diagnostic-02.png").generic_string()},
        {"post", (relative_node_directory / "diagnostic-03.png").generic_string()}}},
  };
}

Json RunPairedPoseQualificationCycle(const std::filesystem::path &adb, ConcurrentNode &leader,
                                     ConcurrentNode &shadow, PreparedPcmReplay *prepared_pcm,
                                     StopBothGuard *cleanup, std::size_t cycle_index,
                                     std::chrono::steady_clock::time_point qualification_started,
                                     const ExternalTools &tools) {
  const auto cycle_started = std::chrono::steady_clock::now();
  const std::filesystem::path cycle_relative_directory =
      std::filesystem::path("cycles") / ("cycle-" + std::to_string(cycle_index));
  const std::filesystem::path cycle_directory = OutputDirectory() / cycle_relative_directory;
  CollectClockExchanges(&leader, &shadow, cycle_started + kStageDeadline);
  WriteArtifact(cycle_directory / leader.role / "clock.json",
                ClockEvidenceJson(leader).dump(2) + "\n");
  WriteArtifact(cycle_directory / shadow.role / "clock.json",
                ClockEvidenceJson(shadow).dump(2) + "\n");
  const auto baseline_deadline = std::chrono::steady_clock::now() + kStageDeadline;
  const auto leader_baseline = ReadyCaptureSessionIds(leader, baseline_deadline);
  const auto shadow_baseline = ReadyCaptureSessionIds(shadow, baseline_deadline);
  const auto transition_deadline = std::chrono::steady_clock::now() + kStageDeadline;
  const HttpResponse transition = RequireNodeHttp({.port = leader.host_port,
                                                   .method = "POST",
                                                   .path = "/api/v1/hil/pose-arm",
                                                   .bearer_token = leader.control_token,
                                                   .body = {},
                                                   .deadline = transition_deadline},
                                                  202);
  const Json transition_json = Json::parse(transition.body);
  const std::string shared_session_id = transition_json.value("shared_session_id", "");
  if (transition_json.value("schema_version", 0) != 1 ||
      transition_json.value("state", "") != "transitioning_to_high_speed" ||
      !SafeSessionId(shared_session_id)) {
    throw std::runtime_error("qualification pose-arm response is invalid");
  }
  RegisterAutonomousSessionCleanup(*cleanup, leader.serial, shadow.serial, shared_session_id);
  static_cast<void>(
      WaitForPairedPosePhase(leader, shadow, "high_speed", shared_session_id, transition_deadline));

  const auto trigger_started = std::chrono::steady_clock::now();
  const auto trigger_deadline = trigger_started + kStageDeadline;
  const std::int64_t recalibration_milliseconds =
      RecalibratePreparedPcmReplay(prepared_pcm, OutputDirectory());
  const auto playback = PlayPreparedPcmReplay(prepared_pcm, OutputDirectory());
  const PcmAutomaticTriggerObservation automatic_observation =
      ObserveAutomaticPcmTrigger(leader, shadow, trigger_deadline);
  if (automatic_observation.outcome != PcmReplayPairOutcome::kBoth) {
    throw std::runtime_error(
        "qualification PCM replay lacked a complete paired automatic trigger (outcome=" +
        std::string(PcmReplayPairOutcomeName(automatic_observation.outcome)) + ")");
  }
  const Json mapped_impact =
      ReadMappedPeerImpactEvidence(leader, shadow, shared_session_id, trigger_deadline);
  const std::array<std::string, 2> session_ids =
      WaitForNewPoseSessions(leader, shadow, leader_baseline, shadow_baseline, trigger_deadline);
  const std::int64_t capture_milliseconds = ElapsedMilliseconds(trigger_started);

  const auto report_deadline = std::chrono::steady_clock::now() + kStageDeadline;
  std::array<std::string, 2> reports = {
      BuildPoseCaptureReport(adb, leader, session_ids[0], report_deadline),
      BuildPoseCaptureReport(adb, shadow, session_ids[1], report_deadline),
  };
  CapturedNode leader_capture =
      PullConcurrentPcmNode(adb, leader, shared_session_id, std::move(reports[0]), playback,
                            capture_milliseconds, "local_audio");
  CapturedNode shadow_capture = PullConcurrentPcmNode(
      adb, shadow, shared_session_id, std::move(reports[1]), playback, capture_milliseconds,
      "peer_audio_clock_candidate", "diagnostic_audio.wav");
  ValidateDualSession(shadow_capture.evidence, leader_capture.evidence);
  const Json leader_peer =
      ValidatePersistedPeerArm(leader_capture.manifest, "accepted", shared_session_id);
  const Json shadow_peer =
      ValidatePersistedPeerArm(shadow_capture.manifest, "inbound_accepted", shared_session_id);
  CoordinationRunEvidence coordination =
      BuildAndPersistCoordination(shadow, leader, shadow_capture, leader_capture, shared_session_id,
                                  "peer_audio_clock_candidate", "local_audio");
  WriteArtifact(cycle_directory / "coordination.json",
                coordination::ToCanonicalJson(coordination.record) + "\n");
  Json cycle_coordination = coordination.report;
  cycle_coordination["canonical_record"] =
      (cycle_relative_directory / "coordination.json").generic_string();
  cycle_coordination["persistence"] = {{"passed", true}, {"node_count", 2}};

  const auto rearm_started = std::chrono::steady_clock::now();
  const Json rearm =
      WaitForPairedPosePhase(leader, shadow, "monitoring", {}, rearm_started + kStageDeadline);
  Json leader_node = QualificationNodeCaptureJson(
      &leader_capture, cycle_directory, cycle_relative_directory, "audio_evidence.wav", tools);
  Json shadow_node = QualificationNodeCaptureJson(
      &shadow_capture, cycle_directory, cycle_relative_directory, "diagnostic_audio.wav", tools);
  const Json cycle_artifacts = {
      {"cycle", (cycle_relative_directory / "cycle.json").generic_string()},
      {"coordination", (cycle_relative_directory / "coordination.json").generic_string()},
      {"face_on",
       {{"clock", (cycle_relative_directory / "face_on" / "clock.json").generic_string()},
        {"report", leader_node.at("report")},
        {"manifest", leader_node.at("manifest")},
        {"media", leader_node.at("media")},
        {"audio", leader_node.at("audio")},
        {"ffprobe", leader_node.at("ffprobe")},
        {"diagnostic_frames", leader_node.at("diagnostic_frames")}}},
      {"down_the_line",
       {{"clock", (cycle_relative_directory / "down_the_line" / "clock.json").generic_string()},
        {"report", shadow_node.at("report")},
        {"manifest", shadow_node.at("manifest")},
        {"media", shadow_node.at("media")},
        {"audio", shadow_node.at("audio")},
        {"ffprobe", shadow_node.at("ffprobe")},
        {"diagnostic_frames", shadow_node.at("diagnostic_frames")}}},
  };
  Json cycle = {
      {"cycle_index", cycle_index},
      {"passed", true},
      {"artifact_path_scope", "undeclared_output_root"},
      {"shared_session_id", shared_session_id},
      {"elapsed_start_seconds",
       std::chrono::duration<double>(cycle_started - qualification_started).count()},
      {"elapsed_end_seconds",
       std::chrono::duration<double>(std::chrono::steady_clock::now() - qualification_started)
           .count()},
      {"pose_to_high_speed", true},
      {"feather_audio_trigger", true},
      {"publication", {{"face_on", true}, {"down_the_line", true}}},
      {"rearmed", true},
      {"mapped_impact_evidence", mapped_impact},
      {"persisted_peer_arm", {{"face_on", leader_peer}, {"down_the_line", shadow_peer}}},
      {"rearm_inference", rearm},
      {"coordination", std::move(cycle_coordination)},
      {"stages",
       {{"feather_recalibration_milliseconds", recalibration_milliseconds},
        {"capture_milliseconds", capture_milliseconds},
        {"coordination_milliseconds", coordination.stage_milliseconds},
        {"rearm_milliseconds", ElapsedMilliseconds(rearm_started)}}},
      {"nodes", {{"face_on", std::move(leader_node)}, {"down_the_line", std::move(shadow_node)}}},
      {"clock",
       {{"face_on", cycle_artifacts.at("face_on").at("clock")},
        {"down_the_line", cycle_artifacts.at("down_the_line").at("clock")}}},
      {"artifacts", cycle_artifacts},
  };
  WriteArtifact(cycle_directory / "cycle.json", cycle.dump(2) + "\n");
  return cycle;
}

int RunPairedPoseQualification(const std::filesystem::path &adb, ConcurrentNode &leader,
                               ConcurrentNode &shadow, PreparedPcmReplay *prepared_pcm,
                               StopBothGuard *cleanup, const PairedPoseQualificationPolicy &policy,
                               Json setup_evidence, const ExternalTools &tools) {
  if (prepared_pcm->replay_case.definition.expectation != PcmReplayExpectation::kRequiredPositive) {
    throw std::runtime_error(
        "paired pose qualification requires a required-positive PCM replay case");
  }
  QualificationDisplayGuard displays(adb, cleanup);
  const Json leader_display =
      displays.TurnOffAndVerify(leader, std::chrono::steady_clock::now() + kStageDeadline);
  const Json shadow_display =
      displays.TurnOffAndVerify(shadow, std::chrono::steady_clock::now() + kStageDeadline);
  const Json screen_off = {
      {"passed", true},
      {"face_on", leader_display.at("confirmed_off")},
      {"down_the_line", shadow_display.at("confirmed_off")},
      {"evidence", {{"face_on", leader_display}, {"down_the_line", shadow_display}}},
  };

  const auto started = std::chrono::steady_clock::now();
  const auto end = started + std::chrono::seconds(policy.duration_seconds);
  auto next_telemetry = started;
  auto next_cycle = started;
  std::array<std::optional<PoseStatusSample>, 2> previous_pose;
  bool capture_since_last_sample = false;
  Json report = {
      {"schema_version", 1},
      {"report_type", policy.report_type},
      {"mode", policy.mode},
      {"passed", false},
      {"qualification_eligible", false},
      {"requested_duration_seconds", policy.duration_seconds},
      {"observed_duration_seconds", 0.0},
      {"telemetry_interval_seconds", policy.telemetry_interval_seconds},
      {"cycle_interval_seconds", policy.cycle_interval_seconds},
      {"screen_off", screen_off},
      {"high_speed_profile", "720p240"},
      {"standby_inference", "real_5hz_on_device"},
      {"peer_transport", "wifi_lan_direct"},
      {"artifact_path_scope", "undeclared_output_root"},
      {"setup", std::move(setup_evidence)},
      {"cycles", Json::array()},
      {"telemetry_samples", Json::array()},
      {"artifacts",
       {{"report", "report.json"},
        {"progress", "qualification-progress.json"},
        {"cycles", Json::array()},
        {"latest_face_on_media", "face_on/face_on.mp4"},
        {"latest_down_the_line_media", "down_the_line/down_the_line.mp4"}}},
  };
  const auto publish_progress = [&]() {
    report["observed_duration_seconds"] =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    WriteArtifact(OutputDirectory() / "qualification-progress.json", report.dump(2) + "\n");
  };
  publish_progress();

  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_telemetry) {
      const auto sample_deadline = std::chrono::steady_clock::now() + kStageDeadline;
      const bool cadence_eligible = !capture_since_last_sample;
      QualificationNodeTelemetry leader_telemetry = ReadQualificationNodeTelemetry(
          adb, leader, previous_pose[0], cadence_eligible, sample_deadline);
      QualificationNodeTelemetry shadow_telemetry = ReadQualificationNodeTelemetry(
          adb, shadow, previous_pose[1], cadence_eligible, sample_deadline);
      previous_pose = {leader_telemetry.pose, shadow_telemetry.pose};
      Json sample = {
          {"sample_index", report.at("telemetry_samples").size()},
          {"elapsed_seconds",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()},
          {"roles",
           {{"face_on", std::move(leader_telemetry.report)},
            {"down_the_line", std::move(shadow_telemetry.report)}}},
      };
      report["telemetry_samples"].push_back(std::move(sample));
      capture_since_last_sample = false;
      next_telemetry = std::chrono::steady_clock::now() +
                       std::chrono::seconds(policy.telemetry_interval_seconds);
      publish_progress();
      const Json &latest = report["telemetry_samples"].back().at("roles");
      for (const std::string_view role : {"face_on", "down_the_line"}) {
        if (latest.at(role).value("thermal_status", 6) > 2) {
          throw std::runtime_error(
              "paired pose qualification reached thermal status above "
              "MODERATE on " +
              std::string(role));
        }
      }
    }
    if (std::chrono::steady_clock::now() >= end) {
      break;
    }
    if (std::chrono::steady_clock::now() >= next_cycle) {
      const std::size_t cycle_index = report.at("cycles").size();
      Json cycle = RunPairedPoseQualificationCycle(adb, leader, shadow, prepared_pcm, cleanup,
                                                   cycle_index, started, tools);
      report["artifacts"]["cycles"].push_back(cycle.at("artifacts"));
      report["cycles"].push_back(std::move(cycle));
      capture_since_last_sample = true;
      next_cycle = started + std::chrono::seconds(static_cast<int>((cycle_index + 1U) *
                                                                   policy.cycle_interval_seconds));
      publish_progress();
      continue;
    }
    const auto wake_at = std::min({end, next_telemetry, next_cycle});
    const auto wait = std::min<std::chrono::steady_clock::duration>(
        kPollInterval, wake_at - std::chrono::steady_clock::now());
    if (wait > std::chrono::steady_clock::duration::zero()) {
      std::this_thread::sleep_for(wait);
    }
  }

  if (report.at("telemetry_samples").empty() ||
      report.at("telemetry_samples").back().value("elapsed_seconds", 0.0) <
          policy.duration_seconds) {
    const auto sample_deadline = std::chrono::steady_clock::now() + kStageDeadline;
    QualificationNodeTelemetry leader_telemetry = ReadQualificationNodeTelemetry(
        adb, leader, previous_pose[0], !capture_since_last_sample, sample_deadline);
    QualificationNodeTelemetry shadow_telemetry = ReadQualificationNodeTelemetry(
        adb, shadow, previous_pose[1], !capture_since_last_sample, sample_deadline);
    report["telemetry_samples"].push_back({
        {"sample_index", report.at("telemetry_samples").size()},
        {"elapsed_seconds",
         std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()},
        {"roles",
         {{"face_on", std::move(leader_telemetry.report)},
          {"down_the_line", std::move(shadow_telemetry.report)}}},
    });
  }
  report["observed_duration_seconds"] =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  const auto performance = BuildPoseQualificationPerformanceSummary(report.dump());
  if (!performance.valid) {
    report["diagnostic"] = performance.diagnostic;
    publish_progress();
    throw std::runtime_error("paired pose performance summary failed: " + performance.diagnostic);
  }
  report["performance_summary"] = Json::parse(performance.summary_json);
  Json validation_candidate = report;
  validation_candidate["passed"] = true;
  validation_candidate["qualification_eligible"] = true;
  const auto validation =
      ValidatePairedPoseQualificationReport(validation_candidate.dump(), policy.mode);
  if (!validation.passed) {
    report["diagnostic"] = validation.diagnostic;
    publish_progress();
    throw std::runtime_error("paired pose qualification acceptance failed: " +
                             validation.diagnostic);
  }
  displays.RestoreChecked();
  cleanup->RestoreNodeConfigurationsChecked();
  report["configuration_restore"] = {
      {"passed", true},
      {"scope", "complete_private_node_configuration_generation"},
      {"temporary_peer_configuration_removed", true},
      {"secret_material_preserved_in_artifacts", false},
  };
  Json final_report = report;
  final_report["passed"] = true;
  final_report["qualification_eligible"] = true;
  ValidateHilReportArtifacts(final_report.dump(), OutputDirectory(), "report.json");
  report = std::move(final_report);
  WriteArtifact(OutputDirectory() / "qualification-progress.json", report.dump(2) + "\n");
  WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
  std::cout << "Published " << policy.duration_seconds
            << "-second paired pose production-workload qualification evidence\n";
  return 0;
}

int RunPairedPose(int argument_count, char **arguments) {
  const std::string_view mode = argument_count == 4 ? std::string_view(arguments[3]) : "";
  const bool qualification = IsPairedPoseQualificationMode(mode);
  if (argument_count != 4 || (mode != "paired-pose-lan" && !qualification)) {
    throw std::runtime_error(
        "expected Bazel runfiles: <adb> <APK> paired-pose-lan, "
        "paired-pose-qualify-5m, or paired-pose-soak-30m");
  }
  const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
  const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
  if (!std::filesystem::is_regular_file(adb) || access(adb.c_str(), X_OK) != 0 ||
      !std::filesystem::is_regular_file(apk)) {
    throw std::runtime_error("paired pose-arm dual Android HIL runfiles are missing");
  }
  const std::string leader_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_SERIAL");
  const std::string shadow_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_SERIAL");
  if (leader_serial == shadow_serial) {
    throw std::runtime_error("paired pose-arm HIL requires two distinct devices");
  }
  // Reject incomplete PCM configuration before stopping apps, snapshotting
  // private state, installing APKs, or opening either camera. PreparePcmReplay
  // performs the full file/CRC/fixture validation after node association.
  static_cast<void>(RequiredEnvironment("SWING_CAPTURE_PCM_REPLAY_MANIFEST"));
  static_cast<void>(RequiredEnvironment("SWING_CAPTURE_PCM_REPLAY_WAV"));
  static_cast<void>(RequiredEnvironment("SWING_CAPTURE_PCM_REPLAY_CASE"));

  StopBothGuard cleanup(adb, shadow_serial, leader_serial);
  StopPackage(adb, leader_serial);
  StopPackage(adb, shadow_serial);
  cleanup.SnapshotNodeConfigurations();
  const auto identity_deadline = std::chrono::steady_clock::now() + 3s;
  const std::string leader_model = ReadDeviceModel(adb, leader_serial, identity_deadline);
  const std::string shadow_model = ReadDeviceModel(adb, shadow_serial, identity_deadline);
  if (!leader_model.starts_with("Pixel 6") || shadow_model != "Pixel 5a") {
    throw std::runtime_error(
        "paired pose-arm HIL requires Pixel 6-family leader and Pixel 5a shadow");
  }
  const ExternalTools external_tools = DiscoverExternalTools();
  ConcurrentNode leader =
      ConfigureConcurrentNode(adb, apk, leader_serial, "face_on", &cleanup, true);
  ConcurrentNode shadow =
      ConfigureConcurrentNode(adb, apk, shadow_serial, "down_the_line", &cleanup, true);
  if (leader.identity.node_id == shadow.identity.node_id) {
    throw std::runtime_error("paired pose-arm HIL nodes reuse one persistent identity");
  }

  const LanOrigin leader_lan_origin =
      ParseLanOrigin(RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN"),
                     "SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN");
  const LanOrigin shadow_lan_origin =
      ParseLanOrigin(RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN"),
                     "SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN");
  if (leader_lan_origin.ipv4_address == shadow_lan_origin.ipv4_address) {
    throw std::runtime_error("LAN-only paired pose HIL requires two distinct phone addresses");
  }

  const auto association_started = std::chrono::steady_clock::now();
  const auto association_deadline = association_started + kStageDeadline;
  const auto peer_configuration_started = std::chrono::steady_clock::now();
  ConfigurePoseMode(shadow, "shadow", std::nullopt, std::nullopt, association_deadline);
  ConfigurePeerPoseModeWithRetries(leader, shadow_lan_origin.origin, shadow.control_token,
                                   association_deadline);
  PreservePoseConfiguredNodeDescriptor(shadow, "shadow", false, association_deadline);
  PreservePoseConfiguredNodeDescriptor(leader, "leader", true, association_deadline);
  const std::int64_t peer_configuration_milliseconds =
      ElapsedMilliseconds(peer_configuration_started);
  const auto lan_validation_started = std::chrono::steady_clock::now();
  Json lan_endpoint_evidence;
  std::int64_t face_on_lan_validation_milliseconds = 0;
  std::int64_t down_the_line_lan_validation_milliseconds = 0;
  auto validate_face_on = std::async(std::launch::async, [&] {
    const auto started = std::chrono::steady_clock::now();
    Json evidence = ValidateLanNodeEndpoint(leader, leader_lan_origin, leader_model, "leader",
                                            shadow_lan_origin.origin, association_deadline);
    return std::pair(std::move(evidence), ElapsedMilliseconds(started));
  });
  const auto down_the_line_started = std::chrono::steady_clock::now();
  Json down_the_line_evidence = ValidateLanNodeEndpoint(shadow, shadow_lan_origin, shadow_model,
                                                        "shadow", {}, association_deadline);
  down_the_line_lan_validation_milliseconds = ElapsedMilliseconds(down_the_line_started);
  auto [face_on_evidence, face_on_elapsed] = validate_face_on.get();
  face_on_lan_validation_milliseconds = face_on_elapsed;
  lan_endpoint_evidence = {
      {"face_on", std::move(face_on_evidence)},
      {"down_the_line", std::move(down_the_line_evidence)},
  };
  const std::int64_t lan_validation_milliseconds = ElapsedMilliseconds(lan_validation_started);
  const auto clock_exchange_started = std::chrono::steady_clock::now();
  CollectClockExchanges(&leader, &shadow, association_deadline);
  WriteArtifact(OutputDirectory() / leader.role / "clock.json",
                ClockEvidenceJson(leader).dump(2) + "\n");
  WriteArtifact(OutputDirectory() / shadow.role / "clock.json",
                ClockEvidenceJson(shadow).dump(2) + "\n");
  const std::int64_t clock_exchange_milliseconds = ElapsedMilliseconds(clock_exchange_started);
  const std::int64_t association_stage_milliseconds = ElapsedMilliseconds(association_started);
  const auto network_health_started = std::chrono::steady_clock::now();
  PairNetworkHealthAcceptance pair_network_health =
      WaitForPairNetworkHealth(leader, shadow_lan_origin.origin, shadow.identity.node_id,
                               network_health_started + kStageDeadline);
  const std::int64_t pair_network_health_wait_milliseconds =
      ElapsedMilliseconds(network_health_started);
  WriteArtifact(
      OutputDirectory() / "peer-association-timing.json",
      Json{{"total", StageJson(association_stage_milliseconds)},
           {"peer_configuration", StageJson(peer_configuration_milliseconds)},
           {"lan_validation", StageJson(lan_validation_milliseconds)},
           {"face_on_lan_validation", StageJson(face_on_lan_validation_milliseconds)},
           {"down_the_line_lan_validation", StageJson(down_the_line_lan_validation_milliseconds)},
           {"clock_exchange", StageJson(clock_exchange_milliseconds)},
           {"pair_network_health_wait", StageJson(pair_network_health_wait_milliseconds)}}
              .dump(2) +
          "\n");

  PreparedPcmReplay prepared_pcm = PreparePcmReplay(OutputDirectory());

  const auto monitoring_started = std::chrono::steady_clock::now();
  const auto monitoring_deadline = monitoring_started + kStageDeadline;
  const auto leader_baseline = ReadyCaptureSessionIds(leader, monitoring_deadline);
  const auto shadow_baseline = ReadyCaptureSessionIds(shadow, monitoring_deadline);
  RegisterAutonomousStationCheckpointCleanup(cleanup, adb, leader.serial, monitoring_deadline);
  ArmPoseStandby(shadow, Json{{"armed", true}}.dump(), monitoring_deadline);
  ArmPoseStandby(leader, PairNetworkHealthArmRequestBody(pair_network_health.decision),
                 monitoring_deadline);
  Json standby_inference_evidence =
      WaitForPairedPosePhase(leader, shadow, "monitoring", {}, monitoring_deadline);
  const std::int64_t monitoring_stage_milliseconds = ElapsedMilliseconds(monitoring_started);

  if (qualification) {
    const auto &policy = PairedPoseQualificationPolicyForMode(mode);
    Json setup_evidence = {
        {"leader_role", leader.role},
        {"shadow_role", shadow.role},
        {"leader_device_model", leader_model},
        {"shadow_device_model", shadow_model},
        {"leader_node_id", leader.identity.node_id},
        {"shadow_node_id", shadow.identity.node_id},
        {"lan_endpoint_validation", lan_endpoint_evidence},
        {"pair_network_health_admission", pair_network_health.evidence},
        {"standby_inference_validation", standby_inference_evidence},
        {"node_setup",
         {{"face_on", NodeSetupTimingJson(leader)},
          {"down_the_line", NodeSetupTimingJson(shadow)}}},
        {"peer_association_detail",
         {{"peer_configuration", StageJson(peer_configuration_milliseconds)},
          {"lan_validation", StageJson(lan_validation_milliseconds)},
          {"face_on_lan_validation", StageJson(face_on_lan_validation_milliseconds)},
          {"down_the_line_lan_validation", StageJson(down_the_line_lan_validation_milliseconds)},
          {"clock_exchange", StageJson(clock_exchange_milliseconds)},
          {"pair_network_health_wait", StageJson(pair_network_health_wait_milliseconds)}}},
        {"stages",
         {{"peer_association_milliseconds", association_stage_milliseconds},
          {"pair_network_health_wait_milliseconds", pair_network_health_wait_milliseconds},
          {"standby_monitoring_start_milliseconds", monitoring_stage_milliseconds}}},
    };
    return RunPairedPoseQualification(adb, leader, shadow, &prepared_pcm, &cleanup, policy,
                                      std::move(setup_evidence), external_tools);
  }

  const auto preview_started = std::chrono::steady_clock::now();
  const auto preview_deadline = preview_started + kStageDeadline;
  Json setup_preview_evidence = {
      {"face_on", {{"monitoring", WaitForLiveSetupPreview(leader, preview_deadline)}}},
      {"down_the_line", {{"monitoring", WaitForLiveSetupPreview(shadow, preview_deadline)}}},
  };
  const std::int64_t preview_stage_milliseconds = ElapsedMilliseconds(preview_started);

  const auto transition_started = std::chrono::steady_clock::now();
  const auto transition_deadline = transition_started + kStageDeadline;

  const HttpResponse transition = RequireNodeHttp({.port = leader.host_port,
                                                   .method = "POST",
                                                   .path = "/api/v1/hil/pose-arm",
                                                   .bearer_token = leader.control_token,
                                                   .body = {},
                                                   .deadline = transition_deadline},
                                                  202);
  WriteArtifact(OutputDirectory() / leader.role / "pose-arm-response.json", transition.body);
  const Json transition_json = Json::parse(transition.body);
  const std::string shared_session_id = transition_json.value("shared_session_id", "");
  if (transition_json.value("schema_version", 0) != 1 ||
      transition_json.value("state", "") != "transitioning_to_high_speed" ||
      !SafeSessionId(shared_session_id)) {
    throw std::runtime_error("leader deterministic pose-arm response is invalid");
  }
  RegisterAutonomousSessionCleanup(cleanup, leader.serial, shadow.serial, shared_session_id);
  static_cast<void>(
      WaitForPairedPosePhase(leader, shadow, "high_speed", shared_session_id, transition_deadline));
  setup_preview_evidence["face_on"]["high_speed"] =
      ValidateHighSpeedSetupPreviewUnavailable(leader, transition_deadline);
  setup_preview_evidence["down_the_line"]["high_speed"] =
      ValidateHighSpeedSetupPreviewUnavailable(shadow, transition_deadline);
  const std::int64_t transition_stage_milliseconds = ElapsedMilliseconds(transition_started);
  constexpr auto kStageDeadlineMilliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count();
  for (const std::int64_t elapsed :
       {monitoring_stage_milliseconds, preview_stage_milliseconds, transition_stage_milliseconds}) {
    if (elapsed > kStageDeadlineMilliseconds) {
      throw std::runtime_error(
          "paired pose monitoring/preview/transition stage exceeded 15 seconds");
    }
  }

  const auto pcm_trigger_started = std::chrono::steady_clock::now();
  const auto pcm_trigger_deadline = pcm_trigger_started + kStageDeadline;
  const std::int64_t recalibration_stage_milliseconds =
      RecalibratePreparedPcmReplay(&prepared_pcm, OutputDirectory());
  const swing_capture::hil::FeatherPcmPlaybackReceipt pcm_playback =
      PlayPreparedPcmReplay(&prepared_pcm, OutputDirectory());
  const PcmAutomaticTriggerObservation automatic_observation =
      ObserveAutomaticPcmTrigger(leader, shadow, pcm_trigger_deadline);
  const PcmReplayPairOutcome automatic_trigger_outcome = automatic_observation.outcome;
  const bool detected_for_expectation =
      prepared_pcm.replay_case.definition.expectation == PcmReplayExpectation::kRequiredPositive
          ? automatic_trigger_outcome == PcmReplayPairOutcome::kBoth
          : automatic_trigger_outcome != PcmReplayPairOutcome::kNeither;
  const PcmReplayRunDisposition replay_disposition = ClassifyPcmReplayRun(
      prepared_pcm.replay_case.definition.expectation, detected_for_expectation);
  if (replay_disposition == PcmReplayRunDisposition::kExpectedDiagnosticNegative) {
    const auto status_deadline = std::chrono::steady_clock::now() + 2s;
    const auto post_stimulus_status =
        ReadAndPreservePostStimulusStatus(leader, shadow, status_deadline, "diagnostic-negative");
    const Json observation = PcmReplayObservationJson(prepared_pcm.replay_case, {}, {});
    WriteArtifact(OutputDirectory() / "pcm-replay-observation.json", observation.dump(2) + "\n");
    const auto disarm_started = std::chrono::steady_clock::now();
    const auto disarm_deadline = disarm_started + kStageDeadline;
    DisarmPoseStandby(leader, disarm_deadline);
    DisarmPoseStandby(shadow, disarm_deadline);
    const std::int64_t disarm_stage_milliseconds = ElapsedMilliseconds(disarm_started);
    cleanup.RestoreNodeConfigurationsChecked();
    const Json report = {
        {"schema_version", 1},
        {"report_type", "android_dual_phone_pcm_replay_diagnostic_negative_hil"},
        {"passed", true},
        {"qualification_eligible", false},
        {"expected_outcome_observed", true},
        {"automatic_trigger_outcome", "neither"},
        {"paired_automatic_trigger_observed", false},
        {"any_automatic_trigger_observed", false},
        {"case_name", prepared_pcm.replay_case.definition.name},
        {"expectation", "diagnostic_negative"},
        {"source_id", prepared_pcm.replay_case.source_id},
        {"source_crc32", prepared_pcm.replay_case.source_crc32},
        {"gain_permille", prepared_pcm.replay_case.definition.gain_permille},
        {"playback", FeatherPcmPlaybackJson(pcm_playback)},
        {"automatic_observation", PcmAutomaticTriggerObservationJson(automatic_observation)},
        {"scoring_observation", observation.at("observations").at(0)},
        {"post_stimulus_status",
         {{"face_on", Json::parse(post_stimulus_status[0])},
          {"down_the_line", Json::parse(post_stimulus_status[1])}}},
        {"stages", {{"safe_disarm", StageJson(disarm_stage_milliseconds)}}},
        {"diagnostic_images_required", false},
        {"artifacts",
         {{"report", "report.json"},
          {"observation", "pcm-replay-observation.json"},
          {"feather_pcm_playback", "feather-pcm-playback.json"},
          {"face_on_status", "face_on/pose-status-post-stimulus-diagnostic-negative.json"},
          {"down_the_line_status",
           "down_the_line/pose-status-post-stimulus-diagnostic-negative.json"}}},
        {"configuration_restore",
         {{"passed", true},
          {"scope", "complete_private_node_configuration_generation"},
          {"temporary_peer_configuration_removed", true},
          {"secret_material_preserved_in_artifacts", false}}},
    };
    WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
    std::cout << "Published expected diagnostic-negative PCM replay evidence\n";
    return 0;
  }
  if (automatic_trigger_outcome != PcmReplayPairOutcome::kBoth) {
    const auto status_deadline = std::chrono::steady_clock::now() + 2s;
    const auto post_stimulus_status =
        ReadAndPreservePostStimulusStatus(leader, shadow, status_deadline, "failure");
    Json salvage;
    try {
      salvage = SalvageMissedPcmTrigger(adb, leader, shadow, shared_session_id, pcm_playback,
                                        prepared_pcm.replay_case, post_stimulus_status,
                                        automatic_observation);
    } catch (const std::exception &salvage_failure) {
      throw std::runtime_error("incomplete paired automatic trigger salvage failed: " +
                               std::string(salvage_failure.what()));
    }
    const std::string expectation =
        replay_disposition == PcmReplayRunDisposition::kRequiredPositiveMissed
            ? "required-positive PCM replay lacked a complete paired automatic trigger"
            : "diagnostic-negative PCM replay produced an automatic trigger";
    throw std::runtime_error(expectation +
                             " (outcome=" + salvage.value("automatic_trigger_outcome", "unknown") +
                             "); non-qualifying salvage retained both phone rings");
  }
  const Json mapped_impact =
      ReadMappedPeerImpactEvidence(leader, shadow, shared_session_id, pcm_trigger_deadline);
  const std::array<std::string, 2> session_ids = WaitForNewPoseSessions(
      leader, shadow, leader_baseline, shadow_baseline, pcm_trigger_deadline);
  const std::int64_t capture_stage_milliseconds = ElapsedMilliseconds(pcm_trigger_started);
  if (capture_stage_milliseconds >
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count()) {
    throw std::runtime_error("paired pose-arm capture exceeded 15 seconds");
  }

  const auto report_deadline = std::chrono::steady_clock::now() + kStageDeadline;
  std::array<std::string, 2> reports = {
      BuildPoseCaptureReport(adb, leader, session_ids[0], report_deadline),
      BuildPoseCaptureReport(adb, shadow, session_ids[1], report_deadline),
  };
  CapturedNode leader_capture =
      PullConcurrentPcmNode(adb, leader, shared_session_id, std::move(reports[0]), pcm_playback,
                            capture_stage_milliseconds, "local_audio");
  CapturedNode shadow_capture = PullConcurrentPcmNode(
      adb, shadow, shared_session_id, std::move(reports[1]), pcm_playback,
      capture_stage_milliseconds, "peer_audio_clock_candidate", "diagnostic_audio.wav");
  if (replay_disposition == PcmReplayRunDisposition::kDiagnosticNegativeFalseTrigger) {
    const Json observation = PcmReplayObservationJson(prepared_pcm.replay_case, "local_audio",
                                                      "peer_audio_clock_candidate");
    WriteArtifact(OutputDirectory() / "pcm-replay-observation.json", observation.dump(2) + "\n");
    const auto status_deadline = std::chrono::steady_clock::now() + 2s;
    const auto post_stimulus_status = ReadAndPreservePostStimulusStatus(
        leader, shadow, status_deadline, "diagnostic-negative-false-trigger");
    cleanup.RestoreNodeConfigurationsChecked();
    const Json report = {
        {"schema_version", 1},
        {"report_type", "android_dual_phone_pcm_replay_diagnostic_negative_hil"},
        {"passed", false},
        {"qualification_eligible", false},
        {"expected_outcome_observed", false},
        {"automatic_trigger_outcome", "both"},
        {"paired_automatic_trigger_observed", true},
        {"any_automatic_trigger_observed", true},
        {"diagnostic", "diagnostic-negative PCM replay caused an automatic trigger"},
        {"case_name", prepared_pcm.replay_case.definition.name},
        {"expectation", "diagnostic_negative"},
        {"source_id", prepared_pcm.replay_case.source_id},
        {"source_crc32", prepared_pcm.replay_case.source_crc32},
        {"gain_permille", prepared_pcm.replay_case.definition.gain_permille},
        {"shared_session_id", shared_session_id},
        {"mapped_impact_evidence", mapped_impact},
        {"playback", FeatherPcmPlaybackJson(pcm_playback)},
        {"automatic_observation", PcmAutomaticTriggerObservationJson(automatic_observation)},
        {"scoring_observation", observation.at("observations").at(0)},
        {"post_stimulus_status",
         {{"face_on", Json::parse(post_stimulus_status[0])},
          {"down_the_line", Json::parse(post_stimulus_status[1])}}},
        {"retained_false_trigger",
         {{"face_on",
           {{"local_session_id", leader_capture.evidence.local_session_id},
            {"manifest", "face_on/manifest.json"},
            {"media", "face_on/face_on.mp4"},
            {"audio", "face_on/audio_evidence.wav"},
            {"trigger_source", "local_audio"}}},
          {"down_the_line",
           {{"local_session_id", shadow_capture.evidence.local_session_id},
            {"manifest", "down_the_line/manifest.json"},
            {"media", "down_the_line/down_the_line.mp4"},
            {"audio", "down_the_line/audio_evidence.wav"},
            {"trigger_source", "peer_audio_clock_candidate"}}}}},
        {"diagnostic_images_required", false},
        {"artifacts",
         {{"report", "report.json"},
          {"observation", "pcm-replay-observation.json"},
          {"feather_pcm_playback", "feather-pcm-playback.json"},
          {"face_on_status",
           "face_on/pose-status-post-stimulus-diagnostic-negative-false-trigger.json"},
          {"down_the_line_status",
           "down_the_line/pose-status-post-stimulus-diagnostic-negative-false-trigger.json"}}},
        {"configuration_restore",
         {{"passed", true},
          {"scope", "complete_private_node_configuration_generation"},
          {"temporary_peer_configuration_removed", true},
          {"secret_material_preserved_in_artifacts", false}}},
    };
    WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
    std::cerr << "Diagnostic-negative PCM replay produced a false trigger; retained both phone "
                 "captures\n";
    return 2;
  }
  const auto parallel_media_analysis_started = std::chrono::steady_clock::now();
  auto leader_media_analysis = std::async(std::launch::async, [&] {
    AnalyzeNodeMedia(&leader_capture, OutputDirectory() / leader.role, external_tools);
  });
  AnalyzeNodeMedia(&shadow_capture, OutputDirectory() / shadow.role, external_tools);
  leader_media_analysis.get();
  const std::int64_t parallel_media_analysis_milliseconds =
      ElapsedMilliseconds(parallel_media_analysis_started);
  const Json leader_evidence =
      PcmEvidenceJson(leader_capture, prepared_pcm.replay_case, "audio_evidence.wav");
  WriteArtifact(OutputDirectory() / leader.role / "evidence.json", leader_evidence.dump(2) + "\n");
  ValidateCompletedPcmNode(leader_capture);
  const Json shadow_evidence =
      PcmEvidenceJson(shadow_capture, prepared_pcm.replay_case, "diagnostic_audio.wav");
  WriteArtifact(OutputDirectory() / shadow.role / "evidence.json", shadow_evidence.dump(2) + "\n");
  ValidateCompletedPcmNode(shadow_capture);
  // The production leader is face-on for this fixture, while the validator's positional
  // contract is down-the-line followed by face-on. Keep transport leadership independent of
  // camera-role ordering.
  ValidateDualSession(shadow_capture.evidence, leader_capture.evidence);
  WriteArtifact(OutputDirectory() / "pcm-replay-observation.json",
                PcmReplayObservationJson(prepared_pcm.replay_case, "local_audio",
                                         "peer_audio_clock_candidate")
                        .dump(2) +
                    "\n");
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
  CoordinationRunEvidence coordination_evidence =
      BuildAndPersistCoordination(shadow, leader, shadow_capture, leader_capture, shared_session_id,
                                  "peer_audio_clock_candidate", "local_audio");
  if (coordination_evidence.stage_milliseconds >
      std::chrono::duration_cast<std::chrono::milliseconds>(kStageDeadline).count()) {
    throw std::runtime_error("paired pose-arm coordination persistence exceeded 15 seconds");
  }

  Json aggregate = {
      {"schema_version", 1},
      {"report_type", "android_dual_phone_paired_pose_arm_lan_hil"},
      {"passed", true},
      {"camera_jobs_concurrent", true},
      {"single_pcm_replay_count", 1},
      {"automatic_trigger_outcome", "both"},
      {"paired_automatic_trigger_observed", true},
      {"any_automatic_trigger_observed", true},
      {"automatic_observation", PcmAutomaticTriggerObservationJson(automatic_observation)},
      {"shared_session_id", shared_session_id},
      {"timing_claim", UncalibratedTimingClaimJson()},
      {"pose_transition",
       {
           {"passed", true},
           {"leader_role", leader.role},
           {"shadow_role", shadow.role},
           {"leader_device_model", leader_model},
           {"shadow_device_model", shadow_model},
           {"leader_candidate_source", "explicit_hil_endpoint"},
           {"peer_dispatch", "production_pose_peer_arm_client"},
           {"peer_transport", "wifi_lan_direct"},
           {"adb_reverse_used", false},
           {"host_control_transport", "adb_forward"},
           {"leader_trigger_source", "local_audio"},
           {"shadow_trigger_source", "peer_audio_clock_candidate"},
           {"peer_impact_schema_version", mapped_impact.at("schema_version")},
           {"target_peer_node_id_verified",
            mapped_impact.value("target_peer_node_id", "") == shadow.identity.node_id},
           {"mapped_fallback_semantics", mapped_impact.at("fallback_semantics")},
           {"mapped_impact_evidence", mapped_impact},
           {"pcm_case_name", prepared_pcm.replay_case.definition.name},
           {"pcm_source_id", prepared_pcm.replay_case.source_id},
           {"pcm_source_crc32", prepared_pcm.replay_case.source_crc32},
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
           {"down_the_line", ClockEvidenceJson(shadow)},
           {"face_on", ClockEvidenceJson(leader)},
       }},
      {"node_setup",
       {{"face_on", NodeSetupTimingJson(leader)}, {"down_the_line", NodeSetupTimingJson(shadow)}}},
      {"peer_association_detail",
       {{"peer_configuration", StageJson(peer_configuration_milliseconds)},
        {"lan_validation", StageJson(lan_validation_milliseconds)},
        {"face_on_lan_validation", StageJson(face_on_lan_validation_milliseconds)},
        {"down_the_line_lan_validation", StageJson(down_the_line_lan_validation_milliseconds)},
        {"clock_exchange", StageJson(clock_exchange_milliseconds)},
        {"pair_network_health_wait", StageJson(pair_network_health_wait_milliseconds)}}},
      {"pair_network_health_admission", pair_network_health.evidence},
      {"standby_inference_validation", std::move(standby_inference_evidence)},
      {"setup_preview_validation", std::move(setup_preview_evidence)},
      {"lan_endpoint_validation", std::move(lan_endpoint_evidence)},
      {"coordination", std::move(coordination_evidence.report)},
      {"stages",
       {
           {"down_the_line_setup", StageJson(shadow.setup_stage_milliseconds)},
           {"face_on_setup", StageJson(leader.setup_stage_milliseconds)},
           {"peer_association", StageJson(association_stage_milliseconds)},
           {"pair_network_health_wait", StageJson(pair_network_health_wait_milliseconds)},
           {"standby_monitoring_start", StageJson(monitoring_stage_milliseconds)},
           {"setup_preview", StageJson(preview_stage_milliseconds)},
           {"high_speed_transition", StageJson(transition_stage_milliseconds)},
           {"feather_recalibration", StageJson(recalibration_stage_milliseconds)},
           {"pcm_trigger_and_capture", StageJson(capture_stage_milliseconds)},
           {"parallel_media_analysis", StageJson(parallel_media_analysis_milliseconds)},
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
           {"feather_pcm_playback", "feather-pcm-playback.json"},
           {"feather_pcm_transaction", "feather-pcm-transaction.json"},
           {"coordination_record", "coordination.json"},
           {"leader_pose_arm_response", leader.role + "/pose-arm-response.json"},
           {"down_the_line",
            {
                {"initial_node_descriptor", "down_the_line/node-descriptor-initial.json"},
                {"pose_configured_node_descriptor",
                 "down_the_line/node-descriptor-pose-configured.json"},
                {"pose_setup", "down_the_line/pose-setup.json"},
                {"pose_status_monitoring", "down_the_line/pose-status-monitoring.json"},
                {"pose_status_high_speed", "down_the_line/pose-status-high_speed.json"},
                {"pose_status_triggered", "down_the_line/pose-status-triggered.json"},
                {"setup_preview", "down_the_line/setup-preview.jpg"},
                {"setup_preview_metadata", "down_the_line/setup-preview.json"},
                {"setup_preview_high_speed", "down_the_line/setup-preview-high-speed.json"},
                {"clock", "down_the_line/clock.json"},
                {"trigger_report", "down_the_line/trigger-report.json"},
                {"evidence", "down_the_line/evidence.json"},
                {"capture", PcmNodeArtifactPaths("down_the_line", "diagnostic_audio.wav")},
                {"lan_node_descriptor", "down_the_line/lan-node-descriptor.json"},
                {"lan_setup_authenticated", "down_the_line/lan-setup-authenticated.json"},
                {"lan_setup_unauthenticated", "down_the_line/lan-setup-unauthenticated.json"},
                {"lan_capture_status", "down_the_line/lan-capture-status.json"},
                {"lan_clock", "down_the_line/lan-clock.json"},
            }},
           {"face_on",
            {
                {"initial_node_descriptor", "face_on/node-descriptor-initial.json"},
                {"pose_configured_node_descriptor", "face_on/node-descriptor-pose-configured.json"},
                {"pose_setup", "face_on/pose-setup.json"},
                {"pose_status_monitoring", "face_on/pose-status-monitoring.json"},
                {"pose_status_high_speed", "face_on/pose-status-high_speed.json"},
                {"setup_preview", "face_on/setup-preview.jpg"},
                {"setup_preview_metadata", "face_on/setup-preview.json"},
                {"setup_preview_high_speed", "face_on/setup-preview-high-speed.json"},
                {"clock", "face_on/clock.json"},
                {"trigger_report", "face_on/trigger-report.json"},
                {"evidence", "face_on/evidence.json"},
                {"capture", PcmNodeArtifactPaths("face_on", "audio_evidence.wav")},
                {"lan_node_descriptor", "face_on/lan-node-descriptor.json"},
                {"lan_setup_authenticated", "face_on/lan-setup-authenticated.json"},
                {"lan_setup_unauthenticated", "face_on/lan-setup-unauthenticated.json"},
                {"lan_capture_status", "face_on/lan-capture-status.json"},
                {"lan_clock", "face_on/lan-clock.json"},
                {"pair_network_health_accepted", "face_on/pair-network-health-accepted.json"},
            }},
       }},
      {"nodes", Json::array({leader_evidence, shadow_evidence})},
  };
  cleanup.PreserveRegisteredPrivateFile(leader.serial,
                                        "files/coordination/" + shared_session_id + ".json");
  cleanup.PreserveRegisteredPrivateFile(shadow.serial,
                                        "files/coordination/" + shared_session_id + ".json");
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
  const auto parallel_media_analysis_started = std::chrono::steady_clock::now();
  auto down_media_analysis = std::async(std::launch::async, [&] {
    AnalyzeNodeMedia(&down_capture, OutputDirectory() / down_node.role, external_tools);
  });
  AnalyzeNodeMedia(&face_capture, OutputDirectory() / face_node.role, external_tools);
  down_media_analysis.get();
  const std::int64_t parallel_media_analysis_milliseconds =
      ElapsedMilliseconds(parallel_media_analysis_started);
  WriteArtifact(OutputDirectory() / down_node.role / "evidence.json",
                EvidenceJson(down_capture).dump(2) + "\n");
  ValidateCompletedNode(down_capture);
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
  CoordinationRunEvidence coordination_evidence =
      BuildAndPersistCoordination(down_node, face_node, down_capture, face_capture,
                                  shared_session_id, "local_audio", "local_audio");
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
      {"node_setup",
       {{"down_the_line", NodeSetupTimingJson(down_node)},
        {"face_on", NodeSetupTimingJson(face_node)}}},
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
           {"parallel_media_analysis", StageJson(parallel_media_analysis_milliseconds)},
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

Json LatestJsonArtifact(std::string_view relative_path) {
  const std::filesystem::path path = OutputDirectory() / relative_path;
  if (!std::filesystem::is_regular_file(path)) {
    return {{"present", false}, {"artifact", relative_path}};
  }
  try {
    const std::uintmax_t size = std::filesystem::file_size(path);
    if (size == 0U || size > 4U * 1024U * 1024U) {
      throw std::runtime_error("artifact size is outside the bounded JSON evidence contract");
    }
    std::ifstream input(path, std::ios::binary);
    std::string contents(static_cast<std::size_t>(size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input) {
      throw std::runtime_error("cannot read JSON artifact");
    }
    return {{"present", true}, {"artifact", relative_path}, {"evidence", Json::parse(contents)}};
  } catch (const std::exception &failure) {
    return {{"present", true}, {"artifact", relative_path}, {"parse_error", failure.what()}};
  }
}

Json CleanupEvidenceUnavailable(std::string diagnostic, bool required) {
  return {
      {"schema_version", 1},
      {"report_type", "android_dual_phone_hil_cleanup"},
      {"required", required},
      {"finalized", !required},
      {"passed", !required},
      {"obligation_count", 0},
      {"attempt_count", 0},
      {"restored_count", 0},
      {"failed_count", required ? 1 : 0},
      {"obligations", Json::array()},
      {"attempts", Json::array()},
      {"diagnostic", std::move(diagnostic)},
      {"artifact", "cleanup.json"},
  };
}

Json ReadFinalCleanupEvidence() {
  const Json artifact = LatestJsonArtifact("cleanup.json");
  if (!artifact.value("present", false)) {
    return CleanupEvidenceUnavailable("no mutating stage started", false);
  }
  if (!artifact.contains("evidence")) {
    return CleanupEvidenceUnavailable(
        artifact.value("parse_error", "cleanup evidence is unreadable"), true);
  }
  Json evidence = artifact.at("evidence");
  const bool valid = evidence.is_object() && evidence.value("schema_version", 0) == 1 &&
                     evidence.value("report_type", "") == "android_dual_phone_hil_cleanup" &&
                     evidence.contains("finalized") && evidence.at("finalized").is_boolean() &&
                     evidence.contains("passed") && evidence.at("passed").is_boolean() &&
                     evidence.contains("failed_count") &&
                     evidence.at("failed_count").is_number_unsigned() &&
                     evidence.contains("obligations") && evidence.at("obligations").is_array() &&
                     evidence.contains("attempts") && evidence.at("attempts").is_array();
  if (!valid) {
    return CleanupEvidenceUnavailable("cleanup evidence contract is invalid", true);
  }
  const bool computed_passed =
      evidence.value("finalized", false) && evidence.value("failed_count", 1U) == 0U;
  if (evidence.value("passed", false) != computed_passed) {
    return CleanupEvidenceUnavailable("cleanup evidence pass state is inconsistent", true);
  }
  evidence["required"] = true;
  evidence["artifact"] = "cleanup.json";
  return evidence;
}

void AttachCleanupArtifact(Json *report) {
  if (!report->contains("artifacts") || !report->at("artifacts").is_object()) {
    (*report)["artifacts"] = Json::object();
  }
  (*report)["artifacts"]["cleanup"] = "cleanup.json";
}

int FinalizeReturnedHilReport(int primary_exit_code) {
  const Json report_artifact = LatestJsonArtifact("report.json");
  if (!report_artifact.contains("evidence")) {
    throw std::runtime_error("successful HIL dispatch did not publish a readable report.json");
  }
  Json report = report_artifact.at("evidence");
  const bool primary_passed = report.value("passed", primary_exit_code == 0);
  const std::string primary_diagnostic = report.value("diagnostic", "");
  const Json cleanup = ReadFinalCleanupEvidence();
  const auto combined =
      CombineHilPrimaryAndCleanup(HilPrimaryOutcome{.passed = primary_passed,
                                                    .exit_code = primary_exit_code,
                                                    .diagnostic = primary_diagnostic},
                                  cleanup.value("passed", false));
  report["passed"] = combined.passed;
  report["primary_outcome"] = {
      {"passed", combined.primary_passed},
      {"exit_code", primary_exit_code},
      {"diagnostic", primary_diagnostic.empty() ? Json(nullptr) : Json(primary_diagnostic)},
  };
  report["cleanup"] = cleanup;
  AttachCleanupArtifact(&report);
  if (!combined.passed && combined.primary_passed) {
    report["diagnostic"] = combined.diagnostic;
  }
  const std::string report_type = report.value("report_type", "");
  const bool file_only_paired_pose_report =
      report_type == "android_dual_phone_paired_pose_arm_lan_hil" ||
      report_type == "android_dual_phone_paired_pose_qualification_5m" ||
      report_type == "android_dual_phone_paired_pose_soak_30m";
  if (combined.passed && file_only_paired_pose_report) {
    ValidateHilReportArtifacts(report.dump(), OutputDirectory(), "report.json");
  }
  WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
  return combined.exit_code;
}

}  // namespace

namespace swing_capture::android::dual_hil {

int RunDualAndroidHil(int argument_count, char **arguments) {
  const bool concurrent = argument_count == 4 && std::string_view(arguments[3]) == "concurrent";
  const bool paired_pose_qualification =
      argument_count == 4 && IsPairedPoseQualificationMode(arguments[3]);
  const bool paired_pose =
      argument_count == 4 &&
      (std::string_view(arguments[3]) == "paired-pose-lan" || paired_pose_qualification);
  const bool discovery_pairing =
      argument_count == 4 && std::string_view(arguments[3]) == "discovery-pairing";
  const bool autonomous_restart =
      argument_count == 4 && std::string_view(arguments[3]) == "autonomous-restart";
  const bool autonomous_disturbance =
      argument_count == 4 && std::string_view(arguments[3]) == "autonomous-disturbance";
  try {
    int primary_exit_code = 0;
    if (autonomous_disturbance) {
      primary_exit_code = RunAutonomousDisturbance(argument_count, arguments);
    } else if (autonomous_restart) {
      primary_exit_code = RunAutonomousRestartRecovery(argument_count, arguments);
    } else if (discovery_pairing) {
      primary_exit_code = RunDiscoveryPairing(argument_count, arguments);
    } else if (paired_pose) {
      primary_exit_code = RunPairedPose(argument_count, arguments);
    } else {
      primary_exit_code =
          concurrent ? RunConcurrent(argument_count, arguments) : Run(argument_count, arguments);
    }
    return FinalizeReturnedHilReport(primary_exit_code);
  } catch (const std::exception &failure) {
    try {
      const std::string report_type =
          autonomous_disturbance ? "android_dual_phone_autonomous_disturbance_hil"
          : autonomous_restart   ? "android_dual_phone_autonomous_restart_recovery_hil"
          : discovery_pairing    ? "android_dual_phone_discovery_pairing_hil"
          : paired_pose_qualification
              ? std::string(PairedPoseQualificationPolicyForMode(arguments[3]).report_type)
          : paired_pose ? "android_dual_phone_paired_pose_arm_lan_hil"
                        : (concurrent ? "android_dual_phone_concurrent_hil"
                                      : "android_dual_phone_sequential_hil");
      const Json cleanup = ReadFinalCleanupEvidence();
      const auto combined = CombineHilPrimaryAndCleanup(
          HilPrimaryOutcome{.passed = false, .exit_code = 1, .diagnostic = failure.what()},
          cleanup.value("passed", false));
      Json report = {
          {"schema_version", 1},
          {"report_type", report_type},
          {"passed", combined.passed},
          {"camera_jobs_concurrent", concurrent || paired_pose},
          {"diagnostic", failure.what()},
          {"primary_outcome",
           {{"passed", false}, {"exit_code", 1}, {"diagnostic", failure.what()}}},
          {"cleanup", cleanup},
          {"last_node_diagnostics",
           {
               {"down_the_line", LatestNodeDiagnostics("down_the_line")},
               {"face_on", LatestNodeDiagnostics("face_on")},
           }},
          {"pcm_failure_salvage", LatestJsonArtifact("pcm-failure-salvage.json")},
          {"qualification_progress", LatestJsonArtifact("qualification-progress.json")},
          {"post_stimulus_status",
           {{"down_the_line",
             LatestJsonArtifact("down_the_line/pose-status-post-stimulus-failure.json")},
            {"face_on", LatestJsonArtifact("face_on/pose-status-post-stimulus-failure.json")}}},
      };
      AttachCleanupArtifact(&report);
      WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
    } catch (const std::exception &write_failure) {
      std::cerr << "Could not publish failing dual-phone report: " << write_failure.what() << '\n';
    }
    std::cerr << (autonomous_disturbance ? "Autonomous disturbance"
                  : autonomous_restart   ? "Autonomous restart recovery"
                  : discovery_pairing    ? "Discovery/pairing"
                  : paired_pose_qualification
                      ? "Paired pose qualification"
                      : (paired_pose ? "Paired pose-arm"
                                     : (concurrent ? "Concurrent" : "Sequential")))
              << " dual-phone Android HIL failed: " << failure.what() << '\n';
    return 1;
  }
}

}  // namespace swing_capture::android::dual_hil
