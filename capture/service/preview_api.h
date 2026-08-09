#ifndef SWING_CAPTURE_CAPTURE_SERVICE_PREVIEW_API_H_
#define SWING_CAPTURE_CAPTURE_SERVICE_PREVIEW_API_H_

#include <httplib.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::service {

enum class CameraRole {
  kDownTheLine,
  kFaceOn,
};

struct NumericSettingStatus {
  double value = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
  double increment = 1.0;
};

struct PreviewQualityStatus {
  std::string assessment = "unavailable";
  double mean = 0.0;
  double p99 = 0.0;
  double gradient_energy = 0.0;
};

struct CameraStatus {
  CameraRole role = CameraRole::kDownTheLine;
  std::string serial;
  std::string model;
  bool connected = false;
  std::string error;
  double stream_fps = 0.0;
  std::uint64_t preview_sequence = 0;
  std::uint32_t preview_width = 0;
  std::uint32_t preview_height = 0;
  NumericSettingStatus exposure_microseconds;
  NumericSettingStatus gain_decibels;
  PreviewQualityStatus image_quality;
};

struct CameraSettingsUpdate {
  double exposure_microseconds = 0.0;
  double gain_decibels = 0.0;
};

struct PreviewPng {
  std::uint64_t sequence = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::string bytes;
};

class StationBackend {
 public:
  StationBackend() = default;
  virtual ~StationBackend() = default;

  StationBackend(const StationBackend &) = delete;
  StationBackend &operator=(const StationBackend &) = delete;
  StationBackend(StationBackend &&) = delete;
  StationBackend &operator=(StationBackend &&) = delete;

  [[nodiscard]] virtual std::vector<CameraStatus> CameraStatuses() = 0;
  [[nodiscard]] virtual std::optional<PreviewPng> LatestPreview(CameraRole role,
                                                                bool full_resolution) = 0;
  [[nodiscard]] virtual CameraStatus UpdateCameraSettings(CameraRole role,
                                                          const CameraSettingsUpdate &settings) = 0;
};

[[nodiscard]] std::string_view CameraRoleName(CameraRole role) noexcept;
[[nodiscard]] std::optional<CameraRole> ParseCameraRole(std::string_view value) noexcept;

// Registers the versioned station API. If static_root is present, it must be
// an existing directory and is mounted at / after the API routes.
void RegisterPreviewRoutes(httplib::Server &server, StationBackend &backend,
                           const std::optional<std::filesystem::path> &static_root = std::nullopt);

}  // namespace swing_capture::service

#endif  // SWING_CAPTURE_CAPTURE_SERVICE_PREVIEW_API_H_
