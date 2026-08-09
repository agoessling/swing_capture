#ifndef SWING_CAPTURE_STATION_STATION_CONFIG_H_
#define SWING_CAPTURE_STATION_STATION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace swing_capture::station {

inline constexpr std::string_view kStationConfigEnvironment = "SWING_CAPTURE_STATION_CONFIG";

struct StationConfig {
  bool camera_roles_verified = false;
  std::string down_the_line_camera_serial;
  std::string face_on_camera_serial;
  std::string audio_alsa_device;
  std::uint16_t audio_channel_count = 0;
  std::uint16_t audio_selected_channel = 0;
  std::filesystem::path feather_serial_path;
};

StationConfig ParseStationConfig(std::string_view contents);
StationConfig LoadStationConfig(const std::filesystem::path &path);
std::optional<std::filesystem::path> StationConfigPathFromEnvironment();

}  // namespace swing_capture::station

#endif  // SWING_CAPTURE_STATION_STATION_CONFIG_H_
