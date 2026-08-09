#include "capture/optical/frame_selection.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "capture/core/device_clock_mapper.h"
#include "capture/optical/led_pulse.h"

namespace {

using Clock = std::chrono::steady_clock;
using swing_capture::DeviceClockMapper;
using swing_capture::optical::BayerRg8FrameView;
using swing_capture::optical::EstimateStimulusHostSchedule;
using swing_capture::optical::FeatherAckScheduleBracket;
using swing_capture::optical::kLedAnalysisFrameCount;
using swing_capture::optical::LedFrameDiagnostic;
using swing_capture::optical::MapFrameHostTimes;
using swing_capture::optical::MappedTagTimelineView;
using swing_capture::optical::SelectLedAnalysisFrames;
using swing_capture::optical::SelectTagRepresentativeFrame;

constexpr std::uint32_t kWidth = 8;
constexpr std::uint32_t kHeight = 6;
constexpr std::uint64_t kTicksPerSecond = 1'000'000;
constexpr std::uint64_t kFrameTicks = 4'000;

int failures = 0;

void Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct OwnedFrames {
  std::vector<std::vector<std::byte>> pixels;
  std::vector<BayerRg8FrameView> views;
};

OwnedFrames MakeFrames(std::size_t count) {
  OwnedFrames frames;
  frames.pixels.resize(count);
  frames.views.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    frames.pixels[index].assign(static_cast<std::size_t>(kWidth) * kHeight, std::byte{80});
    frames.views.push_back({
        .image =
            {
                .pixels = frames.pixels[index],
                .width = kWidth,
                .height = kHeight,
                .row_stride_bytes = 0,
            },
        .frame_id = 1'000U + index,
        .device_timestamp = 2'000'000U + index * kFrameTicks,
    });
  }
  return frames;
}

DeviceClockMapper MakeMapper(std::span<const BayerRg8FrameView> frames,
                             Clock::time_point first_host_time) {
  DeviceClockMapper mapper(kTicksPerSecond);
  const std::uint64_t first_device_time = frames.front().device_timestamp;
  for (const auto &frame : frames) {
    mapper.AddSample(
        frame.device_timestamp,
        first_host_time + std::chrono::microseconds(frame.device_timestamp - first_device_time));
  }
  return mapper;
}

std::vector<LedFrameDiagnostic> MakeDiagnostics(std::span<const BayerRg8FrameView> frames,
                                                std::size_t active_begin,
                                                std::size_t active_end_exclusive) {
  std::vector<LedFrameDiagnostic> diagnostics;
  diagnostics.reserve(frames.size());
  for (std::size_t index = 0; index < frames.size(); ++index) {
    LedFrameDiagnostic diagnostic;
    diagnostic.frame_id = frames[index].frame_id;
    diagnostic.device_timestamp = frames[index].device_timestamp;
    diagnostic.changed_red_samples = index >= active_begin && index < active_end_exclusive ? 8 : 0;
    diagnostic.red_samples = 12;
    diagnostic.changed_red_fraction =
        index >= active_begin && index < active_end_exclusive ? 2.0 / 3.0 : 0.0;
    diagnostic.mean_positive_red_delta =
        index >= active_begin && index < active_end_exclusive ? 60.0 : 0.0;
    diagnostic.baseline_frame = index < 8;
    diagnostic.active = index >= active_begin && index < active_end_exclusive;
    diagnostics.push_back(diagnostic);
  }
  return diagnostics;
}

void EstimatesBracketAndSelectsBoundedWindow() {
  auto frames = MakeFrames(80);
  const auto first_host = Clock::time_point(std::chrono::seconds(10));
  const auto mapper = MakeMapper(frames.views, first_host);
  const auto mapped = MapFrameHostTimes(frames.views, mapper);
  const auto expected_target = mapped[40];
  const FeatherAckScheduleBracket bracket{
      .host_command_sent = expected_target - std::chrono::milliseconds(101),
      .host_acknowledgement_received = expected_target - std::chrono::milliseconds(99),
      .accepted_device_microseconds = 500'000,
      .scheduled_device_microseconds = 600'000,
  };

  const auto schedule = EstimateStimulusHostSchedule(bracket);
  Expect(schedule.earliest_start == expected_target - std::chrono::milliseconds(1),
         "earliest scheduled host start");
  Expect(schedule.latest_start == expected_target + std::chrono::milliseconds(1),
         "latest scheduled host start");
  Expect(schedule.target_start == expected_target, "scheduled host midpoint");

  const auto selected = SelectLedAnalysisFrames(frames.views, mapped, schedule);
  Expect(selected.begin_index == 24, "LED window has 16 frames before pivot");
  Expect(selected.pivot_index == 40, "LED window pivot");
  Expect(selected.end_index_exclusive == 72, "LED window has 32 frames at-or-after pivot");
  Expect(selected.end_index_exclusive - selected.begin_index == kLedAnalysisFrameCount,
         "LED window contains exactly 48 frames");

  const auto mapped_target_selection =
      SelectLedAnalysisFrames(frames.views, mapped, expected_target);
  Expect(mapped_target_selection.begin_index == selected.begin_index &&
             mapped_target_selection.pivot_index == selected.pivot_index &&
             mapped_target_selection.end_index_exclusive == selected.end_index_exclusive,
         "already-mapped host target overload agrees");

  const auto convenience = SelectLedAnalysisFrames(frames.views, mapper, bracket);
  Expect(convenience.begin_index == selected.begin_index &&
             convenience.pivot_index == selected.pivot_index &&
             convenience.end_index_exclusive == selected.end_index_exclusive,
         "mapper and pre-mapped overloads agree");
}

