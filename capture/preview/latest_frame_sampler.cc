#include "capture/preview/latest_frame_sampler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "capture/core/camera_source.h"

namespace swing_capture::preview {
namespace {

PreviewFramePublishResult ValidateFrame(const FrameView &frame, std::size_t maximum_payload_bytes) {
  if (!frame.metadata.complete) {
    return PreviewFramePublishResult::kIncompleteFrame;
  }
  if (frame.metadata.width == 0 || frame.metadata.height == 0) {
    return PreviewFramePublishResult::kInvalidDimensions;
  }
  if (frame.metadata.height > std::numeric_limits<std::size_t>::max() / frame.metadata.width) {
    return PreviewFramePublishResult::kInvalidDimensions;
  }
  const std::size_t expected_payload_bytes =
      static_cast<std::size_t>(frame.metadata.width) * frame.metadata.height;
  if (frame.payload.size() != expected_payload_bytes) {
    return PreviewFramePublishResult::kPayloadSizeMismatch;
  }
  if (frame.payload.size() > maximum_payload_bytes) {
    return PreviewFramePublishResult::kPayloadTooLarge;
  }
  return PreviewFramePublishResult::kPublished;
}

}  // namespace

LatestFrameSampler::LatestFrameSampler(LatestFrameSamplerConfig config) : config_(config) {
  if (config.minimum_interval <= std::chrono::steady_clock::duration::zero()) {
    throw std::invalid_argument("preview sampling interval must be positive");
  }
  if (config.maximum_payload_bytes == 0) {
    throw std::invalid_argument("preview maximum payload must be nonzero");
  }
}

PreviewFramePublishResult LatestFrameSampler::TryPublish(const FrameView &frame) {
  const PreviewFramePublishResult validation = ValidateFrame(frame, config_.maximum_payload_bytes);
  if (validation != PreviewFramePublishResult::kPublished) {
    return validation;
  }

  const std::scoped_lock lock(mutex_);
  if (has_published_frame_ &&
      (frame.metadata.host_received_at < last_published_at_ ||
       frame.metadata.host_received_at - last_published_at_ < config_.minimum_interval)) {
    return PreviewFramePublishResult::kRateLimited;
  }
  if (next_preview_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("preview publication sequence exhausted");
  }

  auto sampled = std::make_shared<SampledPreviewFrame>();
  sampled->metadata = frame.metadata;
  sampled->preview_sequence = next_preview_sequence_;
  sampled->bayer_pixels = std::vector<std::byte>(frame.payload.begin(), frame.payload.end());

  latest_ = std::move(sampled);
  last_published_at_ = frame.metadata.host_received_at;
  has_published_frame_ = true;
  ++next_preview_sequence_;
  return PreviewFramePublishResult::kPublished;
}

std::shared_ptr<const SampledPreviewFrame> LatestFrameSampler::Latest() const {
  const std::scoped_lock lock(mutex_);
  return latest_;
}

void LatestFrameSampler::Reset() {
  const std::scoped_lock lock(mutex_);
  latest_.reset();
  has_published_frame_ = false;
  last_published_at_ = {};
}

std::chrono::steady_clock::duration LatestFrameSampler::minimum_interval() const noexcept {
  return config_.minimum_interval;
}

std::size_t LatestFrameSampler::maximum_payload_bytes() const noexcept {
  return config_.maximum_payload_bytes;
}

}  // namespace swing_capture::preview
