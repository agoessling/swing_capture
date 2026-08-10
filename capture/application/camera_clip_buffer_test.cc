#include "capture/application/camera_clip_buffer.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "capture/core/camera_source.h"

namespace {

using swing_capture::FrameMetadata;
using swing_capture::FrameView;
using swing_capture::application::CameraClipBuffer;

FrameView Frame(std::uint64_t frame_id, const std::array<std::byte, 4> &pixels,
                bool complete = true) {
  return {
      .metadata =
          FrameMetadata{
              .frame_id = frame_id,
              .device_timestamp = frame_id * 10U,
              .host_received_at =
                  std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(frame_id),
              .width = 2,
              .height = 2,
              .complete = complete,
          },
      .payload = pixels,
  };
}

void TestArmFreezeDisarmAndRearm() {
  CameraClipBuffer buffer(
      {.active_frame_capacity = 3, .reserve_frame_blocks = 3, .maximum_payload_bytes = 4});
  const std::array pixels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

  buffer.ObserveFrame(Frame(1, pixels));
  assert(!buffer.Status().armed);

  buffer.Arm();
  for (std::uint64_t frame_id = 1; frame_id <= 4; ++frame_id) {
    buffer.ObserveFrame(Frame(frame_id, pixels));
  }
  const auto first = buffer.Freeze();
  assert(first.size() == 3);
  assert(first.at(0).metadata().frame_id == 2);
  assert(first.at(2).metadata().frame_id == 4);

  const auto armed = buffer.Status();
  assert(armed.armed);
  assert(armed.generation == 1);
  assert(armed.frames_observed == 4);
  assert(armed.frames_stored == 4);
  assert(armed.failures == 0);
  assert(armed.retained_frames == 3);
  assert(armed.allocated_bytes > 0);

  buffer.Disarm();
  buffer.ObserveFrame(Frame(5, pixels));
  assert(!buffer.Status().armed);
  assert(first.at(0).payload().front() == std::byte{1});

  buffer.Arm();
  buffer.ObserveFrame(Frame(10, pixels));
  const auto second = buffer.Freeze();
  assert(second.size() == 1);
  assert(second.at(0).metadata().frame_id == 10);
  assert(buffer.Status().generation == 2);
  assert(buffer.Status().frames_stored == 1);
}

void TestRejectedFrameSurfacesFailure() {
  CameraClipBuffer buffer(
      {.active_frame_capacity = 2, .reserve_frame_blocks = 0, .maximum_payload_bytes = 4});
  const std::array pixels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  buffer.Arm();
  bool rejected = false;
  try {
    buffer.ObserveFrame(Frame(1, pixels, false));
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  assert(rejected);
  assert(buffer.Status().failures == 1);
  assert(buffer.Status().frames_stored == 0);
}

void TestRotateKeepsLiveCaptureIndependentOfHeldSnapshot() {
  CameraClipBuffer buffer(
      {.active_frame_capacity = 3, .reserve_frame_blocks = 0, .maximum_payload_bytes = 4});
  const std::array pixels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  buffer.Arm();
  for (std::uint64_t frame_id = 1; frame_id <= 3; ++frame_id) {
    buffer.ObserveFrame(Frame(frame_id, pixels));
  }

  const auto retained = buffer.FreezeAndRotate();
  assert(retained.size() == 3);
  assert(buffer.Status().generation == 2);
  assert(buffer.Status().retained_frames == 0);

  for (std::uint64_t frame_id = 4; frame_id <= 20; ++frame_id) {
    buffer.ObserveFrame(Frame(frame_id, pixels));
  }
  const auto live = buffer.Freeze();
  assert(live.size() == 3);
  assert(live.at(0).metadata().frame_id == 18);
  assert(live.at(2).metadata().frame_id == 20);
  assert(retained.at(0).metadata().frame_id == 1);
  assert(retained.at(2).metadata().frame_id == 3);
  assert(buffer.Status().failures == 0);
}

void TestRetiredGenerationIsRecycledForAnotherCapture() {
  CameraClipBuffer buffer(
      {.active_frame_capacity = 3, .reserve_frame_blocks = 0, .maximum_payload_bytes = 4});
  const std::array pixels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  buffer.Arm();
  for (std::uint64_t frame_id = 1; frame_id <= 3; ++frame_id) {
    buffer.ObserveFrame(Frame(frame_id, pixels));
  }
  {
    const auto first = buffer.FreezeAndRotate();
    assert(first.size() == 3);
  }
  buffer.RecycleRetiredRing();

  for (std::uint64_t frame_id = 10; frame_id <= 12; ++frame_id) {
    buffer.ObserveFrame(Frame(frame_id, pixels));
  }
  {
    const auto second = buffer.FreezeAndRotate();
    assert(second.size() == 3);
    assert(second.at(0).metadata().frame_id == 10);
    assert(second.at(2).metadata().frame_id == 12);
  }
  buffer.RecycleRetiredRing();
  assert(buffer.Status().armed);
  assert(buffer.Status().generation == 3);
}

}  // namespace

int main() {
  TestArmFreezeDisarmAndRearm();
  TestRejectedFrameSurfacesFailure();
  TestRotateKeepsLiveCaptureIndependentOfHeldSnapshot();
  TestRetiredGenerationIsRecycledForAnotherCapture();
  return 0;
}
