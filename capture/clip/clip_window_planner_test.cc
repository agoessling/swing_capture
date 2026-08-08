#include "capture/clip/clip_window_planner.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using swing_capture::CameraClipPlan;
using swing_capture::ClipPlanFailure;
using swing_capture::DeviceClockMapper;
using swing_capture::FrameTiming;
using swing_capture::ImpactEvent;
using swing_capture::PlanCameraClipWindow;
using swing_capture::PlanDualViewClipWindow;

DeviceClockMapper IdentityMapper(std::uint64_t first_device_timestamp = 0,
                                 Clock::time_point first_host_time = {}) {
  DeviceClockMapper mapper(1000);
  mapper.AddSample(first_device_timestamp, first_host_time);
  mapper.AddSample(first_device_timestamp + 1000, first_host_time + 1s);
  return mapper;
}

DeviceClockMapper BinaryExactMapper() {
  DeviceClockMapper mapper(1024);
  mapper.AddSample(0, Clock::time_point{});
  mapper.AddSample(1024, Clock::time_point(1s));
  return mapper;
}

std::vector<FrameTiming> RegularFrames(std::uint64_t first_frame_id, std::uint64_t first_timestamp,
                                       std::uint64_t step, std::size_t count) {
  std::vector<FrameTiming> frames;
  frames.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    frames.push_back(FrameTiming{
        .frame_id = first_frame_id + index,
        .device_timestamp = first_timestamp + index * step,
    });
  }
  return frames;
}

void SelectsExactBoundariesAndExactStrike() {
  const DeviceClockMapper mapper = BinaryExactMapper();
  const std::vector<FrameTiming> frames = RegularFrames(100, 0, 128, 9);
  const auto strike = Clock::time_point(250ms);

  const CameraClipPlan plan = PlanCameraClipWindow(frames, mapper, strike, 125ms, 125ms);

  assert(plan.ok());
  assert(plan.frame_range.has_value());
  assert(plan.frame_range->begin_index == 1);
  assert(plan.frame_range->end_index_exclusive == 4);
  assert(plan.strike_frame->frame_index == 2);
  assert(plan.strike_frame->time_error == Clock::duration::zero());
  assert(plan.coverage->selected_start_time == Clock::time_point(125ms));
  assert(plan.coverage->selected_end_time == Clock::time_point(375ms));
  assert(plan.coverage->actual_pre_roll == 125ms);
  assert(plan.coverage->actual_post_roll == 125ms);
}

void BracketsInexactBoundariesAndChoosesEarlierNearestTie() {
  const DeviceClockMapper mapper = BinaryExactMapper();
  const std::vector<FrameTiming> frames = RegularFrames(10, 0, 128, 8);
  const auto strike = Clock::time_point(312500us);

  const CameraClipPlan plan = PlanCameraClipWindow(frames, mapper, strike, 100ms, 100ms);

  assert(plan.ok());
  assert(plan.frame_range->begin_index == 1);
  assert(plan.frame_range->end_index_exclusive == 5);
  assert(plan.strike_frame->frame_index == 2);
  assert(plan.strike_frame->time_error == -62500us);
  assert(plan.coverage->requested_start_time == Clock::time_point(212500us));
  assert(plan.coverage->requested_end_time == Clock::time_point(412500us));
  assert(plan.coverage->selected_start_time == Clock::time_point(125ms));
  assert(plan.coverage->selected_end_time == Clock::time_point(500ms));
  assert(plan.coverage->actual_pre_roll == 187500us);
  assert(plan.coverage->actual_post_roll == 187500us);
}

void MapsIndependentOffsetsAndDriftsForDualView() {
  constexpr std::uint64_t kTicksPerSecond = 1000000;
  constexpr std::uint64_t kFirstDeviceA = 10000000;
  constexpr std::uint64_t kFirstDeviceB = 80000000;
  const auto host_origin = Clock::time_point(100s);

  DeviceClockMapper mapper_a(kTicksPerSecond);
  mapper_a.AddSample(kFirstDeviceA, host_origin);
  mapper_a.AddSample(kFirstDeviceA + kTicksPerSecond, host_origin + 1000200us);
  mapper_a.AddSample(kFirstDeviceA + 2 * kTicksPerSecond, host_origin + 2000400us);

  DeviceClockMapper mapper_b(kTicksPerSecond);
  mapper_b.AddSample(kFirstDeviceB, host_origin + 3ms);
  mapper_b.AddSample(kFirstDeviceB + kTicksPerSecond, host_origin + 1002700us);
  mapper_b.AddSample(kFirstDeviceB + 2 * kTicksPerSecond, host_origin + 2002400us);

  assert(std::abs(mapper_a.clock_drift_parts_per_million() - 200.0) < 0.01);
  assert(std::abs(mapper_b.clock_drift_parts_per_million() + 300.0) < 0.01);

  const std::vector<FrameTiming> frames_a = RegularFrames(1000, kFirstDeviceA, 4400, 320);
  const std::vector<FrameTiming> frames_b = RegularFrames(9000, kFirstDeviceB, 4410, 320);
  ImpactEvent impact;
  impact.strike_time = host_origin + 700ms;

  const auto plan =
      PlanDualViewClipWindow(impact, frames_a, mapper_a, frames_b, mapper_b, 250ms, 250ms);

  assert(plan.ok());
  assert(plan.first_view.strike_frame.has_value());
  assert(plan.second_view.strike_frame.has_value());
  assert(plan.mapped_nearest_frame_skew.has_value());
  assert(*plan.mapped_nearest_frame_skew == plan.second_view.strike_frame->mapped_frame_time -
                                                plan.first_view.strike_frame->mapped_frame_time);
  assert(std::chrono::abs(plan.first_view.strike_frame->time_error) < 3ms);
  assert(std::chrono::abs(plan.second_view.strike_frame->time_error) < 3ms);
}

