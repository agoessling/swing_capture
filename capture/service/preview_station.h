#ifndef SWING_CAPTURE_CAPTURE_SERVICE_PREVIEW_STATION_H_
#define SWING_CAPTURE_CAPTURE_SERVICE_PREVIEW_STATION_H_

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "capture/service/preview_api.h"
#include "station/station_config.h"

namespace swing_capture::service {

// Owns the Galaxy SDK and both role-assigned camera workers. Missing or failed
// cameras remain represented as disconnected status entries so the setup UI
// can still identify the affected station role.
class PreviewStation final : public StationBackend {
 public:
  PreviewStation(const station::StationConfig &config, std::filesystem::path session_output_root,
                 bool enable_hil_controls = false);
  ~PreviewStation() override;

  PreviewStation(const PreviewStation &) = delete;
  PreviewStation &operator=(const PreviewStation &) = delete;
  PreviewStation(PreviewStation &&) = delete;
  PreviewStation &operator=(PreviewStation &&) = delete;

  [[nodiscard]] std::vector<CameraStatus> CameraStatuses() override;
  [[nodiscard]] std::optional<PreviewImage> LatestPreview(CameraRole role,
                                                          bool full_resolution) override;
  [[nodiscard]] CameraStatus UpdateCameraSettings(CameraRole role,
                                                  const CameraSettingsUpdate &settings) override;
  [[nodiscard]] CaptureApplicationStatus CaptureStatus() override;
  [[nodiscard]] CaptureApplicationStatus SetCaptureArmed(bool armed) override;
  [[nodiscard]] SessionSummaryStatus CaptureManually() override;
  [[nodiscard]] CaptureApplicationStatus RunSyntheticSwingHil() override;
  [[nodiscard]] std::vector<SessionSummaryStatus> Sessions() override;
  [[nodiscard]] std::optional<SessionAsset> SessionManifest(std::string_view session_id) override;
  [[nodiscard]] std::optional<SessionAsset> SessionMedia(std::string_view session_id,
                                                         CameraRole role) override;
  [[nodiscard]] std::optional<SessionImpactPreview> ImpactPreview(std::string_view session_id,
                                                                  CameraRole role) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace swing_capture::service

#endif  // SWING_CAPTURE_CAPTURE_SERVICE_PREVIEW_STATION_H_
