#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "android/dual_hil/android_apk_install.h"
#include "android/dual_hil/hil_command.h"
#include "android/dual_hil/hil_http_client.h"
#include "android/dual_hil/os_reboot_ceremony_validation.h"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
using swing_capture::android::dual_hil::HilCommandResult;
using swing_capture::android::dual_hil::HilHttpRequest;
using swing_capture::android::dual_hil::HilHttpResponse;
using swing_capture::android::dual_hil::OsRebootCeremonyInspection;
using swing_capture::android::dual_hil::OsRebootNodeEvidence;
using swing_capture::android::dual_hil::OsRebootNodeInspection;
using swing_capture::android::dual_hil::RequestHilHttp;
using swing_capture::android::dual_hil::RunHilCommand;
using swing_capture::android::dual_hil::RunRequiredHilCommand;
using swing_capture::android::dual_hil::ValidateOsRebootCeremony;
using swing_capture::android::dual_hil::VerifyInstalledAndroidApk;

constexpr std::string_view kPackage = "com.agoessling.swingcapture";
constexpr std::string_view kActivity = "com.agoessling.swingcapture/.MainActivity";
constexpr std::string_view kService = "com.agoessling.swingcapture/.CaptureForegroundService";
constexpr std::string_view kApproval =
    "I_UNDERSTAND_THIS_REBOOTS_BOTH_PHONES_AND_REQUIRES_LOCAL_UNLOCK";
constexpr auto kAutomaticStageDeadline = 15s;
constexpr auto kOperatorStageDeadline = 120s;
constexpr auto kPollInterval = 250ms;
constexpr std::uint16_t kNodePort = 8088U;

struct LanOrigin {
  std::string text;
  std::string host;
};

struct Node {
  std::string role;
  std::string serial;
  LanOrigin origin;
  std::string control_token;
  std::string apk_sha256;
  OsRebootNodeInspection inspection;
  bool arm_may_be_active = false;
  bool cleanup_attempted = false;
  std::string cleanup_diagnostic;
};

std::string Environment(std::string_view name) {
  // Bazel fixes the environment before starting this single-threaded HIL executable.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *value = std::getenv(std::string(name).c_str());
  return value == nullptr ? std::string() : std::string(value);
}

std::string RequiredEnvironment(std::string_view name) {
  std::string value = Environment(name);
  if (value.empty()) {
    throw std::runtime_error("set --test_env=" + std::string(name) + "=<value>");
  }
  return value;
}

std::filesystem::path OutputDirectory() {
  const std::string value = Environment("TEST_UNDECLARED_OUTPUTS_DIR");
  return value.empty() ? std::filesystem::current_path() : std::filesystem::path(value);
}

void WriteArtifact(std::string_view name, std::string_view contents) {
  const std::filesystem::path path = OutputDirectory() / name;
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open reboot ceremony artifact " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    throw std::runtime_error("cannot write reboot ceremony artifact " + path.string());
  }
}

std::string Trim(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.pop_back();
  }
  const auto first = std::ranges::find_if(text, [](unsigned char character) {
    return character != ' ' && character != '\t' && character != '\n' && character != '\r';
  });
  text.erase(text.begin(), first);
  return text;
}

std::vector<std::string> DeviceArguments(std::string_view serial,
                                         std::initializer_list<std::string_view> suffix) {
  std::vector<std::string> arguments = {"-s", std::string(serial)};
  for (const std::string_view argument : suffix) {
    arguments.emplace_back(argument);
  }
  return arguments;
}

HilCommandResult Adb(const std::filesystem::path &adb, std::string_view serial,
                     std::initializer_list<std::string_view> suffix,
                     std::chrono::steady_clock::time_point deadline) {
  return RunHilCommand(adb, DeviceArguments(serial, suffix), deadline);
}

std::string RequiredAdb(const std::filesystem::path &adb, std::string_view serial,
                        std::initializer_list<std::string_view> suffix,
                        std::chrono::steady_clock::time_point deadline) {
  const HilCommandResult result = Adb(adb, serial, suffix, deadline);
  if (result.timed_out || result.exit_code != 0) {
    throw std::runtime_error("adb failed for " + std::string(serial) +
                             (result.timed_out ? ": deadline expired" : ": " + result.output));
  }
  return result.output;
}

