#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "capture/clip/clip_window_planner.h"
#include "capture/core/device_clock_mapper.h"
#include "capture/core/pooled_raw_frame_ring.h"
#include "capture/session/capture_session_coordinator.h"
#include "capture/trigger/impact_detector.h"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using swing_capture::CaptureSessionConfig;
using swing_capture::CaptureSessionCoordinator;
using swing_capture::CaptureSessionState;
using swing_capture::DeviceClockMapper;
using swing_capture::FrameMetadata;
using swing_capture::FrameTiming;
using swing_capture::FrameView;
using swing_capture::ImpactDetector;
using swing_capture::ImpactDetectorConfig;
using swing_capture::ImpactEvent;
using swing_capture::PlanDualViewClipWindow;
using swing_capture::PooledRawFramePushResult;
using swing_capture::PooledRawFrameRing;
using swing_capture::PooledRawFrameRingConfig;
using swing_capture::PooledRawFrameSnapshot;
using swing_capture::TickDisposition;

constexpr std::size_t kRingCapacity = 384;
constexpr auto kFramePeriod = 3906250ns;  // Exactly 256 frames/second.

constexpr std::int16_t Pcm(float normalized_amplitude) {
  return static_cast<std::int16_t>(normalized_amplitude * 32767.0F);
}

class SyntheticCamera final {
 public:
  SyntheticCamera(std::byte camera_marker, std::uint64_t first_frame_id,
                  std::uint64_t first_device_timestamp, std::uint64_t device_ticks_per_second,
                  std::uint64_t device_ticks_per_frame, Clock::time_point first_frame_time)
      : camera_marker_(camera_marker),
        first_frame_id_(first_frame_id),
        first_device_timestamp_(first_device_timestamp),
        device_ticks_per_frame_(device_ticks_per_frame),
        first_frame_time_(first_frame_time),
        mapper_(device_ticks_per_second),
        ring_(PooledRawFrameRingConfig{
            .active_frame_capacity = kRingCapacity,
            .reserve_frame_blocks = kRingCapacity,
            .maximum_payload_bytes = kPayloadBytes,
        }) {}

  void CaptureThrough(Clock::time_point inclusive_end) {
    while (NextFrameTime() <= inclusive_end) {
      CaptureOne();
    }
  }

  void CaptureFrames(std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
      CaptureOne();
    }
  }

  [[nodiscard]] DeviceClockMapper &mapper() { return mapper_; }
  [[nodiscard]] PooledRawFrameRing &ring() { return ring_; }

 private:
  static constexpr std::size_t kPayloadBytes = 4;

  [[nodiscard]] Clock::time_point NextFrameTime() const {
    return first_frame_time_ + kFramePeriod * static_cast<Clock::duration::rep>(next_frame_index_);
  }

  void CaptureOne() {
    const std::uint64_t frame_id = first_frame_id_ + next_frame_index_;
    const std::uint64_t device_timestamp =
        first_device_timestamp_ + next_frame_index_ * device_ticks_per_frame_;
    const Clock::time_point host_time = NextFrameTime();
    const std::array<std::byte, kPayloadBytes> payload = {
        camera_marker_,
        static_cast<std::byte>(frame_id & 0xffU),
        static_cast<std::byte>((frame_id >> 8U) & 0xffU),
        static_cast<std::byte>((frame_id >> 16U) & 0xffU),
    };
    const FrameView frame{
        .metadata =
            FrameMetadata{
                .frame_id = frame_id,
                .device_timestamp = device_timestamp,
                .host_received_at = host_time,
                .width = 2,
                .height = 2,
                .complete = true,
            },
        .payload = payload,
    };

    assert(ring_.TryPush(frame) == PooledRawFramePushResult::kStored);
    mapper_.AddSample(device_timestamp, host_time);
    ++next_frame_index_;
  }

  std::byte camera_marker_;
  std::uint64_t first_frame_id_;
  std::uint64_t first_device_timestamp_;
  std::uint64_t device_ticks_per_frame_;
  Clock::time_point first_frame_time_;
  std::uint64_t next_frame_index_ = 0;
  DeviceClockMapper mapper_;
  PooledRawFrameRing ring_;
};

