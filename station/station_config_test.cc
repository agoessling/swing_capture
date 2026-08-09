#include "station/station_config.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kValidConfig = R"(
# Machine-local capture station configuration.
schema_version = 1
camera.roles_verified = true
camera.down_the_line.serial = DOWN123
camera.face_on.serial = FACE456
audio.alsa_device = hw:CARD=microphone,DEV=0
audio.channel_count = 1
audio.selected_channel = 0
feather.serial_path = /dev/serial/by-id/usb-Raspberry_Pi_Pico_123-if00
)";

bool Rejects(std::string_view config) {
  try {
    static_cast<void>(swing_capture::station::ParseStationConfig(config));
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

void TestValidConfiguration() {
  const auto config = swing_capture::station::ParseStationConfig(kValidConfig);
  assert(config.camera_roles_verified);
  assert(config.down_the_line_camera_serial == "DOWN123");
  assert(config.face_on_camera_serial == "FACE456");
  assert(config.audio_alsa_device == "hw:CARD=microphone,DEV=0");
  assert(config.audio_channel_count == 1);
  assert(config.audio_selected_channel == 0);
  assert(config.feather_serial_path == "/dev/serial/by-id/usb-Raspberry_Pi_Pico_123-if00");
}

void TestUnverifiedRolesRemainRepresentable() {
  std::string config(kValidConfig);
  config.replace(config.find("camera.roles_verified = true"),
                 std::string("camera.roles_verified = true").size(),
                 "camera.roles_verified = false");
  assert(!swing_capture::station::ParseStationConfig(config).camera_roles_verified);
}

void TestLoadConfigurationFile() {
  const char *test_temporary_directory = std::getenv("TEST_TMPDIR");
  assert(test_temporary_directory != nullptr);
  const std::filesystem::path config_path =
      std::filesystem::path(test_temporary_directory) / "station.conf";
  {
    std::ofstream output(config_path);
    output << kValidConfig;
    output.close();
    assert(output);
  }
  const auto config = swing_capture::station::LoadStationConfig(config_path);
  assert(config.down_the_line_camera_serial == "DOWN123");
}

void TestMalformedConfigurationsAreRejected() {
  assert(Rejects("schema_version = 1\n"));
  assert(Rejects(std::string(kValidConfig) + "unknown.key = value\n"));
  assert(Rejects(std::string(kValidConfig) + "schema_version = 1\n"));

  std::string duplicate_serials(kValidConfig);
  duplicate_serials.replace(duplicate_serials.find("FACE456"), 7, "DOWN123");
  assert(Rejects(duplicate_serials));

  std::string numeric_audio_card(kValidConfig);
  numeric_audio_card.replace(numeric_audio_card.find("CARD=microphone"), 15, "CARD=1");
  assert(Rejects(numeric_audio_card));

  std::string volatile_serial_path(kValidConfig);
  const std::string stable_path = "/dev/serial/by-id/usb-Raspberry_Pi_Pico_123-if00";
  volatile_serial_path.replace(volatile_serial_path.find(stable_path), stable_path.size(),
                               "/dev/ttyACM0");
  assert(Rejects(volatile_serial_path));

  std::string zero_channels(kValidConfig);
  zero_channels.replace(zero_channels.find("audio.channel_count = 1"),
                        std::string("audio.channel_count = 1").size(), "audio.channel_count = 0");
  assert(Rejects(zero_channels));

  std::string selected_channel_out_of_range(kValidConfig);
  selected_channel_out_of_range.replace(
      selected_channel_out_of_range.find("audio.selected_channel = 0"),
      std::string("audio.selected_channel = 0").size(), "audio.selected_channel = 1");
  assert(Rejects(selected_channel_out_of_range));
}

}  // namespace

int main() {
  TestValidConfiguration();
  TestUnverifiedRolesRemainRepresentable();
  TestLoadConfigurationFile();
  TestMalformedConfigurationsAreRejected();
  return 0;
}
