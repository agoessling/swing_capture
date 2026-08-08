#ifndef SWING_CAPTURE_CAPTURE_DAHENG_DAHENG_CAMERA_H_
#define SWING_CAPTURE_CAPTURE_DAHENG_DAHENG_CAMERA_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "capture/core/camera_source.h"

namespace swing_capture::daheng {

struct DiscoveredCamera {
  CameraIdentity identity;
  std::string transport;
};

class GalaxySdk final {
 public:
  GalaxySdk();
  ~GalaxySdk();

  GalaxySdk(const GalaxySdk &) = delete;
  GalaxySdk &operator=(const GalaxySdk &) = delete;
  GalaxySdk(GalaxySdk &&) = delete;
  GalaxySdk &operator=(GalaxySdk &&) = delete;

  [[nodiscard]] std::vector<DiscoveredCamera> Discover(std::chrono::milliseconds timeout) const;

 private:
  friend class DahengCamera;
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

struct DahengConfiguration {
  std::uint32_t width = 1440;
  std::uint32_t height = 1080;
  double target_frames_per_second = 227.0;
  double exposure_microseconds = 4000.0;
  double gain_decibels = 0.0;
  std::uint64_t acquisition_buffer_count = 40;
  // Zero preserves the Galaxy transport producer's detected default.
  std::int64_t stream_transfer_bytes = 0;
  std::int64_t stream_urb_count = 0;
};

struct DahengDiagnostics {
  bool deterministic_free_run_verified = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::int64_t offset_x = 0;
  std::int64_t offset_y = 0;
  double requested_exposure_microseconds = 0.0;
  double exposure_microseconds = 0.0;
  double minimum_exposure_microseconds = 0.0;
  double maximum_exposure_microseconds = 0.0;
  double exposure_increment_microseconds = 0.0;
  bool exposure_increment_valid = false;
  double requested_gain_decibels = 0.0;
  double gain_decibels = 0.0;
  double minimum_gain_decibels = 0.0;
  double maximum_gain_decibels = 0.0;
  double gain_increment_decibels = 0.0;
  bool gain_increment_valid = false;
  double requested_frames_per_second = 0.0;
  double target_frames_per_second = 0.0;
  double minimum_target_frames_per_second = 0.0;
  double maximum_target_frames_per_second = 0.0;
  double frame_rate_increment = 0.0;
  bool frame_rate_increment_valid = false;
  double resulting_frames_per_second = 0.0;
  std::string acquisition_mode;
  std::string trigger_selector;
  std::string trigger_mode;
  std::string pixel_format;
  std::string exposure_mode;
  std::string exposure_auto;
  std::string gain_selector;
  std::string gain_auto;
  std::string exposure_time_mode;
  std::string throughput_limit_mode;
  std::string frame_rate_mode;
  std::uint64_t acquisition_buffer_count = 0;
  std::uint64_t timestamp_ticks_per_second = 0;
  std::string timestamp_frequency_source;
  std::uint64_t payload_bytes = 0;
  std::uint64_t estimated_bandwidth_bytes_per_second = 0;
  std::int64_t stream_transfer_bytes = 0;
  std::int64_t stream_urb_count = 0;
};

using FrameHandler = std::function<void(const FrameView &)>;

class DahengCamera final : public CameraSource {
 public:
  DahengCamera(GalaxySdk &sdk, std::string serial_number);
  ~DahengCamera() override;

  DahengCamera(const DahengCamera &) = delete;
  DahengCamera &operator=(const DahengCamera &) = delete;
  DahengCamera(DahengCamera &&) = delete;
  DahengCamera &operator=(DahengCamera &&) = delete;

  [[nodiscard]] CameraIdentity identity() const override;
  [[nodiscard]] CaptureProfile profile() const override;
  [[nodiscard]] DahengDiagnostics diagnostics() const;

  void Configure(const DahengConfiguration &configuration);
  void Start() override;
  void Stop() override;

  // The payload view remains valid only for the duration of handler.
  // Returns false on timeout and true when a frame (complete or incomplete)
  // was delivered to handler.
  bool CaptureOne(std::chrono::milliseconds timeout, const FrameHandler &handler);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace swing_capture::daheng

#endif  // SWING_CAPTURE_CAPTURE_DAHENG_DAHENG_CAMERA_H_
