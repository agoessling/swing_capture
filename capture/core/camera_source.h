#ifndef SWING_CAPTURE_CAPTURE_CORE_CAMERA_SOURCE_H_
#define SWING_CAPTURE_CAPTURE_CORE_CAMERA_SOURCE_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace swing_capture {

struct CameraIdentity {
  std::string model;
  std::string serial_number;
  std::string vendor;
};

struct CaptureProfile {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  double frames_per_second = 0.0;
  std::string pixel_format;
};

struct FrameMetadata {
  std::uint64_t frame_id = 0;
  std::uint64_t device_timestamp = 0;
  std::chrono::steady_clock::time_point host_received_at;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool complete = false;
};

struct FrameView {
  FrameMetadata metadata;
  std::span<const std::byte> payload;
};

class CameraSource {
 public:
  CameraSource() = default;
  virtual ~CameraSource() = default;

  CameraSource(const CameraSource &) = delete;
  CameraSource &operator=(const CameraSource &) = delete;
  CameraSource(CameraSource &&) = delete;
  CameraSource &operator=(CameraSource &&) = delete;

  [[nodiscard]] virtual CameraIdentity identity() const = 0;
  [[nodiscard]] virtual CaptureProfile profile() const = 0;

  virtual void Start() = 0;
  virtual void Stop() = 0;
};

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_CORE_CAMERA_SOURCE_H_
