#include "capture/application/camera_clip_buffer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "capture/core/camera_source.h"
#include "capture/core/pooled_raw_frame_ring.h"

namespace swing_capture::application {
namespace {

std::string_view PushFailureMessage(PooledRawFramePushResult result) {
  switch (result) {
    case PooledRawFramePushResult::kStored:
      return "none";
    case PooledRawFramePushResult::kPoolExhausted:
      return "camera clip buffer exhausted its preallocated pool";
    case PooledRawFramePushResult::kIncompleteFrame:
      return "camera clip buffer received an incomplete frame";
    case PooledRawFramePushResult::kPayloadTooLarge:
      return "camera clip frame exceeds the configured payload capacity";
  }
  return "camera clip buffer rejected a frame";
}

}  // namespace

CameraClipBuffer::CameraClipBuffer(CameraClipBufferConfig config) : config_(config) {
  if (config_.active_frame_capacity == 0 || config_.maximum_payload_bytes == 0) {
    throw std::invalid_argument("camera clip buffer capacities must be positive");
  }
}

void CameraClipBuffer::Arm() {
  const std::scoped_lock lock(lifecycle_mutex_);
  if (active_ring_.load(std::memory_order_acquire) != nullptr) {
    throw std::logic_error("camera clip buffer is already armed");
  }
  const PooledRawFrameRingConfig ring_config = {
      .active_frame_capacity = config_.active_frame_capacity,
      .reserve_frame_blocks = config_.reserve_frame_blocks,
      .maximum_payload_bytes = config_.maximum_payload_bytes,
  };
  auto ring = std::make_shared<PooledRawFrameRing>(ring_config);
  auto standby = std::make_shared<PooledRawFrameRing>(ring_config);
  frames_observed_.store(0, std::memory_order_relaxed);
  frames_stored_.store(0, std::memory_order_relaxed);
  failures_.store(0, std::memory_order_relaxed);
  generation_.fetch_add(1, std::memory_order_relaxed);
  standby_ring_ = std::move(standby);
  retired_ring_.reset();
  active_ring_.store(std::move(ring), std::memory_order_release);
}

void CameraClipBuffer::Disarm() noexcept {
  const std::scoped_lock lock(lifecycle_mutex_);
  active_ring_.store(nullptr, std::memory_order_release);
  standby_ring_.reset();
  retired_ring_.reset();
}

void CameraClipBuffer::ObserveFrame(const FrameView &frame) {
  const std::shared_ptr<PooledRawFrameRing> ring = active_ring_.load(std::memory_order_acquire);
  if (ring == nullptr) {
    return;
  }
  frames_observed_.fetch_add(1, std::memory_order_relaxed);
  const PooledRawFramePushResult result = ring->TryPush(frame);
  if (result == PooledRawFramePushResult::kStored) {
    frames_stored_.fetch_add(1, std::memory_order_release);
    return;
  }
  failures_.fetch_add(1, std::memory_order_release);
  throw std::runtime_error(std::string(PushFailureMessage(result)));
}

PooledRawFrameSnapshot CameraClipBuffer::Freeze() const {
  const std::shared_ptr<PooledRawFrameRing> ring = active_ring_.load(std::memory_order_acquire);
  if (ring == nullptr) {
    throw std::logic_error("camera clip buffer is not armed");
  }
  return ring->Freeze();
}

PooledRawFrameSnapshot CameraClipBuffer::FreezeAndRotate() {
  const std::scoped_lock lock(lifecycle_mutex_);
  if (active_ring_.load(std::memory_order_acquire) == nullptr) {
    throw std::logic_error("camera clip buffer is not armed");
  }
  if (standby_ring_ == nullptr || retired_ring_ != nullptr) {
    throw std::logic_error("camera clip buffer has no prepared standby generation");
  }
  const std::shared_ptr<PooledRawFrameRing> retired =
      active_ring_.exchange(std::exchange(standby_ring_, nullptr), std::memory_order_acq_rel);
  if (retired == nullptr) {
    throw std::logic_error("camera clip buffer was disarmed while rotating");
  }
  retired_ring_ = retired;
  generation_.fetch_add(1, std::memory_order_relaxed);
  return retired->Freeze();
}

void CameraClipBuffer::RecycleRetiredRing() {
  const std::scoped_lock lock(lifecycle_mutex_);
  if (active_ring_.load(std::memory_order_acquire) == nullptr || retired_ring_ == nullptr ||
      standby_ring_ != nullptr) {
    throw std::logic_error("camera clip buffer has no retired generation to recycle");
  }
  retired_ring_->Reset();
  standby_ring_ = std::move(retired_ring_);
}

CameraClipBufferStatus CameraClipBuffer::Status() const noexcept {
  const std::shared_ptr<PooledRawFrameRing> ring = active_ring_.load(std::memory_order_acquire);
  return {
      .armed = ring != nullptr,
      .generation = generation_.load(std::memory_order_relaxed),
      .frames_observed = frames_observed_.load(std::memory_order_relaxed),
      .frames_stored = frames_stored_.load(std::memory_order_acquire),
      .failures = failures_.load(std::memory_order_acquire),
      .retained_frames = ring == nullptr ? 0U : ring->size(),
      .capacity_frames = config_.active_frame_capacity,
      .reserve_frames = config_.reserve_frame_blocks,
      .allocated_bytes = ring == nullptr ? 0U : ring->allocated_bytes() * 2U,
  };
}

}  // namespace swing_capture::application
