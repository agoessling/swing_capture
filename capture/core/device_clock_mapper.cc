#include "capture/core/device_clock_mapper.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace swing_capture {

DeviceClockMapper::DeviceClockMapper(std::uint64_t ticks_per_second)
    : ticks_per_second_(ticks_per_second) {
  if (ticks_per_second == 0) {
    throw std::invalid_argument("device clock tick frequency must be nonzero");
  }
}

void DeviceClockMapper::AddSample(std::uint64_t device_timestamp,
                                  std::chrono::steady_clock::time_point host_received_at) {
  if (sample_count_ == 0) {
    first_device_timestamp_ = device_timestamp;
    last_device_timestamp_ = device_timestamp;
    first_host_timestamp_ = host_received_at;
    sample_count_ = 1;
    return;
  }
  if (device_timestamp <= last_device_timestamp_) {
    throw std::invalid_argument("device timestamps must be strictly increasing");
  }
  last_device_timestamp_ = device_timestamp;

  const double device_seconds = static_cast<double>(device_timestamp - first_device_timestamp_) /
                                static_cast<double>(ticks_per_second_);
  const double host_seconds =
      std::chrono::duration<double>(host_received_at - first_host_timestamp_).count();

  ++sample_count_;
  const auto count = static_cast<double>(sample_count_);
  const double device_delta = device_seconds - mean_device_seconds_;
  const double host_delta = host_seconds - mean_host_seconds_;
  mean_device_seconds_ += device_delta / count;
  mean_host_seconds_ += host_delta / count;
  device_sum_squared_delta_ += device_delta * (device_seconds - mean_device_seconds_);
  host_sum_squared_delta_ += host_delta * (host_seconds - mean_host_seconds_);
  covariance_sum_ += device_delta * (host_seconds - mean_host_seconds_);
}

std::size_t DeviceClockMapper::sample_count() const { return sample_count_; }

bool DeviceClockMapper::ready() const {
  return sample_count_ >= 2 && device_sum_squared_delta_ > 0.0;
}

std::optional<std::chrono::steady_clock::time_point> DeviceClockMapper::EstimateHostTime(
    std::uint64_t device_timestamp) const {
  if (!ready() || device_timestamp < first_device_timestamp_) {
    return std::nullopt;
  }
  const double device_seconds = static_cast<double>(device_timestamp - first_device_timestamp_) /
                                static_cast<double>(ticks_per_second_);
  const double estimated_host_seconds =
      mean_host_seconds_ + (clock_rate_ratio() * (device_seconds - mean_device_seconds_));
  return first_host_timestamp_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(estimated_host_seconds));
}

double DeviceClockMapper::clock_rate_ratio() const {
  return ready() ? covariance_sum_ / device_sum_squared_delta_ : 0.0;
}

double DeviceClockMapper::clock_drift_parts_per_million() const {
  return ready() ? (clock_rate_ratio() - 1.0) * 1000000.0 : 0.0;
}

double DeviceClockMapper::residual_standard_deviation_seconds() const {
  if (sample_count_ < 3 || !ready()) {
    return 0.0;
  }
  const double residual_sum_squares =
      std::max(0.0, host_sum_squared_delta_ - (clock_rate_ratio() * covariance_sum_));
  return std::sqrt(residual_sum_squares / static_cast<double>(sample_count_ - 2));
}

}  // namespace swing_capture
