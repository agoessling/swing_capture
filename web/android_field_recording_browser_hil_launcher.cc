#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "android/dual_hil/android_apk_install.h"
#include "android/dual_hil/hil_command.h"
#include "web/android_browser_hil_credentials.h"
#include "web/android_field_recording_browser_hil_report_validation.h"
#include "web/android_phone_http_readiness.h"

namespace swing_capture::web {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
using swing_capture::android::dual_hil::AndroidApkInstallEvidence;
using swing_capture::android::dual_hil::HilCommandResult;
using swing_capture::android::dual_hil::InstallAndroidApk;
using swing_capture::android::dual_hil::RunHilCommand;
using swing_capture::android::dual_hil::RunRequiredHilCommand;

constexpr std::string_view kPackageName = "com.agoessling.swingcapture";

std::string RequiredEnvironment(std::string_view name) {
  // Bazel establishes the environment before this single-threaded launcher starts.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *value = std::getenv(std::string(name).c_str());
  if (value == nullptr || *value == '\0') {
    throw std::runtime_error("set --test_env=" + std::string(name) + "=<value>");
  }
  return value;
}

std::filesystem::path OutputDirectory() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *configured = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  return configured == nullptr ? std::filesystem::current_path()
                               : std::filesystem::path(configured);
}

void WriteReport(const Json &report) {
  const std::filesystem::path path = OutputDirectory() / "browser-hil-launcher-report.json";
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot write browser HIL launcher report");
  }
  output << report.dump(2) << '\n';
}

std::string ReadChildReport() {
  const std::filesystem::path path = OutputDirectory() / "report.json";
  constexpr std::uintmax_t kMaximumReportBytes = 8U * 1024U * 1024U;
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error("Playwright did not write report.json");
  }
  const std::uintmax_t size = std::filesystem::file_size(path);
  if (size == 0U || size > kMaximumReportBytes) {
    throw std::runtime_error("Playwright report.json has an invalid size");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read Playwright report.json");
  }
  std::string contents(std::istreambuf_iterator<char>(input), {});
  if (input.bad() || contents.size() != static_cast<std::size_t>(size)) {
    throw std::runtime_error("could not completely read Playwright report.json");
  }
  return contents;
}

Json BrowserEvidenceJson(const AndroidFieldRecordingBrowserHilBrowserEvidence &evidence) {
  return {{"engine", evidence.engine},
          {"distribution", evidence.distribution},
          {"version", evidence.version},
          {"user_agent", evidence.user_agent},
          {"h264_decode_required", evidence.h264_decode_required}};
}

Json FreshMediaDecodeEvidenceJson(
    const std::vector<AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence> &evidence) {
  Json result = Json::array();
  for (const AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence &item : evidence) {
    result.push_back({{"role", item.role},
                      {"origin", item.origin},
                      {"media_identity", item.media_identity},
                      {"duration_seconds", item.duration_seconds},
                      {"video_width", item.video_width},
                      {"video_height", item.video_height},
                      {"rvfc_media_time_seconds", item.rvfc_media_time_seconds},
                      {"rvfc_presented_frames", item.rvfc_presented_frames},
                      {"nonblack_fraction", item.nonblack_fraction},
                      {"playback_start_seconds", item.playback_start_seconds},
                      {"playback_end_seconds", item.playback_end_seconds},
                      {"playback_advanced_seconds", item.playback_advanced_seconds}});
  }
  return result;
}

Json HttpReadinessNodeEvidenceJson(const AndroidPhoneHttpReadinessEvidence &evidence) {
  Json attempts = Json::array();
  for (const AndroidPhoneHttpReadinessAttempt &attempt : evidence.attempts) {
    attempts.push_back({{"sequence", attempt.sequence},
                        {"elapsed_ms", attempt.elapsed_ms},
                        {"http_status", attempt.http_status.has_value() ? Json(*attempt.http_status)
                                                                        : Json(nullptr)},
                        {"outcome", attempt.outcome}});
  }
  return {{"role", evidence.role},
          {"origin", evidence.origin},
          {"timeout_ms", evidence.timeout_ms},
          {"passed", evidence.passed},
          {"attempts", std::move(attempts)}};
}

