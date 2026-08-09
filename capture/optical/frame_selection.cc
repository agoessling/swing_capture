#include "capture/optical/frame_selection.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "capture/core/device_clock_mapper.h"
#include "capture/image/image_quality.h"
#include "capture/optical/led_pulse.h"

namespace swing_capture::optical {
namespace {

void ValidateRetainedFrames(std::span<const BayerRg8FrameView> frames) {
  if (frames.empty()) {
    throw std::invalid_argument("optical frame selection requires retained frames");
  }
  const std::uint32_t width = frames.front().image.width;
  const std::uint32_t height = frames.front().image.height;
  if (width == 0 || height == 0 ||
      (height != 0 && width > std::numeric_limits<std::size_t>::max() / height)) {
    throw std::invalid_argument("optical retained-frame geometry is invalid");
  }
  const std::size_t expected_payload = static_cast<std::size_t>(width) * height;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const auto &frame = frames[index];
    if (frame.image.width != width || frame.image.height != height) {
      throw std::invalid_argument("optical retained frames must have identical geometry");
    }
    if (frame.image.row_stride_bytes != 0 && frame.image.row_stride_bytes != width) {
      throw std::invalid_argument("optical retained BayerRG8 frames must be tightly packed");
    }
    if (frame.image.pixels.size() != expected_payload) {
      throw std::invalid_argument("optical retained BayerRG8 payload size does not match geometry");
    }
    if (index == 0) {
      continue;
    }
    const auto &previous = frames[index - 1];
    if (frame.frame_id != previous.frame_id + 1U) {
      throw std::invalid_argument("optical retained frame IDs must be contiguous");
    }
    if (frame.device_timestamp <= previous.device_timestamp) {
      throw std::invalid_argument("optical retained device timestamps must be strictly increasing");
    }
  }
}

void ValidateMappedHostTimes(
    std::span<const BayerRg8FrameView> frames,
    std::span<const std::chrono::steady_clock::time_point> mapped_host_times) {
  if (mapped_host_times.size() != frames.size()) {
    throw std::invalid_argument("mapped optical host timeline must match the retained frame count");
  }
  for (std::size_t index = 1; index < mapped_host_times.size(); ++index) {
    if (mapped_host_times[index] <= mapped_host_times[index - 1]) {
      throw std::invalid_argument("mapped optical host times must be strictly increasing");
    }
  }
}

std::chrono::steady_clock::duration AbsoluteDuration(std::chrono::steady_clock::duration duration) {
  return duration < std::chrono::steady_clock::duration::zero() ? -duration : duration;
}

void ValidateLedDiagnostics(std::span<const BayerRg8FrameView> frames,
                            std::span<const LedFrameDiagnostic> diagnostics) {
  if (diagnostics.size() != frames.size()) {
    throw std::invalid_argument("LED diagnostics must match the tag-selection frame count");
  }
  for (std::size_t index = 0; index < frames.size(); ++index) {
    if (diagnostics[index].frame_id != frames[index].frame_id ||
        diagnostics[index].device_timestamp != frames[index].device_timestamp) {
      throw std::invalid_argument("LED diagnostics do not identify their corresponding frame");
    }
  }
}

std::size_t SelectMappedTagFrame(
    std::span<const LedFrameDiagnostic> diagnostics,
    std::span<const std::chrono::steady_clock::time_point> mapped_host_times,
    std::chrono::steady_clock::time_point target) {
  std::size_t selected = diagnostics.size();
  auto selected_distance = std::chrono::steady_clock::duration::max();
  for (std::size_t index = 0; index < diagnostics.size(); ++index) {
    if (diagnostics[index].active) {
      continue;
    }
    const auto distance = AbsoluteDuration(mapped_host_times[index] - target);
    if (selected == diagnostics.size() || distance < selected_distance) {
      selected = index;
      selected_distance = distance;
    }
  }
  return selected;
}

std::size_t SelectInactiveRunMidpoint(std::span<const LedFrameDiagnostic> diagnostics) {
  std::size_t best_start = diagnostics.size();
  std::size_t best_length = 0;
  for (std::size_t index = 0; index < diagnostics.size();) {
    if (diagnostics[index].active) {
      ++index;
      continue;
    }
    const std::size_t start = index;
    while (index < diagnostics.size() && !diagnostics[index].active) {
      ++index;
    }
    const std::size_t length = index - start;
    if (length > best_length) {
      best_start = start;
      best_length = length;
    }
  }
  if (best_start == diagnostics.size()) {
    throw std::invalid_argument("tag selection requires at least one inactive LED frame");
  }
  return best_start + (best_length - 1U) / 2U;
}

}  // namespace

EstimatedStimulusHostSchedule EstimateStimulusHostSchedule(
    const FeatherAckScheduleBracket &bracket) {
  if (bracket.host_acknowledgement_received < bracket.host_command_sent) {
    throw std::invalid_argument("Feather ACK host time precedes the command send time");
  }
  if (bracket.scheduled_device_microseconds < bracket.accepted_device_microseconds) {
    throw std::invalid_argument("Feather scheduled time precedes its accepted device time");
  }
  const std::uint64_t lead_count =
      bracket.scheduled_device_microseconds - bracket.accepted_device_microseconds;
  if (lead_count >
      static_cast<std::uint64_t>(std::numeric_limits<std::chrono::microseconds::rep>::max())) {
    throw std::invalid_argument("Feather accepted-to-scheduled interval is not representable");
  }
  const auto lead =
      std::chrono::microseconds(static_cast<std::chrono::microseconds::rep>(lead_count));
  EstimatedStimulusHostSchedule result{
      .earliest_start = bracket.host_command_sent + lead,
      .latest_start = bracket.host_acknowledgement_received + lead,
      .target_start = {},
  };
  result.target_start = result.earliest_start + (result.latest_start - result.earliest_start) / 2;
  return result;
}

