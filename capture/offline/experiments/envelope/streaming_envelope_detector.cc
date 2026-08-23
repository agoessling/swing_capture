#include "capture/offline/experiments/envelope/streaming_envelope_detector.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "capture/offline/experiments/envelope/envelope_experiment.h"

namespace swing_capture::offline::envelope {
namespace {

constexpr double kPcmScale = 32768.0;

std::size_t FramesFor(std::int64_t duration_us, std::uint32_t sample_rate_hz) {
  if (duration_us <= 0) {
    throw std::invalid_argument("detector durations must be positive");
  }
  return std::max<std::size_t>(
      1, static_cast<std::size_t>(duration_us * static_cast<std::int64_t>(sample_rate_hz) /
                                  1'000'000));
}

std::int64_t TimeUs(std::uint64_t frame, std::uint32_t sample_rate_hz) {
  if (frame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 1'000'000U) {
    throw std::overflow_error("audio frame position exceeds microsecond timeline range");
  }
  return static_cast<std::int64_t>(frame * 1'000'000U / sample_rate_hz);
}

void Validate(std::uint32_t sample_rate_hz, const DetectorConfig &config) {
  if (sample_rate_hz == 0 || config.name.empty() || config.high_pass_hz <= 0.0 ||
      config.hop_us <= 0 || config.fast_window_us <= 0 || config.background_window_us <= 0 ||
      config.background_guard_us <= 0 || config.background_block_us <= 0 ||
      config.background_multiplier <= 0.0 || config.rise_ratio <= 0.0 ||
      config.minimum_peak < 0.0 || config.minimum_high_frequency_ratio < 0.0 ||
      config.minimum_crest_factor < 0.0 || config.cluster_gap_us < 0 || config.refractory_us < 0) {
    throw std::invalid_argument("detector configuration is invalid");
  }
  if (config.high_pass_hz >= static_cast<double>(sample_rate_hz) / 2.0 ||
      config.background_block_us > config.background_window_us) {
    throw std::invalid_argument("detector frequency or background block is invalid");
  }
}

}  // namespace

DetectorConfig RobustHp120X12Config() {
  return DetectorConfig{
      .name = "robust_hp120_x12",
      .high_pass_hz = 120.0,
      .hop_us = 500,
      .fast_window_us = 1'500,
      .background_window_us = 240'000,
      .background_guard_us = 10'000,
      .background_block_us = 10'000,
      .background_multiplier = 12.0,
      .rise_ratio = 1.4,
      .minimum_peak = 0.004,
      .minimum_high_frequency_ratio = 0.0,
      .minimum_crest_factor = 0.0,
      .cluster_gap_us = 20'000,
      .refractory_us = 30'000,
  };
}

class StreamingEnvelopeDetector::State {
 public:
  State(std::uint32_t sample_rate_hz, const DetectorConfig &config,
        std::uint64_t first_frame_position)
      : sample_rate_hz_(sample_rate_hz),
        config_(config),
        first_frame_position_(first_frame_position),
        hop_frames_(FramesFor(config.hop_us, sample_rate_hz)),
        fast_frames_(FramesFor(config.fast_window_us, sample_rate_hz)),
        background_frames_(FramesFor(config.background_window_us, sample_rate_hz)),
        guard_frames_(FramesFor(config.background_guard_us, sample_rate_hz)),
        block_frames_(FramesFor(config.background_block_us, sample_rate_hz)),
        cluster_gap_frames_(
            FramesFor(std::max<std::int64_t>(1, config.cluster_gap_us), sample_rate_hz)),
        refractory_frames_(
            FramesFor(std::max<std::int64_t>(1, config.refractory_us), sample_rate_hz)),
        background_refresh_frames_(FramesFor(5'000, sample_rate_hz)),
        ring_capacity_(background_frames_ + guard_frames_ + fast_frames_ + 2U),
        signal_(ring_capacity_),
        square_prefix_(ring_capacity_),
        difference_square_prefix_(ring_capacity_),
        background_blocks_(background_frames_ / block_frames_),
        next_feature_frame_(background_frames_ + guard_frames_) {
    Validate(sample_rate_hz, config);
    if (background_blocks_.empty()) {
      throw std::invalid_argument("background window contains no complete robust blocks");
    }
    const double time_constant = 1.0 / (2.0 * std::numbers::pi * config.high_pass_hz);
    const double sample_period = 1.0 / static_cast<double>(sample_rate_hz);
    high_pass_alpha_ = time_constant / (time_constant + sample_period);
  }

  void Process(std::span<const std::int16_t> samples, const CandidateCallback &emit) {
    if (finished_) {
      throw std::logic_error("cannot process audio after detector finish");
    }
    if (!emit) {
      throw std::invalid_argument("candidate callback is empty");
    }
    for (const std::int16_t sample : samples) {
      Append(sample);
      ProcessReadyFeatures(emit);
    }
  }

  void Finish(const CandidateCallback &emit) {
    if (finished_) {
      throw std::logic_error("detector finish may only be called once");
    }
    if (!emit) {
      throw std::invalid_argument("candidate callback is empty");
    }
    finished_ = true;
    if (cluster_.has_value() &&
        cluster_->last_frame + fast_frames_ + cluster_gap_frames_ <= processed_frames_) {
      FinishCluster(emit);
    }
    cluster_.reset();
  }

  [[nodiscard]] std::size_t buffered_frame_capacity() const { return ring_capacity_; }
  [[nodiscard]] std::uint64_t processed_frames() const { return processed_frames_; }

 private:
  struct Hit {
    std::size_t frame = 0;
    std::size_t peak_frame = 0;
    double score = 0.0;
    Candidate feature;
  };

  struct Cluster {
    std::size_t first_frame = 0;
    std::size_t last_frame = 0;
    Hit best;
  };

  void Append(std::int16_t pcm_sample) {
    if (processed_frames_ == std::numeric_limits<std::size_t>::max()) {
      throw std::overflow_error("audio stream frame count exceeds detector range");
    }
    const auto frame = static_cast<std::size_t>(processed_frames_);
    const double input = static_cast<double>(pcm_sample) / kPcmScale;
    double output = 0.0;
    double difference = 0.0;
    if (!filter_initialized_) {
      previous_input_ = input;
      filter_initialized_ = true;
    } else {
      output = high_pass_alpha_ * (previous_output_ + input - previous_input_);
      difference = output - previous_output_;
      previous_input_ = input;
      previous_output_ = output;
    }
    signal_[frame % ring_capacity_] = output;
    square_sum_ += output * output;
    difference_square_sum_ += difference * difference;
    square_prefix_[(frame + 1U) % ring_capacity_] = square_sum_;
    difference_square_prefix_[(frame + 1U) % ring_capacity_] = difference_square_sum_;
    ++processed_frames_;
  }

  [[nodiscard]] double RangeRms(const std::vector<double> &prefix, std::size_t begin,
                                std::size_t end) const {
    if (begin >= end || end > processed_frames_) {
      return 0.0;
    }
    const double total = prefix[end % ring_capacity_] - prefix[begin % ring_capacity_];
    return std::sqrt(std::max(0.0, total) / static_cast<double>(end - begin));
  }

  [[nodiscard]] double RobustBackgroundRms(std::size_t end) {
    const std::size_t begin = end - background_frames_;
    for (std::size_t index = 0; index < background_blocks_.size(); ++index) {
      const std::size_t block_begin = begin + index * block_frames_;
      background_blocks_[index] =
          RangeRms(square_prefix_, block_begin, block_begin + block_frames_);
    }
    const auto middle =
        background_blocks_.begin() + static_cast<std::ptrdiff_t>(background_blocks_.size() / 2U);
    std::nth_element(background_blocks_.begin(), middle, background_blocks_.end());
    return *middle;
  }

  [[nodiscard]] Candidate MakeCandidate(std::size_t frame) const {
    const std::size_t end = frame + fast_frames_;
    const std::size_t prior_begin = frame > fast_frames_ * 6U ? frame - fast_frames_ * 6U : 0U;
    const double fast_rms = RangeRms(square_prefix_, frame, end);
    const double prior_rms = RangeRms(square_prefix_, prior_begin, frame);
    const double difference_rms = RangeRms(difference_square_prefix_, frame, end);
    std::size_t peak_frame = frame;
    double peak = 0.0;
    for (std::size_t sample = frame; sample < end; ++sample) {
      if (std::abs(signal_[sample % ring_capacity_]) > peak) {
        peak = std::abs(signal_[sample % ring_capacity_]);
        peak_frame = sample;
      }
    }
    return Candidate{
        .strike_us = TimeUs(first_frame_position_ + peak_frame, sample_rate_hz_),
        .decision_us = TimeUs(first_frame_position_ + end, sample_rate_hz_),
        .peak = peak,
        .fast_rms = fast_rms,
        .background_rms = cached_background_,
        .background_ratio = fast_rms / std::max(cached_background_, 1e-7),
        .rise_ratio = fast_rms / std::max(prior_rms, 1e-7),
        .high_frequency_ratio = difference_rms / std::max(2.0 * fast_rms, 1e-7),
        .crest_factor = peak / std::max(fast_rms, 1e-7),
        .cluster_duration_us = 0,
    };
  }

  void ProcessReadyFeatures(const CandidateCallback &emit) {
    while (next_feature_frame_ + fast_frames_ < processed_frames_) {
      const std::size_t frame = next_feature_frame_;
      if (!background_cached_ || frame >= cached_background_frame_ + background_refresh_frames_) {
        cached_background_ = RobustBackgroundRms(frame - guard_frames_);
        cached_background_frame_ = frame;
        background_cached_ = true;
      }
      const Candidate feature = MakeCandidate(frame);
      const bool qualifies = feature.background_ratio >= config_.background_multiplier &&
                             feature.rise_ratio >= config_.rise_ratio &&
                             feature.peak >= config_.minimum_peak &&
                             feature.high_frequency_ratio >= config_.minimum_high_frequency_ratio &&
                             feature.crest_factor >= config_.minimum_crest_factor;
      if (cluster_.has_value() && frame > cluster_->last_frame + cluster_gap_frames_) {
        FinishCluster(emit);
      }
      if (qualifies) {
        AddHit(frame, feature);
      }
      next_feature_frame_ += hop_frames_;
    }
  }

  void AddHit(std::size_t frame, const Candidate &feature) {
    const double score = feature.background_ratio * feature.rise_ratio *
                         (0.5 + feature.high_frequency_ratio) * feature.crest_factor;
    const std::int64_t local_strike_us =
        feature.strike_us - TimeUs(first_frame_position_, sample_rate_hz_);
    const auto peak_frame = static_cast<std::size_t>(
        local_strike_us * static_cast<std::int64_t>(sample_rate_hz_) / 1'000'000);
    const Hit hit{.frame = frame, .peak_frame = peak_frame, .score = score, .feature = feature};
    if (!cluster_.has_value()) {
      cluster_ = Cluster{.first_frame = frame, .last_frame = frame, .best = hit};
      return;
    }
    cluster_->last_frame = frame;
    if (score > cluster_->best.score) {
      cluster_->best = hit;
    }
  }

  void FinishCluster(const CandidateCallback &emit) {
    if (!cluster_.has_value()) {
      return;
    }
    Candidate candidate = cluster_->best.feature;
    const std::size_t cluster_end = cluster_->last_frame + fast_frames_;
    candidate.decision_us =
        TimeUs(first_frame_position_ + cluster_end + cluster_gap_frames_, sample_rate_hz_);
    candidate.cluster_duration_us = TimeUs(cluster_end - cluster_->first_frame, sample_rate_hz_);
    if (!candidate_emitted_ ||
        cluster_->best.peak_frame >= last_accepted_frame_ + refractory_frames_) {
      emit(candidate);
      candidate_emitted_ = true;
      last_accepted_frame_ = cluster_->best.peak_frame;
    }
    cluster_.reset();
  }

  std::uint32_t sample_rate_hz_;
  DetectorConfig config_;
  std::uint64_t first_frame_position_;
  std::size_t hop_frames_;
  std::size_t fast_frames_;
  std::size_t background_frames_;
  std::size_t guard_frames_;
  std::size_t block_frames_;
  std::size_t cluster_gap_frames_;
  std::size_t refractory_frames_;
  std::size_t background_refresh_frames_;
  std::size_t ring_capacity_;
  std::vector<double> signal_;
  std::vector<double> square_prefix_;
  std::vector<double> difference_square_prefix_;
  std::vector<double> background_blocks_;
  std::size_t next_feature_frame_;
  std::uint64_t processed_frames_ = 0;
  double high_pass_alpha_ = 0.0;
  bool filter_initialized_ = false;
  double previous_input_ = 0.0;
  double previous_output_ = 0.0;
  double square_sum_ = 0.0;
  double difference_square_sum_ = 0.0;
  bool background_cached_ = false;
  double cached_background_ = 0.0;
  std::size_t cached_background_frame_ = 0;
  std::optional<Cluster> cluster_;
  bool candidate_emitted_ = false;
  std::size_t last_accepted_frame_ = 0;
  bool finished_ = false;
};

StreamingEnvelopeDetector::StreamingEnvelopeDetector(std::uint32_t sample_rate_hz,
                                                     const DetectorConfig &config,
                                                     std::uint64_t first_frame_position)
    : state_(std::make_unique<State>(sample_rate_hz, config, first_frame_position)) {}

StreamingEnvelopeDetector::~StreamingEnvelopeDetector() = default;
StreamingEnvelopeDetector::StreamingEnvelopeDetector(StreamingEnvelopeDetector &&) noexcept =
    default;
StreamingEnvelopeDetector &StreamingEnvelopeDetector::operator=(
    StreamingEnvelopeDetector &&) noexcept = default;

void StreamingEnvelopeDetector::Process(std::span<const std::int16_t> samples,
                                        const CandidateCallback &emit) {
  state_->Process(samples, emit);
}

void StreamingEnvelopeDetector::Finish(const CandidateCallback &emit) { state_->Finish(emit); }

std::size_t StreamingEnvelopeDetector::buffered_frame_capacity() const {
  return state_->buffered_frame_capacity();
}

std::uint64_t StreamingEnvelopeDetector::processed_frames() const {
  return state_->processed_frames();
}

}  // namespace swing_capture::offline::envelope
