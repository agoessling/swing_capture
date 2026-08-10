#ifndef SWING_CAPTURE_CAPTURE_APPLICATION_CAMERA_CLIP_BUFFER_H_
#define SWING_CAPTURE_CAPTURE_APPLICATION_CAMERA_CLIP_BUFFER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "capture/core/camera_source.h"
#include "capture/core/pooled_raw_frame_ring.h"

namespace swing_capture::application {

struct CameraClipBufferConfig {
  // Covers the default 1.4 s pre-roll plus 0.5 s post-roll at 227 fps with
  // margin for free-running phase and the post-roll boundary wait.
  std::size_t active_frame_capacity = 448;
  std::size_t reserve_frame_blocks = 0;
  std::size_t maximum_payload_bytes = static_cast<std::size_t>(1440U) * 1080U;
};

struct CameraClipBufferStatus {
  bool armed = false;
  std::uint64_t generation = 0;
  std::uint64_t frames_observed = 0;
  std::uint64_t frames_stored = 0;
  std::uint64_t failures = 0;
  std::size_t retained_frames = 0;
  std::size_t capacity_frames = 0;
  std::size_t reserve_frames = 0;
  std::size_t allocated_bytes = 0;
};

// A single-camera, preallocated capture buffer that can be armed and replaced
// without stopping the camera owner thread. ObserveFrame is called only by
// that owner. Freeze and lifecycle methods may run on a control/encoding
// thread. A snapshot keeps its old pool alive across a later disarm or re-arm.
class CameraClipBuffer final {
 public:
  explicit CameraClipBuffer(CameraClipBufferConfig config = {});
  ~CameraClipBuffer() = default;

  CameraClipBuffer(const CameraClipBuffer &) = delete;
  CameraClipBuffer &operator=(const CameraClipBuffer &) = delete;
  CameraClipBuffer(CameraClipBuffer &&) = delete;
  CameraClipBuffer &operator=(CameraClipBuffer &&) = delete;

  void Arm();
  void Disarm() noexcept;
  void ObserveFrame(const FrameView &frame);

  [[nodiscard]] PooledRawFrameSnapshot Freeze() const;
  // Atomically directs subsequent camera frames to a fresh preallocated ring,
  // then freezes the retired generation. Encoding can retain the old window
  // indefinitely without consuming blocks needed by live capture.
  [[nodiscard]] PooledRawFrameSnapshot FreezeAndRotate();
  // Recycles the retired generation after every handle in the returned
  // snapshot has been released by the publisher.
  void RecycleRetiredRing();
  [[nodiscard]] CameraClipBufferStatus Status() const noexcept;

 private:
  CameraClipBufferConfig config_;
  mutable std::mutex lifecycle_mutex_;
  std::atomic<std::shared_ptr<PooledRawFrameRing>> active_ring_;
  std::shared_ptr<PooledRawFrameRing> standby_ring_;
  std::shared_ptr<PooledRawFrameRing> retired_ring_;
  std::atomic<std::uint64_t> generation_ = 0;
  std::atomic<std::uint64_t> frames_observed_ = 0;
  std::atomic<std::uint64_t> frames_stored_ = 0;
  std::atomic<std::uint64_t> failures_ = 0;
};

}  // namespace swing_capture::application

#endif  // SWING_CAPTURE_CAPTURE_APPLICATION_CAMERA_CLIP_BUFFER_H_
