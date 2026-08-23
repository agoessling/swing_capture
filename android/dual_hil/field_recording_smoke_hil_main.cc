#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "android/dual_hil/android_apk_install.h"
#include "android/dual_hil/field_recording_hil_transport.h"
#include "android/dual_hil/field_recording_smoke_validation.h"
#include "android/dual_hil/hil_command.h"
#include "android/dual_hil/hil_http_client.h"

namespace swing_capture::android::dual_hil {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

constexpr std::string_view kPackageName = "com.agoessling.swingcapture";
constexpr std::uint16_t kNodeHttpPort = 8088;
constexpr auto kOverallDeadline = 15s;
constexpr auto kRecordingDuration = 1s;
constexpr auto kPollInterval = 100ms;
constexpr std::string_view kBrowserTestOrigin = "http://field-review-client.invalid";

struct Node {
  std::string serial;
  std::string role;
  std::string node_id;
  FieldRecordingHilHttpEndpoint endpoint;
  std::string control_token;
};

std::string EnvironmentValue(std::string_view name) {
  // The Bazel test environment is immutable after process startup.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *value = std::getenv(std::string(name).c_str());
  return value == nullptr ? std::string() : std::string(value);
}

std::string RequiredEnvironment(std::string_view name) {
  std::string value = EnvironmentValue(name);
  if (value.empty()) {
    throw std::runtime_error("set --test_env=" + std::string(name) + "=<value>");
  }
  return value;
}

bool SkipExactApkInstall() {
  const std::string configured = EnvironmentValue("SWING_CAPTURE_ANDROID_SKIP_EXACT_APK_INSTALL");
  if (configured.empty() || configured == "false" || configured == "0") {
    return false;
  }
  if (configured == "true" || configured == "1") {
    return true;
  }
  throw std::runtime_error(
      "SWING_CAPTURE_ANDROID_SKIP_EXACT_APK_INSTALL must be true, false, 1, or 0");
}

std::filesystem::path OutputDirectory() {
  const std::string configured = EnvironmentValue("TEST_UNDECLARED_OUTPUTS_DIR");
  return configured.empty() ? std::filesystem::current_path() : std::filesystem::path(configured);
}

void WriteArtifact(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open field-recording HIL artifact " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    throw std::runtime_error("cannot write field-recording HIL artifact " + path.string());
  }
}

std::vector<std::string> DeviceArguments(std::string_view serial,
                                         std::initializer_list<std::string_view> suffix) {
  std::vector<std::string> arguments = {"-s", std::string(serial)};
  for (const std::string_view value : suffix) {
    arguments.emplace_back(value);
  }
  return arguments;
}

std::string RunRequiredAdb(const std::filesystem::path &adb,
                           const std::vector<std::string> &arguments,
                           std::chrono::steady_clock::time_point deadline) {
  return RunRequiredHilCommand(adb, arguments, deadline);
}