std::vector<std::chrono::steady_clock::time_point> MapFrameHostTimes(
    std::span<const BayerRg8FrameView> retained_frames, const DeviceClockMapper &clock_mapper) {
  ValidateRetainedFrames(retained_frames);
  if (!clock_mapper.ready()) {
    throw std::invalid_argument("camera device-clock mapper is not ready for optical selection");
  }
  std::vector<std::chrono::steady_clock::time_point> mapped;
  mapped.reserve(retained_frames.size());
  for (const auto &frame : retained_frames) {
    const auto host_time = clock_mapper.EstimateHostTime(frame.device_timestamp);
    if (!host_time.has_value()) {
      throw std::invalid_argument("camera device-clock mapper cannot map a retained frame");
    }
    mapped.push_back(*host_time);
  }
  ValidateMappedHostTimes(retained_frames, mapped);
  return mapped;
}

LedAnalysisFrameSelection SelectLedAnalysisFrames(
    std::span<const BayerRg8FrameView> retained_frames,
    std::span<const std::chrono::steady_clock::time_point> mapped_host_times,
    const EstimatedStimulusHostSchedule &schedule) {
  ValidateRetainedFrames(retained_frames);
  ValidateMappedHostTimes(retained_frames, mapped_host_times);
  if (schedule.latest_start < schedule.earliest_start ||
      schedule.target_start < schedule.earliest_start ||
      schedule.target_start > schedule.latest_start) {
    throw std::invalid_argument("estimated Feather host schedule is inconsistent");
  }

  const auto pivot = std::ranges::lower_bound(mapped_host_times, schedule.target_start);
  if (pivot == mapped_host_times.end()) {
    throw std::invalid_argument("scheduled LED start is after the retained frame timeline");
  }
  const auto pivot_index =
      static_cast<std::size_t>(std::distance(mapped_host_times.begin(), pivot));
  if (pivot_index < kLedAnalysisFramesBefore ||
      retained_frames.size() - pivot_index < kLedAnalysisFramesAtOrAfter) {
    throw std::invalid_argument("retained frames do not contain the bounded LED analysis window");
  }
  return {
      .begin_index = pivot_index - kLedAnalysisFramesBefore,
      .pivot_index = pivot_index,
      .end_index_exclusive = pivot_index + kLedAnalysisFramesAtOrAfter,
      .schedule = schedule,
  };
}

LedAnalysisFrameSelection SelectLedAnalysisFrames(
    std::span<const BayerRg8FrameView> retained_frames,
    std::span<const std::chrono::steady_clock::time_point> mapped_host_times,
    std::chrono::steady_clock::time_point mapped_host_target) {
  return SelectLedAnalysisFrames(retained_frames, mapped_host_times,
                                 {
                                     .earliest_start = mapped_host_target,
                                     .latest_start = mapped_host_target,
                                     .target_start = mapped_host_target,
                                 });
}

LedAnalysisFrameSelection SelectLedAnalysisFrames(
    std::span<const BayerRg8FrameView> retained_frames, const DeviceClockMapper &clock_mapper,
    const FeatherAckScheduleBracket &bracket) {
  const auto mapped = MapFrameHostTimes(retained_frames, clock_mapper);
  return SelectLedAnalysisFrames(retained_frames, mapped, EstimateStimulusHostSchedule(bracket));
}

TagRepresentativeFrameSelection SelectTagRepresentativeFrame(
    std::span<const BayerRg8FrameView> frames, std::span<const LedFrameDiagnostic> led_diagnostics,
    std::optional<MappedTagTimelineView> mapped_timeline) {
  ValidateRetainedFrames(frames);
  ValidateLedDiagnostics(frames, led_diagnostics);

  if (mapped_timeline.has_value()) {
    ValidateMappedHostTimes(frames, mapped_timeline->frame_host_times);
    const auto target = mapped_timeline->scheduled_start - kTagPreflashOffset;
    if (target >= mapped_timeline->frame_host_times.front() &&
        target <= mapped_timeline->frame_host_times.back()) {
      const std::size_t selected =
          SelectMappedTagFrame(led_diagnostics, mapped_timeline->frame_host_times, target);
      if (selected != frames.size()) {
        return {
            .frame_index = selected,
            .used_preflash_mapping = true,
            .diagnostic = "selected inactive frame nearest scheduled LED start minus 50 ms",
        };
      }
    }
  }

  return {
      .frame_index = SelectInactiveRunMidpoint(led_diagnostics),
      .used_preflash_mapping = false,
      .diagnostic =
          "mapped pre-flash target unavailable; selected midpoint of longest inactive "
          "LED run",
  };
}

}  // namespace swing_capture::optical
