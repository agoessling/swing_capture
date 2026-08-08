#include "capture/trigger/impact_detector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace swing_capture {
namespace {

void ValidateConfig(const ImpactDetectorConfig &config) {
  const auto finite = [](double value) { return std::isfinite(value); };
  if (!finite(config.threshold_multiplier) || config.threshold_multiplier <= 0.0F) {
    throw std::invalid_argument("threshold_multiplier must be positive");
  }
  if (!finite(config.minimum_peak_amplitude) || config.minimum_peak_amplitude < 0.0F ||
      config.minimum_peak_amplitude > 1.0F) {
    throw std::invalid_argument("minimum_peak_amplitude must be between zero and one");
  }
  if (!finite(config.initial_noise_floor) || config.initial_noise_floor < 0.0F ||
      config.initial_noise_floor > 1.0F) {
    throw std::invalid_argument("initial_noise_floor must be between zero and one");
  }
  if (!finite(config.noise_update_clip_multiplier) || config.noise_update_clip_multiplier < 1.0F) {
    throw std::invalid_argument("noise_update_clip_multiplier must be at least one");
  }
  if (!finite(config.noise_floor_time_constant_seconds) ||
      config.noise_floor_time_constant_seconds <= 0.0) {
    throw std::invalid_argument("noise_floor_time_constant_seconds must be positive");
  }
  if (!finite(config.peak_confirmation_seconds) || config.peak_confirmation_seconds < 0.0) {
    throw std::invalid_argument("peak_confirmation_seconds cannot be negative");
  }
  if (!finite(config.refractory_period_seconds) || config.refractory_period_seconds < 0.0) {
    throw std::invalid_argument("refractory_period_seconds cannot be negative");
  }
}

}  // namespace

ImpactDetector::ImpactDetector(ImpactDetectorConfig config) : config_(config) {
  ValidateConfig(config_);
  Reset();
}

ImpactProcessResult ImpactDetector::ProcessBlock(std::span<const std::int16_t> samples,
                                                 std::chrono::steady_clock::time_point block_start,
                                                 std::uint32_t sample_rate_hz,
                                                 std::span<ImpactEvent> output_events) {
  if (sample_rate_hz == 0) {
    throw std::invalid_argument("sample_rate_hz must be positive");
  }
  if (!samples.empty() && has_processed_sample_ && block_start <= last_sample_time_) {
    throw std::invalid_argument(
        "audio blocks must have monotonically increasing sample timestamps");
  }

  const auto smoothing_factor = static_cast<float>(-std::expm1(
      -1.0 / (config_.noise_floor_time_constant_seconds * static_cast<double>(sample_rate_hz))));
  ImpactProcessResult result;

  for (std::size_t index = 0; index < samples.size(); ++index) {
    const auto sample_time =
        block_start + Seconds(static_cast<double>(index) / static_cast<double>(sample_rate_hz));
    const float amplitude = NormalizedAmplitude(samples[index]);
    const CandidateSample candidate_sample = {
        .amplitude = amplitude,
        .sample_time = sample_time,
        .block_start = block_start,
        .sample_index = index,
        .sample_rate_hz = sample_rate_hz,
    };

    // A discontinuity between blocks can move directly past the confirmation
    // deadline. Finalize the old peak before allowing this later sample to
    // influence it; the sample can then begin a distinct candidate when the
    // refractory period has also elapsed.
    if (candidate_active_ && sample_time >= candidate_confirmation_deadline_) {
      EmitCandidate(sample_time, output_events, result);
    }

    if (candidate_active_) {
      UpdateCandidate(candidate_sample);
      if (sample_time >= candidate_confirmation_deadline_) {
        EmitCandidate(sample_time, output_events, result);
      }
    } else {
      const float threshold = Threshold();
      if (sample_time >= refractory_until_ && amplitude >= threshold) {
        BeginCandidate(candidate_sample, threshold);
        if (sample_time >= candidate_confirmation_deadline_) {
          EmitCandidate(sample_time, output_events, result);
        }
      } else {
        UpdateNoiseFloor({.amplitude = amplitude, .smoothing_factor = smoothing_factor});
      }
    }

    has_processed_sample_ = true;
    last_sample_time_ = sample_time;
  }

  return result;
}

void ImpactDetector::Reset() {
  noise_floor_ = config_.initial_noise_floor;
  has_processed_sample_ = false;
  last_sample_time_ = {};
  refractory_until_ = std::chrono::steady_clock::time_point::min();
  candidate_active_ = false;
  candidate_ = {};
  candidate_confirmation_deadline_ = {};
}

float ImpactDetector::noise_floor() const { return noise_floor_; }

float ImpactDetector::detection_threshold() const { return Threshold(); }

bool ImpactDetector::peak_confirmation_pending() const { return candidate_active_; }

float ImpactDetector::NormalizedAmplitude(std::int16_t sample) {
  const std::int32_t widened = sample;
  const std::int32_t magnitude = widened < 0 ? -widened : widened;
  return static_cast<float>(magnitude) / 32768.0F;
}

std::chrono::steady_clock::duration ImpactDetector::Seconds(double seconds) {
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(seconds));
}

float ImpactDetector::Threshold() const {
  return std::max(config_.minimum_peak_amplitude, noise_floor_ * config_.threshold_multiplier);
}

void ImpactDetector::UpdateNoiseFloor(NoiseObservation observation) {
  const float clipping_basis = std::max(noise_floor_, config_.initial_noise_floor);
  const float clipped_amplitude =
      std::min(observation.amplitude, clipping_basis * config_.noise_update_clip_multiplier);
  noise_floor_ += observation.smoothing_factor * (clipped_amplitude - noise_floor_);
  noise_floor_ = std::clamp(noise_floor_, 0.0F, 1.0F);
}

void ImpactDetector::BeginCandidate(const CandidateSample &sample, float threshold) {
  candidate_active_ = true;
  candidate_ = {
      .strike_time = sample.sample_time,
      .confirmation_time = sample.sample_time,
      .source_block_start = sample.block_start,
      .sample_index_in_block = sample.sample_index,
      .sample_rate_hz = sample.sample_rate_hz,
      .peak_amplitude = sample.amplitude,
      .noise_floor_at_detection = noise_floor_,
      .threshold_at_detection = threshold,
  };
  candidate_confirmation_deadline_ =
      sample.sample_time + Seconds(config_.peak_confirmation_seconds);
}

void ImpactDetector::UpdateCandidate(const CandidateSample &sample) {
  if (sample.amplitude <= candidate_.peak_amplitude) {
    return;
  }
  candidate_.strike_time = sample.sample_time;
  candidate_.source_block_start = sample.block_start;
  candidate_.sample_index_in_block = sample.sample_index;
  candidate_.sample_rate_hz = sample.sample_rate_hz;
  candidate_.peak_amplitude = sample.amplitude;
  candidate_confirmation_deadline_ =
      sample.sample_time + Seconds(config_.peak_confirmation_seconds);
}

void ImpactDetector::EmitCandidate(std::chrono::steady_clock::time_point confirmation_time,
                                   std::span<ImpactEvent> output_events,
                                   ImpactProcessResult &result) {
  candidate_.confirmation_time = confirmation_time;
  ++result.events_detected;
  if (result.events_written < output_events.size()) {
    output_events[result.events_written] = candidate_;
    ++result.events_written;
  }
  refractory_until_ = candidate_.strike_time + Seconds(config_.refractory_period_seconds);
  candidate_active_ = false;
}

}  // namespace swing_capture
