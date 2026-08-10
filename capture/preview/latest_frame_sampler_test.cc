#include "capture/preview/latest_frame_sampler.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>

#include "capture/core/camera_source.h"

namespace {

using swing_capture::FrameMetadata;
using swing_capture::FrameView;
using swing_capture::preview::LatestFrameSampler;
using swing_capture::preview::PreviewFramePublishResult;

FrameView MakeFrame(std::uint64_t frame_id, std::chrono::steady_clock::time_point received_at,
                    const std::array<std::byte, 4> &payload) {
  return {
      .metadata =
          FrameMetadata{
              .frame_id = frame_id,
              .device_timestamp = frame_id * 100U,
              .host_received_at = received_at,
              .width = 2,
              .height = 2,
              .complete = true,
          },
      .payload = payload,
  };
}

std::array<std::byte, 4> PayloadFor(std::uint64_t frame_id) {
  return {
      std::byte(frame_id & 0xffU),
      std::byte((frame_id + 1U) & 0xffU),
      std::byte((frame_id + 2U) & 0xffU),
      std::byte((frame_id + 3U) & 0xffU),
  };
}

void RateLimitsBeforeCopyingAndPreservesMetadata() {
  using namespace std::chrono_literals;
  LatestFrameSampler sampler({.minimum_interval = 200ms, .maximum_payload_bytes = 4});
  const auto start = std::chrono::steady_clock::time_point(5s);
  auto first_payload = PayloadFor(10);
  auto too_soon_payload = PayloadFor(11);
  const auto on_cadence_payload = PayloadFor(12);

  assert(sampler.TryPublish(MakeFrame(10, start, first_payload)) ==
         PreviewFramePublishResult::kPublished);
  first_payload[0] = std::byte{99};
  assert(sampler.TryPublish(MakeFrame(11, start + 199ms, too_soon_payload)) ==
         PreviewFramePublishResult::kRateLimited);
  too_soon_payload[0] = std::byte{88};

  const auto first = sampler.Latest();
  assert(first != nullptr);
  assert(first->metadata.frame_id == 10);
  assert(first->metadata.device_timestamp == 1000);
  assert(first->metadata.host_received_at == start);
  assert(first->preview_sequence == 1);
  assert(first->bayer_pixels[0] == std::byte{10});

  assert(sampler.TryPublish(MakeFrame(12, start + 200ms, on_cadence_payload)) ==
         PreviewFramePublishResult::kPublished);
  const auto current = sampler.Latest();
  assert(current->metadata.frame_id == 12);
  assert(current->preview_sequence == 2);
}

void OverwriteKeepsOlderSnapshotsImmutable() {
  using namespace std::chrono_literals;
  LatestFrameSampler sampler({.minimum_interval = 1ns, .maximum_payload_bytes = 4});
  const auto start = std::chrono::steady_clock::time_point{};
  const auto first_payload = PayloadFor(1);
  const auto second_payload = PayloadFor(2);

  assert(sampler.Latest() == nullptr);
  assert(sampler.TryPublish(MakeFrame(1, start, first_payload)) ==
         PreviewFramePublishResult::kPublished);
  const auto first = sampler.Latest();
  assert(sampler.TryPublish(MakeFrame(2, start + 1ns, second_payload)) ==
         PreviewFramePublishResult::kPublished);
  const auto second = sampler.Latest();

  assert(first->metadata.frame_id == 1);
  assert(first->bayer_pixels[0] == std::byte{1});
  assert(second->metadata.frame_id == 2);
  assert(second->bayer_pixels[0] == std::byte{2});
  assert(first.get() != second.get());
}

void ResetInvalidatesLatestWithoutReusingSequence() {
  using namespace std::chrono_literals;
  LatestFrameSampler sampler({.minimum_interval = 1s, .maximum_payload_bytes = 4});
  const auto start = std::chrono::steady_clock::time_point{};
  const auto first_payload = PayloadFor(1);
  const auto second_payload = PayloadFor(2);

  assert(sampler.TryPublish(MakeFrame(1, start, first_payload)) ==
         PreviewFramePublishResult::kPublished);
  const auto retained_before_reset = sampler.Latest();
  sampler.Reset();
  assert(sampler.Latest() == nullptr);
  assert(retained_before_reset->metadata.frame_id == 1);

  // Reset also clears the cadence deadline, but not the cache-buster sequence.
  assert(sampler.TryPublish(MakeFrame(2, start + 1ns, second_payload)) ==
         PreviewFramePublishResult::kPublished);
  const auto after_reset = sampler.Latest();
  assert(after_reset->metadata.frame_id == 2);
  assert(after_reset->preview_sequence == 2);
}

void ChangesCadenceWithoutResettingTheLatestFrame() {
  using namespace std::chrono_literals;
  LatestFrameSampler sampler({.minimum_interval = 33ms, .maximum_payload_bytes = 4});
  const auto start = std::chrono::steady_clock::time_point(5s);
  const auto first_payload = PayloadFor(1);
  const auto second_payload = PayloadFor(2);
  const auto third_payload = PayloadFor(3);

  assert(sampler.TryPublish(MakeFrame(1, start, first_payload)) ==
         PreviewFramePublishResult::kPublished);
  assert(sampler.TryPublish(MakeFrame(2, start + 8ms, second_payload)) ==
         PreviewFramePublishResult::kRateLimited);
  sampler.SetMinimumInterval(8ms);
  assert(sampler.minimum_interval() == 8ms);
  assert(sampler.TryPublish(MakeFrame(2, start + 8ms, second_payload)) ==
         PreviewFramePublishResult::kPublished);
  assert(sampler.Latest()->preview_sequence == 2U);

  sampler.SetMinimumInterval(33ms);
  assert(sampler.Latest()->metadata.frame_id == 2U);
  assert(sampler.TryPublish(MakeFrame(3, start + 40ms, third_payload)) ==
         PreviewFramePublishResult::kRateLimited);
  assert(sampler.TryPublish(MakeFrame(3, start + 41ms, third_payload)) ==
         PreviewFramePublishResult::kPublished);

  bool rejected = false;
  try {
    sampler.SetMinimumInterval(0ns);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
  assert(sampler.minimum_interval() == 33ms);
}

void RejectsInvalidFramesAndConfiguration() {
  using namespace std::chrono_literals;
  bool rejected_interval = false;
  try {
    LatestFrameSampler sampler({.minimum_interval = 0ns, .maximum_payload_bytes = 4});
  } catch (const std::invalid_argument &) {
    rejected_interval = true;
  }
  assert(rejected_interval);

  bool rejected_capacity = false;
  try {
    LatestFrameSampler sampler({.minimum_interval = 1ns, .maximum_payload_bytes = 0});
  } catch (const std::invalid_argument &) {
    rejected_capacity = true;
  }
  assert(rejected_capacity);

  LatestFrameSampler sampler({.minimum_interval = 1ns, .maximum_payload_bytes = 4});
  const auto payload = PayloadFor(1);
  FrameView incomplete = MakeFrame(1, std::chrono::steady_clock::time_point{}, payload);
  incomplete.metadata.complete = false;
  assert(sampler.TryPublish(incomplete) == PreviewFramePublishResult::kIncompleteFrame);

  FrameView zero_width = MakeFrame(1, std::chrono::steady_clock::time_point{}, payload);
  zero_width.metadata.width = 0;
  assert(sampler.TryPublish(zero_width) == PreviewFramePublishResult::kInvalidDimensions);

  const std::array<std::byte, 3> too_short{};
  FrameView mismatched = MakeFrame(1, std::chrono::steady_clock::time_point{}, payload);
  mismatched.payload = too_short;
  assert(sampler.TryPublish(mismatched) == PreviewFramePublishResult::kPayloadSizeMismatch);

  const std::array<std::byte, 6> oversized{};
  FrameView too_large = {
      .metadata =
          FrameMetadata{
              .frame_id = 1,
              .device_timestamp = 0,
              .host_received_at = {},
              .width = 3,
              .height = 2,
              .complete = true,
          },
      .payload = oversized,
  };
  assert(sampler.TryPublish(too_large) == PreviewFramePublishResult::kPayloadTooLarge);
  assert(sampler.Latest() == nullptr);
}

void SnapshotReadsAreSafeDuringPublication() {
  using namespace std::chrono_literals;
  LatestFrameSampler sampler({.minimum_interval = 1ns, .maximum_payload_bytes = 4});
  constexpr std::uint64_t kFrameCount = 5000;
  std::atomic<bool> producer_done = false;
  std::thread producer([&] {
    for (std::uint64_t frame_id = 1; frame_id <= kFrameCount; ++frame_id) {
      const auto payload = PayloadFor(frame_id);
      const auto frame_time =
          std::chrono::steady_clock::time_point(std::chrono::nanoseconds(frame_id));
      assert(sampler.TryPublish(MakeFrame(frame_id, frame_time, payload)) ==
             PreviewFramePublishResult::kPublished);
    }
    producer_done.store(true, std::memory_order_release);
  });

  do {
    const auto snapshot = sampler.Latest();
    if (snapshot != nullptr) {
      assert(snapshot->preview_sequence == snapshot->metadata.frame_id);
      assert(snapshot->bayer_pixels[0] == std::byte(snapshot->metadata.frame_id & 0xffU));
      std::this_thread::yield();
      assert(snapshot->bayer_pixels[0] == std::byte(snapshot->metadata.frame_id & 0xffU));
    }
  } while (!producer_done.load(std::memory_order_acquire));
  producer.join();

  const auto latest = sampler.Latest();
  assert(latest->metadata.frame_id == kFrameCount);
}

void CadenceChangesAreSafeDuringPublication() {
  using namespace std::chrono_literals;
  LatestFrameSampler sampler({.minimum_interval = 33ms, .maximum_payload_bytes = 4});
  constexpr std::uint64_t kFrameCount = 2000;
  std::atomic<bool> producer_done = false;
  std::thread producer([&] {
    for (std::uint64_t frame_id = 1; frame_id <= kFrameCount; ++frame_id) {
      const auto payload = PayloadFor(frame_id);
      const auto frame_time =
          std::chrono::steady_clock::time_point(std::chrono::milliseconds(frame_id));
      const auto result = sampler.TryPublish(MakeFrame(frame_id, frame_time, payload));
      assert(result == PreviewFramePublishResult::kPublished ||
             result == PreviewFramePublishResult::kRateLimited);
    }
    producer_done.store(true, std::memory_order_release);
  });
  std::thread cadence_changer([&] {
    while (!producer_done.load(std::memory_order_acquire)) {
      sampler.SetMinimumInterval(8ms);
      sampler.SetMinimumInterval(33ms);
    }
  });
  producer.join();
  cadence_changer.join();

  const auto latest = sampler.Latest();
  assert(latest != nullptr);
  assert(latest->metadata.frame_id <= kFrameCount);
  assert(sampler.minimum_interval() == 33ms);
}

}  // namespace

int main() {
  RateLimitsBeforeCopyingAndPreservesMetadata();
  OverwriteKeepsOlderSnapshotsImmutable();
  ResetInvalidatesLatestWithoutReusingSequence();
  ChangesCadenceWithoutResettingTheLatestFrame();
  RejectsInvalidFramesAndConfiguration();
  SnapshotReadsAreSafeDuringPublication();
  CadenceChangesAreSafeDuringPublication();
  return 0;
}