std::vector<std::string> DeviceArguments(std::string_view serial,
                                         std::initializer_list<std::string_view> suffix) {
  std::vector<std::string> arguments = {"-s", std::string(serial)};
  for (const std::string_view value : suffix) {
    arguments.emplace_back(value);
  }
  return arguments;
}

std::string ReadToken(const std::filesystem::path &adb, std::string_view serial) {
  const std::string preferences =
      RunRequiredHilCommand(adb,
                            DeviceArguments(serial, {"exec-out", "run-as", kPackageName, "cat",
                                                     "shared_prefs/node_configuration.xml"}),
                            std::chrono::steady_clock::now() + 3s);
  return ExtractAndroidControlToken(preferences);
}

void StartApplication(const std::filesystem::path &adb, std::string_view serial) {
  static_cast<void>(
      RunRequiredHilCommand(adb,
                            DeviceArguments(serial, {"shell", "am", "start", "-W", "-n",
                                                     "com.agoessling.swingcapture/.MainActivity"}),
                            std::chrono::steady_clock::now() + 4s));
}

Json SleepBoth(const std::filesystem::path &adb,
               const std::vector<std::pair<std::string, std::string>> &devices) {
  Json attempts = Json::array();
  bool passed = true;
  for (const auto &[role, serial] : devices) {
    try {
      const HilCommandResult sleep = RunHilCommand(
          adb, DeviceArguments(serial, {"shell", "input", "keyevent", "KEYCODE_SLEEP"}),
          std::chrono::steady_clock::now() + 2s);
      const HilCommandResult power =
          RunHilCommand(adb, DeviceArguments(serial, {"shell", "dumpsys", "power"}),
                        std::chrono::steady_clock::now() + 2s);
      const bool sleeping = power.output.find("mWakefulness=Dozing") != std::string::npos ||
                            power.output.find("mWakefulness=Asleep") != std::string::npos;
      const bool restored = !sleep.timed_out && sleep.exit_code == 0 && !power.timed_out &&
                            power.exit_code == 0 && sleeping;
      passed = passed && restored;
      attempts.push_back({{"role", role}, {"screen_sleep_restored", restored}});
    } catch (const std::exception &failure) {
      passed = false;
      attempts.push_back(
          {{"role", role}, {"screen_sleep_restored", false}, {"error", failure.what()}});
    }
  }
  return {{"passed", passed}, {"attempts", std::move(attempts)}};
}

void SetChildEnvironment(const std::string &down_url, const std::string &down_token,
                         const std::string &face_url, const std::string &face_token) {
  if (setenv("SWING_CAPTURE_ANDROID_DTL_NODE_URL", down_url.c_str(), 1) != 0 ||
      setenv("SWING_CAPTURE_ANDROID_DTL_TOKEN", down_token.c_str(), 1) != 0 ||
      setenv("SWING_CAPTURE_ANDROID_FACE_NODE_URL", face_url.c_str(), 1) != 0 ||
      setenv("SWING_CAPTURE_ANDROID_FACE_TOKEN", face_token.c_str(), 1) != 0) {
    throw std::runtime_error("cannot inject Android browser HIL credentials");
  }
}

Json ApkEvidence(std::string_view role, const AndroidApkInstallEvidence &evidence) {
  return {{"role", role},
          {"installed", evidence.installed},
          {"reason", evidence.reason},
          {"sha256", evidence.sha256},
          {"installed_before_sha256", evidence.installed_before_sha256.has_value()
                                          ? Json(*evidence.installed_before_sha256)
                                          : Json(nullptr)},
          {"elapsed_ms", evidence.elapsed_ms},
          {"installed_after_matches_bazel_apk", true}};
}

struct PreparedPhone {
  std::string role;
  std::optional<AndroidApkInstallEvidence> apk;
  std::optional<AndroidPhoneHttpReadinessEvidence> readiness;
  std::string token;
  std::string error;
};

