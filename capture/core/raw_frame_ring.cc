#include "capture/core/raw_frame_ring.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>

#include "capture/core/camera_source.h"

namespace swing_capture {

RawFrameRing::RawFrameRing(std::size_t frame_capacity, std::size_t maximum_payload_bytes)
    : frame_capacity_(frame_capacity), maximum_payload_bytes_(maximum_payload_bytes) {
  if (frame_capacity == 0 || maximum_payload_bytes == 0) {
    throw std::invalid_argument("raw frame ring dimensions must be nonzero");
  }
  if (frame_capacity > std::numeric_limits<std::size_t>::max() / maximum_payload_bytes) {
    throw std::overflow_error("raw frame ring allocation overflows size_t");
  }

  payload_storage_.resize(frame_capacity * maximum_payload_bytes);
  metadata_.resize(frame_capacity);
  payload_sizes_.resize(frame_capacity);
}

void RawFrameRing::Push(const FrameView &frame) {
  if (!frame.metadata.complete) {
    throw std::invalid_argument("cannot retain an incomplete frame");
  }
  if (frame.payload.size() > maximum_payload_bytes_) {
    throw std::length_error("frame payload exceeds ring slot size");
  }

  const std::span<std::byte> payload_storage(payload_storage_);
  const std::span<std::byte> destination =
      payload_storage.subspan(next_slot_ * maximum_payload_bytes_, frame.payload.size());
  std::ranges::copy(frame.payload, destination.begin());
  metadata_[next_slot_] = frame.metadata;
  payload_sizes_[next_slot_] = frame.payload.size();

  next_slot_ = (next_slot_ + 1) % frame_capacity_;
  size_ = std::min(size_ + 1, frame_capacity_);
}

std::size_t RawFrameRing::size() const { return size_; }

std::size_t RawFrameRing::capacity() const { return frame_capacity_; }

std::size_t RawFrameRing::maximum_payload_bytes() const { return maximum_payload_bytes_; }

std::size_t RawFrameRing::allocated_bytes() const {
  return (payload_storage_.size() * sizeof(payload_storage_.front())) +
         (metadata_.size() * sizeof(metadata_.front())) +
         (payload_sizes_.size() * sizeof(payload_sizes_.front()));
}

std::size_t RawFrameRing::SlotForChronologicalIndex(std::size_t chronological_index) const {
  if (chronological_index >= size_) {
    throw std::out_of_range("raw frame ring index");
  }
  const std::size_t oldest_slot = size_ == frame_capacity_ ? next_slot_ : 0;
  return (oldest_slot + chronological_index) % frame_capacity_;
}

FrameView RawFrameRing::at(std::size_t chronological_index) const {
  const std::size_t slot = SlotForChronologicalIndex(chronological_index);
  const std::span<const std::byte> payload_storage(payload_storage_);
  return {
      .metadata = metadata_[slot],
      .payload = payload_storage.subspan(slot * maximum_payload_bytes_, payload_sizes_[slot]),
  };
}

}  // namespace swing_capture
