#include "capture/clip/clip_window_planner.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "capture/core/device_clock_mapper.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

CameraClipPlan Failure(ClipPlanFailure failure) {
  return CameraClipPlan{
      .failure = failure,
      .frame_range = std::nullopt,
      .strike_frame = std::nullopt,
      .coverage = std::nullopt,
  };
}

std::size_t NearestFrameIndex(std::span<const TimePoint> mapped_times, TimePoint strike_time) {
  const auto after = std::ranges::lower_bound(mapped_times, strike_time);
  if (after == mapped_times.begin()) {
    return 0;
  }
  if (after == mapped_times.end()) {
    return mapped_times.size() - 1;
  }

  const std::size_t after_index = static_cast<std::size_t>(after - mapped_times.begin());
  const std::size_t before_index = after_index - 1;
  const auto before_error = strike_time - mapped_times[before_index];
  const auto after_error = mapped_times[after_index] - strike_time;
  return before_error <= after_error ? before_index : after_index;
}

}  // namespace

std::string_view ClipPlanFailureMessage(ClipPlanFailure failure) {
  switch (failure) {
    case ClipPlanFailure::kNone:
      return "none";
    case ClipPlanFailure::kInvalidWindow:
      return "pre-roll and post-roll must be nonnegative";
    case ClipPlanFailure::kMapperNotReady:
      return "device-to-host clock mapper is not ready";
    case ClipPlanFailure::kNoFrames:
      return "frame timing input is empty";
    case ClipPlanFailure::kNonMonotonicFrameId:
      return "frame IDs must be strictly increasing";
    case ClipPlanFailure::kFrameIdGap:
      return "frame timing input contains a frame ID gap";
    case ClipPlanFailure::kNonMonotonicDeviceTimestamp:
      return "device timestamps must be strictly increasing";
    case ClipPlanFailure::kTimestampNotMappable:
      return "a device timestamp cannot be mapped to host time";
    case ClipPlanFailure::kNonMonotonicMappedTime:
      return "mapped host frame times must be strictly increasing";
    case ClipPlanFailure::kInsufficientPreRollRetention:
      return "retention does not cover the requested pre-roll";
    case ClipPlanFailure::kInsufficientPostRollRetention:
      return "retention does not cover the requested post-roll";
    case ClipPlanFailure::kInsufficientPreAndPostRollRetention:
      return "retention does not cover the requested pre-roll or post-roll";
  }
  return "unknown clip-plan failure";
}

CameraClipPlan PlanCameraClipWindow(std::span<const FrameTiming> frames,
                                    const DeviceClockMapper &mapper, Clock::time_point strike_time,
                                    Clock::duration pre_roll, Clock::duration post_roll) {
  if (pre_roll < Clock::duration::zero() || post_roll < Clock::duration::zero()) {
    return Failure(ClipPlanFailure::kInvalidWindow);
  }
  if (!mapper.ready()) {
    return Failure(ClipPlanFailure::kMapperNotReady);
  }
  if (frames.empty()) {
    return Failure(ClipPlanFailure::kNoFrames);
  }

  for (std::size_t index = 1; index < frames.size(); ++index) {
    const FrameTiming &previous = frames[index - 1];
    const FrameTiming &current = frames[index];
    if (current.frame_id <= previous.frame_id) {
      return Failure(ClipPlanFailure::kNonMonotonicFrameId);
    }
    if (current.frame_id - previous.frame_id != 1) {
      return Failure(ClipPlanFailure::kFrameIdGap);
    }
    if (current.device_timestamp <= previous.device_timestamp) {
      return Failure(ClipPlanFailure::kNonMonotonicDeviceTimestamp);
    }
  }

  std::vector<TimePoint> mapped_times;
  mapped_times.reserve(frames.size());
  for (const FrameTiming &frame : frames) {
    const std::optional<TimePoint> mapped = mapper.EstimateHostTime(frame.device_timestamp);
    if (!mapped.has_value()) {
      return Failure(ClipPlanFailure::kTimestampNotMappable);
    }
    if (!mapped_times.empty() && *mapped <= mapped_times.back()) {
      return Failure(ClipPlanFailure::kNonMonotonicMappedTime);
    }
    mapped_times.push_back(*mapped);
  }

  const TimePoint requested_start = strike_time - pre_roll;
  const TimePoint requested_end = strike_time + post_roll;
  const bool missing_pre_roll = mapped_times.front() > requested_start;
  const bool missing_post_roll = mapped_times.back() < requested_end;
  if (missing_pre_roll && missing_post_roll) {
    return Failure(ClipPlanFailure::kInsufficientPreAndPostRollRetention);
  }
  if (missing_pre_roll) {
    return Failure(ClipPlanFailure::kInsufficientPreRollRetention);
  }
  if (missing_post_roll) {
    return Failure(ClipPlanFailure::kInsufficientPostRollRetention);
  }

  // Include the last frame at or before the requested start.
  const auto after_start = std::ranges::upper_bound(mapped_times, requested_start);
  const std::size_t begin_index = static_cast<std::size_t>(after_start - mapped_times.begin()) - 1;

  // Include the first frame at or after the requested end.
  const auto at_or_after_end = std::ranges::lower_bound(mapped_times, requested_end);
  const std::size_t last_index = static_cast<std::size_t>(at_or_after_end - mapped_times.begin());

  const std::size_t nearest_index = NearestFrameIndex(mapped_times, strike_time);

  CameraClipPlan plan;
  plan.frame_range = FrameIndexRange{
      .begin_index = begin_index,
      .end_index_exclusive = last_index + 1,
  };
  plan.strike_frame = StrikeFrameMatch{
      .frame_index = nearest_index,
      .mapped_frame_time = mapped_times[nearest_index],
      .time_error = mapped_times[nearest_index] - strike_time,
  };
  plan.coverage = ClipCoverage{
      .requested_start_time = requested_start,
      .requested_end_time = requested_end,
      .selected_start_time = mapped_times[begin_index],
      .selected_end_time = mapped_times[last_index],
      .actual_pre_roll = strike_time - mapped_times[begin_index],
      .actual_post_roll = mapped_times[last_index] - strike_time,
  };
  return plan;
}

DualViewClipPlan PlanDualViewClipWindow(const ImpactEvent &impact,
                                        std::span<const FrameTiming> first_view_frames,
                                        const DeviceClockMapper &first_view_mapper,
                                        std::span<const FrameTiming> second_view_frames,
                                        const DeviceClockMapper &second_view_mapper,
                                        Clock::duration pre_roll, Clock::duration post_roll) {
  DualViewClipPlan plan{
      .first_view = PlanCameraClipWindow(first_view_frames, first_view_mapper, impact.strike_time,
                                         pre_roll, post_roll),
      .second_view = PlanCameraClipWindow(second_view_frames, second_view_mapper,
                                          impact.strike_time, pre_roll, post_roll),
      .mapped_nearest_frame_skew = std::nullopt,
  };
  if (plan.ok() && plan.first_view.strike_frame.has_value() &&
      plan.second_view.strike_frame.has_value()) {
    plan.mapped_nearest_frame_skew = plan.second_view.strike_frame->mapped_frame_time -
                                     plan.first_view.strike_frame->mapped_frame_time;
  }
  return plan;
}

}  // namespace swing_capture