std::vector<FrameTiming> ExtractTimings(const PooledRawFrameSnapshot &snapshot) {
  std::vector<FrameTiming> timings;
  timings.reserve(snapshot.size());
  for (const auto &frame : snapshot.frames()) {
    timings.push_back({
        .frame_id = frame.metadata().frame_id,
        .device_timestamp = frame.metadata().device_timestamp,
    });
  }
  return timings;
}

void AssertBoundedCoverage(const swing_capture::CameraClipPlan &plan, Clock::duration pre_roll,
                           Clock::duration post_roll) {
  assert(plan.ok());
  assert(plan.frame_range.has_value());
  assert(plan.strike_frame.has_value());
  assert(plan.coverage.has_value());

  const std::size_t selected_frame_count =
      plan.frame_range->end_index_exclusive - plan.frame_range->begin_index;
  // A requested interval has at most ceil(duration / period) + 2 bracketing
  // frames. This prevents a successful plan from silently returning the
  // entire retention ring.
  constexpr std::size_t kMaximumExpectedClipFrames = 361;
  assert(selected_frame_count > 0);
  assert(selected_frame_count <= kMaximumExpectedClipFrames);
  assert(selected_frame_count < kRingCapacity);

  assert(plan.coverage->actual_pre_roll >= pre_roll);
  assert(plan.coverage->actual_pre_roll < pre_roll + kFramePeriod);
  assert(plan.coverage->actual_post_roll >= post_roll);
  assert(plan.coverage->actual_post_roll < post_roll + kFramePeriod);
  assert(plan.coverage->selected_start_time <= plan.coverage->requested_start_time);
  assert(plan.coverage->selected_end_time >= plan.coverage->requested_end_time);
  assert(std::chrono::abs(plan.strike_frame->time_error) <= kFramePeriod / 2);
}

