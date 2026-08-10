#ifndef SWING_CAPTURE_CAPTURE_SERVICE_SYNTHETIC_SWING_HIL_TIMELINE_H_
#define SWING_CAPTURE_CAPTURE_SERVICE_SYNTHETIC_SWING_HIL_TIMELINE_H_

#include <chrono>
#include <cstdint>
#include <vector>

#include "capture/hil/feather_hil_controller.h"
#include "capture/optical/rgb_swing.h"

namespace swing_capture::service {

// Maps one Feather device timestamp into the host steady-clock domain using
// the midpoint of the host send/ACK bracket around the accepted timestamp.
// This preserves the bracket uncertainty; it is not an absolute USB latency
// calibration.
[[nodiscard]] std::chrono::steady_clock::time_point EstimateFeatherHostTime(
    std::uint64_t accepted_device_microseconds,
    std::chrono::steady_clock::time_point host_command_sent,
    std::chrono::steady_clock::time_point host_acknowledgement_received,
    std::uint64_t target_device_microseconds);

// Builds the exact optical schedules advertised and validated by the Feather
// controller. baseline_start must precede the first scheduled stimulus and
// lets pre-command camera samples establish the local OFF model.
[[nodiscard]] std::vector<optical::FeatherRgbStep> BuildCalibrationRgbSchedule(
    const hil::FeatherCalibrationReceipt &receipt,
    std::chrono::steady_clock::time_point baseline_start);

[[nodiscard]] std::vector<optical::FeatherRgbStep> BuildSyntheticSwingRgbSchedule(
    const hil::FeatherSwingReceipt &receipt, std::chrono::steady_clock::time_point baseline_start);

}  // namespace swing_capture::service

#endif  // SWING_CAPTURE_CAPTURE_SERVICE_SYNTHETIC_SWING_HIL_TIMELINE_H_