void SelectsMappedPreflashTagAndInactiveFallback() {
  auto retained = MakeFrames(80);
  const auto first_host = Clock::time_point(std::chrono::seconds(20));
  const auto mapper = MakeMapper(retained.views, first_host);
  const auto all_mapped = MapFrameHostTimes(retained.views, mapper);
  const FeatherAckScheduleBracket bracket{
      .host_command_sent = all_mapped[40] - std::chrono::milliseconds(100),
      .host_acknowledgement_received = all_mapped[40] - std::chrono::milliseconds(100),
      .accepted_device_microseconds = 1'000,
      .scheduled_device_microseconds = 101'000,
  };
  const auto window = SelectLedAnalysisFrames(retained.views, mapper, bracket);
  const std::span<const BayerRg8FrameView> frames(retained.views.begin() + window.begin_index,
                                                  kLedAnalysisFrameCount);
  const std::span<const Clock::time_point> mapped(all_mapped.begin() + window.begin_index,
                                                  kLedAnalysisFrameCount);
  const auto diagnostics = MakeDiagnostics(frames, 16, 26);

  const auto mapped_tag =
      SelectTagRepresentativeFrame(frames, diagnostics,
                                   MappedTagTimelineView{
                                       .frame_host_times = mapped,
                                       .scheduled_start = window.schedule.target_start,
                                   });
  Expect(mapped_tag.used_preflash_mapping, "tag uses mapped pre-flash target");
  Expect(mapped_tag.frame_index == 3, "tag chooses earlier frame on equal-distance tie");
  Expect(!diagnostics[mapped_tag.frame_index].active, "mapped tag frame is inactive");

  const auto fallback = SelectTagRepresentativeFrame(frames, diagnostics, std::nullopt);
  Expect(!fallback.used_preflash_mapping, "tag records inactive-run fallback");
  Expect(fallback.frame_index == 36, "fallback uses lower midpoint of longest inactive run");
  Expect(!diagnostics[fallback.frame_index].active, "fallback tag frame is inactive");
}

template <typename Function>
void ExpectInvalidArgument(Function &&function, const std::string &message) {
  bool rejected = false;
  try {
    function();
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Expect(rejected, message);
}

void RejectsMalformedFramesAndUnboundedSelections() {
  auto frames = MakeFrames(80);
  const auto first_host = Clock::time_point(std::chrono::seconds(30));
  const auto mapper = MakeMapper(frames.views, first_host);
  const auto mapped = MapFrameHostTimes(frames.views, mapper);
  const auto schedule = swing_capture::optical::EstimatedStimulusHostSchedule{
      .earliest_start = mapped[8],
      .latest_start = mapped[8],
      .target_start = mapped[8],
  };
  ExpectInvalidArgument([&] { (void)SelectLedAnalysisFrames(frames.views, mapped, schedule); },
                        "window without 16 preceding frames rejected");

  frames = MakeFrames(80);
  frames.views[20].frame_id += 1;
  ExpectInvalidArgument([&] { (void)MapFrameHostTimes(frames.views, mapper); },
                        "frame-ID gap rejected");

  frames = MakeFrames(80);
  frames.views[20].device_timestamp = frames.views[19].device_timestamp;
  ExpectInvalidArgument([&] { (void)MapFrameHostTimes(frames.views, mapper); },
                        "nonmonotonic device timestamp rejected");

  frames = MakeFrames(80);
  frames.views[20].image.pixels = frames.views[20].image.pixels.first(10);
  ExpectInvalidArgument([&] { (void)MapFrameHostTimes(frames.views, mapper); },
                        "incorrect Bayer payload rejected");
}

void RejectsMismatchedDiagnosticsAndInvalidSchedule() {
  auto frames = MakeFrames(kLedAnalysisFrameCount);
  auto diagnostics = MakeDiagnostics(frames.views, 16, 26);
  diagnostics[4].frame_id += 1;
  ExpectInvalidArgument(
      [&] { (void)SelectTagRepresentativeFrame(frames.views, diagnostics, std::nullopt); },
      "misaligned LED diagnostic rejected");

  const FeatherAckScheduleBracket invalid{
      .host_command_sent = Clock::time_point(std::chrono::seconds(2)),
      .host_acknowledgement_received = Clock::time_point(std::chrono::seconds(1)),
      .accepted_device_microseconds = 10,
      .scheduled_device_microseconds = 20,
  };
  ExpectInvalidArgument([&] { (void)EstimateStimulusHostSchedule(invalid); },
                        "ACK before command rejected");
}

}  // namespace

int main() {
  EstimatesBracketAndSelectsBoundedWindow();
  SelectsMappedPreflashTagAndInactiveFallback();
  RejectsMalformedFramesAndUnboundedSelections();
  RejectsMismatchedDiagnosticsAndInvalidSchedule();
  return failures == 0 ? 0 : 1;
}
