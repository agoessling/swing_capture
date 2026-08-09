#ifndef SWING_CAPTURE_CAPTURE_OPTICAL_FRAME_SELECTION_H_
#define SWING_CAPTURE_CAPTURE_OPTICAL_FRAME_SELECTION_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "capture/core/device_clock_mapper.h"
#include "capture/optical/led_pulse.h"

namespace swing_capture::optical {

inline constexpr std::size_t kLedAnalysisFramesBefore = 16;
inline constexpr std::size_t kLedAnalysisFramesAtOrAfter = 32;
inline constexpr std::size_t kLedAnalysisFrameCount =
    kLedAnalysisFramesBefore + kLedAnalysisFramesAtOrAfter;
inline constexpr auto kTagPreflashOffset = std::chrono::microseconds(50'000);

struct FeatherAckScheduleBracket {
  std::chrono::steady_clock::time_point host_command_sent;
  std::chrono::steady_clock::time_point host_acknowledgement_received;
  std::uint64_t accepted_device_microseconds = 0;
  std::uint64_t scheduled_device_microseconds = 0;
};

struct EstimatedStimulusHostSchedule {
  std::chrono::steady_clock::time_point earliest_start;
  std::chrono::steady_clock::time_point latest_start;
  std::chrono::steady_clock::time_point target_start;
};

struct LedAnalysisFrameSelection {
  std::size_t begin_index = 0;
  std::size_t pivot_index = 0;
  std::size_t end_index_exclusive = 0;
  EstimatedStimulusHostSchedule schedule;
};

struct MappedTagTimelineView {
  std::span<const std::chrono::steady_clock::time_point> frame_host_times;
  std::chrono::steady_clock::time_point scheduled_start;
};

struct TagRepresentativeFrameSelection {
  // Relative to the frame/diagnostic spans supplied to
  // SelectTagRepresentativeFrame.
  std::size_t frame_index = 0;
  bool used_preflash_mapping = false;
  std::string diagnostic;
};

// Converts the Feather's ACK timing into a conservative host-time bracket.
// The actual device acceptance occurred between the host write and ACK receipt;
// both bounds are advanced by the accepted-to-scheduled device interval.
[[nodiscard]] EstimatedStimulusHostSchedule EstimateStimulusHostSchedule(
    const FeatherAckScheduleBracket &bracket);

// Validates tightly packed BayerRG8 payloads, fixed geometry, contiguous frame
// IDs, and strictly increasing device timestamps. It then maps every frame to
// host time. Throws std::invalid_argument if the mapper is not usable.
[[nodiscard]] std::vector<std::chrono::steady_clock::time_point> MapFrameHostTimes(
    std::span<const BayerRg8FrameView> retained_frames, const DeviceClockMapper &clock_mapper);

// Selects 16 frames before the first mapped frame at-or-after target_start and
// 32 frames beginning with that pivot. Indices refer to retained_frames; no
// payload or ownership is copied.
[[nodiscard]] LedAnalysisFrameSelection SelectLedAnalysisFrames(
    std::span<const BayerRg8FrameView> retained_frames,
    std::span<const std::chrono::steady_clock::time_point> mapped_host_times,
    const EstimatedStimulusHostSchedule &schedule);

// Convenience overload when the caller has already reduced its timing evidence
// to one mapped host target.
[[nodiscard]] LedAnalysisFrameSelection SelectLedAnalysisFrames(
    std::span<const BayerRg8FrameView> retained_frames,
    std::span<const std::chrono::steady_clock::time_point> mapped_host_times,
    std::chrono::steady_clock::time_point mapped_host_target);

// Convenience overload for the normal HIL path.
[[nodiscard]] LedAnalysisFrameSelection SelectLedAnalysisFrames(
    std::span<const BayerRg8FrameView> retained_frames, const DeviceClockMapper &clock_mapper,
    const FeatherAckScheduleBracket &bracket);

// Selects the inactive frame closest to scheduled_start - 50 ms when a mapped
// timeline is available and covers that target. Otherwise it deterministically
// selects the lower midpoint of the longest inactive run (earliest run wins a
// tie). Diagnostics must correspond one-for-one with frames.
[[nodiscard]] TagRepresentativeFrameSelection SelectTagRepresentativeFrame(
    std::span<const BayerRg8FrameView> frames, std::span<const LedFrameDiagnostic> led_diagnostics,
    std::optional<MappedTagTimelineView> mapped_timeline = std::nullopt);

}  // namespace swing_capture::optical

#endif  // SWING_CAPTURE_CAPTURE_OPTICAL_FRAME_SELECTION_H_
