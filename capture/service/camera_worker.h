#ifndef SWING_CAPTURE_CAPTURE_SERVICE_CAMERA_WORKER_H_
#define SWING_CAPTURE_CAPTURE_SERVICE_CAMERA_WORKER_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "capture/core/camera_source.h"
#include "capture/daheng/daheng_camera.h"
#include "capture/preview/latest_frame_sampler.h"
#include "capture/preview/preview_image.h"
#include "capture/service/preview_api.h"

namespace swing_capture::service {

using CapturedFrameSink = std::function<void(const FrameView &)>;
// Optional deterministic fault-injection seam used by software tests. Production
// callers leave this empty; when present it runs after the capture sink and
// before the bounded latest-frame sampler.
using PreviewSamplingHook = std::function<void()>;

class PreviewCameraDevice {
 public:
  PreviewCameraDevice() = default;
  virtual ~PreviewCameraDevice() = default;

  PreviewCameraDevice(const PreviewCameraDevice &) = delete;
  PreviewCameraDevice &operator=(const PreviewCameraDevice &) = delete;
  PreviewCameraDevice(PreviewCameraDevice &&) = delete;
  PreviewCameraDevice &operator=(PreviewCameraDevice &&) = delete;

  [[nodiscard]] virtual CameraIdentity identity() const = 0;
  [[nodiscard]] virtual CaptureProfile profile() const = 0;
  [[nodiscard]] virtual daheng::DahengDiagnostics diagnostics() const = 0;
  virtual void Configure(const daheng::DahengConfiguration &configuration) = 0;
  virtual void Start() = 0;
  virtual void Stop() = 0;
  virtual bool CaptureOne(std::chrono::milliseconds timeout,
                          const daheng::FrameHandler &handler) = 0;
};

class CameraWorker final {
 public:
  CameraWorker(CameraRole role, std::unique_ptr<PreviewCameraDevice> camera,
               daheng::DahengConfiguration configuration = {},
               std::unique_ptr<preview::PreviewFrameProcessor> frame_processor = nullptr,
               CapturedFrameSink captured_frame_sink = {},
               PreviewSamplingHook preview_sampling_hook = {});
  ~CameraWorker();

  CameraWorker(const CameraWorker &) = delete;
  CameraWorker &operator=(const CameraWorker &) = delete;
  CameraWorker(CameraWorker &&) = delete;
  CameraWorker &operator=(CameraWorker &&) = delete;

  void Start();
  void Stop() noexcept;

  [[nodiscard]] CameraStatus Status();
  [[nodiscard]] std::uint64_t TimestampTicksPerSecond();
  // Immutable full-resolution Bayer sample used by explicitly enabled local
  // HIL calibration. It reuses the preview sampler's bounded latest-only
  // publication slot and does not add another camera SDK consumer. Calibration
  // may retain a separately capped set of returned immutable snapshots.
  [[nodiscard]] std::shared_ptr<const preview::SampledPreviewFrame> LatestSampledFrame();
  void SetLatestFrameSamplingInterval(std::chrono::steady_clock::duration minimum_interval);
  [[nodiscard]] std::chrono::steady_clock::duration LatestFrameSamplingInterval() const;
  [[nodiscard]] std::optional<PreviewImage> LatestPreview(bool full_resolution);
  [[nodiscard]] CameraStatus UpdateSettings(const CameraSettingsUpdate &settings);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace swing_capture::service

#endif  // SWING_CAPTURE_CAPTURE_SERVICE_CAMERA_WORKER_H_
