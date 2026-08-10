#include "capture/service/synthetic_swing_hil_timeline.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "capture/hil/feather_hil_controller.h"
#include "capture/optical/rgb_swing.h"
#include "embedded/prop_maker/swing_sequence.h"

namespace swing_capture::service {
namespace {

optical::Rgb8 NormalizedRgb(swing_hil_rgb8 value) {
  const std::uint32_t maximum = std::max({value.red, value.green, value.blue});
  if (maximum == 0U) {
    return {};
  }
  const auto normalized = [maximum](std::uint8_t component) {
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(component) * 255U + maximum / 2U) /
                                     maximum);
  };
  return {.red = normalized(value.red),
          .green = normalized(value.green),
          .blue = normalized(value.blue)};
}

std::chrono::steady_clock::time_point CalibrationHostTime(
    const hil::FeatherCalibrationReceipt &receipt, std::uint64_t device_microseconds) {
  return EstimateFeatherHostTime(receipt.accepted_device_microseconds, receipt.host_command_sent,
                                 receipt.host_acknowledgement_received, device_microseconds);
}

std::chrono::steady_clock::time_point SwingHostTime(const hil::FeatherSwingReceipt &receipt,
                                                    std::uint64_t device_microseconds) {
  return EstimateFeatherHostTime(receipt.accepted_device_microseconds, receipt.host_command_sent,
                                 receipt.host_acknowledgement_received, device_microseconds);
}

void RequireBaselineBefore(std::chrono::steady_clock::time_point baseline_start,
                           std::chrono::steady_clock::time_point stimulus_start) {
  if (baseline_start >= stimulus_start) {
    throw std::invalid_argument("RGB HIL baseline must precede the stimulus schedule");
  }
}

}  // namespace

std::chrono::steady_clock::time_point EstimateFeatherHostTime(
    std::uint64_t accepted_device_microseconds,
    std::chrono::steady_clock::time_point host_command_sent,
    std::chrono::steady_clock::time_point host_acknowledgement_received,
    std::uint64_t target_device_microseconds) {
  if (host_acknowledgement_received < host_command_sent ||
      target_device_microseconds < accepted_device_microseconds) {
    throw std::invalid_argument("invalid Feather device/host timing bracket");
  }
  const std::uint64_t delta = target_device_microseconds - accepted_device_microseconds;
  if (delta > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::overflow_error("Feather device time delta does not fit host duration");
  }
  const auto host_accepted =
      host_command_sent + (host_acknowledgement_received - host_command_sent) / 2;
  return host_accepted + std::chrono::microseconds(static_cast<std::int64_t>(delta));
}

std::vector<optical::FeatherRgbStep> BuildCalibrationRgbSchedule(
    const hil::FeatherCalibrationReceipt &receipt,
    std::chrono::steady_clock::time_point baseline_start) {
  if (receipt.steps.empty() || receipt.step_microseconds == 0U ||
      receipt.steps.size() != receipt.candidates.size()) {
    throw std::invalid_argument("incomplete Feather calibration receipt");
  }
  const auto first_start =
      CalibrationHostTime(receipt, receipt.steps.front().scheduled_device_microseconds);
  RequireBaselineBefore(baseline_start, first_start);

  std::vector<optical::FeatherRgbStep> schedule;
  schedule.reserve(receipt.steps.size() + 1U);
  schedule.push_back({.start = baseline_start,
                      .end_exclusive = first_start,
                      .color = {},
                      .brightness = 0,
                      .phase = optical::RgbSwingPhase::kBaseline});
  for (const hil::FeatherCalibrationStep &step : receipt.steps) {
    const auto start = CalibrationHostTime(receipt, step.scheduled_device_microseconds);
    const auto end = start + std::chrono::microseconds(receipt.step_microseconds);
    const auto brightness = static_cast<std::uint8_t>(step.brightness);
    schedule.push_back(
        {.start = start,
         .end_exclusive = end,
         .color = NormalizedRgb({.red = brightness, .green = brightness, .blue = brightness}),
         .brightness = brightness,
         .phase = optical::RgbSwingPhase::kBrightnessProbe});
  }
  return schedule;
}

std::vector<optical::FeatherRgbStep> BuildSyntheticSwingRgbSchedule(
    const hil::FeatherSwingReceipt &receipt, std::chrono::steady_clock::time_point baseline_start) {
  if (receipt.brightness == 0U) {
    throw std::invalid_argument("synthetic swing receipt has zero brightness");
  }
  const auto sequence_start =
      SwingHostTime(receipt, receipt.sequence_start_scheduled_device_microseconds);
  RequireBaselineBefore(baseline_start, sequence_start);

  std::vector<optical::FeatherRgbStep> schedule;
  schedule.reserve(static_cast<std::size_t>(SWING_HIL_SWING_PRE_STEPS) +
                   static_cast<std::size_t>(SWING_HIL_SWING_POST_STEPS) + 2U);
  schedule.push_back({.start = baseline_start,
                      .end_exclusive = sequence_start,
                      .color = {},
                      .brightness = 0,
                      .phase = optical::RgbSwingPhase::kBaseline});
  const auto brightness = static_cast<std::uint8_t>(receipt.brightness);
  for (std::uint32_t step = 0; step < SWING_HIL_SWING_PRE_STEPS; ++step) {
    const auto start = sequence_start + std::chrono::microseconds(static_cast<std::uint64_t>(step) *
                                                                  SWING_HIL_SWING_STEP_US);
    schedule.push_back({.start = start,
                        .end_exclusive = start + std::chrono::microseconds(SWING_HIL_SWING_STEP_US),
                        .color = NormalizedRgb(swing_hil_swing_pre_color(step, brightness)),
                        .brightness = brightness,
                        .phase = optical::RgbSwingPhase::kPreImpact});
  }
  const auto impact_start = SwingHostTime(receipt, receipt.impact_scheduled_device_microseconds);
  schedule.push_back(
      {.start = impact_start,
       .end_exclusive = impact_start + std::chrono::microseconds(SWING_HIL_SWING_IMPACT_WHITE_US),
       .color = NormalizedRgb(swing_hil_swing_impact_color(brightness)),
       .brightness = brightness,
       .phase = optical::RgbSwingPhase::kImpact});
  const auto post_start = SwingHostTime(receipt, receipt.post_scheduled_device_microseconds);
  for (std::uint32_t step = 0; step < SWING_HIL_SWING_POST_STEPS; ++step) {
    const auto start = post_start + std::chrono::microseconds(static_cast<std::uint64_t>(step) *
                                                              SWING_HIL_SWING_STEP_US);
    schedule.push_back({.start = start,
                        .end_exclusive = start + std::chrono::microseconds(SWING_HIL_SWING_STEP_US),
                        .color = NormalizedRgb(swing_hil_swing_post_color(step, brightness)),
                        .brightness = brightness,
                        .phase = optical::RgbSwingPhase::kPostImpact});
  }
  return schedule;
}

}  // namespace swing_capture::service