void OneImpactFreezesBoundedDualViewClipAndCaptureContinues() {
  const auto origin = Clock::time_point(100s);
  const auto strike_time = origin + 3s;
  constexpr auto kPreRoll = 1s;
  constexpr auto kPostRoll = 400ms;

  // The cameras use unrelated timestamp origins and tick frequencies. Their
  // host-correlated frame trains are deliberately half a frame out of phase.
  SyntheticCamera down_the_line(std::byte{0xA1}, 1000, 10000000, 1024000, 4000, origin);
  SyntheticCamera face_on(std::byte{0xB2}, 9000, 87000000, 2048000, 8000,
                          origin + kFramePeriod / 2);

  down_the_line.CaptureThrough(strike_time);
  face_on.CaptureThrough(strike_time);

  ImpactDetectorConfig detector_config;
  detector_config.peak_confirmation_seconds = 0.001;
  detector_config.refractory_period_seconds = 0.25;
  ImpactDetector detector(detector_config);
  constexpr std::uint32_t kAudioSampleRate = 8192;
  constexpr std::size_t kStrikeSampleIndex = kAudioSampleRate;
  std::vector<std::int16_t> audio(kStrikeSampleIndex + 64, 0);
  audio[kStrikeSampleIndex - 1] = Pcm(0.45F);
  audio[kStrikeSampleIndex] = Pcm(-0.92F);
  audio[kStrikeSampleIndex + 1] = Pcm(0.60F);
  std::array<ImpactEvent, 2> impacts;
  const auto detector_result = detector.ProcessBlock(audio, origin + 2s, kAudioSampleRate, impacts);

  assert(detector_result.events_detected == 1);
  assert(detector_result.events_written == 1);
  assert(impacts[0].strike_time == strike_time);
  assert(impacts[0].confirmation_time > impacts[0].strike_time);
  assert(impacts[0].sample_index_in_block == kStrikeSampleIndex);

  CaptureSessionCoordinator coordinator(CaptureSessionConfig{
      .pre_roll = kPreRoll,
      .post_roll = kPostRoll,
      .frame_boundary_margin = kFramePeriod,
      .active_ring_retention = 1500ms,
      .cooldown_after_freeze = 250ms,
  });
  const auto handling = coordinator.HandleImpact(impacts[0]);
  assert(handling.accepted());
  assert(handling.related_clip_id == 1);
  assert(coordinator.state() == CaptureSessionState::kWaitingForPostRoll);

  const auto just_before_deadline = strike_time + kPostRoll + kFramePeriod - 1ns;
  down_the_line.CaptureThrough(just_before_deadline);
  face_on.CaptureThrough(just_before_deadline);
  const auto waiting = coordinator.Tick(just_before_deadline);
  assert(waiting.disposition == TickDisposition::kWaitingForFreezeDeadline);
  assert(!waiting.freeze_request.has_value());

  // Tick one frame period late so both free-running views have a boundary
  // frame at or after the desired endpoint. The request itself remains
  // anchored to the detected impact.
  const auto freeze_time = strike_time + kPostRoll + kFramePeriod;
  down_the_line.CaptureThrough(freeze_time);
  face_on.CaptureThrough(freeze_time);
  const auto tick_result = coordinator.Tick(freeze_time);
  assert(tick_result.freeze_requested());
  assert(tick_result.freeze_request.has_value());
  const auto &freeze_request = *tick_result.freeze_request;
  assert(freeze_request.clip_id == 1);
  assert(freeze_request.strike_time == strike_time);
  assert(freeze_request.impact_confirmation_time == impacts[0].confirmation_time);
  assert(freeze_request.desired_start_time == strike_time - kPreRoll);
  assert(freeze_request.desired_end_time == strike_time + kPostRoll);
  assert(coordinator.Tick(freeze_time).disposition == TickDisposition::kNoClipPending);

  const PooledRawFrameSnapshot frozen_down_the_line = down_the_line.ring().Freeze();
  const PooledRawFrameSnapshot frozen_face_on = face_on.ring().Freeze();
  assert(frozen_down_the_line.size() == kRingCapacity);
  assert(frozen_face_on.size() == kRingCapacity);

  const std::uint64_t frozen_down_first_id = frozen_down_the_line.at(0).metadata().frame_id;
  const std::uint64_t frozen_down_last_id =
      frozen_down_the_line.at(frozen_down_the_line.size() - 1).metadata().frame_id;
  const std::uint64_t frozen_face_first_id = frozen_face_on.at(0).metadata().frame_id;
  const std::uint64_t frozen_face_last_id =
      frozen_face_on.at(frozen_face_on.size() - 1).metadata().frame_id;

  // Hold both frozen snapshots while each producer captures more than two
  // complete active-ring rotations. The reserve pool must preserve the clip
  // and permit uninterrupted capture.
  constexpr std::size_t kFramesAfterFreeze = kRingCapacity * 2 + 17;
  down_the_line.CaptureFrames(kFramesAfterFreeze);
  face_on.CaptureFrames(kFramesAfterFreeze);

  assert(frozen_down_the_line.at(0).metadata().frame_id == frozen_down_first_id);
  assert(frozen_down_the_line.at(frozen_down_the_line.size() - 1).metadata().frame_id ==
         frozen_down_last_id);
  assert(frozen_down_the_line.at(0).payload()[0] == std::byte{0xA1});
  assert(frozen_face_on.at(0).metadata().frame_id == frozen_face_first_id);
  assert(frozen_face_on.at(frozen_face_on.size() - 1).metadata().frame_id == frozen_face_last_id);
  assert(frozen_face_on.at(0).payload()[0] == std::byte{0xB2});

  const auto current_down = down_the_line.ring().Freeze();
  const auto current_face = face_on.ring().Freeze();
  assert(current_down.size() == kRingCapacity);
  assert(current_face.size() == kRingCapacity);
  assert(current_down.at(0).metadata().frame_id > frozen_down_last_id);
  assert(current_face.at(0).metadata().frame_id > frozen_face_last_id);

  const std::vector<FrameTiming> down_timings = ExtractTimings(frozen_down_the_line);
  const std::vector<FrameTiming> face_timings = ExtractTimings(frozen_face_on);
  const auto plan = PlanDualViewClipWindow(impacts[0], down_timings, down_the_line.mapper(),
                                           face_timings, face_on.mapper(), kPreRoll, kPostRoll);

  assert(plan.ok());
  AssertBoundedCoverage(plan.first_view, kPreRoll, kPostRoll);
  AssertBoundedCoverage(plan.second_view, kPreRoll, kPostRoll);
  assert(plan.mapped_nearest_frame_skew.has_value());
  assert(std::chrono::abs(*plan.mapped_nearest_frame_skew) <= kFramePeriod / 2);
}

}  // namespace

int main() {
  OneImpactFreezesBoundedDualViewClipAndCaptureContinues();
  return 0;
}
