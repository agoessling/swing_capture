#include "station/station_config.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace swing_capture::station {
namespace {

constexpr std::string_view kSchemaVersionKey = "schema_version";
constexpr std::string_view kRolesVerifiedKey = "camera.roles_verified";
constexpr std::string_view kDownTheLineSerialKey = "camera.down_the_line.serial";
constexpr std::string_view kFaceOnSerialKey = "camera.face_on.serial";
constexpr std::string_view kAudioDeviceKey = "audio.alsa_device";
constexpr std::string_view kAudioChannelCountKey = "audio.channel_count";
constexpr std::string_view kAudioSelectedChannelKey = "audio.selected_channel";
constexpr std::string_view kFeatherSerialPathKey = "feather.serial_path";

constexpr std::array<std::string_view, 8> kRequiredKeys = {
    kSchemaVersionKey, kRolesVerifiedKey,     kDownTheLineSerialKey,    kFaceOnSerialKey,
    kAudioDeviceKey,   kAudioChannelCountKey, kAudioSelectedChannelKey, kFeatherSerialPathKey,
};

bool IsRequiredKey(std::string_view key) {
  return std::ranges::find(kRequiredKeys, key) != kRequiredKeys.end();
}

std::string_view Trim(std::string_view value) {
  const std::size_t first = value.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = value.find_last_not_of(" \t\r");
  return value.substr(first, last - first + 1);
}

std::map<std::string, std::string> ParseValues(std::string_view contents) {
  std::map<std::string, std::string> values;
  std::istringstream input{std::string(contents)};
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const std::string_view trimmed = Trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }
    const std::size_t equals = trimmed.find('=');
    if (equals == std::string_view::npos) {
      throw std::invalid_argument("station config line " + std::to_string(line_number) +
                                  " must contain '='");
    }
    const std::string key(Trim(trimmed.substr(0, equals)));
    const std::string value(Trim(trimmed.substr(equals + 1)));
    if (!IsRequiredKey(key)) {
      throw std::invalid_argument("unknown station config key on line " +
                                  std::to_string(line_number) + ": " + key);
    }
    if (value.empty()) {
      throw std::invalid_argument("empty station config value on line " +
                                  std::to_string(line_number) + ": " + key);
    }
    if (!values.emplace(key, value).second) {
      throw std::invalid_argument("duplicate station config key on line " +
                                  std::to_string(line_number) + ": " + key);
    }
  }
  for (const std::string_view key : kRequiredKeys) {
    if (!values.contains(std::string(key))) {
      throw std::invalid_argument("missing station config key: " + std::string(key));
    }
  }
  return values;
}

bool ParseBoolean(std::string_view key, const std::string &value) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  throw std::invalid_argument(std::string(key) + " must be true or false");
}

std::uint16_t ParseAudioChannelValue(std::string_view key, const std::string &value) {
  std::uint64_t parsed = 0;
  for (const char character : value) {
    if (character < '0' || character > '9') {
      throw std::invalid_argument(std::string(key) + " must be an unsigned integer");
    }
    parsed = parsed * 10U + static_cast<unsigned>(character - '0');
    if (parsed > std::numeric_limits<std::uint16_t>::max()) {
      throw std::invalid_argument(std::string(key) + " exceeds uint16");
    }
  }
  return static_cast<std::uint16_t>(parsed);
}

void ValidateStableAudioDevice(const std::string &device) {
  constexpr std::string_view prefix = "hw:CARD=";
  const std::size_t device_separator = device.find(",DEV=");
  if (!device.starts_with(prefix) || device_separator == std::string::npos ||
      device_separator == prefix.size() || device_separator + 5 >= device.size()) {
    throw std::invalid_argument(
        "audio.alsa_device must use a stable hw:CARD=<id>,DEV=<number> identifier");
  }
  const std::string_view card =
      std::string_view(device).substr(prefix.size(), device_separator - prefix.size());
  const std::string_view device_number = std::string_view(device).substr(device_separator + 5);
  if (card.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
          std::string_view::npos ||
      card.find_first_not_of("0123456789") == std::string_view::npos ||
      device_number.find_first_not_of("0123456789") != std::string_view::npos) {
    throw std::invalid_argument(
        "audio.alsa_device must use a stable hw:CARD=<id>,DEV=<number> identifier");
  }
}

void ValidateFeatherPath(const std::filesystem::path &path) {
  constexpr std::string_view prefix = "/dev/serial/by-id/";
  if (!path.is_absolute() || !path.string().starts_with(prefix) ||
      path.filename().string().empty()) {
    throw std::invalid_argument("feather.serial_path must be an absolute /dev/serial/by-id path");
  }
}

}  // namespace

StationConfig ParseStationConfig(std::string_view contents) {
  const std::map<std::string, std::string> values = ParseValues(contents);
  if (values.at(std::string(kSchemaVersionKey)) != "1") {
    throw std::invalid_argument("unsupported station config schema_version");
  }

  StationConfig config = {
      .camera_roles_verified =
          ParseBoolean(kRolesVerifiedKey, values.at(std::string(kRolesVerifiedKey))),
      .down_the_line_camera_serial = values.at(std::string(kDownTheLineSerialKey)),
      .face_on_camera_serial = values.at(std::string(kFaceOnSerialKey)),
      .audio_alsa_device = values.at(std::string(kAudioDeviceKey)),
      .audio_channel_count = ParseAudioChannelValue(kAudioChannelCountKey,
                                                    values.at(std::string(kAudioChannelCountKey))),
      .audio_selected_channel = ParseAudioChannelValue(
          kAudioSelectedChannelKey, values.at(std::string(kAudioSelectedChannelKey))),
      .feather_serial_path = values.at(std::string(kFeatherSerialPathKey)),
  };
  if (config.down_the_line_camera_serial == config.face_on_camera_serial) {
    throw std::invalid_argument("camera role serials must be distinct");
  }
  ValidateStableAudioDevice(config.audio_alsa_device);
  if (config.audio_channel_count == 0 ||
      config.audio_selected_channel >= config.audio_channel_count) {
    throw std::invalid_argument(
        "audio.channel_count must be positive and audio.selected_channel must be smaller");
  }
  ValidateFeatherPath(config.feather_serial_path);
  return config;
}

StationConfig LoadStationConfig(const std::filesystem::path &path) {
  const std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open station config: " + path.string());
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (input.bad()) {
    throw std::runtime_error("cannot read station config: " + path.string());
  }
  try {
    return ParseStationConfig(contents.str());
  } catch (const std::invalid_argument &error) {
    throw std::invalid_argument(path.string() + ": " + error.what());
  }
}

std::optional<std::filesystem::path> StationConfigPathFromEnvironment() {
  // Station configuration is read once at process startup, before any worker
  // threads are created.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *value = std::getenv(std::string(kStationConfigEnvironment).c_str());
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::filesystem::path(value);
}

}  // namespace swing_capture::station
