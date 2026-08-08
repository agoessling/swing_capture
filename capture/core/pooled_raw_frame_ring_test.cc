#include "capture/core/pooled_raw_frame_ring.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {

using swing_capture::FrameMetadata;
using swing_capture::FrameView;
using swing_capture::PooledRawFrameHandle;
using swing_capture::PooledRawFramePushResult;
using swing_capture::PooledRawFrameRing;

std::array<std::byte, 4> PayloadFor(std::uint64_t frame_id) {
  return {
      std::byte(frame_id & 0xffU),
      std::byte((frame_id + 1) & 0xffU),
      std::byte((frame_id + 2) & 0xffU),
      std::byte((frame_id + 3) & 0xffU),
  };
}

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

void Push(PooledRawFrameRing &ring, std::uint64_t frame_id) {
  const auto payload = PayloadFor(frame_id);
  assert(ring.TryPush(MakeFrame(frame_id, payload)) == PooledRawFramePushResult::kStored);
}

void PreservesChronologicalOrderingAcrossWrap() {
  PooledRawFrameRing ring({
      .active_frame_capacity = 3,
      .reserve_frame_blocks = 0,
      .maximum_payload_bytes = 4,
  });
  for (std::uint64_t id = 1; id <= 5; ++id) {
    Push(ring, id);
  }

  const auto snapshot = ring.Freeze();
  assert(snapshot.size() == 3);
  assert(snapshot.at(0).metadata().frame_id == 3);
  assert(snapshot.at(1).metadata().frame_id == 4);
  assert(snapshot.at(2).metadata().frame_id == 5);
  assert(snapshot.at(0).payload()[0] == std::byte{3});
  assert(snapshot.at(2).view().payload[3] == std::byte{8});
}

void FrozenSnapshotSurvivesOverwritesAndReserveAllowsCapture() {
  PooledRawFrameRing ring({
      .active_frame_capacity = 3,
      .reserve_frame_blocks = 3,
      .maximum_payload_bytes = 4,
  });
  Push(ring, 1);
  Push(ring, 2);
  Push(ring, 3);
  const auto frozen = ring.Freeze();

  for (std::uint64_t id = 4; id <= 20; ++id) {
    Push(ring, id);
  }

  assert(frozen.size() == 3);
  for (std::size_t index = 0; index < frozen.size(); ++index) {
    const std::uint64_t expected_id = index + 1;
    assert(frozen.at(index).metadata().frame_id == expected_id);
    assert(frozen.at(index).payload()[0] == std::byte(expected_id));
  }

  const auto current = ring.Freeze();
  assert(current.at(0).metadata().frame_id == 18);
  assert(current.at(1).metadata().frame_id == 19);
  assert(current.at(2).metadata().frame_id == 20);
}

void InsufficientReserveReportsExhaustionWithoutMutation() {
  PooledRawFrameRing ring({
      .active_frame_capacity = 3,
      .reserve_frame_blocks = 1,
      .maximum_payload_bytes = 4,
  });
  Push(ring, 1);
  Push(ring, 2);
  Push(ring, 3);
  const auto frozen = ring.Freeze();

  Push(ring, 4);
  const auto fifth_payload = PayloadFor(5);
  assert(ring.TryPush(MakeFrame(5, fifth_payload)) == PooledRawFramePushResult::kPoolExhausted);

  const auto current = ring.Freeze();
  assert(current.size() == 3);
  assert(current.at(0).metadata().frame_id == 2);
  assert(current.at(1).metadata().frame_id == 3);
  assert(current.at(2).metadata().frame_id == 4);
  assert(frozen.at(0).metadata().frame_id == 1);
}

void ReleasingSnapshotMakesBlocksReusable() {
  PooledRawFrameRing ring({
      .active_frame_capacity = 2,
      .reserve_frame_blocks = 1,
      .maximum_payload_bytes = 4,
  });
  Push(ring, 1);
  Push(ring, 2);

  {
    const auto frozen = ring.Freeze();
    Push(ring, 3);
    const auto fourth_payload = PayloadFor(4);
    assert(ring.TryPush(MakeFrame(4, fourth_payload)) == PooledRawFramePushResult::kPoolExhausted);
    assert(frozen.at(0).metadata().frame_id == 1);
  }

  Push(ring, 4);
  const auto current = ring.Freeze();
  assert(current.at(0).metadata().frame_id == 3);
  assert(current.at(1).metadata().frame_id == 4);
}

