#include "android/dual_hil/android_apk_install.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "android/dual_hil/hil_command.h"

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

bool IsSha256(std::string_view value) {
  return value.size() == 64U && std::ranges::all_of(value, [](const unsigned char character) {
           return std::isdigit(character) != 0 || (character >= static_cast<unsigned char>('a') &&
                                                   character <= static_cast<unsigned char>('f'));
         });
}

std::optional<std::string> OptionalSha256(const Json &report, std::string_view name) {
  const auto field = report.find(name);
  if (field == report.end() || field->is_null()) {
    return std::nullopt;
  }
  if (!field->is_string() || !IsSha256(field->get_ref<const std::string &>())) {
    throw std::runtime_error("Android APK installer returned an invalid prior identity");
  }
  return field->get<std::string>();
}

}  // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
AndroidApkInstallEvidence InstallAndroidApk(const std::filesystem::path &installer,
                                            const std::filesystem::path &adb,
                                            const std::filesystem::path &apk,
                                            std::string_view serial, bool skip_exact_match,
                                            std::chrono::steady_clock::time_point deadline) {
  std::vector<std::string> arguments = {
      adb.string(), "--serial", std::string(serial), "--apk", apk.string(),
  };
  if (skip_exact_match) {
    arguments.emplace_back("--skip-exact-match");
  }
  const std::string output = RunRequiredHilCommand(installer, arguments, deadline);
  Json report;
  try {
    report = Json::parse(output);
  } catch (const Json::exception &) {
    throw std::runtime_error("Android APK installer did not return JSON evidence");
  }
  if (!report.is_object() || !report.value("passed", false) || !report.contains("installed") ||
      !report.at("installed").is_boolean() || !report.contains("reason") ||
      !report.at("reason").is_string() || !report.contains("local_sha256") ||
      !report.at("local_sha256").is_string() || !report.contains("installed_after_sha256") ||
      !report.at("installed_after_sha256").is_string() || !report.contains("elapsed_ms") ||
      !report.at("elapsed_ms").is_number_integer()) {
    throw std::runtime_error("Android APK installer evidence is incomplete");
  }

  AndroidApkInstallEvidence evidence{
      .installed = report.at("installed").get<bool>(),
      .reason = report.at("reason").get<std::string>(),
      .sha256 = report.at("local_sha256").get<std::string>(),
      .installed_before_sha256 = OptionalSha256(report, "installed_before_sha256"),
      .elapsed_ms = report.at("elapsed_ms").get<std::int64_t>(),
  };
  const std::string installed_after = report.at("installed_after_sha256").get<std::string>();
  if (!IsSha256(evidence.sha256) || installed_after != evidence.sha256 || evidence.elapsed_ms < 0 ||
      (evidence.reason == "exact_match" && (evidence.installed || !skip_exact_match ||
                                            evidence.installed_before_sha256 != evidence.sha256)) ||
      (evidence.reason == "forced" && (!evidence.installed || skip_exact_match)) ||
      (evidence.reason == "different_or_unverifiable" &&
       (!evidence.installed || !skip_exact_match)) ||
      (evidence.reason != "exact_match" && evidence.reason != "forced" &&
       evidence.reason != "different_or_unverifiable")) {
    throw std::runtime_error("Android APK installer evidence violates the install policy");
  }
  return evidence;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
AndroidApkInstallEvidence VerifyInstalledAndroidApk(
    const std::filesystem::path &installer, const std::filesystem::path &adb,
    const std::filesystem::path &apk, std::string_view serial,
    std::chrono::steady_clock::time_point deadline) {
  const std::string output = RunRequiredHilCommand(installer,
                                                   {adb.string(), "--serial", std::string(serial),
                                                    "--apk", apk.string(), "--require-exact-match"},
                                                   deadline);
  Json report;
  try {
    report = Json::parse(output);
  } catch (const Json::exception &) {
    throw std::runtime_error("Android APK verifier did not return JSON evidence");
  }
  if (!report.is_object() || !report.value("passed", false) || report.value("installed", true) ||
      report.value("reason", "") != "exact_match" || !report.contains("local_sha256") ||
      !report.at("local_sha256").is_string() || !report.contains("installed_before_sha256") ||
      !report.at("installed_before_sha256").is_string() ||
      !report.contains("installed_after_sha256") ||
      !report.at("installed_after_sha256").is_string() || !report.contains("elapsed_ms") ||
      !report.at("elapsed_ms").is_number_integer()) {
    throw std::runtime_error("Android APK verifier evidence is incomplete");
  }
  const std::string local = report.at("local_sha256").get<std::string>();
  const std::string before = report.at("installed_before_sha256").get<std::string>();
  const std::string after = report.at("installed_after_sha256").get<std::string>();
  const std::int64_t elapsed = report.at("elapsed_ms").get<std::int64_t>();
  if (!IsSha256(local) || before != local || after != local || elapsed < 0) {
    throw std::runtime_error("Android APK verifier evidence violates the read-only policy");
  }
  return {.installed = false,
          .reason = "exact_match",
          .sha256 = local,
          .installed_before_sha256 = before,
          .elapsed_ms = elapsed};
}

}  // namespace swing_capture::android::dual_hil
