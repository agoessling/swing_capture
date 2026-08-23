#ifndef SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_ENVELOPE_STREAMING_ENVELOPE_DETECTOR_H_
#define SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_ENVELOPE_STREAMING_ENVELOPE_DETECTOR_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "capture/offline/experiments/envelope/envelope_experiment.h"

namespace swing_capture::offline::envelope {

using CandidateCallback = std::function<void(const Candidate &)>;

// Returns the conservative development-set candidate selected for holdout
// evaluation. Every field is assigned explicitly so the frozen replay digest
// does not depend on DetectorConfig defaults.
[[nodiscard]] DetectorConfig RobustHp120X12Config();

// Incremental implementation of the offline envelope candidate generator. Its
// sample history, robust-background scratch, and active cluster are allocated
// once at construction and remain bounded independently of stream duration.
class StreamingEnvelopeDetector {
 public:
  StreamingEnvelopeDetector(std::uint32_t sample_rate_hz, const DetectorConfig &config,
                            std::uint64_t first_frame_position = 0);
  ~StreamingEnvelopeDetector();

  StreamingEnvelopeDetector(const StreamingEnvelopeDetector &) = delete;
  StreamingEnvelopeDetector &operator=(const StreamingEnvelopeDetector &) = delete;
  StreamingEnvelopeDetector(StreamingEnvelopeDetector &&) noexcept;
  StreamingEnvelopeDetector &operator=(StreamingEnvelopeDetector &&) noexcept;

  void Process(std::span<const std::int16_t> samples, const CandidateCallback &emit);
  void Finish(const CandidateCallback &emit);

  [[nodiscard]] std::size_t buffered_frame_capacity() const;
  [[nodiscard]] std::uint64_t processed_frames() const;

 private:
  class State;
  std::unique_ptr<State> state_;
};

}  // namespace swing_capture::offline::envelope

#endif  // SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_ENVELOPE_STREAMING_ENVELOPE_DETECTOR_H_