LanOrigin ParseOrigin(std::string text, std::string_view name) {
  constexpr std::string_view prefix = "http://";
  if (!text.starts_with(prefix) || !text.ends_with(":" + std::to_string(kNodePort))) {
    throw std::runtime_error(std::string(name) + " must be http://<canonical-ipv4>:8088");
  }
  const std::size_t host_start = prefix.size();
  const std::size_t host_end = text.rfind(':');
  const std::string host = text.substr(host_start, host_end - host_start);
  in_addr parsed = {};
  char canonical[INET_ADDRSTRLEN] = {};
  if (inet_pton(AF_INET, host.c_str(), &parsed) != 1 ||
      inet_ntop(AF_INET, &parsed, canonical, sizeof(canonical)) == nullptr || host != canonical ||
      (ntohl(parsed.s_addr) >> 24U) == 127U || parsed.s_addr == htonl(INADDR_ANY)) {
    throw std::runtime_error(std::string(name) + " must contain a canonical non-loopback IPv4");
  }
  return {.text = std::move(text), .host = host};
}

std::string ReadControlToken(const std::filesystem::path &adb, std::string_view serial,
                             std::chrono::steady_clock::time_point deadline) {
  const std::string preferences = RequiredAdb(
      adb, serial, {"exec-out", "run-as", kPackage, "cat", "shared_prefs/node_configuration.xml"},
      deadline);
  constexpr std::string_view prefix = "<string name=\"control_token\">";
  constexpr std::string_view suffix = "</string>";
  const std::size_t start = preferences.find(prefix);
  const std::size_t end = start == std::string::npos
                              ? std::string::npos
                              : preferences.find(suffix, start + prefix.size());
  if (start == std::string::npos || end == std::string::npos) {
    throw std::runtime_error("node control credential is unavailable");
  }
  const std::string token = preferences.substr(start + prefix.size(), end - start - prefix.size());
  if (token.size() != 32U || !std::ranges::all_of(token, [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-';
      })) {
    throw std::runtime_error("node control credential has an invalid format");
  }
  return token;
}

HilHttpResponse Request(const Node &node, std::string_view method, std::string_view path,
                        std::string_view body, bool authenticated,
                        std::chrono::steady_clock::time_point deadline) {
  return RequestHilHttp(HilHttpRequest{.ipv4_host = node.origin.host,
                                       .port = kNodePort,
                                       .method = method,
                                       .path = path,
                                       .bearer_token = authenticated ? node.control_token : "",
                                       .body = body,
                                       .headers = {},
                                       .deadline = deadline});
}

std::string RequireRequest(const Node &node, std::string_view method, std::string_view path,
                           std::string_view body, bool authenticated, int expected_status,
                           std::chrono::steady_clock::time_point deadline) {
  const HilHttpResponse response = Request(node, method, path, body, authenticated, deadline);
  if (response.status != expected_status) {
    throw std::runtime_error(node.role + " " + std::string(method) + " " + std::string(path) +
                             " returned HTTP " + std::to_string(response.status));
  }
  return response.body;
}

std::string BootId(const std::filesystem::path &adb, const Node &node,
                   std::chrono::steady_clock::time_point deadline) {
  const std::string value = Trim(
      RequiredAdb(adb, node.serial, {"shell", "cat", "/proc/sys/kernel/random/boot_id"}, deadline));
  if (value.size() != 36U || std::ranges::count(value, '-') != 4) {
    throw std::runtime_error(node.role + " returned an invalid Linux boot ID");
  }
  return value;
}

void PreserveNodeArtifact(const Node &node, std::string_view name, std::string_view contents) {
  WriteArtifact(node.role + "/" + std::string(name), contents);
}

