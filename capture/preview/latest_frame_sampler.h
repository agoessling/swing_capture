#ifndef SWING_CAPTURE_CAPTURE_PREVIEW_LATEST_FRAME_SAMPLER_H_
#define SWING_CAPTURE_CAPTURE_PREVIEW_LATEST_FRAME_SAMPLER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "capture/core/camera_source.h"

namespace swing_capture::preview {

struct SampledPreviewFrame {
  FrameMetadata metadata;
  std::uint64_t preview_sequence = 0;
  std::vector<std::byte> bayer_pixels;
};

struct LatestFrameSamplerConfig {
  std::chrono::steady_clock::duration minimum_interval;
  std::size_t maximum_payload_bytes;
};

enum class PreviewFramePublishResult {
  kPublished,
  kRateLimited,
  kIncompleteFrame,
  kInvalidDimensions,
  kPayloadSizeMismatch,
  kPayloadTooLarge,
};

// Retains at most one sampled Bayer frame. Frames that arrive before the
// configured cadence deadline are rejected without copying their payload.
//
// Latest returns shared immutable ownership, so a worker may continue using a
// snapshot after the acquisition thread overwrites the sampler's latest slot.
// The sampler itself never queues work; memory retained beyond the latest
// frame is owned explicitly by callers still holding older snapshots.
class LatestFrameSampler final {
 public:
  explicit LatestFrameSampler(LatestFrameSamplerConfig config);
  ~LatestFrameSampler() = default;

  LatestFrameSampler(const LatestFrameSampler &) = delete;
  LatestFrameSampler &operator=(const LatestFrameSampler &) = delete;
  LatestFrameSampler(LatestFrameSampler &&) = delete;
  LatestFrameSampler &operator=(LatestFrameSampler &&) = delete;

  [[nodiscard]] PreviewFramePublishResult TryPublish(const FrameView &frame);
  [[nodiscard]] std::shared_ptr<const SampledPreviewFrame> Latest() const;

  // Invalidates the retained frame and cadence deadline. Publication sequence
  // numbers remain monotonic so browser cache keys are never reused after a
  // camera reconfiguration.
  void Reset();

  [[nodiscard]] std::chrono::steady_clock::duration minimum_interval() const noexcept;
  [[nodiscard]] std::size_t maximum_payload_bytes() const noexcept;

 private:
  const LatestFrameSamplerConfig config_;
  mutable std::mutex mutex_;
  bool has_published_frame_ = false;
  std::chrono::steady_clock::time_point last_published_at_;
  std::uint64_t next_preview_sequence_ = 1;
  std::shared_ptr<const SampledPreviewFrame> latest_;
};

}  // namespace swing_capture::preview

#endif  // SWING_CAPTURE_CAPTURE_PREVIEW_LATEST_FRAME_SAMPLER_H_
