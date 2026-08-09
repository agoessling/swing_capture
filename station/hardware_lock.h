#ifndef SWING_CAPTURE_STATION_HARDWARE_LOCK_H_
#define SWING_CAPTURE_STATION_HARDWARE_LOCK_H_

#include <filesystem>

namespace swing_capture::station {

// Non-blocking process lock shared by every camera-owning entry point.
class HardwareLock final {
 public:
  explicit HardwareLock(const std::filesystem::path &path);
  ~HardwareLock();

  HardwareLock(const HardwareLock &) = delete;
  HardwareLock &operator=(const HardwareLock &) = delete;
  HardwareLock(HardwareLock &&) = delete;
  HardwareLock &operator=(HardwareLock &&) = delete;

 private:
  static int Open(const std::filesystem::path &path);

  int descriptor_ = -1;
};

}  // namespace swing_capture::station

#endif  // SWING_CAPTURE_STATION_HARDWARE_LOCK_H_