void ReportsInsufficientRetentionBySide() {
  const DeviceClockMapper mapper = IdentityMapper();
  const std::vector<FrameTiming> frames = RegularFrames(1, 0, 100, 11);
  const auto strike = Clock::time_point(500ms);

  assert(PlanCameraClipWindow(frames, mapper, strike, 600ms, 100ms).failure ==
         ClipPlanFailure::kInsufficientPreRollRetention);
  assert(PlanCameraClipWindow(frames, mapper, strike, 100ms, 600ms).failure ==
         ClipPlanFailure::kInsufficientPostRollRetention);
  assert(PlanCameraClipWindow(frames, mapper, strike, 600ms, 600ms).failure ==
         ClipPlanFailure::kInsufficientPreAndPostRollRetention);
}

void RejectsFrameIdGapsAndNonMonotonicInput() {
  const DeviceClockMapper mapper = IdentityMapper();
  const auto strike = Clock::time_point(100ms);

  const std::array<FrameTiming, 3> gap = {{
      {.frame_id = 10, .device_timestamp = 0},
      {.frame_id = 11, .device_timestamp = 100},
      {.frame_id = 13, .device_timestamp = 200},
  }};
  assert(PlanCameraClipWindow(gap, mapper, strike, 0ms, 0ms).failure ==
         ClipPlanFailure::kFrameIdGap);

  const std::array<FrameTiming, 3> bad_ids = {{
      {.frame_id = 10, .device_timestamp = 0},
      {.frame_id = 9, .device_timestamp = 100},
      {.frame_id = 11, .device_timestamp = 200},
  }};
  assert(PlanCameraClipWindow(bad_ids, mapper, strike, 0ms, 0ms).failure ==
         ClipPlanFailure::kNonMonotonicFrameId);

  const std::array<FrameTiming, 3> bad_timestamps = {{
      {.frame_id = 10, .device_timestamp = 0},
      {.frame_id = 11, .device_timestamp = 200},
      {.frame_id = 12, .device_timestamp = 100},
  }};
  assert(PlanCameraClipWindow(bad_timestamps, mapper, strike, 0ms, 0ms).failure ==
         ClipPlanFailure::kNonMonotonicDeviceTimestamp);
}

void ReportsMapperAndTimestampFailuresExplicitly() {
  DeviceClockMapper not_ready(1000);
  const std::vector<FrameTiming> frames = RegularFrames(1, 100, 100, 4);
  assert(PlanCameraClipWindow(frames, not_ready, Clock::time_point(200ms), 0ms, 0ms).failure ==
         ClipPlanFailure::kMapperNotReady);

  const DeviceClockMapper mapper = IdentityMapper(100);
  const std::vector<FrameTiming> too_early = RegularFrames(1, 50, 100, 4);
  assert(PlanCameraClipWindow(too_early, mapper, Clock::time_point(200ms), 0ms, 0ms).failure ==
         ClipPlanFailure::kTimestampNotMappable);

  DeviceClockMapper reverse_mapper(1000);
  reverse_mapper.AddSample(0, Clock::time_point(1s));
  reverse_mapper.AddSample(1000, Clock::time_point(0s));
  const std::vector<FrameTiming> regular = RegularFrames(1, 0, 100, 11);
  assert(
      PlanCameraClipWindow(regular, reverse_mapper, Clock::time_point(500ms), 0ms, 0ms).failure ==
      ClipPlanFailure::kNonMonotonicMappedTime);
}

void ReportsEmptyAndInvalidWindowFailures() {
  const DeviceClockMapper mapper = IdentityMapper();
  const std::span<const FrameTiming> no_frames;
  assert(PlanCameraClipWindow(no_frames, mapper, Clock::time_point{}, 0ms, 0ms).failure ==
         ClipPlanFailure::kNoFrames);

  const std::array<FrameTiming, 1> one_frame = {{
      {.frame_id = 1, .device_timestamp = 0},
  }};
  const CameraClipPlan invalid =
      PlanCameraClipWindow(one_frame, mapper, Clock::time_point{}, -1ms, 0ms);
  assert(invalid.failure == ClipPlanFailure::kInvalidWindow);
  assert(!invalid.frame_range.has_value());
  assert(swing_capture::ClipPlanFailureMessage(invalid.failure) ==
         "pre-roll and post-roll must be nonnegative");
}

}  // namespace

int main() {
  SelectsExactBoundariesAndExactStrike();
  BracketsInexactBoundariesAndChoosesEarlierNearestTie();
  MapsIndependentOffsetsAndDriftsForDualView();
  ReportsInsufficientRetentionBySide();
  RejectsFrameIdGapsAndNonMonotonicInput();
  ReportsMapperAndTimestampFailuresExplicitly();
  ReportsEmptyAndInvalidWindowFailures();
  return 0;
}
