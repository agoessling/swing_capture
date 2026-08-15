#ifndef SWING_CAPTURE_CAPTURE_SERVICE_SYNTHETIC_SWING_STATION_WORKFLOW_H_
#define SWING_CAPTURE_CAPTURE_SERVICE_SYNTHETIC_SWING_STATION_WORKFLOW_H_

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

#include "capture/application/capture_controller.h"
#include "capture/application/clip_session_publisher.h"
#include "capture/hil/feather_hil_controller.h"
#include "capture/optical/rgb_swing.h"
#include "capture/preview/latest_frame_sampler.h"

namespace swing_capture::service {

struct SyntheticSwingCameraAnalysisProfile {
  std::chrono::steady_clock::duration exposure_duration{};
  double gain_decibels = 0.0;
};

using SyntheticSwingCameraAnalysisProfiles = std::array<SyntheticSwingCameraAnalysisProfile, 2>;

struct SyntheticSwingCameraCalibrationSource {
  std::string role;
  std::uint64_t device_ticks_per_second = 0;
  std::function<std::shared_ptr<const preview::SampledPreviewFrame>()> latest_frame;
  // Exact current CameraWorker readback. The workflow never changes camera
  // settings; this profile is retained with the optical evidence.
  std::function<SyntheticSwingCameraAnalysisProfile()> analysis_profile;
  std::function<std::chrono::steady_clock::duration()> sampling_interval;
  // Must accept any positive interval and must restore a previously reported
  // interval without throwing.
  std::function<void(std::chrono::steady_clock::duration)> set_sampling_interval;
};

struct SyntheticSwingStationWorkflowHooks {
  std::function<hil::FeatherCalibrationReceipt()> calibrate_feather;
  std::function<hil::FeatherSwingReceipt(std::uint32_t)> run_feather_swing;
};

namespace synthetic_swing_workflow_internal {

[[nodiscard]] std::string FormatBrightnessRecommendationFailure(
    std::span<const optical::RgbBrightnessCalibration> calibrations,
    const optical::SharedBrightnessRecommendation &recommendation);

// Retained camera mappings are fitted to host receipt timestamps. Receipt can
// lag an optical exposure, but it cannot causally precede it, so an optical
// calibration result that asks us to move a received frame later is a PWM-fit
// artifact rather than a physical transport correction.
[[nodiscard]] std::chrono::steady_clock::duration ConstrainReceiptTimelineCorrection(
    std::chrono::steady_clock::duration estimated_correction) noexcept;

}  // namespace synthetic_swing_workflow_internal

// Bridges the typed Feather schedule, explicitly capped preview calibration samples,
// full retained camera windows, and portable session evidence. The owning
// station serializes calls through SyntheticSwingHilOperation; the publisher
// may concurrently wait for the Feather's final typed SWING receipt.
class SyntheticSwingStationWorkflow final {
 public:
  SyntheticSwingStationWorkflow(std::array<SyntheticSwingCameraCalibrationSource, 2> cameras,
                                SyntheticSwingStationWorkflowHooks hooks);
  ~SyntheticSwingStationWorkflow() = default;

  SyntheticSwingStationWorkflow(const SyntheticSwingStationWorkflow &) = delete;
  SyntheticSwingStationWorkflow &operator=(const SyntheticSwingStationWorkflow &) = delete;
  SyntheticSwingStationWorkflow(SyntheticSwingStationWorkflow &&) = delete;
  SyntheticSwingStationWorkflow &operator=(SyntheticSwingStationWorkflow &&) = delete;

  [[nodiscard]] std::uint8_t CalibrateBrightness(const std::stop_token &stop_token);
  void RunStimulus(std::uint8_t brightness, const std::stop_token &stop_token);
  [[nodiscard]] std::optional<application::SyntheticSwingSessionEvidence> AnalyzeCapturedSession(
      const application::CapturedSession &captured);
  void ValidatePublishedSession(std::string_view session_id) const;
  void Reset() noexcept;

 private:
  std::array<SyntheticSwingCameraCalibrationSource, 2> cameras_;
  SyntheticSwingStationWorkflowHooks hooks_;

  mutable std::mutex mutex_;
  std::condition_variable receipt_ready_;
  std::optional<std::uint8_t> selected_brightness_;
  std::array<std::optional<optical::RgbBrightnessCalibration>, 2> calibrations_;
  std::optional<SyntheticSwingCameraAnalysisProfiles> camera_profiles_;
  std::optional<hil::FeatherSwingReceipt> swing_receipt_;
  bool capture_pending_ = false;
  std::optional<std::string> analyzed_session_id_;
  bool analyzed_optical_passed_ = false;
  std::string analysis_diagnostic_;
};

}  // namespace swing_capture::service

#endif  // SWING_CAPTURE_CAPTURE_SERVICE_SYNTHETIC_SWING_STATION_WORKFLOW_H_
