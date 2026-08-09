#include "capture/service/preview_station.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "capture/core/camera_source.h"
#include "capture/daheng/daheng_camera.h"
#include "capture/service/camera_worker.h"
#include "capture/service/preview_api.h"
#include "station/station_config.h"

namespace swing_capture::service {
namespace {

class DahengPreviewCamera final : public PreviewCameraDevice {
 public:
  DahengPreviewCamera(daheng::GalaxySdk &sdk, const std::string &serial) : camera_(sdk, serial) {}

  [[nodiscard]] CameraIdentity identity() const override { return camera_.identity(); }
  [[nodiscard]] CaptureProfile profile() const override { return camera_.profile(); }
  [[nodiscard]] daheng::DahengDiagnostics diagnostics() const override {
    return camera_.diagnostics();
  }

  void Configure(const daheng::DahengConfiguration &configuration) override {
    camera_.Configure(configuration);
  }
  void Start() override { camera_.Start(); }
  void Stop() override { camera_.Stop(); }
  bool CaptureOne(std::chrono::milliseconds timeout, const daheng::FrameHandler &handler) override {
    return camera_.CaptureOne(timeout, handler);
  }

 private:
  daheng::DahengCamera camera_;
};

CameraStatus DisconnectedStatus(CameraRole role, const std::string &serial,
                                const std::string &startup_error) {
  return {
      .role = role,
      .serial = serial,
      .model = "Assigned Daheng camera",
      .connected = false,
      .error = startup_error,
      .stream_fps = 0.0,
      .preview_sequence = 0,
      .preview_width = 1440,
      .preview_height = 1080,
      .exposure_microseconds = {.value = daheng::kDefaultExposureMicroseconds,
                                .minimum = 1.0,
                                .maximum = 1000000.0,
                                .increment = 1.0},
      .gain_decibels = {.value = daheng::kDefaultGainDecibels,
                        .minimum = 0.0,
                        .maximum = 24.0,
                        .increment = 0.1},
      .image_quality = {.assessment = "unavailable"},
  };
}

}  // namespace

struct PreviewStation::Impl {
  struct CameraSlot {
    CameraRole role;
    std::string serial;
    std::unique_ptr<CameraWorker> worker;
    std::string startup_error;
  };

  explicit Impl(const station::StationConfig &config) : sdk(std::make_unique<daheng::GalaxySdk>()) {
    if (!config.camera_roles_verified) {
      throw std::invalid_argument("station camera roles must be verified before starting preview");
    }
    slots.reserve(2);
    slots.push_back({.role = CameraRole::kDownTheLine,
                     .serial = config.down_the_line_camera_serial,
                     .worker = nullptr,
                     .startup_error = {}});
    slots.push_back({.role = CameraRole::kFaceOn,
                     .serial = config.face_on_camera_serial,
                     .worker = nullptr,
                     .startup_error = {}});
    const std::vector<daheng::DiscoveredCamera> discovered = sdk->Discover(std::chrono::seconds(1));
    for (CameraSlot &slot : slots) {
      const bool present =
          std::ranges::any_of(discovered, [&slot](const daheng::DiscoveredCamera &camera) {
            return camera.identity.serial_number == slot.serial;
          });
      if (!present) {
        slot.startup_error = "assigned camera was not discovered";
        continue;
      }
      try {
        slot.worker = std::make_unique<CameraWorker>(
            slot.role, std::make_unique<DahengPreviewCamera>(*sdk, slot.serial));
        slot.worker->Start();
      } catch (const std::exception &error) {
        slot.startup_error = error.what();
        slot.worker.reset();
      }
    }
  }

  CameraSlot &Slot(CameraRole role) {
    const auto slot = std::ranges::find(slots, role, &CameraSlot::role);
    if (slot == slots.end()) {
      throw std::invalid_argument("unknown camera role");
    }
    return *slot;
  }

  std::unique_ptr<daheng::GalaxySdk> sdk;
  std::vector<CameraSlot> slots;
};

PreviewStation::PreviewStation(const station::StationConfig &config)
    : impl_(std::make_unique<Impl>(config)) {}

PreviewStation::~PreviewStation() = default;

std::vector<CameraStatus> PreviewStation::CameraStatuses() {
  std::vector<CameraStatus> statuses;
  statuses.reserve(impl_->slots.size());
  for (Impl::CameraSlot &slot : impl_->slots) {
    statuses.push_back(slot.worker == nullptr
                           ? DisconnectedStatus(slot.role, slot.serial, slot.startup_error)
                           : slot.worker->Status());
  }
  return statuses;
}

std::optional<PreviewPng> PreviewStation::LatestPreview(CameraRole role, bool full_resolution) {
  Impl::CameraSlot &slot = impl_->Slot(role);
  return slot.worker == nullptr ? std::nullopt : slot.worker->LatestPreview(full_resolution);
}

CameraStatus PreviewStation::UpdateCameraSettings(CameraRole role,
                                                  const CameraSettingsUpdate &settings) {
  Impl::CameraSlot &slot = impl_->Slot(role);
  if (slot.worker == nullptr) {
    throw std::logic_error("camera is disconnected");
  }
  return slot.worker->UpdateSettings(settings);
}

}  // namespace swing_capture::service
