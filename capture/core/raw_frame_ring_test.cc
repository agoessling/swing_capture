#include "capture/core/raw_frame_ring.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {

using swing_capture::FrameMetadata;
using swing_capture::FrameView;
using swing_capture::RawFrameRing;

FrameView MakeFrame(std::uint64_t frame_id, const std::array<std::byte, 4> &payload) {
  return {
      .metadata =
          FrameMetadata{
              .frame_id = frame_id,
              .device_timestamp = frame_id * 100,
              .host_received_at =
                  std::chrono::steady_clock::time_point(std::chrono::nanoseconds(frame_id * 100)),
              .width = 2,
              .height = 2,
              .complete = true,
          },
      .payload = payload,
  };
}

void RetainsFramesInChronologicalOrder() {
  RawFrameRing ring(3, 4);
  const std::array<std::byte, 4> first = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const std::array<std::byte, 4> second = {std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};

  ring.Push(MakeFrame(10, first));
  ring.Push(MakeFrame(11, second));

  assert(ring.size() == 2);
  assert(ring.at(0).metadata.frame_id == 10);
  assert(ring.at(1).metadata.frame_id == 11);
  assert(ring.at(0).payload[2] == std::byte{3});
}

void WrapDropsOnlyTheOldestFrame() {
  RawFrameRing ring(2, 4);
  const std::array<std::byte, 4> payload = {};
  ring.Push(MakeFrame(1, payload));
  ring.Push(MakeFrame(2, payload));
  ring.Push(MakeFrame(3, payload));

  assert(ring.size() == 2);
  assert(ring.at(0).metadata.frame_id == 2);
  assert(ring.at(1).metadata.frame_id == 3);
}

void RejectsInvalidFramesAndIndexes() {
  RawFrameRing ring(1, 4);
  const std::array<std::byte, 4> payload = {};
  FrameView incomplete = MakeFrame(1, payload);
  incomplete.metadata.complete = false;

  bool rejected_incomplete = false;
  try {
    ring.Push(incomplete);
  } catch (const std::invalid_argument &) {
    rejected_incomplete = true;
  }
  assert(rejected_incomplete);

  bool rejected_index = false;
  try {
    static_cast<void>(ring.at(0));
  } catch (const std::out_of_range &) {
    rejected_index = true;
  }
  assert(rejected_index);
}

}  // namespace

int main() {
  RetainsFramesInChronologicalOrder();
  WrapDropsOnlyTheOldestFrame();
  RejectsInvalidFramesAndIndexes();
  return 0;
}