void RequireSafeBaseline(const Node &node) {
  const Json setup = Json::parse(node.inspection.before_setup_json);
  const Json status = Json::parse(node.inspection.before_status_json);
  const std::string expected_mode = node.role == "face_on" ? "leader" : "shadow";
  if (status.value("schema_version", 0) != 2 || status.value("state", "") != "ready" ||
      status.value("armed", true) || setup.at("configuration").value("role", "") != node.role ||
      setup.at("configuration").value("capture_profile", "") != "720p240" ||
      setup.at("configuration").at("pose").value("mode", "") != expected_mode ||
      !setup.at("reboot_recovery").value("ready_this_boot", false)) {
    throw std::runtime_error(node.role + " is not a safe, configured READY reboot baseline");
  }
  if (node.role == "face_on" &&
      (!setup.at("pairing").is_object() ||
       setup.at("pairing").value("credential_status", "") != "verified")) {
    throw std::runtime_error("face_on peer binding is not verified before reboot");
  }
}

Json NodeProgress(const Node &node) {
  return {
      {"role", node.role},
      {"serial", node.serial},
      {"lan_origin", node.origin.text},
      {"exact_apk_sha256", node.apk_sha256.empty() ? Json(nullptr) : Json(node.apk_sha256)},
      {"exact_apk_verified", node.inspection.exact_apk_verified},
      {"reboot_command_accepted", node.inspection.reboot_command_accepted},
      {"boot_id_changed", !node.inspection.before_boot_id.empty() &&
                              !node.inspection.after_boot_id.empty() &&
                              node.inspection.before_boot_id != node.inspection.after_boot_id},
      {"user_unlocked_before_launch", node.inspection.user_unlocked_before_launch},
      {"capture_service_absent_before_launch",
       node.inspection.capture_service_absent_before_launch},
      {"node_api_unavailable_before_launch", node.inspection.node_api_unavailable_before_launch},
      {"foreground_activity_observed", node.inspection.foreground_activity_observed},
      {"adb_foreground_launch_sent", node.inspection.adb_foreground_launch_sent},
      {"disarm_acknowledged", node.inspection.disarm_acknowledged},
      {"cleanup_attempted", node.cleanup_attempted},
      {"cleanup_diagnostic",
       node.cleanup_diagnostic.empty() ? Json(nullptr) : Json(node.cleanup_diagnostic)},
  };
}

void PublishReport(const Node &face, const Node &down, std::string_view phase, bool passed,
                   std::string_view diagnostic, std::optional<Json> evidence = std::nullopt) {
  const bool face_cleanup_required = face.arm_may_be_active || face.cleanup_attempted;
  const bool down_cleanup_required = down.arm_may_be_active || down.cleanup_attempted;
  const bool cleanup_required = face_cleanup_required || down_cleanup_required;
  const bool face_restored =
      !face_cleanup_required || (!face.arm_may_be_active && face.cleanup_diagnostic.empty() &&
                                 face.inspection.disarm_acknowledged);
  const bool down_restored =
      !down_cleanup_required || (!down.arm_may_be_active && down.cleanup_diagnostic.empty() &&
                                 down.inspection.disarm_acknowledged);
  const bool cleanup_passed = face_restored && down_restored;
  Json report = {
      {"schema_version", 1},
      {"report_type", "android_dual_phone_os_reboot_ceremony_hil"},
      {"passed", passed},
      {"phase", phase},
      {"diagnostic", diagnostic.empty() ? Json(nullptr) : Json(diagnostic)},
      {"safety",
       {{"explicit_reboot_approval_required", true},
        {"usb_adb_required", true},
        {"adb_foreground_launch_allowed", false},
        {"automatic_stage_limit_ms",
         std::chrono::duration_cast<std::chrono::milliseconds>(kAutomaticStageDeadline).count()},
        {"operator_stage_limit_ms",
         std::chrono::duration_cast<std::chrono::milliseconds>(kOperatorStageDeadline).count()},
        {"configuration_mutated", false},
        {"application_data_deleted", false}}},
      {"nodes", Json::array({NodeProgress(face), NodeProgress(down)})},
      {"cleanup",
       {{"required", cleanup_required},
        {"attempted", face.cleanup_attempted || down.cleanup_attempted},
        {"passed", cleanup_passed},
        {"configuration_endpoint_mutated", false},
        {"private_application_data_removed", false},
        {"terminal_ready_unarmed", cleanup_required && cleanup_passed}}},
      {"artifacts",
       {{"face_on_before_setup", "face_on/before-setup.json"},
        {"face_on_after_setup", "face_on/after-setup.json"},
        {"face_on_armed_status", "face_on/armed-status.json"},
        {"face_on_cleanup_status", "face_on/cleanup-status.json"},
        {"face_on_cleanup_setup", "face_on/cleanup-setup.json"},
        {"down_the_line_before_setup", "down_the_line/before-setup.json"},
        {"down_the_line_after_setup", "down_the_line/after-setup.json"},
        {"down_the_line_armed_status", "down_the_line/armed-status.json"},
        {"down_the_line_cleanup_status", "down_the_line/cleanup-status.json"},
        {"down_the_line_cleanup_setup", "down_the_line/cleanup-setup.json"}}},
  };
  if (evidence.has_value()) {
    report["validation"] = std::move(*evidence);
  }
  WriteArtifact("report.json", report.dump(2) + "\n");
}

