#ifndef SWING_CAPTURE_CAPTURE_CLIP_CLIP_WINDOW_PLANNER_H_
#define SWING_CAPTURE_CAPTURE_CLIP_CLIP_WINDOW_PLANNER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "capture/core/device_clock_mapper.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture {

// Metadata-only input. The planner neither owns nor copies image payloads.
struct FrameTiming {
  std::uint64_t frame_id = 0;
  std::uint64_t device_timestamp = 0;
};

enum class ClipPlanFailure {
  kNone,
  kInvalidWindow,
  kMapperNotReady,
  kNoFrames,
  kNonMonotonicFrameId,
  kFrameIdGap,
  kNonMonotonicDeviceTimestamp,
  kTimestampNotMappable,
  kNonMonotonicMappedTime,
  kInsufficientPreRollRetention,
  kInsufficientPostRollRetention,
  kInsufficientPreAndPostRollRetention,
};

[[nodiscard]] std::string_view ClipPlanFailureMessage(ClipPlanFailure failure);

struct FrameIndexRange {
  // The selected input-span range is [begin_index, end_index_exclusive).
  std::size_t begin_index = 0;
  std::size_t end_index_exclusive = 0;
};

struct StrikeFrameMatch {
  std::size_t frame_index = 0;
  std::chrono::steady_clock::time_point mapped_frame_time;

  // Signed mapped_frame_time - strike_time. A negative value means the
  // selected frame precedes the audio impact. Equidistant ties choose the
  // earlier frame.
  std::chrono::steady_clock::duration time_error{};
};

struct ClipCoverage {
  std::chrono::steady_clock::time_point requested_start_time;
  std::chrono::steady_clock::time_point requested_end_time;
  std::chrono::steady_clock::time_point selected_start_time;
  std::chrono::steady_clock::time_point selected_end_time;

  // Quantized coverage from the selected boundary frames to the strike.
  // Boundary frames bracket the requested window, so these are at least the
  // requested durations when planning succeeds.
  std::chrono::steady_clock::duration actual_pre_roll{};
  std::chrono::steady_clock::duration actual_post_roll{};
};

struct CameraClipPlan {
  ClipPlanFailure failure = ClipPlanFailure::kNone;
  std::optional<FrameIndexRange> frame_range;
  std::optional<StrikeFrameMatch> strike_frame;
  std::optional<ClipCoverage> coverage;

  [[nodiscard]] bool ok() const { return failure == ClipPlanFailure::kNone; }
};

// Selects boundary frames that bracket [strike_time - pre_roll,
// strike_time + post_roll]. The input must be chronological, have consecutive
// frame IDs, and be wholly representable by mapper.
[[nodiscard]] CameraClipPlan PlanCameraClipWindow(std::span<const FrameTiming> frames,
                                                  const DeviceClockMapper &mapper,
                                                  std::chrono::steady_clock::time_point strike_time,
                                                  std::chrono::steady_clock::duration pre_roll,
                                                  std::chrono::steady_clock::duration post_roll);

struct DualViewClipPlan {
  CameraClipPlan first_view;
  CameraClipPlan second_view;

  // Signed second-view nearest-frame time minus first-view nearest-frame
  // time, after mapping both device clocks to the host monotonic clock.
  //
  // This is a clock-correlation and frame-selection diagnostic only. It is
  // not proof that the two camera exposures were synchronized.
  std::optional<std::chrono::steady_clock::duration> mapped_nearest_frame_skew;

  [[nodiscard]] bool ok() const { return first_view.ok() && second_view.ok(); }
};

[[nodiscard]] DualViewClipPlan PlanDualViewClipWindow(
    const ImpactEvent &impact, std::span<const FrameTiming> first_view_frames,
    const DeviceClockMapper &first_view_mapper, std::span<const FrameTiming> second_view_frames,
    const DeviceClockMapper &second_view_mapper, std::chrono::steady_clock::duration pre_roll,
    std::chrono::steady_clock::duration post_roll);

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_CLIP_CLIP_WINDOW_PLANNER_H_
