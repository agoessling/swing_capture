#ifndef SWING_CAPTURE_CAPTURE_CORE_ROBUST_DEVICE_CLOCK_MAPPING_H_
#define SWING_CAPTURE_CAPTURE_CORE_ROBUST_DEVICE_CLOCK_MAPPING_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace swing_capture {

inline constexpr std::size_t kMinimumRobustDeviceClockSamples = 8;
inline constexpr std::size_t kMaximumRobustDeviceClockSamples = 512;
inline constexpr std::size_t kMaximumRobustDeviceClockSlopePairs =
    kMaximumRobustDeviceClockSamples * (kMaximumRobustDeviceClockSamples - 1U) / 2U;

struct DeviceHostTimestampSample {
  std::uint64_t device_timestamp = 0;
  std::chrono::steady_clock::time_point host_received_at;
};

struct HostTimeBracket {
  std::chrono::steady_clock::time_point earliest;
  std::chrono::steady_clock::time_point latest;
};

struct RobustDeviceClockMappingOptions {
  // Receipt timestamps cannot reveal the fixed camera/USB transport latency.
  // When the caller can independently guarantee a nonnegative upper bound,
  // supplying it enables conservative device-event host-time brackets. It is
  // deliberately optional so an uncalibrated fit cannot silently claim an
  // absolute exposure/event-time bound.
  std::optional<std::chrono::steady_clock::duration> fixed_transport_latency_upper_bound;
};

struct RobustDeviceClockMappingDiagnostics {
  std::size_t sample_count = 0;
  std::size_t slope_pair_count = 0;
  std::size_t inlier_count = 0;
  std::size_t outlier_count = 0;
  double clock_rate_ratio = 0.0;
  double clock_drift_parts_per_million = 0.0;
  double robust_slope_sigma = 0.0;
  std::chrono::duration<double> median_absolute_residual{};
  std::chrono::duration<double> robust_residual_sigma{};
  std::chrono::duration<double> maximum_inlier_absolute_residual{};
  std::chrono::duration<double> maximum_positive_queue_delay{};
  std::chrono::duration<double> receipt_time_uncertainty{};
  std::optional<std::chrono::duration<double>> fixed_transport_latency_upper_bound;
  std::optional<std::chrono::duration<double>> device_event_time_uncertainty;
  std::string timestamp_model;
};

struct RobustDeviceClockMappingResult {
  // Preserves input order and has exactly sample_count entries.
  std::vector<std::chrono::steady_clock::time_point> mapped_host_receipt_times;

  // Present only when fixed_transport_latency_upper_bound was supplied. Each
  // midpoint is the center of the corresponding conservative bracket, not a
  // measurement of the actual fixed transport latency.
  std::vector<std::chrono::steady_clock::time_point> bounded_device_event_host_midpoints;
  std::vector<HostTimeBracket> bounded_device_event_host_brackets;

  std::vector<std::size_t> outlier_sample_indices;
  RobustDeviceClockMappingDiagnostics diagnostics;
};

// Fits a robust affine mapping from camera device time to normal host receipt
// time. The Theil-Sen slope and median intercept resist a short contiguous
// queued-delivery burst. Production memory is strictly bounded: at most 512
// samples and 130,816 pair slopes are accepted.
//
// receipt_time_uncertainty describes robust inlier receipt-time fit error. It
// excludes samples classified as queued-delivery outliers and does not include
// the unobservable fixed transport latency. Malformed, underdetermined, or
// nonmonotonic inputs throw std::invalid_argument.
[[nodiscard]] RobustDeviceClockMappingResult FitRobustDeviceClockMapping(
    std::span<const DeviceHostTimestampSample> samples, std::uint64_t device_ticks_per_second,
    const RobustDeviceClockMappingOptions &options = {});

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_CORE_ROBUST_DEVICE_CLOCK_MAPPING_H_