std::string TrimLineEndings(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

std::uint16_t EstablishForward(const std::filesystem::path &adb, std::string_view serial,
                               std::chrono::steady_clock::time_point deadline) {
  const std::string output = TrimLineEndings(RunRequiredAdb(
      adb, DeviceArguments(serial, {"forward", "tcp:0", "tcp:" + std::to_string(kNodeHttpPort)}),
      deadline));
  unsigned int parsed = 0;
  const auto [end, error] = std::from_chars(output.data(), output.data() + output.size(), parsed);
  if (error != std::errc() || end != output.data() + output.size() || parsed == 0U ||
      parsed > 65'535U) {
    throw std::runtime_error("adb did not return an ephemeral forward port");
  }
  return static_cast<std::uint16_t>(parsed);
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
  const std::size_t end = start == std::string::npos
                              ? std::string::npos
                              : preferences.find(kSuffix, start + kPrefix.size());
  if (start == std::string::npos || end == std::string::npos) {
    throw std::runtime_error("Android node does not have a persisted bearer credential");
  }
  const std::string token =
      preferences.substr(start + kPrefix.size(), end - start - kPrefix.size());
  if (token.size() != 32U || !std::ranges::all_of(token, [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' ||
               character == '-';
      })) {
    throw std::runtime_error("Android node bearer credential has an invalid format");
  }
  return token;
}

Node PrepareNode(const std::filesystem::path &adb, std::string serial, std::string role,
                 std::optional<FieldRecordingHilHttpEndpoint> direct_lan_endpoint,
                 std::chrono::steady_clock::time_point deadline) {
  RunRequiredAdb(adb, DeviceArguments(serial, {"get-state"}), deadline);
  RunRequiredAdb(adb,
                 DeviceArguments(serial, {"shell", "am", "start", "-W", "-n",
                                          "com.agoessling.swingcapture/.MainActivity"}),
                 deadline);
  const FieldRecordingHilHttpEndpoint endpoint =
      direct_lan_endpoint.has_value()
          ? std::move(*direct_lan_endpoint)
          : MakeFieldRecordingAdbForwardEndpoint(EstablishForward(adb, serial, deadline));
  try {
    Node node{
        .serial = serial,
        .role = role,
        .node_id = {},
        .endpoint = endpoint,
        .control_token = ReadControlToken(adb, serial, deadline),
    };
    while (std::chrono::steady_clock::now() < deadline) {
      try {
        const HilHttpResponse response = RequireHilHttp({.ipv4_host = node.endpoint.ipv4_address,
                                                         .port = node.endpoint.port,
                                                         .method = "GET",
                                                         .path = "/api/v1/node",
                                                         .bearer_token = node.control_token,
                                                         .body = {},
                                                         .headers = {},
                                                         .deadline = deadline},
                                                        200);
        const Json identity = Json::parse(response.body);
        if (identity.value("schema_version", 0) != 1 || identity.value("role", "") != node.role ||
            identity.value("node_id", "").empty()) {
          throw std::runtime_error("configured Android node identity or role is incorrect");
        }
        node.node_id = identity.at("node_id").get<std::string>();
        const Json status = Json::parse(RequireHilHttp({.ipv4_host = node.endpoint.ipv4_address,
                                                        .port = node.endpoint.port,
                                                        .method = "GET",
                                                        .path = "/api/v1/field-recording/status",
                                                        .bearer_token = node.control_token,
                                                        .body = {},
                                                        .headers = {},
                                                        .deadline = deadline},
                                                       200)
                                            .body);
        const std::string state = status.value("state", "");
        if (state != "idle" && state != "ready") {
          throw std::runtime_error("Android node already has an active field recording");
        }
        return node;
      } catch (const nlohmann::json::exception &) {
        throw std::runtime_error("Android node readiness response is malformed");
      } catch (const std::runtime_error &failure) {
        if (std::string_view(failure.what()).find("connect") == std::string_view::npos) {
          throw;
        }
        std::this_thread::sleep_for(kPollInterval);
      }
    }
    throw std::runtime_error("Android node HTTP service did not become ready");
  } catch (...) {
    if (endpoint.UsesAdbForward()) {
      static_cast<void>(RunHilCommand(
          adb,
          DeviceArguments(serial, {"forward", "--remove", "tcp:" + std::to_string(endpoint.port)}),
          std::chrono::steady_clock::now() + 2s));
    }
    throw;
  }
}

std::string_view RequiredHttpHeader(const HilHttpResponse &response, std::string_view name) {
  const auto found = response.headers.find(name);
  if (found == response.headers.end()) {
    throw std::runtime_error("Android node HTTP response lacks required " + std::string(name) +
                             " header");
  }
  return found->second;
}

std::map<std::string, std::string, std::less<>> BrowserHeaders(
    const Node &node, std::map<std::string, std::string, std::less<>> headers = {}) {
  if (!node.endpoint.UsesAdbForward()) {
    headers.emplace("Origin", kBrowserTestOrigin);
  }
  return headers;
}

void ValidateBrowserCorsHeaders(const HilHttpResponse &response) {
  if (RequiredHttpHeader(response, "access-control-allow-origin") != "*" ||
      RequiredHttpHeader(response, "access-control-allow-methods") !=
          "GET, HEAD, POST, PUT, OPTIONS" ||
      RequiredHttpHeader(response, "access-control-allow-headers") !=
          "Authorization, Range, Content-Type") {
    throw std::runtime_error("Android node browser CORS contract is incomplete");
  }
}

Json ValidateBrowserOrigin(const Node &node, std::chrono::steady_clock::time_point deadline) {
  if (node.endpoint.UsesAdbForward()) {
    throw std::invalid_argument("browser-origin validation requires a direct-LAN endpoint");
  }
  const HilHttpResponse preflight = RequireHilHttp(
      {.ipv4_host = node.endpoint.ipv4_address,
       .port = node.endpoint.port,
       .method = "OPTIONS",
       .path = "/api/v1/field-recording/start",
       .bearer_token = {},
       .body = {},
       .headers = {{"Access-Control-Request-Headers", "Authorization, Range, Content-Type"},
                   {"Access-Control-Request-Method", "POST"},
                   {"Origin", std::string(kBrowserTestOrigin)}},
       .deadline = deadline},
      204);
  ValidateBrowserCorsHeaders(preflight);
  const HilHttpResponse unauthorized =
      RequireHilHttp({.ipv4_host = node.endpoint.ipv4_address,
                      .port = node.endpoint.port,
                      .method = "POST",
                      .path = "/api/v1/field-recording/start",
                      .bearer_token = {},
                      .body = "{}",
                      .headers = {{"Origin", std::string(kBrowserTestOrigin)}},
                      .deadline = deadline},
                     401);
  ValidateBrowserCorsHeaders(unauthorized);
  if (!unauthorized.body.contains("valid bearer control credential")) {
    throw std::runtime_error("Android node browser-visible 401 is not actionable");
  }
  const HilHttpResponse unauthorized_status =
      RequireHilHttp({.ipv4_host = node.endpoint.ipv4_address,
                      .port = node.endpoint.port,
                      .method = "GET",
                      .path = "/api/v1/field-recording/status",
                      .bearer_token = {},
                      .body = {},
                      .headers = {{"Origin", std::string(kBrowserTestOrigin)}},
                      .deadline = deadline},
                     401);
  ValidateBrowserCorsHeaders(unauthorized_status);
  return {
      {"role", node.role},
      {"origin", node.endpoint.origin},
      {"preflight_status", preflight.status},
      {"unauthenticated_control_status", unauthorized.status},
      {"unauthenticated_metadata_status", unauthorized_status.status},
      {"allow_origin", "*"},
      {"passed", true},
  };
}

class Cleanup final {
 public:
  Cleanup(std::filesystem::path adb, std::string down_serial, std::string face_serial)
      : adb_(std::move(adb)),
        devices_({{"down_the_line", std::move(down_serial)}, {"face_on", std::move(face_serial)}}) {
  }
  Cleanup(const Cleanup &) = delete;
  Cleanup &operator=(const Cleanup &) = delete;
  ~Cleanup() {
    if (!finished_) {
      try {
        static_cast<void>(Finish());
      } catch (...) {
      }
    }
  }
  void Register(const Node &node) { nodes_.push_back(node); }

  Json Finish() {
    if (finished_) {
      return evidence_;
    }
    bool passed = true;
    Json attempts = Json::array();
    auto record = [&](std::string operation, std::string target, bool restored,
                      std::string diagnostic) {
      passed = passed && restored;
      Json attempt = {{"operation", std::move(operation)},
                      {"target", std::move(target)},
                      {"restored", restored}};
      if (!diagnostic.empty()) {
        attempt["diagnostic"] = std::move(diagnostic);
      }
      attempts.push_back(std::move(attempt));
    };
    for (const Node &node : nodes_) {
      try {
        const HilHttpResponse response = RequestHilHttp({
            .ipv4_host = node.endpoint.ipv4_address,
            .port = node.endpoint.port,
            .method = "POST",
            .path = "/api/v1/field-recording/stop",
            .bearer_token = node.control_token,
            .body = "{}",
            .headers = {},
            .deadline = std::chrono::steady_clock::now() + 2s,
        });
        record("field_recording_stop", node.role, response.status == 202,
               response.status == 202 ? "" : "HTTP " + std::to_string(response.status));
      } catch (const std::exception &failure) {
        record("field_recording_stop", node.role, false, failure.what());
      }
    }
    for (const Node &node : nodes_) {
      if (!node.endpoint.UsesAdbForward()) {
        continue;
      }
      try {
        const HilCommandResult result = RunHilCommand(
            adb_,
            DeviceArguments(node.serial,
                            {"forward", "--remove", "tcp:" + std::to_string(node.endpoint.port)}),
            std::chrono::steady_clock::now() + 2s);
        const bool restored = !result.timed_out && result.exit_code == 0;
        record("adb_forward_remove", node.role, restored, restored ? "" : result.output);
      } catch (const std::exception &failure) {
        record("adb_forward_remove", node.role, false, failure.what());
      }
    }
    for (const auto &[role, serial] : devices_) {
      try {
        const HilCommandResult sleep = RunHilCommand(
            adb_, DeviceArguments(serial, {"shell", "input", "keyevent", "KEYCODE_SLEEP"}),
            std::chrono::steady_clock::now() + 2s);
        const HilCommandResult state =
            RunHilCommand(adb_, DeviceArguments(serial, {"shell", "dumpsys", "power"}),
                          std::chrono::steady_clock::now() + 2s);
        const bool sleeping = state.output.find("mWakefulness=Dozing") != std::string::npos ||
                              state.output.find("mWakefulness=Asleep") != std::string::npos;
        const bool restored = !sleep.timed_out && sleep.exit_code == 0 && !state.timed_out &&
                              state.exit_code == 0 && sleeping;
        record("screen_sleep", role, restored,
               restored ? "" : "KEYCODE_SLEEP or wakefulness verification failed");
      } catch (const std::exception &failure) {
        record("screen_sleep", role, false, failure.what());
      }
    }
    evidence_ = {{"passed", passed}, {"attempts", std::move(attempts)}};
    passed_ = passed;
    finished_ = true;
    return evidence_;
  }

  [[nodiscard]] bool passed() const { return finished_ && passed_; }

 private:
  std::filesystem::path adb_;
  std::vector<std::pair<std::string, std::string>> devices_;
  std::vector<Node> nodes_;
  Json evidence_ = Json::object();
  bool passed_ = false;
  bool finished_ = false;
};

std::string SharedRecordingId() {
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  return "field-hil-" + std::to_string(milliseconds) + "-" + std::to_string(getpid());
}

Json RequireStatus(const Node &node, std::chrono::steady_clock::time_point deadline) {
  const HilHttpResponse response = RequireHilHttp({.ipv4_host = node.endpoint.ipv4_address,
                                                   .port = node.endpoint.port,
                                                   .method = "GET",
                                                   .path = "/api/v1/field-recording/status",
                                                   .bearer_token = node.control_token,
                                                   .body = {},
                                                   .headers = BrowserHeaders(node),
                                                   .deadline = deadline},
                                                  200);
  if (!node.endpoint.UsesAdbForward()) {
    ValidateBrowserCorsHeaders(response);
  }
  return Json::parse(response.body);
}

void WaitForState(const Node &down, const Node &face, std::string_view expected,
                  std::string_view shared_recording_id,
                  std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const Json down_status = RequireStatus(down, deadline);
    const Json face_status = RequireStatus(face, deadline);
    for (const Json *status : {&down_status, &face_status}) {
      if (status->value("state", "") == "error") {
        throw std::runtime_error("field recorder failed: " + status->value("error", "unknown"));
      }
    }
    if (down_status.value("state", "") == expected && face_status.value("state", "") == expected &&
        down_status.value("shared_recording_id", "") == shared_recording_id &&
        face_status.value("shared_recording_id", "") == shared_recording_id) {
      WriteArtifact(OutputDirectory() / "down_the_line" / (std::string(expected) + "-status.json"),
                    down_status.dump(2) + "\n");
      WriteArtifact(OutputDirectory() / "face_on" / (std::string(expected) + "-status.json"),
                    face_status.dump(2) + "\n");
      return;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("both Android nodes did not reach field-recording state " +
                           std::string(expected));
}

FieldRecordingArtifactProbe ProbeArtifact(const Node &node, std::string_view path,
                                          std::chrono::steady_clock::time_point deadline) {
  const HilHttpResponse head = RequireHilHttp({.ipv4_host = node.endpoint.ipv4_address,
                                               .port = node.endpoint.port,
                                               .method = "HEAD",
                                               .path = path,
                                               .bearer_token = node.control_token,
                                               .body = {},
                                               .headers = BrowserHeaders(node),
                                               .deadline = deadline},
                                              200);
  const HilHttpResponse range =
      RequireHilHttp({.ipv4_host = node.endpoint.ipv4_address,
                      .port = node.endpoint.port,
                      .method = "GET",
                      .path = path,
                      .bearer_token = node.control_token,
                      .body = {},
                      .headers = BrowserHeaders(node, {{"Range", "bytes=0-1023"}}),
                      .deadline = deadline},
                     206);
  if (!node.endpoint.UsesAdbForward()) {
    ValidateBrowserCorsHeaders(head);
    ValidateBrowserCorsHeaders(range);
  }
  return {
      .head_status = head.status,
      .head_headers = head.headers,
      .head_body_bytes = head.body.size(),
      .range_status = range.status,
      .range_headers = range.headers,
      .range_body = range.body,
  };
}

FieldRecordingBundleEvidence InspectBundle(const Node &node, std::string_view shared_recording_id,
                                           std::chrono::steady_clock::time_point deadline) {
  const std::string prefix = "/api/v1/field-recordings/" + std::string(shared_recording_id) + "/";
  const HilHttpResponse listing = RequireHilHttp({.ipv4_host = node.endpoint.ipv4_address,
                                                  .port = node.endpoint.port,
                                                  .method = "GET",
                                                  .path = "/api/v1/field-recordings",
                                                  .bearer_token = node.control_token,
                                                  .body = {},
                                                  .headers = BrowserHeaders(node),
                                                  .deadline = deadline},
                                                 200);
  const std::string manifest_path = prefix + "manifest";
  const HilHttpResponse manifest = RequireHilHttp({.ipv4_host = node.endpoint.ipv4_address,
                                                   .port = node.endpoint.port,
                                                   .method = "GET",
                                                   .path = manifest_path,
                                                   .bearer_token = node.control_token,
                                                   .body = {},
                                                   .headers = BrowserHeaders(node),
                                                   .deadline = deadline},
                                                  200);
  if (!node.endpoint.UsesAdbForward()) {
    ValidateBrowserCorsHeaders(listing);
    ValidateBrowserCorsHeaders(manifest);
  }
  const FieldRecordingArtifactProbe video = ProbeArtifact(node, prefix + "video.mp4", deadline);
  const FieldRecordingArtifactProbe audio = ProbeArtifact(node, prefix + "audio.wav", deadline);
  const std::filesystem::path output = OutputDirectory() / node.role;
  WriteArtifact(output / "recordings.json", listing.body);
  WriteArtifact(output / "manifest.json", manifest.body);
  WriteArtifact(output / "video-first-1024.bin", video.range_body);
  WriteArtifact(output / "audio-first-1024.bin", audio.range_body);
  return ValidateFieldRecordingBundle({
      .listing_json = listing.body,
      .manifest_json = manifest.body,
      .expected_shared_recording_id = shared_recording_id,
      .expected_role = node.role,
      .video = video,
      .audio = audio,
  });
}

Json EvidenceJson(const Node &node, const FieldRecordingBundleEvidence &evidence) {
  return {
      {"recording_id", evidence.recording_id},
      {"shared_recording_id", evidence.shared_recording_id},
      {"node_id", evidence.node_id},
      {"role", evidence.role},
      {"duration_us", std::to_string(evidence.duration_us)},
      {"video_bytes", std::to_string(evidence.video_bytes)},
      {"audio_frames", std::to_string(evidence.audio_frames)},
      {"audio_bytes", std::to_string(evidence.audio_bytes)},
      {"head_and_range_retrieval_passed", true},
      {"http_origin", node.endpoint.origin},
      {"host_control_transport", FieldRecordingHilHttpTransportName(node.endpoint.transport)},
      {"adb_forward_used", node.endpoint.UsesAdbForward()},
  };
}

Json ApkInstallJson(std::string_view role, const AndroidApkInstallEvidence &evidence) {
  return {
      {"role", role},
      {"installed", evidence.installed},
      {"reason", evidence.reason},
      {"sha256", evidence.sha256},
      {"installed_before_sha256", evidence.installed_before_sha256.has_value()
                                      ? Json(*evidence.installed_before_sha256)
                                      : Json(nullptr)},
      {"elapsed_ms", evidence.elapsed_ms},
      {"installed_after_matches_current_apk", true},
  };
}

int Run(int argument_count, char **arguments) {
  const bool direct_lan = argument_count == 5 && std::string_view(arguments[4]) == "direct-lan";
  if (argument_count != 4 && !direct_lan) {
    throw std::runtime_error("expected Bazel runfiles: <adb> <APK> <APK-installer> [direct-lan]");
  }
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + kOverallDeadline;
  const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
  const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
  const std::filesystem::path installer = std::filesystem::absolute(arguments[3]);
  if (!std::filesystem::is_regular_file(adb) || access(adb.c_str(), X_OK) != 0 ||
      !std::filesystem::is_regular_file(apk) || !std::filesystem::is_regular_file(installer) ||
      access(installer.c_str(), X_OK) != 0) {
    throw std::runtime_error("adb, APK, or APK-installer Bazel runfile is invalid");
  }
  const std::string down_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_SERIAL");
  const std::string face_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_SERIAL");
  if (down_serial == face_serial) {
    throw std::runtime_error("field-recording HIL requires two distinct Android serials");
  }
  const std::optional<FieldRecordingHilDirectLanPair> direct_lan_pair =
      direct_lan ? std::optional<FieldRecordingHilDirectLanPair>(ParseFieldRecordingDirectLanPair(
                       RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN"),
                       RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN")))
                 : std::nullopt;
  const FieldRecordingHilHttpTransport http_transport =
      direct_lan ? FieldRecordingHilHttpTransport::kDirectLan
                 : FieldRecordingHilHttpTransport::kAdbForward;
  Cleanup cleanup(adb, down_serial, face_serial);
  Json apk_installs = Json::array();
  Json browser_origin_evidence = Json::array();
  std::string shared_recording_id;
  try {
    const bool skip_exact = SkipExactApkInstall();
    apk_installs.push_back(
        ApkInstallJson("down_the_line",
                       InstallAndroidApk(installer, adb, apk, down_serial, skip_exact, deadline)));
    apk_installs.push_back(ApkInstallJson(
        "face_on", InstallAndroidApk(installer, adb, apk, face_serial, skip_exact, deadline)));

    Node down = PrepareNode(
        adb, down_serial, "down_the_line",
        direct_lan ? std::optional(direct_lan_pair->down_the_line) : std::nullopt, deadline);
    cleanup.Register(down);
    Node face =
        PrepareNode(adb, face_serial, "face_on",
                    direct_lan ? std::optional(direct_lan_pair->face_on) : std::nullopt, deadline);
    cleanup.Register(face);
    if (down.node_id == face.node_id) {
      throw std::runtime_error("configured phones advertise the same node identity");
    }
    if (direct_lan) {
      auto down_browser =
          std::async(std::launch::async, [&] { return ValidateBrowserOrigin(down, deadline); });
      const Json face_browser = ValidateBrowserOrigin(face, deadline);
      browser_origin_evidence.push_back(down_browser.get());
      browser_origin_evidence.push_back(face_browser);
    }

    shared_recording_id = SharedRecordingId();
    const std::string start_body =
        Json({{"schema_version", 1}, {"shared_recording_id", shared_recording_id}}).dump();
    auto down_start_future = std::async(std::launch::async, [&] {
      return RequireHilHttp({.ipv4_host = down.endpoint.ipv4_address,
                             .port = down.endpoint.port,
                             .method = "POST",
                             .path = "/api/v1/field-recording/start",
                             .bearer_token = down.control_token,
                             .body = start_body,
                             .headers = BrowserHeaders(down),
                             .deadline = deadline},
                            202);
    });
    const HilHttpResponse face_start = RequireHilHttp({.ipv4_host = face.endpoint.ipv4_address,
                                                       .port = face.endpoint.port,
                                                       .method = "POST",
                                                       .path = "/api/v1/field-recording/start",
                                                       .bearer_token = face.control_token,
                                                       .body = start_body,
                                                       .headers = BrowserHeaders(face),
                                                       .deadline = deadline},
                                                      202);
    const HilHttpResponse down_start = down_start_future.get();
    if (direct_lan) {
      ValidateBrowserCorsHeaders(down_start);
      ValidateBrowserCorsHeaders(face_start);
    }
    WaitForState(down, face, "recording", shared_recording_id, deadline);
    if (std::chrono::steady_clock::now() + kRecordingDuration >= deadline) {
      throw std::runtime_error("field recorders became ready too late for bounded media capture");
    }
    std::this_thread::sleep_for(kRecordingDuration);
    auto down_stop_future = std::async(std::launch::async, [&] {
      return RequireHilHttp({.ipv4_host = down.endpoint.ipv4_address,
                             .port = down.endpoint.port,
                             .method = "POST",
                             .path = "/api/v1/field-recording/stop",
                             .bearer_token = down.control_token,
                             .body = "{}",
                             .headers = BrowserHeaders(down),
                             .deadline = deadline},
                            202);
    });
    const HilHttpResponse face_stop = RequireHilHttp({.ipv4_host = face.endpoint.ipv4_address,
                                                      .port = face.endpoint.port,
                                                      .method = "POST",
                                                      .path = "/api/v1/field-recording/stop",
                                                      .bearer_token = face.control_token,
                                                      .body = "{}",
                                                      .headers = BrowserHeaders(face),
                                                      .deadline = deadline},
                                                     202);
    const HilHttpResponse down_stop = down_stop_future.get();
    if (direct_lan) {
      ValidateBrowserCorsHeaders(down_stop);
      ValidateBrowserCorsHeaders(face_stop);
    }
    WaitForState(down, face, "ready", shared_recording_id, deadline);

    auto down_bundle = std::async(
        std::launch::async, [&] { return InspectBundle(down, shared_recording_id, deadline); });
    const FieldRecordingBundleEvidence face_evidence =
        InspectBundle(face, shared_recording_id, deadline);
    const FieldRecordingBundleEvidence down_evidence = down_bundle.get();
    ValidateFieldRecordingPair(down_evidence, face_evidence, shared_recording_id);
    const Json cleanup_evidence = cleanup.Finish();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const bool passed = cleanup.passed() && elapsed < kOverallDeadline;
    const Json report = {
        {"schema_version", 1},
        {"report_type", "android_dual_field_recording_smoke_hil"},
        {"passed", passed},
        {"error", passed ? "" : "cleanup failed or total duration exceeded 15 seconds"},
        {"shared_recording_id", shared_recording_id},
        {"elapsed_ms", elapsed.count()},
        {"recording_duration_requested_ms",
         std::chrono::duration_cast<std::chrono::milliseconds>(kRecordingDuration).count()},
        {"exact_apk_install_verified", true},
        {"host_control_transport", FieldRecordingHilHttpTransportName(http_transport)},
        {"adb_forward_used", !direct_lan},
        {"browser_origin_validation_performed", direct_lan},
        {"browser_origin_validation", browser_origin_evidence},
        {"apk_installs", std::move(apk_installs)},
        {"cleanup", cleanup_evidence},
        {"nodes",
         Json::array({EvidenceJson(down, down_evidence), EvidenceJson(face, face_evidence)})},
    };
    WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
    std::cout << report.dump(2) << '\n';
    return passed ? 0 : 1;
  } catch (const std::exception &failure) {
    const Json cleanup_evidence = cleanup.Finish();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const Json report = {
        {"schema_version", 1},
        {"report_type", "android_dual_field_recording_smoke_hil"},
        {"passed", false},
        {"error", failure.what()},
        {"shared_recording_id",
         shared_recording_id.empty() ? Json(nullptr) : Json(shared_recording_id)},
        {"elapsed_ms", elapsed.count()},
        {"recording_duration_requested_ms",
         std::chrono::duration_cast<std::chrono::milliseconds>(kRecordingDuration).count()},
        {"exact_apk_install_verified", apk_installs.size() == 2U},
        {"host_control_transport", FieldRecordingHilHttpTransportName(http_transport)},
        {"adb_forward_used", !direct_lan},
        {"browser_origin_validation_performed", direct_lan},
        {"browser_origin_validation", browser_origin_evidence},
        {"apk_installs", std::move(apk_installs)},
        {"cleanup", cleanup_evidence},
        {"nodes", Json::array()},
    };
    WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
    std::cerr << report.dump(2) << '\n';
    return 1;
  }
}

}  // namespace
}  // namespace swing_capture::android::dual_hil

int main(int argument_count, char **arguments) {
  std::signal(SIGPIPE, SIG_IGN);
  try {
    return swing_capture::android::dual_hil::Run(argument_count, arguments);
  } catch (const std::exception &failure) {
    try {
      const bool direct_lan_requested =
          argument_count == 5 && std::string_view(arguments[4]) == "direct-lan";
      const nlohmann::json report = {
          {"schema_version", 1},
          {"report_type", "android_dual_field_recording_smoke_hil"},
          {"passed", false},
          {"error", failure.what()},
          {"shared_recording_id", nullptr},
          {"exact_apk_install_verified", false},
          {"host_control_transport", direct_lan_requested ? "wifi_lan_direct" : "adb_forward"},
          {"adb_forward_used", !direct_lan_requested},
          {"browser_origin_validation_performed", false},
          {"browser_origin_validation", nlohmann::json::array()},
          {"apk_installs", nlohmann::json::array()},
          {"cleanup", nullptr},
          {"nodes", nlohmann::json::array()},
      };
      swing_capture::android::dual_hil::WriteArtifact(
          swing_capture::android::dual_hil::OutputDirectory() / "report.json",
          report.dump(2) + "\n");
    } catch (...) {
    }
    std::cerr << "field-recording HIL failed: " << failure.what() << '\n';
    return 1;
  }
}
