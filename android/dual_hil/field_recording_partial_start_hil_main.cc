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
#include <initializer_list>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "android/dual_hil/android_apk_install.h"
#include "android/dual_hil/field_recording_async_start_failure_validation.h"
#include "android/dual_hil/field_recording_partial_start_validation.h"
#include "android/dual_hil/hil_command.h"
#include "android/dual_hil/hil_http_client.h"

namespace swing_capture::android::dual_hil {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

constexpr std::string_view kPackageName = "com.agoessling.swingcapture";
constexpr std::string_view kActivity = "com.agoessling.swingcapture/.MainActivity";
constexpr std::uint16_t kNodeHttpPort = 8088;
constexpr auto kOverallDeadline = 15s;
constexpr auto kWorkDeadline = 12s;
constexpr auto kPollInterval = 100ms;

struct Node {
  std::string serial;
  std::string role;
  std::string node_id;
  std::uint16_t host_port = 0;
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
    throw std::runtime_error("set --test_env=" + std::string(name) + "=<adb-serial>");
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

std::filesystem::path OutputDirectory() {
  const std::string value = EnvironmentValue("TEST_UNDECLARED_OUTPUTS_DIR");
  return value.empty() ? std::filesystem::current_path() : std::filesystem::path(value);
}

void WriteArtifact(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open partial-start HIL artifact " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    throw std::runtime_error("cannot write partial-start HIL artifact " + path.string());
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
  if (token.size() != 32U || !std::ranges::all_of(token, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' || character == '-';
      })) {
    throw std::runtime_error("Android node bearer credential has an invalid format");
  }
  return token;
}

HilHttpResponse Request(const Node &node, std::string_view method, std::string_view path,
                        std::string_view body, std::chrono::steady_clock::time_point deadline) {
  return RequestHilHttp({
      .ipv4_host = "127.0.0.1",
      .port = node.host_port,
      .method = method,
      .path = path,
      .bearer_token = method == "POST" ? std::string_view(node.control_token) : std::string_view(),
      .body = body,
      .headers = {},
      .deadline = deadline,
  });
}

Json RequireStatus(const Node &node, std::chrono::steady_clock::time_point deadline) {
  return Json::parse(RequireHilHttp(
                         {
                             .ipv4_host = "127.0.0.1",
                             .port = node.host_port,
                             .method = "GET",
                             .path = "/api/v1/field-recording/status",
                             .bearer_token = node.control_token,
                             .body = {},
                             .headers = {},
                             .deadline = deadline,
                         },
                         200)
                         .body);
}

std::string JsonString(const Json &object, std::string_view key) {
  const auto found = object.find(std::string(key));
  return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

Node PrepareNode(const std::filesystem::path &adb, std::string serial, std::string role,
                 bool reject_next_start, bool fail_next_accepted_start,
                 std::chrono::steady_clock::time_point deadline) {
  RunRequiredAdb(adb, DeviceArguments(serial, {"get-state"}), deadline);
  std::vector<std::string> launch = DeviceArguments(
      serial, {"shell", "am", "start", "-W", "--activity-single-top", "-n", kActivity});
  if (reject_next_start) {
    launch.insert(launch.end(), {"--ez", "reject_next_field_recording_start_hil", "true"});
  }
  if (fail_next_accepted_start) {
    launch.insert(launch.end(), {"--ez", "fail_next_accepted_field_recording_start_hil", "true"});
  }
  RunRequiredAdb(adb, launch, deadline);
  const std::uint16_t port = EstablishForward(adb, serial, deadline);
  try {
    Node node{
        .serial = serial,
        .role = role,
        .node_id = {},
        .host_port = port,
        .control_token = {},
    };
    node.control_token = ReadControlToken(adb, node.serial, deadline);
    while (std::chrono::steady_clock::now() < deadline) {
      try {
        const Json identity = Json::parse(RequireHilHttp(
                                              {
                                                  .ipv4_host = "127.0.0.1",
                                                  .port = node.host_port,
                                                  .method = "GET",
                                                  .path = "/api/v1/node",
                                                  .bearer_token = node.control_token,
                                                  .body = {},
                                                  .headers = {},
                                                  .deadline = deadline,
                                              },
                                              200)
                                              .body);
        if (identity.value("schema_version", 0) != 1 || identity.value("role", "") != node.role ||
            identity.value("node_id", "").empty()) {
          throw std::runtime_error("configured Android node identity or role is incorrect");
        }
        node.node_id = identity.at("node_id").get<std::string>();
        const Json status = RequireStatus(node, deadline);
        const std::string state = status.value("state", "");
        if (state != "idle" && state != "ready") {
          throw std::runtime_error("Android node already has an active field recording");
        }
        if (status.value("hil_reject_next_start", false) != reject_next_start) {
          throw std::runtime_error("Android node field-recording HIL fault state is incorrect");
        }
        if (status.value("hil_fail_next_accepted_start", false) != fail_next_accepted_start) {
          throw std::runtime_error("Android node accepted-start HIL fault state is incorrect");
        }
        return node;
      } catch (const nlohmann::json::exception &) {
        throw std::runtime_error("Android node readiness response is malformed");
      } catch (const std::runtime_error &failure) {
        const std::string_view diagnostic(failure.what());
        if (diagnostic.find("connect") == std::string_view::npos &&
            diagnostic.find("endpoint") == std::string_view::npos) {
          throw;
        }
        std::this_thread::sleep_for(kPollInterval);
      }
    }
    throw std::runtime_error("Android node HTTP service did not become ready");
  } catch (...) {
    static_cast<void>(RunHilCommand(
        adb, DeviceArguments(serial, {"forward", "--remove", "tcp:" + std::to_string(port)}),
        deadline));
    throw;
  }
}

Json WaitForState(const Node &node, std::string_view expected_state, std::string_view recording_id,
                  std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const Json status = RequireStatus(node, deadline);
    if (status.value("state", "") == "error") {
      throw std::runtime_error(node.role +
                               " field recorder failed: " + status.value("error", "unknown"));
    }
    if (status.value("state", "") == expected_state &&
        JsonString(status, "shared_recording_id") == recording_id) {
      return status;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error(node.role + " did not reach field-recording state " +
                           std::string(expected_state));
}

void RequireListing(const Node &node, std::string_view required_id,
                    std::optional<std::string_view> forbidden_id,
                    std::chrono::steady_clock::time_point deadline) {
  const Json listing = Json::parse(RequireHilHttp(
                                       {
                                           .ipv4_host = "127.0.0.1",
                                           .port = node.host_port,
                                           .method = "GET",
                                           .path = "/api/v1/field-recordings",
                                           .bearer_token = node.control_token,
                                           .body = {},
                                           .headers = {},
                                           .deadline = deadline,
                                       },
                                       200)
                                       .body);
  bool found_required = false;
  bool found_forbidden = false;
  for (const Json &recording : listing.at("recordings")) {
    const std::string id = recording.value("recording_id", "");
    found_required = found_required || id == required_id;
    found_forbidden = found_forbidden || (forbidden_id.has_value() && id == *forbidden_id);
  }
  if (!found_required || found_forbidden) {
    throw std::runtime_error(node.role + " field-recording listing violates rollback");
  }
  WriteArtifact(OutputDirectory() / node.role / "recordings.json", listing.dump(2) + "\n");
}

std::string SharedRecordingId(std::string_view suffix) {
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  return "field-partial-hil-" + std::to_string(milliseconds) + "-" + std::to_string(getpid()) +
         "-" + std::string(suffix);
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

class Cleanup final {
 public:
  Cleanup(std::filesystem::path adb, std::string down_serial, std::string face_serial,
          std::chrono::steady_clock::time_point deadline)
      : adb_(std::move(adb)),
        down_serial_(std::move(down_serial)),
        face_serial_(std::move(face_serial)),
        deadline_(deadline) {}
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
        const HilHttpResponse response =
            Request(node, "POST", "/api/v1/field-recording/stop", "{}", deadline_);
        record("field_recording_stop", node.role, response.status == 202,
               response.status == 202 ? "" : "HTTP " + std::to_string(response.status));
      } catch (const std::exception &failure) {
        record("field_recording_stop", node.role, false, failure.what());
      }
    }
    try {
      RunRequiredAdb(
          adb_,
          DeviceArguments(face_serial_,
                          {"shell", "am", "start", "-W", "--activity-single-top", "-n", kActivity,
                           "--ez", "clear_field_recording_start_hil", "true"}),
          deadline_);
      bool cleared = false;
      for (const Node &node : nodes_) {
        if (node.role == "face_on") {
          while (std::chrono::steady_clock::now() < deadline_) {
            if (!RequireStatus(node, deadline_).value("hil_reject_next_start", true)) {
              const Json status = RequireStatus(node, deadline_);
              if (!status.value("hil_fail_next_accepted_start", true) &&
                  !status.value("hil_accepted_start_waiting", true)) {
                cleared = true;
                break;
              }
            }
            std::this_thread::sleep_for(kPollInterval);
          }
        }
      }
      record("field_recording_hil_fault_clear", "face_on", cleared,
             cleared ? "" : "HIL fault did not report cleared");
    } catch (const std::exception &failure) {
      record("field_recording_hil_fault_clear", "face_on", false, failure.what());
    }
    for (const Node &node : nodes_) {
      bool terminal = false;
      std::string diagnostic;
      try {
        while (std::chrono::steady_clock::now() < deadline_) {
          const Json status = RequireStatus(node, deadline_);
          const std::string state = status.value("state", "");
          if (state == "idle" || state == "ready") {
            WriteArtifact(OutputDirectory() / node.role / "cleanup-terminal-status.json",
                          status.dump(2) + "\n");
            terminal = true;
            break;
          }
          if (state == "error") {
            const HilHttpResponse acknowledgement =
                Request(node, "POST", "/api/v1/field-recording/stop", "{}", deadline_);
            if (acknowledgement.status != 202) {
              diagnostic =
                  "failure acknowledgement returned HTTP " + std::to_string(acknowledgement.status);
              break;
            }
          }
          std::this_thread::sleep_for(kPollInterval);
        }
        if (!terminal && diagnostic.empty()) {
          diagnostic = "field recorder did not converge to idle or ready";
        }
      } catch (const std::exception &failure) {
        diagnostic = failure.what();
      }
      record("field_recording_terminal", node.role, terminal, diagnostic);
    }
    for (const Node &node : nodes_) {
      try {
        const HilCommandResult result = RunHilCommand(
            adb_,
            DeviceArguments(node.serial,
                            {"forward", "--remove", "tcp:" + std::to_string(node.host_port)}),
            deadline_);
        const bool restored = !result.timed_out && result.exit_code == 0;
        record("adb_forward_remove", node.role, restored, restored ? "" : result.output);
      } catch (const std::exception &failure) {
        record("adb_forward_remove", node.role, false, failure.what());
      }
    }
    for (const auto &[role, serial] : std::vector<std::pair<std::string_view, std::string_view>>{
             {"down_the_line", down_serial_}, {"face_on", face_serial_}}) {
      try {
        const HilCommandResult sleep = RunHilCommand(
            adb_, DeviceArguments(serial, {"shell", "input", "keyevent", "KEYCODE_SLEEP"}),
            deadline_);
        const HilCommandResult state =
            RunHilCommand(adb_, DeviceArguments(serial, {"shell", "dumpsys", "power"}), deadline_);
        const bool sleeping = state.output.find("mWakefulness=Dozing") != std::string::npos ||
                              state.output.find("mWakefulness=Asleep") != std::string::npos;
        const bool restored = !sleep.timed_out && sleep.exit_code == 0 && !state.timed_out &&
                              state.exit_code == 0 && sleeping;
        record("screen_sleep", std::string(role), restored,
               restored ? "" : "KEYCODE_SLEEP or wakefulness verification failed");
      } catch (const std::exception &failure) {
        record("screen_sleep", std::string(role), false, failure.what());
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
  std::string down_serial_;
  std::string face_serial_;
  std::chrono::steady_clock::time_point deadline_;
  std::vector<Node> nodes_;
  Json evidence_ = Json::object();
  bool passed_ = false;
  bool finished_ = false;
};

Json StatusEvidence(const Json &status) {
  return {
      {"state", status.value("state", "")},
      {"shared_recording_id", JsonString(status, "shared_recording_id")},
      {"error", status.value("error", "")},
      {"hil_reject_next_start", status.value("hil_reject_next_start", false)},
      {"hil_fail_next_accepted_start", status.value("hil_fail_next_accepted_start", false)},
      {"hil_accepted_start_waiting", status.value("hil_accepted_start_waiting", false)},
  };
}

Json WaitForError(const Node &node, std::string_view recording_id,
                  std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const Json status = RequireStatus(node, deadline);
    if (status.value("state", "") == "error" &&
        JsonString(status, "shared_recording_id") == recording_id) {
      return status;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error(node.role +
                           " did not surface the asynchronous field-recording start failure");
}

Json WaitForIdle(const Node &node, std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const Json status = RequireStatus(node, deadline);
    if (status.value("state", "") == "idle" && JsonString(status, "shared_recording_id").empty()) {
      return status;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error(node.role + " did not return to idle after failure acknowledgement");
}

void ReleaseAcceptedStartFailure(const std::filesystem::path &adb, std::string_view serial,
                                 std::chrono::steady_clock::time_point deadline) {
  RunRequiredAdb(
      adb,
      DeviceArguments(
          serial, {"shell", "am", "start", "-W", "--activity-single-top", "-n", kActivity, "--ez",
                   "release_accepted_field_recording_start_failure_hil", "true"}),
      deadline);
}

bool ListingContains(const Node &node, std::string_view recording_id,
                     std::string_view artifact_name,
                     std::chrono::steady_clock::time_point deadline) {
  const Json listing = Json::parse(RequireHilHttp(
                                       {
                                           .ipv4_host = "127.0.0.1",
                                           .port = node.host_port,
                                           .method = "GET",
                                           .path = "/api/v1/field-recordings",
                                           .bearer_token = node.control_token,
                                           .body = {},
                                           .headers = {},
                                           .deadline = deadline,
                                       },
                                       200)
                                       .body);
  WriteArtifact(OutputDirectory() / node.role / std::string(artifact_name), listing.dump(2) + "\n");
  for (const Json &recording : listing.at("recordings")) {
    if (recording.value("recording_id", "") == recording_id) {
      return true;
    }
  }
  return false;
}

int RunAsyncStartFailure(int argument_count, char **arguments) {
  if (argument_count != 5 || std::string_view(arguments[4]) != "async-start-failure") {
    throw std::runtime_error(
        "expected Bazel runfiles: <adb> <APK> <APK-installer> async-start-failure");
  }
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + kOverallDeadline;
  const auto work_deadline = started + kWorkDeadline;
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
    throw std::runtime_error(
        "asynchronous start-failure HIL requires two distinct Android serials");
  }
  Cleanup cleanup(adb, down_serial, face_serial, deadline);
  Json apk_installs = Json::array();
  std::string failed_id;
  std::string retry_id;
  try {
    const bool skip_exact = SkipExactApkInstall();
    apk_installs.push_back(ApkInstallJson(
        "down_the_line",
        InstallAndroidApk(installer, adb, apk, down_serial, skip_exact, work_deadline)));
    apk_installs.push_back(ApkInstallJson(
        "face_on", InstallAndroidApk(installer, adb, apk, face_serial, skip_exact, work_deadline)));

    Node down = PrepareNode(adb, down_serial, "down_the_line", false, false, work_deadline);
    cleanup.Register(down);
    Node face = PrepareNode(adb, face_serial, "face_on", false, true, work_deadline);
    cleanup.Register(face);
    if (down.node_id == face.node_id) {
      throw std::runtime_error("configured phones advertise the same node identity");
    }

    failed_id = SharedRecordingId("async-first");
    const Json face_armed = RequireStatus(face, work_deadline);
    WriteArtifact(OutputDirectory() / "face_on" / "armed-accepted-fault-status.json",
                  face_armed.dump(2) + "\n");
    const std::string failed_body =
        Json({{"schema_version", 1}, {"shared_recording_id", failed_id}}).dump();
    const HilHttpResponse down_initial =
        Request(down, "POST", "/api/v1/field-recording/start", failed_body, work_deadline);
    const HilHttpResponse face_initial =
        Request(face, "POST", "/api/v1/field-recording/start", failed_body, work_deadline);
    const Json down_initial_body = Json::parse(down_initial.body);
    const Json face_initial_body = Json::parse(face_initial.body);
    WriteArtifact(OutputDirectory() / "down_the_line" / "initial-start-response.json",
                  down_initial_body.dump(2) + "\n");
    WriteArtifact(OutputDirectory() / "face_on" / "initial-start-response.json",
                  face_initial_body.dump(2) + "\n");
    if (down_initial.status != 202 || face_initial.status != 202) {
      throw std::runtime_error("both asynchronous-failure start requests must return HTTP 202");
    }

    Json face_waiting;
    while (std::chrono::steady_clock::now() < work_deadline) {
      face_waiting = RequireStatus(face, work_deadline);
      if (face_waiting.value("hil_accepted_start_waiting", false)) {
        break;
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    if (!face_waiting.value("hil_accepted_start_waiting", false)) {
      throw std::runtime_error("faulted node did not wait at the post-acceptance start gate");
    }
    WriteArtifact(OutputDirectory() / "face_on" / "waiting-after-both-202.json",
                  face_waiting.dump(2) + "\n");
    ReleaseAcceptedStartFailure(adb, face.serial, work_deadline);
    const Json face_error = WaitForError(face, failed_id, work_deadline);
    WriteArtifact(OutputDirectory() / "face_on" / "asynchronous-start-error.json",
                  face_error.dump(2) + "\n");

    const HilHttpResponse down_rollback =
        Request(down, "POST", "/api/v1/field-recording/stop", "{}", work_deadline);
    const HilHttpResponse face_ack =
        Request(face, "POST", "/api/v1/field-recording/stop", "{}", work_deadline);
    const Json face_ack_body = Json::parse(face_ack.body);
    WriteArtifact(OutputDirectory() / "face_on" / "failure-ack-response.json",
                  face_ack_body.dump(2) + "\n");
    const Json down_rollback_ready = WaitForState(down, "ready", failed_id, work_deadline);
    const Json face_idle = WaitForIdle(face, work_deadline);
    WriteArtifact(OutputDirectory() / "down_the_line" / "rollback-status.json",
                  down_rollback_ready.dump(2) + "\n");
    WriteArtifact(OutputDirectory() / "face_on" / "acknowledged-status.json",
                  face_idle.dump(2) + "\n");
    const bool down_rollback_visible =
        ListingContains(down, failed_id, "rollback-recordings.json", work_deadline);
    const bool face_failed_absent =
        !ListingContains(face, failed_id, "failed-recordings.json", work_deadline);

    retry_id = SharedRecordingId("async-retry");
    const std::string retry_body =
        Json({{"schema_version", 1}, {"shared_recording_id", retry_id}}).dump();
    const HilHttpResponse down_retry =
        Request(down, "POST", "/api/v1/field-recording/start", retry_body, work_deadline);
    const HilHttpResponse face_retry =
        Request(face, "POST", "/api/v1/field-recording/start", retry_body, work_deadline);
    const Json down_recording = WaitForState(down, "recording", retry_id, work_deadline);
    const Json face_recording = WaitForState(face, "recording", retry_id, work_deadline);
    WriteArtifact(OutputDirectory() / "down_the_line" / "retry-recording-status.json",
                  down_recording.dump(2) + "\n");
    WriteArtifact(OutputDirectory() / "face_on" / "retry-recording-status.json",
                  face_recording.dump(2) + "\n");
    const HilHttpResponse down_stop =
        Request(down, "POST", "/api/v1/field-recording/stop", "{}", work_deadline);
    const HilHttpResponse face_stop =
        Request(face, "POST", "/api/v1/field-recording/stop", "{}", work_deadline);
    const Json down_terminal = WaitForState(down, "ready", retry_id, work_deadline);
    const Json face_terminal = WaitForState(face, "ready", retry_id, work_deadline);
    WriteArtifact(OutputDirectory() / "down_the_line" / "terminal-status.json",
                  down_terminal.dump(2) + "\n");
    WriteArtifact(OutputDirectory() / "face_on" / "terminal-status.json",
                  face_terminal.dump(2) + "\n");
    const bool down_retry_visible =
        ListingContains(down, retry_id, "retry-recordings.json", work_deadline);
    const bool face_retry_visible =
        ListingContains(face, retry_id, "retry-recordings.json", work_deadline);

    const Json cleanup_evidence = cleanup.Finish();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const FieldRecordingAsyncStartFailureEvidence evidence{
        .failed_recording_id = failed_id,
        .retry_recording_id = retry_id,
        .down_initial_start_status = down_initial.status,
        .face_initial_start_status = face_initial.status,
        .face_initial_response_state = face_initial_body.value("state", ""),
        .face_fault_armed_before_start = face_armed.value("hil_fail_next_accepted_start", false),
        .face_start_waiting_after_both_accepted =
            face_waiting.value("hil_accepted_start_waiting", false),
        .face_fault_consumed_after_failure =
            !face_error.value("hil_fail_next_accepted_start", true) &&
            !face_error.value("hil_accepted_start_waiting", true),
        .face_failure_state = face_error.value("state", ""),
        .face_failure_recording_id = JsonString(face_error, "shared_recording_id"),
        .face_failure_error = face_error.value("error", ""),
        .down_rollback_stop_status = down_rollback.status,
        .face_failure_ack_status = face_ack.status,
        .down_rollback_state = down_rollback_ready.value("state", ""),
        .down_rollback_recording_id = JsonString(down_rollback_ready, "shared_recording_id"),
        .face_acknowledged_state = face_idle.value("state", ""),
        .face_acknowledged_recording_id = JsonString(face_idle, "shared_recording_id"),
        .face_failure_acknowledged = face_ack_body.value("acknowledged_terminal_failure", false),
        .down_rollback_bundle_visible = down_rollback_visible,
        .face_failed_bundle_absent = face_failed_absent,
        .down_retry_start_status = down_retry.status,
        .face_retry_start_status = face_retry.status,
        .down_retry_recording_state = down_recording.value("state", ""),
        .down_retry_recording_id = JsonString(down_recording, "shared_recording_id"),
        .face_retry_recording_state = face_recording.value("state", ""),
        .face_retry_recording_id = JsonString(face_recording, "shared_recording_id"),
        .down_retry_stop_status = down_stop.status,
        .face_retry_stop_status = face_stop.status,
        .down_terminal_state = down_terminal.value("state", ""),
        .down_terminal_recording_id = JsonString(down_terminal, "shared_recording_id"),
        .face_terminal_state = face_terminal.value("state", ""),
        .face_terminal_recording_id = JsonString(face_terminal, "shared_recording_id"),
        .down_retry_bundle_visible = down_retry_visible,
        .face_retry_bundle_visible = face_retry_visible,
        .cleanup_passed = cleanup.passed(),
        .elapsed_ms = elapsed.count(),
    };
    ValidateFieldRecordingAsyncStartFailure(evidence);
    const Json report = {
        {"schema_version", 1},
        {"report_type", "android_dual_field_recording_async_start_failure_hil"},
        {"passed", true},
        {"error", ""},
        {"elapsed_ms", elapsed.count()},
        {"exact_apk_install_verified", true},
        {"apk_installs", std::move(apk_installs)},
        {"initial_accepted_start",
         {{"shared_recording_id", failed_id},
          {"down_the_line_http_status", down_initial.status},
          {"face_on_http_status", face_initial.status},
          {"face_on_response", StatusEvidence(face_initial_body)},
          {"face_on_fault_armed_before_start", evidence.face_fault_armed_before_start},
          {"face_on_waiting_after_both_202", evidence.face_start_waiting_after_both_accepted}}},
        {"asynchronous_failure", StatusEvidence(face_error)},
        {"rollback",
         {{"down_the_line_stop_http_status", down_rollback.status},
          {"face_on_ack_http_status", face_ack.status},
          {"face_on_failure_acknowledged", evidence.face_failure_acknowledged},
          {"down_the_line_terminal", StatusEvidence(down_rollback_ready)},
          {"face_on_terminal", StatusEvidence(face_idle)},
          {"down_the_line_bundle_visible", down_rollback_visible},
          {"face_on_failed_bundle_absent", face_failed_absent}}},
        {"retry",
         {{"shared_recording_id", retry_id},
          {"fresh_id", retry_id != failed_id},
          {"down_the_line_start_http_status", down_retry.status},
          {"face_on_start_http_status", face_retry.status},
          {"down_the_line_stop_http_status", down_stop.status},
          {"face_on_stop_http_status", face_stop.status},
          {"down_the_line_terminal", StatusEvidence(down_terminal)},
          {"face_on_terminal", StatusEvidence(face_terminal)},
          {"down_the_line_bundle_visible", down_retry_visible},
          {"face_on_bundle_visible", face_retry_visible}}},
        {"cleanup", cleanup_evidence},
    };
    WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
    std::cout << report.dump(2) << '\n';
    return 0;
  } catch (const std::exception &failure) {
    const Json cleanup_evidence = cleanup.Finish();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const Json report = {
        {"schema_version", 1},
        {"report_type", "android_dual_field_recording_async_start_failure_hil"},
        {"passed", false},
        {"error", failure.what()},
        {"elapsed_ms", elapsed.count()},
        {"exact_apk_install_verified", apk_installs.size() == 2U},
        {"apk_installs", std::move(apk_installs)},
        {"failed_recording_id", failed_id.empty() ? Json(nullptr) : Json(failed_id)},
        {"retry_recording_id", retry_id.empty() ? Json(nullptr) : Json(retry_id)},
        {"cleanup", cleanup_evidence},
    };
    WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
    std::cerr << report.dump(2) << '\n';
    return 1;
  }
}

int RunPartialStart(int argument_count, char **arguments) {
  if (argument_count != 4) {
    throw std::runtime_error("expected Bazel runfiles: <adb> <APK> <APK-installer>");
  }
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + kOverallDeadline;
  const auto work_deadline = started + kWorkDeadline;
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
    throw std::runtime_error("partial-start HIL requires two distinct Android serials");
  }
  Cleanup cleanup(adb, down_serial, face_serial, deadline);
  Json apk_installs = Json::array();
  std::string rejected_id;
  std::string retry_id;
  try {
    const bool skip_exact = SkipExactApkInstall();
    apk_installs.push_back(ApkInstallJson(
        "down_the_line",
        InstallAndroidApk(installer, adb, apk, down_serial, skip_exact, work_deadline)));
    apk_installs.push_back(ApkInstallJson(
        "face_on", InstallAndroidApk(installer, adb, apk, face_serial, skip_exact, work_deadline)));

    Node down = PrepareNode(adb, down_serial, "down_the_line", false, false, work_deadline);
    cleanup.Register(down);
    Node face = PrepareNode(adb, face_serial, "face_on", true, false, work_deadline);
    cleanup.Register(face);
    if (down.node_id == face.node_id) {
      throw std::runtime_error("configured phones advertise the same node identity");
    }

    rejected_id = SharedRecordingId("first");
    const Json face_before_rejection = RequireStatus(face, work_deadline);
    WriteArtifact(OutputDirectory() / "face_on" / "armed-fault-status.json",
                  face_before_rejection.dump(2) + "\n");
    const std::string rejected_body =
        Json({{"schema_version", 1}, {"shared_recording_id", rejected_id}}).dump();
    const HilHttpResponse down_initial =
        Request(down, "POST", "/api/v1/field-recording/start", rejected_body, work_deadline);
    const HilHttpResponse face_initial =
        Request(face, "POST", "/api/v1/field-recording/start", rejected_body, work_deadline);
    const std::string face_error = face_initial.body.empty()
                                       ? std::string()
                                       : Json::parse(face_initial.body).value("error", "");
    const Json face_after_rejection = RequireStatus(face, work_deadline);
    WriteArtifact(OutputDirectory() / "face_on" / "rejected-status.json",
                  face_after_rejection.dump(2) + "\n");
    const HilHttpResponse rollback =
        Request(down, "POST", "/api/v1/field-recording/stop", "{}", work_deadline);
    const Json down_after_rollback = WaitForState(down, "ready", rejected_id, work_deadline);
    WriteArtifact(OutputDirectory() / "down_the_line" / "rollback-status.json",
                  down_after_rollback.dump(2) + "\n");

    retry_id = SharedRecordingId("retry");
    const std::string retry_body =
        Json({{"schema_version", 1}, {"shared_recording_id", retry_id}}).dump();
    const HilHttpResponse down_retry =
        Request(down, "POST", "/api/v1/field-recording/start", retry_body, work_deadline);
    const HilHttpResponse face_retry =
        Request(face, "POST", "/api/v1/field-recording/start", retry_body, work_deadline);
    const Json down_recording = WaitForState(down, "recording", retry_id, work_deadline);
    const Json face_recording = WaitForState(face, "recording", retry_id, work_deadline);
    WriteArtifact(OutputDirectory() / "down_the_line" / "retry-recording-status.json",
                  down_recording.dump(2) + "\n");
    WriteArtifact(OutputDirectory() / "face_on" / "retry-recording-status.json",
                  face_recording.dump(2) + "\n");
    const HilHttpResponse down_stop =
        Request(down, "POST", "/api/v1/field-recording/stop", "{}", work_deadline);
    const HilHttpResponse face_stop =
        Request(face, "POST", "/api/v1/field-recording/stop", "{}", work_deadline);
    const Json down_terminal = WaitForState(down, "ready", retry_id, work_deadline);
    const Json face_terminal = WaitForState(face, "ready", retry_id, work_deadline);
    WriteArtifact(OutputDirectory() / "down_the_line" / "terminal-status.json",
                  down_terminal.dump(2) + "\n");
    WriteArtifact(OutputDirectory() / "face_on" / "terminal-status.json",
                  face_terminal.dump(2) + "\n");
    RequireListing(down, retry_id, std::nullopt, work_deadline);
    RequireListing(face, retry_id, rejected_id, work_deadline);

    const Json cleanup_evidence = cleanup.Finish();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    FieldRecordingPartialStartEvidence evidence{
        .rejected_recording_id = rejected_id,
        .retry_recording_id = retry_id,
        .down_initial_start_status = down_initial.status,
        .face_initial_start_status = face_initial.status,
        .face_initial_error = face_error,
        .face_fault_armed_before_start =
            face_before_rejection.value("hil_reject_next_start", false),
        .face_fault_consumed_after_rejection =
            !face_after_rejection.value("hil_reject_next_start", true),
        .down_rollback_stop_status = rollback.status,
        .down_rollback_state = down_after_rollback.value("state", ""),
        .down_rollback_recording_id = JsonString(down_after_rollback, "shared_recording_id"),
        .face_after_rejection_state = face_after_rejection.value("state", ""),
        .face_after_rejection_recording_id =
            JsonString(face_after_rejection, "shared_recording_id"),
        .down_retry_start_status = down_retry.status,
        .face_retry_start_status = face_retry.status,
        .down_retry_recording_state = down_recording.value("state", ""),
        .down_retry_recording_id = JsonString(down_recording, "shared_recording_id"),
        .face_retry_recording_state = face_recording.value("state", ""),
        .face_retry_recording_id = JsonString(face_recording, "shared_recording_id"),
        .down_retry_stop_status = down_stop.status,
        .face_retry_stop_status = face_stop.status,
        .down_terminal_state = down_terminal.value("state", ""),
        .down_terminal_recording_id = JsonString(down_terminal, "shared_recording_id"),
        .face_terminal_state = face_terminal.value("state", ""),
        .face_terminal_recording_id = JsonString(face_terminal, "shared_recording_id"),
        .cleanup_passed = cleanup.passed(),
        .elapsed_ms = elapsed.count(),
    };
    ValidateFieldRecordingPartialStart(evidence);
    const Json report = {
        {"schema_version", 1},
        {"report_type", "android_dual_field_recording_partial_start_hil"},
        {"passed", true},
        {"error", ""},
        {"elapsed_ms", elapsed.count()},
        {"exact_apk_install_verified", true},
        {"apk_installs", std::move(apk_installs)},
        {"rejection",
         {{"shared_recording_id", rejected_id},
          {"down_the_line_start_http_status", down_initial.status},
          {"face_on_start_http_status", face_initial.status},
          {"face_on_error", face_error},
          {"face_on_before_rejection", StatusEvidence(face_before_rejection)},
          {"face_on_after_rejection", StatusEvidence(face_after_rejection)}}},
        {"rollback",
         {{"down_the_line_stop_http_status", rollback.status},
          {"down_the_line_terminal", StatusEvidence(down_after_rollback)},
          {"published_bundle_visible", true}}},
        {"retry",
         {{"shared_recording_id", retry_id},
          {"fresh_id", retry_id != rejected_id},
          {"down_the_line_start_http_status", down_retry.status},
          {"face_on_start_http_status", face_retry.status},
          {"down_the_line_recording", StatusEvidence(down_recording)},
          {"face_on_recording", StatusEvidence(face_recording)},
          {"down_the_line_stop_http_status", down_stop.status},
          {"face_on_stop_http_status", face_stop.status},
          {"down_the_line_terminal", StatusEvidence(down_terminal)},
          {"face_on_terminal", StatusEvidence(face_terminal)},
          {"both_published_bundles_visible", true}}},
        {"cleanup", cleanup_evidence},
    };
    WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
    std::cout << report.dump(2) << '\n';
    return 0;
  } catch (const std::exception &failure) {
    const Json cleanup_evidence = cleanup.Finish();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const Json report = {
        {"schema_version", 1},
        {"report_type", "android_dual_field_recording_partial_start_hil"},
        {"passed", false},
        {"error", failure.what()},
        {"elapsed_ms", elapsed.count()},
        {"exact_apk_install_verified", apk_installs.size() == 2U},
        {"apk_installs", std::move(apk_installs)},
        {"rejected_recording_id", rejected_id.empty() ? Json(nullptr) : Json(rejected_id)},
        {"retry_recording_id", retry_id.empty() ? Json(nullptr) : Json(retry_id)},
        {"cleanup", cleanup_evidence},
    };
    WriteArtifact(OutputDirectory() / "report.json", report.dump(2) + "\n");
    std::cerr << report.dump(2) << '\n';
    return 1;
  }
}

int Run(int argument_count, char **arguments) {
  if (argument_count == 5 && std::string_view(arguments[4]) == "async-start-failure") {
    return RunAsyncStartFailure(argument_count, arguments);
  }
  return RunPartialStart(argument_count, arguments);
}

}  // namespace
}  // namespace swing_capture::android::dual_hil

int main(int argument_count, char **arguments) {
  std::signal(SIGPIPE, SIG_IGN);
  try {
    return swing_capture::android::dual_hil::Run(argument_count, arguments);
  } catch (const std::exception &failure) {
    std::cerr << "field-recording partial-start HIL failed: " << failure.what() << '\n';
    return 1;
  }
}
