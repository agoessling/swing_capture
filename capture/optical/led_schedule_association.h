#ifndef SWING_CAPTURE_CAPTURE_OPTICAL_LED_SCHEDULE_ASSOCIATION_H_
#define SWING_CAPTURE_CAPTURE_OPTICAL_LED_SCHEDULE_ASSOCIATION_H_

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "capture/optical/frame_selection.h"
#include "capture/optical/led_pulse.h"

namespace swing_capture::optical {

struct LedScheduleAssociationOptions {
  std::chrono::steady_clock::duration commanded_duration;
  std::chrono::steady_clock::duration exposure_duration;

  // This is a caller-supplied conservative host-time uncertainty for the
  // camera clock mapping. A caller using the current least-squares mapper may,
  // for example, derive it from a chosen multiple of the reported residual
  // standard deviation.
  std::chrono::duration<double> mapping_uncertainty;
  std::chrono::duration<double> maximum_mapping_uncertainty;

  // A frame timestamp is generated in the camera, while the mapper observes
  // the frame only after readout and USB delivery. When no hardware clock
  // latch is available, callers must state and justify a bound for that
  // one-way timestamp-to-receipt bias instead of hiding it in mapper noise.
  std::chrono::steady_clock::duration frame_delivery_latency_bound;

  std::chrono::steady_clock::duration maximum_schedule_bracket_width;
};

struct LedScheduleAssociationCheck {
  std::string name;
  bool passed = false;
  std::string message;
};

struct LedScheduleAssociationResult {
  bool passed = false;
  std::vector<LedScheduleAssociationCheck> checks;

  std::chrono::steady_clock::duration schedule_bracket_width{};
  std::chrono::steady_clock::duration nominal_frame_interval{};
  std::chrono::steady_clock::duration mapping_uncertainty{};
  std::chrono::steady_clock::duration frame_delivery_latency_bound{};
  std::chrono::steady_clock::duration edge_tolerance{};

  std::chrono::steady_clock::time_point observed_start;
  std::chrono::steady_clock::time_point observed_end_exclusive;
  std::chrono::steady_clock::time_point allowed_start_earliest;
  std::chrono::steady_clock::time_point allowed_start_latest;
  std::chrono::steady_clock::time_point allowed_end_earliest;
  std::chrono::steady_clock::time_point allowed_end_latest;
};

// Associates an already detected LED run with its Feather command. The
// selected timeline must contain exactly the bounded 48-frame analysis slice.
//
// A detected edge is quantized by the free-running camera cadence and can be
// displaced by exposure overlap and camera-to-host mapping uncertainty. Each
// observed edge therefore receives exactly one mapped frame interval, the
// configured exposure duration, caller-supplied mapping uncertainty, and an
// explicit frame-delivery latency bound. The schedule bracket and mapping
// uncertainty are also independently gated so a very broad interval cannot
// manufacture a match.
//
// Malformed inputs throw std::invalid_argument. Valid but insufficient evidence
// returns passed=false with stable, explicit checks.
[[nodiscard]] LedScheduleAssociationResult EvaluateLedScheduleAssociation(
    std::span<const std::chrono::steady_clock::time_point> selected_frame_host_times,
    const LedPulseResult &pulse, const EstimatedStimulusHostSchedule &schedule,
    const LedScheduleAssociationOptions &options);

// Returns the strongest active diagnostic inside AnalyzeLedPulse's accepted
// run. It never selects a baseline, inactive outlier, or a frame from a rejected
// competing run. Equal responses select the earlier frame deterministically.
// Throws std::invalid_argument if pulse is not a self-consistent detection.
[[nodiscard]] std::size_t SelectAcceptedLedPeakFrame(const LedPulseResult &pulse);

}  // namespace swing_capture::optical

#endif  // SWING_CAPTURE_CAPTURE_OPTICAL_LED_SCHEDULE_ASSOCIATION_H_
