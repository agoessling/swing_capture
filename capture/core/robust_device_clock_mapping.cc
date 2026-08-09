#include "capture/core/robust_device_clock_mapping.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace swing_capture {
namespace {

using Clock = std::chrono::steady_clock;
constexpr double kMadToNormalSigma = 1.4826;
constexpr double kOutlierSigmaMultiplier = 6.0;
constexpr double kUncertaintySigmaMultiplier = 3.0;

double Median(std::vector<double> *values) {
  if (values->empty()) {
    throw std::invalid_argument("robust clock median requires values");
  }
  std::ranges::sort(*values);
  const std::size_t middle = values->size() / 2U;
  if ((values->size() & 1U) != 0U) {
    return values->at(middle);
  }
  return std::midpoint(values->at(middle - 1U), values->at(middle));
}

double MedianAbsoluteDeviation(std::span<const double> values, double center,
                               std::vector<double> *scratch) {
  scratch->clear();
  scratch->reserve(values.size());
  for (const double value : values) {
    scratch->push_back(std::abs(value - center));
  }
  return Median(scratch);
}

void ValidateSamples(std::span<const DeviceHostTimestampSample> samples,
                     std::uint64_t device_ticks_per_second,
                     const RobustDeviceClockMappingOptions &options) {
  if (device_ticks_per_second == 0) {
    throw std::invalid_argument("robust device clock tick frequency must be nonzero");
  }
  if (samples.size() < kMinimumRobustDeviceClockSamples) {
    throw std::invalid_argument("robust device clock fit has too few samples");
  }
  if (samples.size() > kMaximumRobustDeviceClockSamples) {
    throw std::invalid_argument("robust device clock fit exceeds its bounded sample capacity");
  }
  for (std::size_t index = 1; index < samples.size(); ++index) {
    if (samples[index].device_timestamp <= samples[index - 1U].device_timestamp) {
      throw std::invalid_argument("robust device clock timestamps must be strictly increasing");
    }
    if (samples[index].host_received_at < samples[index - 1U].host_received_at) {
      throw std::invalid_argument("host receipt timestamps must be nondecreasing");
    }
  }
  if (options.fixed_transport_latency_upper_bound.has_value() &&
      *options.fixed_transport_latency_upper_bound < Clock::duration::zero()) {
    throw std::invalid_argument("fixed transport latency upper bound cannot be negative");
  }
}

Clock::time_point MappedTime(Clock::time_point host_origin, double intercept_seconds,
                             double rate_ratio, double device_seconds) {
  const double mapped_seconds = intercept_seconds + rate_ratio * device_seconds;
  if (!std::isfinite(mapped_seconds)) {
    throw std::invalid_argument("robust device clock mapping is not finite");
  }
  return host_origin +
         std::chrono::round<Clock::duration>(std::chrono::duration<double>(mapped_seconds));
}

}  // namespace

