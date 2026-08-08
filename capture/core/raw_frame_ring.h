#ifndef SWING_CAPTURE_CAPTURE_CORE_RAW_FRAME_RING_H_
#define SWING_CAPTURE_CAPTURE_CORE_RAW_FRAME_RING_H_

#include <cstddef>
#include <span>
#include <vector>

#include "capture/core/camera_source.h"

namespace swing_capture {

// A single-producer, fixed-memory ring for raw camera frames. Storage is
// allocated once in the constructor; Push performs one payload copy and no
// allocation. Views are invalidated when their slot is overwritten.
class RawFrameRing final {
 public:
  RawFrameRing(std::size_t frame_capacity, std::size_t maximum_payload_bytes);
  ~RawFrameRing() = default;

  RawFrameRing(const RawFrameRing &) = delete;
  RawFrameRing &operator=(const RawFrameRing &) = delete;
  RawFrameRing(RawFrameRing &&) = delete;
  RawFrameRing &operator=(RawFrameRing &&) = delete;

  void Push(const FrameView &frame);

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t capacity() const;
  [[nodiscard]] std::size_t maximum_payload_bytes() const;
  [[nodiscard]] std::size_t allocated_bytes() const;

  // Index zero is the oldest retained frame.
  [[nodiscard]] FrameView at(std::size_t chronological_index) const;

 private:
  [[nodiscard]] std::size_t SlotForChronologicalIndex(std::size_t chronological_index) const;

  std::size_t frame_capacity_;
  std::size_t maximum_payload_bytes_;
  std::vector<std::byte> payload_storage_;
  std::vector<FrameMetadata> metadata_;
  std::vector<std::size_t> payload_sizes_;
  std::size_t next_slot_ = 0;
  std::size_t size_ = 0;
};

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_CORE_RAW_FRAME_RING_H_