void PrepareNode(const std::filesystem::path &adb, const std::filesystem::path &apk,
                 const std::filesystem::path &installer, Node *node) {
  const auto deadline = std::chrono::steady_clock::now() + kAutomaticStageDeadline;
  const auto apk_evidence = VerifyInstalledAndroidApk(installer, adb, apk, node->serial, deadline);
  node->apk_sha256 = apk_evidence.sha256;
  node->inspection.exact_apk_verified = true;
  node->control_token = ReadControlToken(adb, node->serial, deadline);
  node->inspection.before_boot_id = BootId(adb, *node, deadline);
  node->inspection.before_setup_json =
      RequireRequest(*node, "GET", "/api/v1/setup", {}, true, 200, deadline);
  node->inspection.before_status_json =
      RequireRequest(*node, "GET", "/api/v1/capture/status", {}, true, 200, deadline);
  RequireSafeBaseline(*node);
  PreserveNodeArtifact(*node, "before-setup.json", node->inspection.before_setup_json);
  PreserveNodeArtifact(*node, "before-status.json", node->inspection.before_status_json);
}

bool UserUnlocked(const std::filesystem::path &adb, const Node &node,
                  std::chrono::steady_clock::time_point deadline) {
  const HilCommandResult result =
      Adb(adb, node.serial, {"shell", "cmd", "user", "is-user-unlocked"}, deadline);
  return !result.timed_out && result.exit_code == 0 && Trim(result.output) == "true";
}