void RejectsIncompleteAndOversizedFramesWithoutMutation() {
  PooledRawFrameRing ring({
      .active_frame_capacity = 2,
      .reserve_frame_blocks = 1,
      .maximum_payload_bytes = 4,
  });
  const auto valid_payload = PayloadFor(1);
  FrameView incomplete = MakeFrame(1, valid_payload);
  incomplete.metadata.complete = false;
  assert(ring.TryPush(incomplete) == PooledRawFramePushResult::kIncompleteFrame);

  const std::array<std::byte, 5> oversized = {};
  FrameView oversized_frame = MakeFrame(2, valid_payload);
  oversized_frame.payload = oversized;
  assert(ring.TryPush(oversized_frame) == PooledRawFramePushResult::kPayloadTooLarge);
  assert(ring.size() == 0);
  assert(ring.Freeze().empty());
}

void ReportsPreallocatedMemory() {
  PooledRawFrameRing ring({
      .active_frame_capacity = 7,
      .reserve_frame_blocks = 5,
      .maximum_payload_bytes = 1024,
  });
  assert(ring.capacity() == 7);
  assert(ring.reserve_block_count() == 5);
  assert(ring.block_count() == 12);
  assert(ring.maximum_payload_bytes() == 1024);
  assert(ring.payload_storage_bytes() == 12 * 1024);
  assert(ring.control_storage_bytes() > 0);
  assert(ring.allocated_bytes() == ring.payload_storage_bytes() + ring.control_storage_bytes());
}

void HandleKeepsPoolStorageAlive() {
  const PooledRawFrameHandle handle = [] {
    PooledRawFrameRing ring({
        .active_frame_capacity = 2,
        .reserve_frame_blocks = 0,
        .maximum_payload_bytes = 4,
    });
    Push(ring, 42);
    const auto snapshot = ring.Freeze();
    return snapshot.at(0);
  }();

  assert(handle.metadata().frame_id == 42);
  assert(handle.payload()[0] == std::byte{42});
}

void RejectsInvalidAndOverflowingDimensions() {
  bool rejected_zero = false;
  try {
    PooledRawFrameRing ring({
        .active_frame_capacity = 0,
        .reserve_frame_blocks = 0,
        .maximum_payload_bytes = 4,
    });
  } catch (const std::invalid_argument &) {
    rejected_zero = true;
  }
  assert(rejected_zero);

  bool rejected_block_overflow = false;
  try {
    PooledRawFrameRing ring({
        .active_frame_capacity = std::numeric_limits<std::size_t>::max(),
        .reserve_frame_blocks = 1,
        .maximum_payload_bytes = 1,
    });
  } catch (const std::overflow_error &) {
    rejected_block_overflow = true;
  }
  assert(rejected_block_overflow);

  bool rejected_payload_overflow = false;
  try {
    PooledRawFrameRing ring({
        .active_frame_capacity = 2,
        .reserve_frame_blocks = 0,
        .maximum_payload_bytes = std::numeric_limits<std::size_t>::max(),
    });
  } catch (const std::overflow_error &) {
    rejected_payload_overflow = true;
  }
  assert(rejected_payload_overflow);
}

void SnapshotIsSafeDuringSingleProducerCapture() {
  PooledRawFrameRing ring({
      .active_frame_capacity = 8,
      .reserve_frame_blocks = 8,
      .maximum_payload_bytes = 4,
  });
  std::atomic<bool> producer_done = false;
  std::thread producer([&] {
    for (std::uint64_t id = 1; id <= 20000; ++id) {
      const auto payload = PayloadFor(id);
      while (ring.TryPush(MakeFrame(id, payload)) == PooledRawFramePushResult::kPoolExhausted) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  do {
    const auto snapshot = ring.Freeze();
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
      const auto &frame = snapshot.at(index);
      if (index != 0) {
        assert(frame.metadata().frame_id == snapshot.at(index - 1).metadata().frame_id + 1);
      }
      assert(frame.payload()[0] == std::byte(frame.metadata().frame_id & 0xffU));
    }
  } while (!producer_done.load(std::memory_order_acquire));

  producer.join();
}

}  // namespace

int main() {
  PreservesChronologicalOrderingAcrossWrap();
  FrozenSnapshotSurvivesOverwritesAndReserveAllowsCapture();
  InsufficientReserveReportsExhaustionWithoutMutation();
  ReleasingSnapshotMakesBlocksReusable();
  RejectsIncompleteAndOversizedFramesWithoutMutation();
  ReportsPreallocatedMemory();
  HandleKeepsPoolStorageAlive();
  RejectsInvalidAndOverflowingDimensions();
  SnapshotIsSafeDuringSingleProducerCapture();
  return 0;
}
