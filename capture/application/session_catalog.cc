#include "capture/application/session_catalog.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace swing_capture::application {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr std::array<std::string_view, 2> kRoles = {"down_the_line", "face_on"};
constexpr std::array<std::size_t, 14> kTimestampDigitIndices = {0U, 1U,  2U,  3U,  5U,  6U,  8U,
                                                                9U, 11U, 12U, 14U, 15U, 17U, 18U};

bool IsSafeComponent(std::string_view value) {
  if (value.empty() || value == "." || value == "..") {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) ||
           (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) ||
           (byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9')) ||
           character == '-' || character == '_' || character == '.';
  });
}

bool IsRegularFileWithoutFollowingLinks(const std::filesystem::path &path) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
  return !error && std::filesystem::is_regular_file(status);
}

bool IsWriterTimestamp(std::string_view value) {
  if (value.size() != 20U || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
    return false;
  }
  if (!std::ranges::all_of(kTimestampDigitIndices, [value](std::size_t index) {
        return value[index] >= '0' && value[index] <= '9';
      })) {
    return false;
  }
  const auto digit = [value](std::size_t index) { return value[index] - '0'; };
  const int year_value = digit(0) * 1000 + digit(1) * 100 + digit(2) * 10 + digit(3);
  const auto month_value = static_cast<unsigned int>(digit(5) * 10 + digit(6));
  const auto day_value = static_cast<unsigned int>(digit(8) * 10 + digit(9));
  const int hour = digit(11) * 10 + digit(12);
  const int minute = digit(14) * 10 + digit(15);
  const int second = digit(17) * 10 + digit(18);
  return std::chrono::year_month_day(std::chrono::year(year_value), std::chrono::month(month_value),
                                     std::chrono::day(day_value))
             .ok() &&
         hour < 24 && minute < 60 && second < 60;
}

std::optional<PublishedSessionRecord> ParseSessionDirectory(
    const std::filesystem::directory_entry &entry) {
  std::error_code error;
  if (!std::filesystem::is_directory(entry.symlink_status(error)) || error) {
    return std::nullopt;
  }
  const std::string session_id = entry.path().filename().string();
  if (!IsSafeComponent(session_id) || session_id.starts_with('.')) {
    return std::nullopt;
  }
  const std::filesystem::path manifest_path = entry.path() / "manifest.json";
  if (!IsRegularFileWithoutFollowingLinks(manifest_path)) {
    return std::nullopt;
  }

  try {
    std::ifstream manifest_stream(manifest_path, std::ios::binary);
    if (!manifest_stream) {
      return std::nullopt;
    }
    const Json manifest = Json::parse(manifest_stream);
    if (!manifest.is_object() || manifest.at("schema_version") != 1 ||
        manifest.at("session_id") != session_id || !manifest.at("created_at_utc").is_string() ||
        !IsWriterTimestamp(manifest.at("created_at_utc").get_ref<const std::string &>()) ||
        !manifest.at("views").is_array() || manifest.at("views").size() != kRoles.size()) {
      return std::nullopt;
    }

    PublishedSessionRecord record = {
        .session_id = session_id,
        .created_at_utc = manifest.at("created_at_utc").get<std::string>(),
        .manifest_path = manifest_path,
        .media_paths = {},
    };
    std::array<bool, 2> found = {false, false};
    for (const Json &view : manifest.at("views")) {
      if (!view.is_object() || !view.at("role").is_string() || !view.at("media").is_object()) {
        return std::nullopt;
      }
      const std::string role = view.at("role").get<std::string>();
      const auto *const role_it = std::ranges::find(kRoles, role);
      if (role_it == kRoles.end()) {
        return std::nullopt;
      }
      const auto role_index = static_cast<std::size_t>(role_it - kRoles.begin());
      if (found[role_index]) {
        return std::nullopt;
      }
      const Json &media = view.at("media");
      const std::string expected_name = role + ".webm";
      if (!media.at("path").is_string() || media.at("path") != expected_name ||
          media.at("mime_type") != "video/webm" ||
          !media.at("encoded_bytes").is_number_unsigned()) {
        return std::nullopt;
      }
      const std::filesystem::path media_path = entry.path() / expected_name;
      if (!IsRegularFileWithoutFollowingLinks(media_path) ||
          std::filesystem::file_size(media_path) !=
              media.at("encoded_bytes").get<std::uintmax_t>()) {
        return std::nullopt;
      }
      record.media_paths[role_index] = media_path;
      found[role_index] = true;
    }
    if (!std::ranges::all_of(found, std::identity{})) {
      return std::nullopt;
    }
    return record;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

}  // namespace

std::vector<PublishedSessionRecord> DiscoverPublishedSessions(const std::filesystem::path &root) {
  std::vector<PublishedSessionRecord> sessions;
  std::error_code status_error;
  const std::filesystem::file_status root_status =
      std::filesystem::symlink_status(root, status_error);
  if ((!status_error && root_status.type() == std::filesystem::file_type::not_found) ||
      status_error == std::errc::no_such_file_or_directory) {
    return sessions;
  }
  if (status_error) {
    throw std::filesystem::filesystem_error("cannot inspect session catalog root", root,
                                            status_error);
  }
  if (!std::filesystem::is_directory(root_status)) {
    throw std::invalid_argument("session catalog root is not a directory: " + root.string());
  }
  std::error_code error;
  const std::filesystem::directory_iterator entries(root, error);
  if (error) {
    throw std::filesystem::filesystem_error("cannot enumerate session catalog", root, error);
  }
  for (const std::filesystem::directory_entry &entry : entries) {
    if (const auto session = ParseSessionDirectory(entry); session.has_value()) {
      sessions.push_back(*session);
    }
  }
  std::ranges::sort(sessions,
                    [](const PublishedSessionRecord &left, const PublishedSessionRecord &right) {
                      if (left.created_at_utc != right.created_at_utc) {
                        return left.created_at_utc > right.created_at_utc;
                      }
                      return left.session_id > right.session_id;
                    });
  return sessions;
}

PublishedSessionCatalog::PublishedSessionCatalog(std::filesystem::path root)
    : root_(std::move(root)) {
  if (root_.empty()) {
    throw std::invalid_argument("session catalog root cannot be empty");
  }
  Refresh();
}

void PublishedSessionCatalog::Refresh() {
  std::vector<PublishedSessionRecord> discovered = DiscoverPublishedSessions(root_);
  const std::scoped_lock lock(mutex_);
  sessions_ = std::move(discovered);
}

std::vector<PublishedSessionRecord> PublishedSessionCatalog::Sessions() const {
  const std::scoped_lock lock(mutex_);
  return sessions_;
}

std::optional<PublishedSessionRecord> PublishedSessionCatalog::Find(
    std::string_view session_id) const {
  if (!IsSafeComponent(session_id)) {
    return std::nullopt;
  }
  const std::scoped_lock lock(mutex_);
  const auto found = std::ranges::find(sessions_, session_id, &PublishedSessionRecord::session_id);
  return found == sessions_.end() ? std::nullopt : std::optional(*found);
}

std::optional<PublishedSessionRecord> FindPublishedSession(const std::filesystem::path &root,
                                                           std::string_view session_id) {
  if (!IsSafeComponent(session_id)) {
    return std::nullopt;
  }
  const std::vector<PublishedSessionRecord> sessions = DiscoverPublishedSessions(root);
  const auto found = std::ranges::find(sessions, session_id, &PublishedSessionRecord::session_id);
  return found == sessions.end() ? std::nullopt : std::optional(*found);
}

}  // namespace swing_capture::application