void WaitForBootAndUnlock(const std::filesystem::path &adb, Node *node,
                          std::chrono::steady_clock::time_point deadline) {
  std::string last = "device has not reconnected";
  while (std::chrono::steady_clock::now() < deadline) {
    const auto command_deadline = std::min(deadline, std::chrono::steady_clock::now() + 2s);
    try {
      const HilCommandResult completed =
          Adb(adb, node->serial, {"shell", "getprop", "sys.boot_completed"}, command_deadline);
      if (!completed.timed_out && completed.exit_code == 0 && Trim(completed.output) == "1") {
        const std::string boot_id = BootId(adb, *node, command_deadline);
        if (boot_id == node->inspection.before_boot_id) {
          throw std::runtime_error("device reconnected without changing Linux boot ID");
        }
        node->inspection.after_boot_id = boot_id;
        if (UserUnlocked(adb, *node, command_deadline)) {
          node->inspection.user_unlocked_before_launch = true;
          return;
        }
        last = "OS booted; waiting for first unlock";
      } else {
        last = "waiting for Android boot completion";
      }
    } catch (const std::exception &failure) {
      last = failure.what();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error(node->role + " unlock watchdog expired: " + last);
}

bool CaptureServiceAbsent(const std::filesystem::path &adb, const Node &node,
                          std::chrono::steady_clock::time_point deadline) {
  const std::string services = RequiredAdb(
      adb, node.serial, {"shell", "dumpsys", "activity", "services", kService}, deadline);
  return !services.contains("ServiceRecord{") && !services.contains(kService);
}

bool NodeApiUnavailable(const Node &node, std::chrono::steady_clock::time_point deadline) {
  try {
    static_cast<void>(Request(node, "GET", "/api/v1/capture/status", {}, true, deadline));
    return false;
  } catch (const std::exception &) {
    return true;
  }
}

void VerifyPrelaunchBoundary(const std::filesystem::path &adb, Node *node) {
  const auto deadline = std::chrono::steady_clock::now() + kAutomaticStageDeadline;
  node->inspection.capture_service_absent_before_launch =
      CaptureServiceAbsent(adb, *node, deadline);
  node->inspection.node_api_unavailable_before_launch =
      NodeApiUnavailable(*node, std::min(deadline, std::chrono::steady_clock::now() + 1s));
  if (!node->inspection.capture_service_absent_before_launch ||
      !node->inspection.node_api_unavailable_before_launch) {
    throw std::runtime_error(node->role +
                             " capture service started before the required operator launch");
  }
}

bool ForegroundActivityObserved(const std::filesystem::path &adb, const Node &node,
                                std::chrono::steady_clock::time_point deadline) {
  const HilCommandResult result =
      Adb(adb, node.serial, {"shell", "dumpsys", "activity", "activities"}, deadline);
  if (result.timed_out || result.exit_code != 0) {
    return false;
  }
  std::size_t line_start = 0;
  while (line_start < result.output.size()) {
    const std::size_t line_end = result.output.find('\n', line_start);
    const std::string_view line(
        result.output.data() + line_start,
        (line_end == std::string::npos ? result.output.size() : line_end) - line_start);
    if ((line.contains("mResumedActivity") || line.contains("topResumedActivity")) &&
        line.contains(kActivity)) {
      return true;
    }
    if (line_end == std::string::npos) {
      break;
    }
    line_start = line_end + 1U;
  }
  return false;
}

bool TryObserveLaunch(const std::filesystem::path &adb, Node *node,
                      std::chrono::steady_clock::time_point deadline) {
  try {
    node->control_token = ReadControlToken(adb, node->serial, deadline);
    const std::string setup =
        RequireRequest(*node, "GET", "/api/v1/setup", {}, true, 200, deadline);
    const std::string status =
        RequireRequest(*node, "GET", "/api/v1/capture/status", {}, true, 200, deadline);
    if (!ForegroundActivityObserved(adb, *node, deadline)) {
      return false;
    }
    node->inspection.foreground_activity_observed = true;
    node->inspection.after_setup_json = setup;
    node->inspection.after_launch_status_json = status;
    PreserveNodeArtifact(*node, "after-setup.json", setup);
    PreserveNodeArtifact(*node, "after-launch-status.json", status);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

void WaitForOperatorLaunches(const std::filesystem::path &adb, Node *face, Node *down,
                             std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const auto command_deadline = std::min(deadline, std::chrono::steady_clock::now() + 2s);
    if (!face->inspection.foreground_activity_observed) {
      static_cast<void>(TryObserveLaunch(adb, face, command_deadline));
    }
    if (!down->inspection.foreground_activity_observed) {
      static_cast<void>(TryObserveLaunch(adb, down, command_deadline));
    }
    if (face->inspection.foreground_activity_observed &&
        down->inspection.foreground_activity_observed) {
      return;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("operator foreground-launch watchdog expired");
}

bool ArmedMonitoring(const Node &node, std::string_view status) {
  try {
    const Json parsed = Json::parse(status);
    const Json &pose = parsed.at("pose");
    if (parsed.value("state", "") != "armed" || !parsed.value("armed", false) ||
        pose.value("phase", "") != "monitoring" ||
        !pose.at("standby_audio").value("ready", false) ||
        pose.at("metrics").value("successful_inferences", 0LL) < 1) {
      return false;
    }
    if (node.role == "face_on") {
      return pose.at("autonomous_pair").value("peer_available", false) &&
             pose.at("peer_clock").is_object() &&
             pose.at("peer_clock").value("sample_count", 0) >= 1;
    }
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

void ArmAndWait(Node *face, Node *down) {
  const auto deadline = std::chrono::steady_clock::now() + kAutomaticStageDeadline;
  constexpr std::string_view request = R"({"armed":true})";
  down->arm_may_be_active = true;
  static_cast<void>(
      RequireRequest(*down, "POST", "/api/v1/capture/arm", request, true, 202, deadline));
  face->arm_may_be_active = true;
  static_cast<void>(
      RequireRequest(*face, "POST", "/api/v1/capture/arm", request, true, 202, deadline));
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      const std::string face_status =
          RequireRequest(*face, "GET", "/api/v1/capture/status", {}, true, 200, deadline);
      const std::string down_status =
          RequireRequest(*down, "GET", "/api/v1/capture/status", {}, true, 200, deadline);
      PreserveNodeArtifact(*face, "armed-status-latest.json", face_status);
      PreserveNodeArtifact(*down, "armed-status-latest.json", down_status);
      if (ArmedMonitoring(*face, face_status) && ArmedMonitoring(*down, down_status)) {
        face->inspection.armed_status_json = face_status;
        down->inspection.armed_status_json = down_status;
        PreserveNodeArtifact(*face, "armed-status.json", face_status);
        PreserveNodeArtifact(*down, "armed-status.json", down_status);
        return;
      }
    } catch (const std::exception &) {
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  throw std::runtime_error("rearmed pair did not reach live monitoring and peer recovery");
}

void Disarm(Node *node) {
  if (!node->arm_may_be_active) {
    return;
  }
  node->cleanup_attempted = true;
  const auto deadline = std::chrono::steady_clock::now() + kAutomaticStageDeadline;
  try {
    constexpr std::string_view request = R"({"armed":false})";
    static_cast<void>(
        RequireRequest(*node, "POST", "/api/v1/capture/arm", request, true, 202, deadline));
    node->inspection.disarm_acknowledged = true;
    while (std::chrono::steady_clock::now() < deadline) {
      const std::string status =
          RequireRequest(*node, "GET", "/api/v1/capture/status", {}, true, 200, deadline);
      PreserveNodeArtifact(*node, "cleanup-status-latest.json", status);
      const Json parsed = Json::parse(status);
      if (parsed.value("state", "") == "ready" && !parsed.value("armed", true)) {
        node->inspection.cleanup_status_json = status;
        PreserveNodeArtifact(*node, "cleanup-status.json", status);
        node->inspection.cleanup_setup_json =
            RequireRequest(*node, "GET", "/api/v1/setup", {}, true, 200, deadline);
        PreserveNodeArtifact(*node, "cleanup-setup.json", node->inspection.cleanup_setup_json);
        node->arm_may_be_active = false;
        return;
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    throw std::runtime_error("terminal READY/unarmed state was not observed");
  } catch (const std::exception &failure) {
    node->cleanup_diagnostic = failure.what();
  }
}

Json NodeEvidenceJson(const OsRebootNodeEvidence &evidence) {
  return {{"role", evidence.role},
          {"node_id", evidence.node_id},
          {"device_model", evidence.device_model},
          {"boot_observation_count_before", evidence.boot_observation_count_before},
          {"boot_observation_count_after", evidence.boot_observation_count_after}};
}

int Run(int argument_count, char **arguments) {
  Node face{};
  face.role = "face_on";
  face.inspection.role = face.role;
  Node down{};
  down.role = "down_the_line";
  down.inspection.role = down.role;
  try {
    if (argument_count != 4) {
      throw std::runtime_error("expected Bazel runfiles: <adb> <APK> <APK verifier>");
    }
    const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
    const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
    const std::filesystem::path installer = std::filesystem::absolute(arguments[3]);
    if (!std::filesystem::is_regular_file(adb) || !std::filesystem::is_regular_file(apk) ||
        !std::filesystem::is_regular_file(installer)) {
      throw std::runtime_error("reboot ceremony Bazel runfiles are missing");
    }
    face.serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_SERIAL");
    face.origin = ParseOrigin(RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN"),
                              "SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN");
    down.serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_SERIAL");
    down.origin = ParseOrigin(RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN"),
                              "SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN");
    if (face.serial == down.serial || face.serial.contains(':') || down.serial.contains(':')) {
      throw std::runtime_error("reboot ceremony requires two distinct USB ADB serials");
    }
    if (RequiredEnvironment("SWING_CAPTURE_APPROVE_OS_REBOOT_CEREMONY") != kApproval) {
      throw std::runtime_error(
          "explicit reboot approval is missing; read docs/android.md before setting "
          "SWING_CAPTURE_APPROVE_OS_REBOOT_CEREMONY");
    }
    PrepareNode(adb, apk, installer, &face);
    PrepareNode(adb, apk, installer, &down);
    if (face.apk_sha256 != down.apk_sha256) {
      throw std::runtime_error("phones did not verify the same exact Bazel APK");
    }
    const Json face_setup = Json::parse(face.inspection.before_setup_json);
    const Json down_setup = Json::parse(down.inspection.before_setup_json);
    if (face_setup.at("node").value("node_id", "") == down_setup.at("node").value("node_id", "") ||
        face_setup.at("pairing").value("peer_node_id", "") !=
            down_setup.at("node").value("node_id", "")) {
      throw std::runtime_error("prepared nodes are not the verified complementary pair");
    }
    PublishReport(face, down, "prepared", false, "awaiting destructive reboot stage");

    std::cout << "Rebooting both USB-connected phones. Unlock each phone after Android boots, "
                 "but DO NOT open Swing Capture until this test asks you to.\n"
              << std::flush;
    const auto reboot_deadline = std::chrono::steady_clock::now() + kAutomaticStageDeadline;
    RequiredAdb(adb, down.serial, {"reboot"}, reboot_deadline);
    down.inspection.reboot_command_accepted = true;
    RequiredAdb(adb, face.serial, {"reboot"}, reboot_deadline);
    face.inspection.reboot_command_accepted = true;
    PublishReport(face, down, "reboot_commanded", false, "waiting for first unlock");

    const auto unlock_deadline = std::chrono::steady_clock::now() + kOperatorStageDeadline;
    WaitForBootAndUnlock(adb, &face, unlock_deadline);
    WaitForBootAndUnlock(adb, &down, unlock_deadline);
    VerifyPrelaunchBoundary(adb, &face);
    VerifyPrelaunchBoundary(adb, &down);
    PublishReport(face, down, "unlocked_prelaunch", false,
                  "capture correctly absent; waiting for operator foreground launch");

    std::cout << "Now open Swing Capture once on BOTH phones and leave each app visible.\n"
              << std::flush;
    WaitForOperatorLaunches(adb, &face, &down,
                            std::chrono::steady_clock::now() + kOperatorStageDeadline);
    PublishReport(face, down, "operator_launched", false, "validating recovered peer and rearm");

    ArmAndWait(&face, &down);
    PublishReport(face, down, "rearmed", false, "returning both nodes to READY/unarmed");
    Disarm(&face);
    Disarm(&down);
    if (!face.cleanup_diagnostic.empty() || !down.cleanup_diagnostic.empty()) {
      throw std::runtime_error("reboot ceremony cleanup did not restore both nodes");
    }

    const auto evidence = ValidateOsRebootCeremony(
        OsRebootCeremonyInspection{.face_on = face.inspection, .down_the_line = down.inspection});
    PublishReport(face, down, "complete", true, {},
                  Json{{"passed", true},
                       {"configuration_preserved", true},
                       {"peer_recovered", true},
                       {"rearm_observed", true},
                       {"cleanup_restored_ready_unarmed", true},
                       {"nodes", Json::array({NodeEvidenceJson(evidence.face_on),
                                              NodeEvidenceJson(evidence.down_the_line)})}});
    std::cout << "OS reboot ceremony passed; both nodes are READY/unarmed.\n";
    return 0;
  } catch (const std::exception &failure) {
    Disarm(&face);
    Disarm(&down);
    try {
      PublishReport(face, down, "failed", false, failure.what());
    } catch (const std::exception &) {
    }
    throw;
  } catch (...) {
    Disarm(&face);
    Disarm(&down);
    try {
      PublishReport(face, down, "failed", false, "unknown reboot ceremony failure");
    } catch (const std::exception &) {
    }
    throw;
  }
}

}  // namespace

int main(int argument_count, char **arguments) {
  std::signal(SIGPIPE, SIG_IGN);
  try {
    return Run(argument_count, arguments);
  } catch (const std::exception &failure) {
    try {
      // The detailed progressive report remains in undeclared outputs. The final stderr is kept
      // token-free and is sufficient to route the operator to that report.
      std::cerr << "OS reboot ceremony HIL failed: " << failure.what() << '\n';
    } catch (...) {
    }
    return 1;
  }
}
