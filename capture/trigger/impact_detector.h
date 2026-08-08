#ifndef SWING_CAPTURE_CAPTURE_TRIGGER_IMPACT_DETECTOR_H_
#define SWING_CAPTURE_CAPTURE_TRIGGER_IMPACT_DETECTOR_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace swing_capture {

// Tuned in normalized PCM amplitude, where 1.0 is full scale. These defaults
// are conservative starting points; recorded simulator-room audio should be
// used to tune them before relying on the detector to save swings.
struct ImpactDetectorConfig {
  float threshold_multiplier = 8.0F;
  float minimum_peak_amplitude = 0.05F;
  float initial_noise_floor = 0.005F;
  float noise_update_clip_multiplier = 4.0F;
  double noise_floor_time_constant_seconds = 0.5;
  double peak_confirmation_seconds = 0.0015;
  double refractory_period_seconds = 0.25;
};

struct ImpactEvent {
  // Timestamp of the largest sample in the confirmed peak, not the later time
  // at which confirmation completed.
  std::chrono::steady_clock::time_point strike_time;

  // Timestamp of the sample that caused the detector to confirm and deliver
  // this event. This is never earlier than strike_time and is the timestamp
  // consumers must use to order event delivery.
  std::chrono::steady_clock::time_point confirmation_time;

  // Retained so consumers can reproduce strike_time and inspect the original
  // PCM block, including when confirmation finishes in a subsequent block.
  std::chrono::steady_clock::time_point source_block_start;
  std::size_t sample_index_in_block = 0;
  std::uint32_t sample_rate_hz = 0;

  float peak_amplitude = 0.0F;
  float noise_floor_at_detection = 0.0F;
  float threshold_at_detection = 0.0F;
};

struct ImpactProcessResult {
  std::size_t events_detected = 0;
  std::size_t events_written = 0;

  [[nodiscard]] std::size_t events_dropped() const { return events_detected - events_written; }
};

// Stateful, single-stream ball-strike detector for signed 16-bit mono PCM.
//
// ProcessBlock performs no allocation. Callers provide event storage and an
// explicit steady-clock timestamp for PCM sample zero. A peak may be reported
// while processing a later block because confirmation deliberately waits for
// the local maximum to pass.
class ImpactDetector final {
 public:
  explicit ImpactDetector(ImpactDetectorConfig config = {});
  ~ImpactDetector() = default;

  ImpactDetector(const ImpactDetector &) = delete;
  ImpactDetector &operator=(const ImpactDetector &) = delete;
  ImpactDetector(ImpactDetector &&) = delete;
  ImpactDetector &operator=(ImpactDetector &&) = delete;

  [[nodiscard]] ImpactProcessResult ProcessBlock(std::span<const std::int16_t> samples,
                                                 std::chrono::steady_clock::time_point block_start,
                                                 std::uint32_t sample_rate_hz,
                                                 std::span<ImpactEvent> output_events);

  void Reset();

  [[nodiscard]] float noise_floor() const;
  [[nodiscard]] float detection_threshold() const;
  [[nodiscard]] bool peak_confirmation_pending() const;

 private:
  struct CandidateSample {
    float amplitude = 0.0F;
    std::chrono::steady_clock::time_point sample_time;
    std::chrono::steady_clock::time_point block_start;
    std::size_t sample_index = 0;
    std::uint32_t sample_rate_hz = 0;
  };

  struct NoiseObservation {
    float amplitude = 0.0F;
    float smoothing_factor = 0.0F;
  };

  [[nodiscard]] static float NormalizedAmplitude(std::int16_t sample);
  [[nodiscard]] static std::chrono::steady_clock::duration Seconds(double seconds);
  [[nodiscard]] float Threshold() const;
  void UpdateNoiseFloor(NoiseObservation observation);
  void BeginCandidate(const CandidateSample &sample, float threshold);
  void UpdateCandidate(const CandidateSample &sample);
  void EmitCandidate(std::chrono::steady_clock::time_point confirmation_time,
                     std::span<ImpactEvent> output_events, ImpactProcessResult &result);

  ImpactDetectorConfig config_;
  float noise_floor_ = 0.0F;
  bool has_processed_sample_ = false;
  std::chrono::steady_clock::time_point last_sample_time_;
  std::chrono::steady_clock::time_point refractory_until_;

  bool candidate_active_ = false;
  ImpactEvent candidate_;
  std::chrono::steady_clock::time_point candidate_confirmation_deadline_;
};

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_TRIGGER_IMPACT_DETECTOR_H_
