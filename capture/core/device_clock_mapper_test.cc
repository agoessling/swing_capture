#include "capture/core/device_clock_mapper.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace {

using swing_capture::DeviceClockMapper;
using SteadyClock = std::chrono::steady_clock;

void FitsRateAndEstimatesHostTime() {
  constexpr std::uint64_t kFrequency = 1000000000;
  DeviceClockMapper mapper(kFrequency);
  const SteadyClock::time_point host_origin = SteadyClock::time_point(std::chrono::seconds(100));
  constexpr double kHostPerDeviceRate = 1.000020;

  for (std::uint64_t index = 0; index < 100; ++index) {
    const std::uint64_t device_timestamp = 5000000000ULL + index * 10000000ULL;
    const double device_elapsed = static_cast<double>(index) * 0.01;
    const double deterministic_jitter = index % 2 == 0 ? 0.0001 : -0.0001;
    const auto host_timestamp =
        host_origin +
        std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(
            device_elapsed * kHostPerDeviceRate + deterministic_jitter));
    mapper.AddSample(device_timestamp, host_timestamp);
  }

  assert(mapper.ready());
  assert(std::abs(mapper.clock_rate_ratio() - kHostPerDeviceRate) < 0.00001);
  assert(std::abs(mapper.clock_drift_parts_per_million() - 20.0) < 10.0);
  assert(mapper.residual_standard_deviation_seconds() < 0.0002);

  const auto estimate = mapper.EstimateHostTime(6000000000ULL);
  assert(estimate.has_value());
  const double estimated_elapsed = std::chrono::duration<double>(*estimate - host_origin).count();
  assert(std::abs(estimated_elapsed - kHostPerDeviceRate) < 0.0002);
}

void RejectsNonMonotonicSamples() {
  DeviceClockMapper mapper(1000);
  mapper.AddSample(10, SteadyClock::time_point{});

  bool rejected = false;
  try {
    mapper.AddSample(10, SteadyClock::time_point{});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

void NeedsTwoSamples() {
  DeviceClockMapper mapper(1000);
  assert(!mapper.ready());
  assert(!mapper.EstimateHostTime(0).has_value());
  mapper.AddSample(10, SteadyClock::time_point{});
  assert(!mapper.ready());
}

}  // namespace

int main() {
  FitsRateAndEstimatesHostTime();
  RejectsNonMonotonicSamples();
  NeedsTwoSamples();
  return 0;
}