RobustDeviceClockMappingResult FitRobustDeviceClockMapping(
    std::span<const DeviceHostTimestampSample> samples, std::uint64_t device_ticks_per_second,
    const RobustDeviceClockMappingOptions &options) {
  ValidateSamples(samples, device_ticks_per_second, options);
  const std::uint64_t first_device_timestamp = samples.front().device_timestamp;
  const Clock::time_point host_origin = samples.front().host_received_at;

  std::vector<double> device_seconds;
  std::vector<double> host_seconds;
  device_seconds.reserve(samples.size());
  host_seconds.reserve(samples.size());
  for (const DeviceHostTimestampSample &sample : samples) {
    device_seconds.push_back(static_cast<double>(sample.device_timestamp - first_device_timestamp) /
                             static_cast<double>(device_ticks_per_second));
    host_seconds.push_back(
        std::chrono::duration<double>(sample.host_received_at - host_origin).count());
  }

  std::vector<double> slopes;
  slopes.reserve(samples.size() * (samples.size() - 1U) / 2U);
  for (std::size_t first = 0; first + 1U < samples.size(); ++first) {
    for (std::size_t second = first + 1U; second < samples.size(); ++second) {
      const double device_delta = device_seconds[second] - device_seconds[first];
      slopes.push_back((host_seconds[second] - host_seconds[first]) / device_delta);
    }
  }
  if (slopes.size() > kMaximumRobustDeviceClockSlopePairs) {
    throw std::logic_error("robust device clock slope storage exceeded its fixed bound");
  }
  const double rate_ratio = Median(&slopes);
  if (!std::isfinite(rate_ratio) || !(rate_ratio > 0.0)) {
    throw std::invalid_argument("robust device clock fit has a nonpositive rate");
  }
  std::vector<double> scratch;
  const double slope_mad = MedianAbsoluteDeviation(slopes, rate_ratio, &scratch);

  std::vector<double> intercepts;
  intercepts.reserve(samples.size());
  for (std::size_t index = 0; index < samples.size(); ++index) {
    intercepts.push_back(host_seconds[index] - rate_ratio * device_seconds[index]);
  }
  const double intercept_seconds = Median(&intercepts);

  std::vector<double> residuals;
  residuals.reserve(samples.size());
  for (std::size_t index = 0; index < samples.size(); ++index) {
    residuals.push_back(host_seconds[index] -
                        (intercept_seconds + rate_ratio * device_seconds[index]));
  }
  std::vector<double> residual_copy = residuals;
  const double median_residual = Median(&residual_copy);
  const double residual_mad = MedianAbsoluteDeviation(residuals, median_residual, &scratch);
  const double robust_residual_sigma = kMadToNormalSigma * residual_mad;
  const double host_clock_tick_seconds = std::chrono::duration<double>(Clock::duration{1}).count();
  const double outlier_threshold =
      std::max(kOutlierSigmaMultiplier * robust_residual_sigma, 8.0 * host_clock_tick_seconds);

  RobustDeviceClockMappingResult result;
  result.mapped_host_receipt_times.reserve(samples.size());
  if (options.fixed_transport_latency_upper_bound.has_value()) {
    result.bounded_device_event_host_midpoints.reserve(samples.size());
    result.bounded_device_event_host_brackets.reserve(samples.size());
  }
  result.outlier_sample_indices.reserve(samples.size());

  double maximum_inlier_absolute_residual = 0.0;
  double maximum_positive_queue_delay = 0.0;
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const double centered_residual = residuals[index] - median_residual;
    const bool inlier = std::abs(centered_residual) <= outlier_threshold;
    if (inlier) {
      maximum_inlier_absolute_residual =
          std::max(maximum_inlier_absolute_residual, std::abs(residuals[index]));
    } else {
      result.outlier_sample_indices.push_back(index);
    }
    maximum_positive_queue_delay = std::max(maximum_positive_queue_delay, residuals[index]);
    result.mapped_host_receipt_times.push_back(
        MappedTime(host_origin, intercept_seconds, rate_ratio, device_seconds[index]));
  }
  for (std::size_t index = 1; index < result.mapped_host_receipt_times.size(); ++index) {
    if (result.mapped_host_receipt_times[index] <= result.mapped_host_receipt_times[index - 1U]) {
      throw std::invalid_argument(
          "robust mapped host times are not representably strictly increasing");
    }
  }

  const double receipt_uncertainty_seconds =
      std::max(maximum_inlier_absolute_residual,
               kUncertaintySigmaMultiplier * robust_residual_sigma) +
      host_clock_tick_seconds;
  result.diagnostics = {
      .sample_count = samples.size(),
      .slope_pair_count = slopes.size(),
      .inlier_count = samples.size() - result.outlier_sample_indices.size(),
      .outlier_count = result.outlier_sample_indices.size(),
      .clock_rate_ratio = rate_ratio,
      .clock_drift_parts_per_million = (rate_ratio - 1.0) * 1'000'000.0,
      .robust_slope_sigma = kMadToNormalSigma * slope_mad,
      .median_absolute_residual = std::chrono::duration<double>(residual_mad),
      .robust_residual_sigma = std::chrono::duration<double>(robust_residual_sigma),
      .maximum_inlier_absolute_residual =
          std::chrono::duration<double>(maximum_inlier_absolute_residual),
      .maximum_positive_queue_delay = std::chrono::duration<double>(maximum_positive_queue_delay),
      .receipt_time_uncertainty = std::chrono::duration<double>(receipt_uncertainty_seconds),
      .fixed_transport_latency_upper_bound = std::nullopt,
      .device_event_time_uncertainty = std::nullopt,
      .timestamp_model =
          "robust affine camera-device to normal host-receipt mapping; queued-delivery outliers "
          "are excluded from uncertainty; fixed camera/USB transport latency is not observable",
  };

  if (options.fixed_transport_latency_upper_bound.has_value()) {
    const Clock::duration latency_bound = *options.fixed_transport_latency_upper_bound;
    const Clock::duration latency_midpoint_offset = latency_bound / 2;
    const Clock::duration latency_half_width = latency_bound - latency_midpoint_offset;
    const Clock::duration receipt_uncertainty =
        std::chrono::ceil<Clock::duration>(result.diagnostics.receipt_time_uncertainty);
    const Clock::duration event_uncertainty = receipt_uncertainty + latency_half_width;
    result.diagnostics.fixed_transport_latency_upper_bound =
        std::chrono::duration<double>(latency_bound);
    result.diagnostics.device_event_time_uncertainty =
        std::chrono::duration<double>(event_uncertainty);
    for (const Clock::time_point receipt : result.mapped_host_receipt_times) {
      result.bounded_device_event_host_midpoints.push_back(receipt - latency_midpoint_offset);
      result.bounded_device_event_host_brackets.push_back({
          .earliest = receipt - latency_bound - receipt_uncertainty,
          .latest = receipt + receipt_uncertainty,
      });
    }
  }
  return result;
}

}  // namespace swing_capture
