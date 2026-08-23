#include "android/dual_hil/hil_artifact_validation.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

[[noreturn]] void Invalid(std::string_view location, std::string_view diagnostic) {
  throw std::invalid_argument("invalid HIL artifact at " + std::string(location) + ": " +
                              std::string(diagnostic));
}

void ValidateRelativePath(
    const std::filesystem::path &relative_path,
    std::string_view raw_path,  // NOLINT(bugprone-easily-swappable-parameters)
    std::string_view location) {
  if (raw_path.empty() || raw_path.contains('\0') || raw_path.contains('\\') ||
      raw_path.ends_with('/') || relative_path.empty() || relative_path.is_absolute() ||
      relative_path.has_root_name() || relative_path.has_root_directory() ||
      relative_path.lexically_normal() != relative_path ||
      relative_path.generic_string() != raw_path) {
    Invalid(location, "path must be a canonical, nonempty relative path");
  }
  for (const auto &component : relative_path) {
    if (component == "." || component == ".." || component.empty()) {
      Invalid(location, "path must not contain empty, current, or parent components");
    }
  }
}

bool IsContainedBy(const std::filesystem::path &candidate, const std::filesystem::path &root) {
  const std::filesystem::path relative = candidate.lexically_relative(root);
  return !relative.empty() && !relative.is_absolute() &&
         std::ranges::none_of(relative, [](const auto &component) { return component == ".."; });
}

bool IsContainedByOrEqual(const std::filesystem::path &candidate,
                          const std::filesystem::path &root) {
  return candidate == root || IsContainedBy(candidate, root);
}

void ValidateArtifactFile(
    const std::string &raw_path, std::string_view location,
    const std::filesystem::path &output_root,  // NOLINT(bugprone-easily-swappable-parameters)
    const std::filesystem::path &canonical_output_root,
    const std::optional<std::filesystem::path> &allowed_missing_self_report) {
  const std::filesystem::path relative_path(raw_path);
  ValidateRelativePath(relative_path, raw_path, location);

  std::error_code status_error;
  const std::filesystem::path candidate = output_root / relative_path;
  const std::filesystem::file_status link_status =
      std::filesystem::symlink_status(candidate, status_error);
  if (link_status.type() == std::filesystem::file_type::not_found) {
    if (allowed_missing_self_report.has_value() && relative_path == *allowed_missing_self_report) {
      std::error_code parent_error;
      const std::filesystem::path canonical_parent =
          std::filesystem::canonical(candidate.parent_path(), parent_error);
      if (parent_error || !std::filesystem::is_directory(canonical_parent) ||
          !IsContainedByOrEqual(canonical_parent, canonical_output_root)) {
        Invalid(location, "missing self-report parent is outside the undeclared-output root");
      }
      return;
    }
    Invalid(location, "referenced file does not exist");
  }
  if (status_error) {
    Invalid(location, "cannot inspect path: " + status_error.message());
  }
  if (!std::filesystem::is_regular_file(link_status)) {
    Invalid(location, "referenced path is not a regular file");
  }

  std::error_code canonical_error;
  const std::filesystem::path canonical_candidate =
      std::filesystem::canonical(candidate, canonical_error);
  if (canonical_error) {
    Invalid(location, "cannot resolve path: " + canonical_error.message());
  }
  if (!IsContainedBy(canonical_candidate, canonical_output_root)) {
    Invalid(location, "referenced file is outside the undeclared-output root");
  }
}

void ValidateArtifactTree(const Json &value, const std::string &location,
                          const std::filesystem::path &output_root,
                          const std::filesystem::path &canonical_output_root,
                          const std::optional<std::filesystem::path> &allowed_missing_self_report) {
  if (value.is_null()) {
    return;
  }
  if (value.is_string()) {
    ValidateArtifactFile(value.get_ref<const std::string &>(), location, output_root,
                         canonical_output_root, allowed_missing_self_report);
    return;
  }
  if (value.is_object()) {
    for (auto entry = value.cbegin(); entry != value.cend(); ++entry) {
      ValidateArtifactTree(entry.value(), location + "/" + entry.key(), output_root,
                           canonical_output_root, allowed_missing_self_report);
    }
    return;
  }
  if (value.is_array()) {
    for (std::size_t index = 0; index < value.size(); ++index) {
      ValidateArtifactTree(value.at(index), location + "/" + std::to_string(index), output_root,
                           canonical_output_root, allowed_missing_self_report);
    }
    return;
  }
  Invalid(location, "artifact tree leaves must be strings or null");
}

}  // namespace

void ValidateHilReportArtifacts(std::string_view report_json,
                                const std::filesystem::path &output_root,
                                std::optional<std::filesystem::path> allowed_missing_self_report) {
  Json report;
  try {
    report = Json::parse(report_json);
  } catch (const nlohmann::json::exception &failure) {
    throw std::invalid_argument("HIL report is not valid JSON: " + std::string(failure.what()));
  }
  if (!report.is_object() || !report.contains("artifacts") || !report.at("artifacts").is_object()) {
    throw std::invalid_argument("HIL report must contain an artifacts object");
  }

  std::error_code canonical_error;
  const std::filesystem::path canonical_output_root =
      std::filesystem::canonical(output_root, canonical_error);
  if (canonical_error || !std::filesystem::is_directory(canonical_output_root)) {
    throw std::invalid_argument("undeclared-output root must be an existing directory");
  }
  if (allowed_missing_self_report.has_value()) {
    const std::string raw_self_report = allowed_missing_self_report->generic_string();
    ValidateRelativePath(*allowed_missing_self_report, raw_self_report, "self-report exception");
  }
  ValidateArtifactTree(report.at("artifacts"), "/artifacts", output_root, canonical_output_root,
                       allowed_missing_self_report);
}

}  // namespace swing_capture::android::dual_hil
