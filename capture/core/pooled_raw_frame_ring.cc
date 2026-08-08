#include "capture/core/pooled_raw_frame_ring.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "capture/core/camera_source.h"

namespace swing_capture {
namespace {

struct PooledRawFrameBlock {
  std::atomic<std::size_t> references{0};
  FrameMetadata metadata{};
  std::size_t payload_size = 0;
};

std::size_t CheckedAdd(std::size_t left, std::size_t right, const char *message) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    throw std::overflow_error(message);
  }
  return left + right;
}

std::size_t CheckedMultiply(std::size_t left, std::size_t right, const char *message) {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    throw std::overflow_error(message);
  }
  return left * right;
}

}  // namespace
namespace detail {

struct PooledRawFrameRingState {
  static constexpr std::size_t kUnusedBlock = std::numeric_limits<std::size_t>::max();

  explicit PooledRawFrameRingState(PooledRawFrameRingConfig config)
      : active_capacity(config.active_frame_capacity),
        reserve_blocks(config.reserve_frame_blocks),
        total_blocks(CheckedAdd(config.active_frame_capacity, config.reserve_frame_blocks,
                                "pooled raw frame block count overflows size_t")),
        maximum_payload(config.maximum_payload_bytes),
        payload_bytes(CheckedMultiply(total_blocks, config.maximum_payload_bytes,
                                      "pooled raw frame payload allocation overflows size_t")),
        block_control_bytes(CheckedMultiply(total_blocks, sizeof(PooledRawFrameBlock),
                                            "pooled raw frame block controls overflow size_t")),
        slot_control_bytes(CheckedMultiply(active_capacity, sizeof(std::atomic<std::size_t>),
                                           "pooled raw frame slot controls overflow size_t")),
        control_bytes(CheckedAdd(block_control_bytes, slot_control_bytes,
                                 "pooled raw frame controls overflow size_t")),
        total_allocated_bytes(CheckedAdd(payload_bytes, control_bytes,
                                         "pooled raw frame allocation overflows size_t")),
        payload_storage(payload_bytes),
        blocks(total_blocks),
        active_slots(active_capacity) {
    for (std::size_t slot = 0; slot < active_capacity; ++slot) {
      active_slots[slot].store(kUnusedBlock, std::memory_order_relaxed);
    }
  }

  const std::size_t active_capacity;
  const std::size_t reserve_blocks;
  const std::size_t total_blocks;
  const std::size_t maximum_payload;
  const std::size_t payload_bytes;
  const std::size_t block_control_bytes;
  const std::size_t slot_control_bytes;
  const std::size_t control_bytes;
  const std::size_t total_allocated_bytes;

  std::vector<std::byte> payload_storage;
  std::vector<PooledRawFrameBlock> blocks;
  std::vector<std::atomic<std::size_t>> active_slots;

