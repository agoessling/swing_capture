#ifndef SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_SERIAL_H_
#define SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_SERIAL_H_

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "capture/hil/feather_hil_protocol.h"

namespace swing_capture::hil {

// Single-threaded newline transport for the Feather's USB CDC endpoint.
class FeatherHilSerial final {
 public:
  explicit FeatherHilSerial(const std::filesystem::path &device_path);
  ~FeatherHilSerial();

  FeatherHilSerial(const FeatherHilSerial &) = delete;
  FeatherHilSerial &operator=(const FeatherHilSerial &) = delete;
  FeatherHilSerial(FeatherHilSerial &&) = delete;
  FeatherHilSerial &operator=(FeatherHilSerial &&) = delete;

  void Write(std::string_view command, std::chrono::milliseconds timeout) const;
  [[nodiscard]] FeatherResponse Read(std::chrono::milliseconds timeout);

 private:
  int file_descriptor_ = -1;
  std::string receive_buffer_;
};

}  // namespace swing_capture::hil

#endif  // SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_SERIAL_H_