PreparedPhone PreparePhone(const std::filesystem::path &installer, const std::filesystem::path &adb,
                           const std::filesystem::path &apk, std::string role, std::string serial,
                           std::string origin) {
  PreparedPhone result = {.role = std::move(role),
                          .apk = std::nullopt,
                          .readiness = std::nullopt,
                          .token = {},
                          .error = {}};
  try {
    result.apk =
        InstallAndroidApk(installer, adb, apk, serial, true, std::chrono::steady_clock::now() + 5s);
    StartApplication(adb, serial);
    result.token = ReadToken(adb, serial);
    result.readiness = ProbeAndroidPhoneHttpReadiness(result.role, origin, result.token);
  } catch (const std::exception &failure) {
    result.error = failure.what();
  }
  return result;
}

int Run(int argument_count, char **arguments) {
  if (argument_count < 6) {
    throw std::runtime_error(
        "expected <adb> <APK> <APK-installer> <Playwright-test> <Playwright arguments...>");
  }
  const std::filesystem::path adb = std::filesystem::absolute(arguments[1]);
  const std::filesystem::path apk = std::filesystem::absolute(arguments[2]);
  const std::filesystem::path installer = std::filesystem::absolute(arguments[3]);
  const std::filesystem::path playwright = std::filesystem::absolute(arguments[4]);
  if (!std::filesystem::is_regular_file(adb) || access(adb.c_str(), X_OK) != 0 ||
      !std::filesystem::is_regular_file(apk) || !std::filesystem::is_regular_file(installer) ||
      access(installer.c_str(), X_OK) != 0 || !std::filesystem::is_regular_file(playwright) ||
      access(playwright.c_str(), X_OK) != 0) {
    throw std::runtime_error("browser HIL adb, APK, installer, or Playwright runfile is invalid");
  }
  const std::string down_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_SERIAL");
  const std::string face_serial = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_ON_SERIAL");
  const std::string down_url = RequiredEnvironment("SWING_CAPTURE_ANDROID_DTL_NODE_URL");
  const std::string face_url = RequiredEnvironment("SWING_CAPTURE_ANDROID_FACE_NODE_URL");
  if (down_serial == face_serial || down_url == face_url) {
    throw std::runtime_error("browser HIL requires two distinct phones and LAN origins");
  }
  ValidateDirectLanNodeOrigin(down_url);
  ValidateDirectLanNodeOrigin(face_url);
  const std::vector<std::pair<std::string, std::string>> devices = {{"down_the_line", down_serial},
                                                                    {"face_on", face_serial}};

  Json primary = {{"passed", false}, {"error", "launcher did not start Playwright"}};
  Json apk_evidence = Json::array();
  Json http_readiness = {{"passed", false},
                         {"probe_method", "GET"},
                         {"probe_path", "/api/v1/node"},
                         {"nodes", Json::array()}};
  Json browser_evidence = nullptr;
  Json fresh_media_decode = Json::array();
  Json phone_preparation_elapsed_ms = nullptr;
  int exit_code = 1;
  try {
    // Installation, launch, credential read, and network readiness are independent per phone.
    // Running complete per-phone preparation concurrently removes one phone's setup latency from
    // the critical path and makes a missing peer fail after one readiness window rather than two.
    const std::chrono::steady_clock::time_point preparation_started =
        std::chrono::steady_clock::now();
    std::future<PreparedPhone> down_future =
        std::async(std::launch::async, PreparePhone, std::cref(installer), std::cref(adb),
                   std::cref(apk), "down_the_line", down_serial, down_url);
    std::future<PreparedPhone> face_future =
        std::async(std::launch::async, PreparePhone, std::cref(installer), std::cref(adb),
                   std::cref(apk), "face_on", face_serial, face_url);
    PreparedPhone down = down_future.get();
    PreparedPhone face = face_future.get();
    phone_preparation_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - preparation_started)
                                       .count();
    for (const PreparedPhone *phone : {&down, &face}) {
      if (phone->apk.has_value()) {
        apk_evidence.push_back(ApkEvidence(phone->role, *phone->apk));
      }
      if (phone->readiness.has_value()) {
        http_readiness["nodes"].push_back(HttpReadinessNodeEvidenceJson(*phone->readiness));
      }
    }
    if (!down.error.empty() || !face.error.empty()) {
      throw std::runtime_error(!down.error.empty()
                                   ? "down_the_line phone preparation failed: " + down.error
                                   : "face_on phone preparation failed: " + face.error);
    }
    http_readiness["passed"] = down.readiness->passed && face.readiness->passed;
    if (!http_readiness.at("passed").get<bool>()) {
      throw std::runtime_error(
          "both Android phone HTTP services did not become ready before Playwright");
    }
    SetChildEnvironment(down_url, down.token, face_url, face.token);
    std::vector<std::string> child_arguments(arguments + 5, arguments + argument_count);
    const HilCommandResult child =
        RunHilCommand(playwright, child_arguments, std::chrono::steady_clock::now() + 23s);
    std::cout << child.output;
    exit_code = child.timed_out ? 1 : child.exit_code;
    if (!child.timed_out && child.exit_code == 0) {
      const AndroidFieldRecordingBrowserHilChildReportEvidence child_evidence =
          ValidateAndroidFieldRecordingBrowserHilChildReport(ReadChildReport(), down_url, face_url);
      browser_evidence = BrowserEvidenceJson(child_evidence.browser);
      fresh_media_decode = FreshMediaDecodeEvidenceJson(child_evidence.fresh_media_decode);
    }
    primary = {{"passed", !child.timed_out && child.exit_code == 0},
               {"timed_out", child.timed_out},
               {"exit_code", child.exit_code},
               {"error", child.timed_out ? "Playwright exceeded 23 seconds" : ""}};
  } catch (const std::exception &failure) {
    primary = {{"passed", false}, {"error", failure.what()}};
  }
  const Json cleanup = SleepBoth(adb, devices);
  bool passed = primary.value("passed", false) && cleanup.value("passed", false);
  Json report = {
      {"schema_version", 1},
      {"report_type", "android_field_recording_browser_hil_launcher"},
      {"passed", passed},
      {"credential_transport", "adb_run_as_to_child_environment"},
      {"credentials_retained", false},
      {"origins", Json::array({down_url, face_url})},
      {"exact_bazel_apk_verified", apk_evidence.size() == 2U},
      {"apk", std::move(apk_evidence)},
      {"phone_preparation_elapsed_ms", std::move(phone_preparation_elapsed_ms)},
      {"http_readiness", std::move(http_readiness)},
      {"primary", primary},
      {"cleanup", cleanup},
      {"browser", std::move(browser_evidence)},
      {"fresh_media_decode", std::move(fresh_media_decode)},
      {"evidence_contract_validated", false},
  };
  if (passed) {
    report["evidence_contract_validated"] = true;
    try {
      static_cast<void>(ValidateAndroidFieldRecordingBrowserHilLauncherReport(report.dump()));
    } catch (const std::exception &failure) {
      passed = false;
      report["passed"] = false;
      report["evidence_contract_validated"] = false;
      report["evidence_validation_error"] = failure.what();
    }
  }
  WriteReport(report);
  return passed ? 0 : (exit_code == 0 ? 1 : exit_code);
}

}  // namespace
}  // namespace swing_capture::web

int main(int argument_count, char **arguments) {
  try {
    return swing_capture::web::Run(argument_count, arguments);
  } catch (const std::exception &failure) {
    try {
      swing_capture::web::WriteReport({
          {"schema_version", 1},
          {"report_type", "android_field_recording_browser_hil_launcher"},
          {"passed", false},
          {"credential_transport", "adb_run_as_to_child_environment"},
          {"credentials_retained", false},
          {"error", failure.what()},
          {"origins", nlohmann::json::array()},
          {"exact_bazel_apk_verified", false},
          {"apk", nlohmann::json::array()},
          {"http_readiness", nullptr},
          {"cleanup", nullptr},
          {"browser", nullptr},
          {"fresh_media_decode", nlohmann::json::array()},
          {"evidence_contract_validated", false},
      });
    } catch (...) {
    }
    std::cerr << "Android field-recording browser HIL launcher failed: " << failure.what() << '\n';
    return 1;
  }
}
