#ifndef SWING_CAPTURE_CAPTURE_CORE_POOLED_RAW_FRAME_RING_H_
#define SWING_CAPTURE_CAPTURE_CORE_POOLED_RAW_FRAME_RING_H_

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "capture/core/camera_source.h"

namespace swing_capture {

namespace detail {
struct PooledRawFrameRingState;
}  // namespace detail

enum class PooledRawFramePushResult {
  kStored,
  kPoolExhausted,
  kIncompleteFrame,
  kPayloadTooLarge,
};

struct PooledRawFrameRingConfig {
  std::size_t active_frame_capacity;
  std::size_t reserve_frame_blocks;
  std::size_t maximum_payload_bytes;
};

// An immutable reference to one retained frame. Copies share the pool block,
// and the block cannot be reused until every handle has been released.
class PooledRawFrameHandle final {
 public:
  PooledRawFrameHandle(const PooledRawFrameHandle &other) noexcept;
  PooledRawFrameHandle &operator=(const PooledRawFrameHandle &other) noexcept;
  PooledRawFrameHandle(PooledRawFrameHandle &&other) noexcept;
  PooledRawFrameHandle &operator=(PooledRawFrameHandle &&other) noexcept;
  ~PooledRawFrameHandle();

  [[nodiscard]] const FrameMetadata &metadata() const noexcept;
  [[nodiscard]] std::span<const std::byte> payload() const noexcept;
  [[nodiscard]] FrameView view() const noexcept;

 private:
  friend class PooledRawFrameRing;

  struct AdoptReferenceTag {};

  PooledRawFrameHandle(std::shared_ptr<detail::PooledRawFrameRingState> state,
                       std::size_t block_index, AdoptReferenceTag adopt_reference) noexcept;

  void AddReference() noexcept;
  void ReleaseReference() noexcept;

  std::shared_ptr<detail::PooledRawFrameRingState> state_;
  std::size_t block_index_ = 0;
};

class PooledRawFrameSnapshot final {
 public:
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] const PooledRawFrameHandle &at(std::size_t chronological_index) const;
  [[nodiscard]] std::span<const PooledRawFrameHandle> frames() const noexcept;

 private:
  friend class PooledRawFrameRing;

  explicit PooledRawFrameSnapshot(std::vector<PooledRawFrameHandle> frames) noexcept;

  std::vector<PooledRawFrameHandle> frames_;
};

// A single-producer raw-frame ring whose snapshots do not copy payloads.
//
// All payload blocks and ring control arrays are allocated in the constructor.
// TryPush copies the payload exactly once and never allocates or waits for a
// snapshot to release a block. Freeze may allocate its vector of handles and
// is safe to call concurrently with the single capture producer.
//
// Reserve blocks let capture replace frames that are still held by a frozen
// snapshot. Holding one full-capacity snapshot while capturing indefinitely
// requires at least active_frame_capacity reserve blocks.
class PooledRawFrameRing final {
 public:
  explicit PooledRawFrameRing(PooledRawFrameRingConfig config);
  ~PooledRawFrameRing();

  PooledRawFrameRing(const PooledRawFrameRing &) = delete;
  PooledRawFrameRing &operator=(const PooledRawFrameRing &) = delete;
  PooledRawFrameRing(PooledRawFrameRing &&) = delete;
  PooledRawFrameRing &operator=(PooledRawFrameRing &&) = delete;

  [[nodiscard]] PooledRawFramePushResult TryPush(const FrameView &frame) noexcept;
  [[nodiscard]] PooledRawFrameSnapshot Freeze() const;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;
  [[nodiscard]] std::size_t reserve_block_count() const noexcept;
  [[nodiscard]] std::size_t block_count() const noexcept;
  [[nodiscard]] std::size_t maximum_payload_bytes() const noexcept;

  // These counts cover the explicitly preallocated payload and control arrays.
  // Allocator bookkeeping and the small shared-state object are not included.
  [[nodiscard]] std::size_t payload_storage_bytes() const noexcept;
  [[nodiscard]] std::size_t control_storage_bytes() const noexcept;
  [[nodiscard]] std::size_t allocated_bytes() const noexcept;

 private:
  std::shared_ptr<detail::PooledRawFrameRingState> state_;
  std::size_t next_pool_candidate_ = 0;
};

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_CORE_POOLED_RAW_FRAME_RING_H_
