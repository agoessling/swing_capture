#ifndef SWING_CAPTURE_CAPTURE_CORE_DEVICE_CLOCK_MAPPER_H_
#define SWING_CAPTURE_CAPTURE_CORE_DEVICE_CLOCK_MAPPER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace swing_capture {

// Fits device timestamps to the host steady clock with an online affine
// regression. This aligns each camera's independent clock to the audio clock;
// it does not prove that two cameras exposed at the same instant.
class DeviceClockMapper final {
 public:
  explicit DeviceClockMapper(std::uint64_t ticks_per_second);

  void AddSample(std::uint64_t device_timestamp,
                 std::chrono::steady_clock::time_point host_received_at);

  [[nodiscard]] std::size_t sample_count() const;
  [[nodiscard]] bool ready() const;
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> EstimateHostTime(
      std::uint64_t device_timestamp) const;
  [[nodiscard]] double clock_rate_ratio() const;
  [[nodiscard]] double clock_drift_parts_per_million() const;
  [[nodiscard]] double residual_standard_deviation_seconds() const;

 private:
  std::uint64_t ticks_per_second_;
  std::uint64_t first_device_timestamp_ = 0;
  std::uint64_t last_device_timestamp_ = 0;
  std::chrono::steady_clock::time_point first_host_timestamp_;
  std::size_t sample_count_ = 0;
  double mean_device_seconds_ = 0.0;
  double mean_host_seconds_ = 0.0;
  double device_sum_squared_delta_ = 0.0;
  double host_sum_squared_delta_ = 0.0;
  double covariance_sum_ = 0.0;
};

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_CORE_DEVICE_CLOCK_MAPPER_H_