  // Even values describe a stable ring. The single producer makes this odd
  // while publishing; Freeze retries if it observes a concurrent publication.
  std::atomic<std::uint64_t> publication_sequence{0};
  std::atomic<std::size_t> next_active_slot{0};
  std::atomic<std::size_t> active_size{0};
};

}  // namespace detail
namespace {

using detail::PooledRawFrameRingState;

void ReleaseBlockReference(PooledRawFrameRingState &state, std::size_t block_index) noexcept {
  PooledRawFrameBlock &block = state.blocks[block_index];
  const std::size_t previous = block.references.fetch_sub(1, std::memory_order_release);
  assert(previous != 0);
  if (previous == 1) {
    std::atomic_thread_fence(std::memory_order_acquire);
  }
}

bool TryAddBlockReference(PooledRawFrameRingState &state, std::size_t block_index) noexcept {
  PooledRawFrameBlock &block = state.blocks[block_index];
  std::size_t references = block.references.load(std::memory_order_acquire);
  while (references != 0 && references != std::numeric_limits<std::size_t>::max()) {
    if (block.references.compare_exchange_weak(
            references, references + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void CopyFrameIntoBlock(PooledRawFrameRingState &state, std::size_t block_index,
                        const FrameView &frame) noexcept {
  PooledRawFrameBlock &block = state.blocks[block_index];
  const std::span<std::byte> payload_storage(state.payload_storage);
  const std::span<std::byte> destination =
      payload_storage.subspan(block_index * state.maximum_payload, frame.payload.size());
  std::ranges::copy(frame.payload, destination.begin());
  block.metadata = frame.metadata;
  block.payload_size = frame.payload.size();
}

}  // namespace

PooledRawFrameHandle::PooledRawFrameHandle(std::shared_ptr<PooledRawFrameRingState> state,
                                           std::size_t block_index,
                                           AdoptReferenceTag adopt_reference) noexcept
    : state_(std::move(state)), block_index_(block_index) {
  static_cast<void>(adopt_reference);
}

PooledRawFrameHandle::PooledRawFrameHandle(const PooledRawFrameHandle &other) noexcept
    : state_(other.state_), block_index_(other.block_index_) {
  AddReference();
}

PooledRawFrameHandle &PooledRawFrameHandle::operator=(const PooledRawFrameHandle &other) noexcept {
  if (this == &other) {
    return *this;
  }
  ReleaseReference();
  state_ = other.state_;
  block_index_ = other.block_index_;
  AddReference();
  return *this;
}

PooledRawFrameHandle::PooledRawFrameHandle(PooledRawFrameHandle &&other) noexcept
    : state_(std::move(other.state_)), block_index_(other.block_index_) {}

PooledRawFrameHandle &PooledRawFrameHandle::operator=(PooledRawFrameHandle &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  ReleaseReference();
  state_ = std::move(other.state_);
  block_index_ = other.block_index_;
  return *this;
}

PooledRawFrameHandle::~PooledRawFrameHandle() { ReleaseReference(); }

const FrameMetadata &PooledRawFrameHandle::metadata() const noexcept {
  assert(state_ != nullptr);
  return state_->blocks[block_index_].metadata;
}

std::span<const std::byte> PooledRawFrameHandle::payload() const noexcept {
  assert(state_ != nullptr);
  const PooledRawFrameBlock &block = state_->blocks[block_index_];
  const std::span<const std::byte> payload_storage(state_->payload_storage);
  return payload_storage.subspan(block_index_ * state_->maximum_payload, block.payload_size);
}

FrameView PooledRawFrameHandle::view() const noexcept {
  return {.metadata = metadata(), .payload = payload()};
}

void PooledRawFrameHandle::AddReference() noexcept {
  if (state_ == nullptr) {
    return;
  }
  const bool added = TryAddBlockReference(*state_, block_index_);
  assert(added);
}

void PooledRawFrameHandle::ReleaseReference() noexcept {
  if (state_ == nullptr) {
    return;
  }
  ReleaseBlockReference(*state_, block_index_);
  state_.reset();
}

PooledRawFrameSnapshot::PooledRawFrameSnapshot(std::vector<PooledRawFrameHandle> frames) noexcept
    : frames_(std::move(frames)) {}

bool PooledRawFrameSnapshot::empty() const noexcept { return frames_.empty(); }

std::size_t PooledRawFrameSnapshot::size() const noexcept { return frames_.size(); }

const PooledRawFrameHandle &PooledRawFrameSnapshot::at(std::size_t chronological_index) const {
  return frames_.at(chronological_index);
}

std::span<const PooledRawFrameHandle> PooledRawFrameSnapshot::frames() const noexcept {
  return frames_;
}

PooledRawFrameRing::PooledRawFrameRing(PooledRawFrameRingConfig config) {
  if (config.active_frame_capacity == 0 || config.maximum_payload_bytes == 0) {
    throw std::invalid_argument("pooled raw frame ring dimensions must be nonzero");
  }
  if (config.reserve_frame_blocks >
      std::numeric_limits<std::size_t>::max() - config.active_frame_capacity) {
    throw std::overflow_error("pooled raw frame block count overflows size_t");
  }
  if (config.active_frame_capacity + config.reserve_frame_blocks ==
      PooledRawFrameRingState::kUnusedBlock) {
    throw std::overflow_error("pooled raw frame block count conflicts with sentinel");
  }
  state_ = std::make_shared<PooledRawFrameRingState>(config);
}

PooledRawFrameRing::~PooledRawFrameRing() {
  if (state_ == nullptr) {
    return;
  }

  state_->publication_sequence.fetch_add(1, std::memory_order_acq_rel);
  for (std::size_t slot = 0; slot < state_->active_capacity; ++slot) {
    const std::size_t block_index = state_->active_slots[slot].exchange(
        PooledRawFrameRingState::kUnusedBlock, std::memory_order_acq_rel);
    if (block_index != PooledRawFrameRingState::kUnusedBlock) {
      ReleaseBlockReference(*state_, block_index);
    }
  }
  state_->active_size.store(0, std::memory_order_relaxed);
  state_->next_active_slot.store(0, std::memory_order_relaxed);
  state_->publication_sequence.fetch_add(1, std::memory_order_release);
}

PooledRawFramePushResult PooledRawFrameRing::TryPush(const FrameView &frame) noexcept {
  if (!frame.metadata.complete) {
    return PooledRawFramePushResult::kIncompleteFrame;
  }
  if (frame.payload.size() > state_->maximum_payload) {
    return PooledRawFramePushResult::kPayloadTooLarge;
  }

  // Prefer an unused block. This lets a publication replace an active block
  // that remains pinned by one or more snapshots.
  std::size_t block_index = PooledRawFrameRingState::kUnusedBlock;
  std::size_t candidate = next_pool_candidate_;
  for (std::size_t checked = 0; checked < state_->total_blocks; ++checked) {
    std::size_t expected = 0;
    if (state_->blocks[candidate].references.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      block_index = candidate;
      next_pool_candidate_ = candidate + 1 == state_->total_blocks ? 0 : candidate + 1;
      break;
    }
    candidate = candidate + 1 == state_->total_blocks ? 0 : candidate + 1;
  }

  if (block_index != PooledRawFrameRingState::kUnusedBlock) {
    CopyFrameIntoBlock(*state_, block_index, frame);

    const std::uint64_t previous =
        state_->publication_sequence.fetch_add(1, std::memory_order_acq_rel);
    assert((previous & 1U) == 0);

    const std::size_t target_slot = state_->next_active_slot.load(std::memory_order_relaxed);
    const std::size_t replaced_block =
        state_->active_slots[target_slot].exchange(block_index, std::memory_order_release);
    state_->next_active_slot.store(target_slot + 1 == state_->active_capacity ? 0 : target_slot + 1,
                                   std::memory_order_relaxed);
    const std::size_t old_size = state_->active_size.load(std::memory_order_relaxed);
    if (old_size < state_->active_capacity) {
      state_->active_size.store(old_size + 1, std::memory_order_relaxed);
    }
    state_->publication_sequence.fetch_add(1, std::memory_order_release);

    if (replaced_block != PooledRawFrameRingState::kUnusedBlock) {
      ReleaseBlockReference(*state_, replaced_block);
    }
    return PooledRawFramePushResult::kStored;
  }

  // No unused block exists. Reuse the block in the slot that would be
  // overwritten, but only if the active ring owns its sole reference. The
  // odd publication sequence prevents a new snapshot from observing the
  // temporarily removed slot.
  const std::uint64_t previous =
      state_->publication_sequence.fetch_add(1, std::memory_order_acq_rel);
  assert((previous & 1U) == 0);

  const std::size_t target_slot = state_->next_active_slot.load(std::memory_order_relaxed);
  const std::size_t reusable_block = state_->active_slots[target_slot].exchange(
      PooledRawFrameRingState::kUnusedBlock, std::memory_order_acq_rel);

  if (reusable_block == PooledRawFrameRingState::kUnusedBlock) {
    state_->publication_sequence.fetch_add(1, std::memory_order_release);
    return PooledRawFramePushResult::kPoolExhausted;
  }

  ReleaseBlockReference(*state_, reusable_block);
  std::size_t expected = 0;
  if (!state_->blocks[reusable_block].references.compare_exchange_strong(
          expected, 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    state_->blocks[reusable_block].references.fetch_add(1, std::memory_order_relaxed);
    state_->active_slots[target_slot].store(reusable_block, std::memory_order_release);
    state_->publication_sequence.fetch_add(1, std::memory_order_release);
    return PooledRawFramePushResult::kPoolExhausted;
  }

  CopyFrameIntoBlock(*state_, reusable_block, frame);
  state_->active_slots[target_slot].store(reusable_block, std::memory_order_release);
  state_->next_active_slot.store(target_slot + 1 == state_->active_capacity ? 0 : target_slot + 1,
                                 std::memory_order_relaxed);
  state_->publication_sequence.fetch_add(1, std::memory_order_release);
  return PooledRawFramePushResult::kStored;
}

PooledRawFrameSnapshot PooledRawFrameRing::Freeze() const {
  const std::size_t active_capacity = state_->active_capacity;
  if (active_capacity == 0) {
    throw std::logic_error("pooled raw frame ring has zero active capacity");
  }

  std::vector<PooledRawFrameHandle> handles;
  handles.reserve(active_capacity);

  std::size_t failed_attempts = 0;
  for (;;) {
    handles.clear();

    const std::uint64_t sequence_before =
        state_->publication_sequence.load(std::memory_order_acquire);
    if ((sequence_before & 1U) != 0) {
      if ((++failed_attempts % 64) == 0) {
        std::this_thread::yield();
      }
      continue;
    }

    const std::size_t active_size = state_->active_size.load(std::memory_order_relaxed);
    const std::size_t next_slot = state_->next_active_slot.load(std::memory_order_relaxed);
    const std::size_t oldest_slot = active_size == active_capacity ? next_slot : 0;

    bool acquired_every_frame = true;
    for (std::size_t index = 0; index < active_size; ++index) {
      const std::size_t slot = (oldest_slot + index) % active_capacity;
      const std::size_t block_index = state_->active_slots[slot].load(std::memory_order_acquire);
      if (block_index == PooledRawFrameRingState::kUnusedBlock ||
          !TryAddBlockReference(*state_, block_index)) {
        acquired_every_frame = false;
        break;
      }
      handles.push_back(
          PooledRawFrameHandle(state_, block_index, PooledRawFrameHandle::AdoptReferenceTag{}));
    }

    const std::uint64_t sequence_after =
        state_->publication_sequence.load(std::memory_order_acquire);
    if (acquired_every_frame && sequence_before == sequence_after) {
      return PooledRawFrameSnapshot(std::move(handles));
    }

    if ((++failed_attempts % 64) == 0) {
      std::this_thread::yield();
    }
  }
}

std::size_t PooledRawFrameRing::size() const noexcept {
  return state_->active_size.load(std::memory_order_acquire);
}

std::size_t PooledRawFrameRing::capacity() const noexcept { return state_->active_capacity; }

std::size_t PooledRawFrameRing::reserve_block_count() const noexcept {
  return state_->reserve_blocks;
}

std::size_t PooledRawFrameRing::block_count() const noexcept { return state_->total_blocks; }

std::size_t PooledRawFrameRing::maximum_payload_bytes() const noexcept {
  return state_->maximum_payload;
}

std::size_t PooledRawFrameRing::payload_storage_bytes() const noexcept {
  return state_->payload_bytes;
}

std::size_t PooledRawFrameRing::control_storage_bytes() const noexcept {
  return state_->control_bytes;
}

std::size_t PooledRawFrameRing::allocated_bytes() const noexcept {
  return state_->total_allocated_bytes;
}

}  // namespace swing_capture
