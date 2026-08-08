#include "capture/daheng/daheng_sdk_runtime.h"

#include <filesystem>
#include <iostream>
#include <string>

int main() {
  std::string error;
  const auto directory = swing_capture::daheng::FindBundledGentlDirectory(&error);
  if (!directory.has_value()) {
    std::cerr << "failed to find bundled GenTL producer: " << error << '\n';
    return 1;
  }

  const std::filesystem::path producer = std::filesystem::path(*directory) / "GxU3VTL.cti";
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(producer, filesystem_error)) {
    std::cerr << "GenTL producer is not a regular file: " << producer << '\n';
    return 1;
  }
  if (std::filesystem::file_size(producer, filesystem_error) == 0 || filesystem_error) {
    std::cerr << "GenTL producer is empty or unreadable: " << producer << '\n';
    return 1;
  }

  std::cout << "resolved bundled GenTL producer: " << producer << '\n';
  return 0;
}
