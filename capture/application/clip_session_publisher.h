#ifndef SWING_CAPTURE_CAPTURE_APPLICATION_CLIP_SESSION_PUBLISHER_H_
#define SWING_CAPTURE_CAPTURE_APPLICATION_CLIP_SESSION_PUBLISHER_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "capture/application/capture_controller.h"
#include "capture/encoding/clip_session.h"

namespace swing_capture::application {

struct ImpactPreviewImage {
  std::string role;
  std::uint64_t frame_id = 0;
  std::int64_t time_from_impact_us = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::string media_type;
  std::string encoded_bytes;
};

using ImpactPreviewCallback =
    std::function<void(const SessionIdentity &, std::array<ImpactPreviewImage, 2>)>;

struct ClipSessionPublisherConfig {
  std::filesystem::path output_root;
  std::chrono::steady_clock::duration pre_roll = kDefaultCapturePreRoll;
  std::chrono::steady_clock::duration post_roll = kDefaultCapturePostRoll;
  // Optional transient delivery path. The exact impact-adjacent frames are
  // rendered before video encoding so a browser can show useful feedback
  // while the complete, frame-steppable review assets are still publishing.
  ImpactPreviewCallback impact_preview_ready;
};

struct SyntheticSwingCameraEvidence {
  std::string role;
  std::uint64_t optical_white_impact_frame_id = 0;
  std::int64_t mapped_time_correction_us = 0;
  std::uint64_t schedule_uncertainty_us = 0;
  bool optical_white_passed = false;
  std::size_t stable_frame_count = 0;
  std::size_t matching_frame_count = 0;
  double matching_fraction = 0.0;
  double mean_signal_delta = 0.0;
  double mean_expected_color_distance = 0.0;
  double maximum_saturated_fraction = 0.0;
  double maximum_bloom_fraction = 0.0;
  double exposure_us = 0.0;
  double gain_db = 0.0;
};

struct SyntheticSwingSessionEvidence {
  std::uint32_t selected_brightness = 0;
  std::uint32_t step_duration_us = 0;
  std::uint32_t pre_impact_step_count = 0;
  std::uint32_t white_impact_duration_us = 0;
  std::uint32_t post_impact_step_count = 0;
  std::uint32_t tone_duration_us = 0;
  std::uint32_t tone_frequency_hz = 0;
  std::array<SyntheticSwingCameraEvidence, 2> cameras;
};

// Converts retained dual-camera snapshots into a strike-relative clip plan and
// delegates browser media/session publication to the configured encoder. It is
// synchronous and is intended to be invoked by CaptureController's encoding
// worker, never a camera or audio thread.
class ClipSessionPublisher final {
 public:
  explicit ClipSessionPublisher(
      ClipSessionPublisherConfig config,
      std::unique_ptr<encoding::ClipMediaEncoder> encoder = encoding::MakeSoftwareVp8WebmEncoder());
  ~ClipSessionPublisher() = default;

  ClipSessionPublisher(const ClipSessionPublisher &) = delete;
  ClipSessionPublisher &operator=(const ClipSessionPublisher &) = delete;
  ClipSessionPublisher(ClipSessionPublisher &&) = delete;
  ClipSessionPublisher &operator=(ClipSessionPublisher &&) = delete;

  [[nodiscard]] PublishedSession Publish(
      CapturedSession captured,
      std::optional<SyntheticSwingSessionEvidence> hil_evidence = std::nullopt) const;

 private:
  ClipSessionPublisherConfig config_;
  std::unique_ptr<encoding::ClipMediaEncoder> encoder_;
};

}  // namespace swing_capture::application

#endif  // SWING_CAPTURE_CAPTURE_APPLICATION_CLIP_SESSION_PUBLISHER_H_
