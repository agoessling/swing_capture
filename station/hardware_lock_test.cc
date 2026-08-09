#include "station/hardware_lock.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

using swing_capture::station::HardwareLock;

std::filesystem::path UniqueTemporaryPath() {
  return std::filesystem::temp_directory_path() /
         ("swing-capture-hardware-lock-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) /
         "hardware.lock";
}

void TestExclusiveAndReusable() {
  const std::filesystem::path path = UniqueTemporaryPath();
  {
    const HardwareLock first(path);
    bool rejected = false;
    try {
      const HardwareLock second(path);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    assert(rejected);
  }
  const HardwareLock after_release(path);
}

}  // namespace

int main() {
  TestExclusiveAndReusable();
  return 0;
}
