#ifndef SWING_CAPTURE_CAPTURE_SERVICE_CAMERA_WORKER_H_
#define SWING_CAPTURE_CAPTURE_SERVICE_CAMERA_WORKER_H_

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "capture/core/camera_source.h"
#include "capture/daheng/daheng_camera.h"
#include "capture/service/preview_api.h"

namespace swing_capture::service {

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
               daheng::DahengConfiguration configuration = {});
  ~CameraWorker();

  CameraWorker(const CameraWorker &) = delete;
  CameraWorker &operator=(const CameraWorker &) = delete;
  CameraWorker(CameraWorker &&) = delete;
  CameraWorker &operator=(CameraWorker &&) = delete;

  void Start();
  void Stop() noexcept;

  [[nodiscard]] CameraStatus Status();
  [[nodiscard]] std::optional<PreviewPng> LatestPreview(bool full_resolution);
  [[nodiscard]] CameraStatus UpdateSettings(const CameraSettingsUpdate &settings);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace swing_capture::service

#endif  // SWING_CAPTURE_CAPTURE_SERVICE_CAMERA_WORKER_H_
