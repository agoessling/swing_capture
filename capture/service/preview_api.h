#ifndef SWING_CAPTURE_CAPTURE_SERVICE_PREVIEW_API_H_
#define SWING_CAPTURE_CAPTURE_SERVICE_PREVIEW_API_H_

#include <httplib.h>

#include <cstdint>
#include <filesystem>
#include <memory>
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

struct PreviewPerformanceStatus {
  std::string media_type;
  std::uint64_t encoded_bytes = 0;
  double source_age_milliseconds = 0.0;
  double rendered_age_milliseconds = 0.0;
  double quality_analysis_milliseconds = 0.0;
  double bayer_transform_milliseconds = 0.0;
  double resize_milliseconds = 0.0;
  double encode_milliseconds = 0.0;
  double total_milliseconds = 0.0;
  // These request-time snapshots partition an old visible preview into an
  // acquisition stall, a latest-frame sampling stall, or renderer backlog.
  // Sequence zero means that the corresponding stage has not published yet.
  std::uint64_t latest_capture_frame_id = 0;
  double latest_capture_age_milliseconds = 0.0;
  std::uint64_t latest_sink_frame_id = 0;
  double latest_sink_completion_age_milliseconds = 0.0;
  std::uint64_t latest_sampler_frame_id = 0;
  double latest_sampler_completion_age_milliseconds = 0.0;
  std::uint64_t sampled_sequence = 0;
  double sampled_age_milliseconds = 0.0;
  double render_queue_milliseconds = 0.0;
  std::string renderer_stage = "idle";
  bool render_pending = false;
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
  PreviewPerformanceStatus preview_performance;
};

struct CameraSettingsUpdate {
  double exposure_microseconds = 0.0;
  double gain_decibels = 0.0;
};

struct PreviewImage {
  std::uint64_t sequence = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::string media_type;
  std::string bytes;
  PreviewPerformanceStatus performance;
};

struct CaptureTriggerStatus {
  std::string source;
  std::int64_t strike_host_monotonic_nanoseconds = 0;
  std::int64_t confirmation_host_monotonic_nanoseconds = 0;
  std::uint32_t sample_rate_hz = 0;
  double peak_amplitude = 0.0;
  double noise_floor = 0.0;
  double threshold = 0.0;
};

struct HilLastRunStatus {
  std::optional<std::string> session_id;
  std::string stage = "idle";
  std::string error;
  std::optional<std::uint8_t> selected_brightness;
};

struct HilControlStatus {
  bool enabled = false;
  bool busy = false;
  std::string stage = "idle";
  std::string error;
  std::optional<std::uint8_t> selected_brightness;
  std::optional<HilLastRunStatus> last_run;
};

struct CaptureApplicationStatus {
  std::string state = "setup";
  bool armed = false;
  std::optional<std::string> active_session_id;
  std::optional<CaptureTriggerStatus> last_trigger;
  std::string error;
  bool audio_running = false;
  bool audio_ready = false;
  std::uint64_t audio_blocks = 0;
  std::uint64_t audio_samples = 0;
  std::uint64_t detected_impacts = 0;
  double audio_noise_floor = 0.0;
  double audio_detection_threshold = 0.0;
  HilControlStatus hil;
};

struct SessionSummaryStatus {
  std::string session_id;
  std::string state = "ready";
  std::string created_at_utc;
  std::string error;
};

struct SessionAsset {
  std::filesystem::path path;
  std::string media_type;
};

struct SessionImpactPreview {
  std::uint64_t frame_id = 0;
  std::int64_t time_from_impact_microseconds = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::string media_type;
  std::shared_ptr<const std::string> bytes;
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
  [[nodiscard]] virtual std::optional<PreviewImage> LatestPreview(CameraRole role,
                                                                  bool full_resolution) = 0;
  [[nodiscard]] virtual CameraStatus UpdateCameraSettings(CameraRole role,
                                                          const CameraSettingsUpdate &settings) = 0;
  [[nodiscard]] virtual CaptureApplicationStatus CaptureStatus() = 0;
  [[nodiscard]] virtual CaptureApplicationStatus SetCaptureArmed(bool armed) = 0;
  [[nodiscard]] virtual SessionSummaryStatus CaptureManually() = 0;
  [[nodiscard]] virtual CaptureApplicationStatus RunSyntheticSwingHil() = 0;
  [[nodiscard]] virtual std::vector<SessionSummaryStatus> Sessions() = 0;
  [[nodiscard]] virtual std::optional<SessionAsset> SessionManifest(
      std::string_view session_id) = 0;
  [[nodiscard]] virtual std::optional<SessionAsset> SessionMedia(std::string_view session_id,
                                                                 CameraRole role) = 0;
  [[nodiscard]] virtual std::optional<SessionImpactPreview> ImpactPreview(
      std::string_view session_id, CameraRole role) = 0;
};

[[nodiscard]] std::string_view CameraRoleName(CameraRole role) noexcept;
[[nodiscard]] std::optional<CameraRole> ParseCameraRole(std::string_view value) noexcept;

// Registers the versioned station API. If static_root is present, it must be
// an existing directory and is mounted at / after the API routes.
void RegisterPreviewRoutes(httplib::Server &server, StationBackend &backend,
                           const std::optional<std::filesystem::path> &static_root = std::nullopt);

}  // namespace swing_capture::service

#endif  // SWING_CAPTURE_CAPTURE_SERVICE_PREVIEW_API_H_
