#include "capture/core/robust_device_clock_mapping.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using swing_capture::DeviceHostTimestampSample;
using swing_capture::FitRobustDeviceClockMapping;
using swing_capture::kMaximumRobustDeviceClockSamples;
using swing_capture::kMaximumRobustDeviceClockSlopePairs;
using swing_capture::RobustDeviceClockMappingOptions;

constexpr std::uint64_t kTicksPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kFrameTicks = 4'405'286ULL;
constexpr double kRateRatio = 1.000025;
constexpr auto kFixedLatency = std::chrono::milliseconds(7);

int failures = 0;

void Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct SyntheticCapture {
  std::vector<DeviceHostTimestampSample> samples;
  std::vector<Clock::time_point> ideal_receipt_times;
};

SyntheticCapture MakeCapture(std::size_t count) {
  SyntheticCapture capture;
  capture.samples.reserve(count);
  capture.ideal_receipt_times.reserve(count);
  const Clock::time_point event_origin = Clock::time_point(std::chrono::seconds(100));
  constexpr std::uint64_t kFirstDeviceTimestamp = 5'000'000'000ULL;
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint64_t device_timestamp = kFirstDeviceTimestamp + index * kFrameTicks;
    const double device_seconds =
        static_cast<double>(device_timestamp - kFirstDeviceTimestamp) / kTicksPerSecond;
    const auto ideal = event_origin + kFixedLatency +
                       std::chrono::round<Clock::duration>(
                           std::chrono::duration<double>(device_seconds * kRateRatio));
    const auto jitter =
        index % 2U == 0U ? std::chrono::microseconds(100) : -std::chrono::microseconds(100);
    capture.samples.push_back({
        .device_timestamp = device_timestamp,
        .host_received_at = ideal + jitter,
    });
    capture.ideal_receipt_times.push_back(ideal);
  }
  return capture;
}

double Seconds(Clock::duration duration) { return std::chrono::duration<double>(duration).count(); }

void FitsCleanCaptureAndReportsBoundedStorage() {
  const auto capture = MakeCapture(300);
  const auto result = FitRobustDeviceClockMapping(capture.samples, kTicksPerSecond);

  Expect(result.diagnostics.sample_count == 300, "sample count retained");
  Expect(result.diagnostics.slope_pair_count == 44'850, "all bounded pair slopes used");
  Expect(result.diagnostics.slope_pair_count <= kMaximumRobustDeviceClockSlopePairs,
         "slope storage respects compile-time bound");
  Expect(std::abs(result.diagnostics.clock_rate_ratio - kRateRatio) < 1e-9,
         "clean robust rate fit");
  Expect(result.outlier_sample_indices.empty(),
         "alternating sub-millisecond jitter is inlier data");
  Expect(result.mapped_host_receipt_times.size() == capture.samples.size(),
         "one mapped estimate per sample");
  for (std::size_t index = 0; index < capture.samples.size(); ++index) {
    Expect(std::abs(Seconds(result.mapped_host_receipt_times[index] -
                            capture.ideal_receipt_times[index])) < 2e-6,
           "clean mapped receipt estimate");
  }
  Expect(result.diagnostics.receipt_time_uncertainty < std::chrono::milliseconds(1),
         "clean fit uncertainty remains sub-millisecond");
  Expect(!result.diagnostics.fixed_transport_latency_upper_bound.has_value(),
         "fixed transport bias remains explicitly unbounded by default");
  Expect(result.bounded_device_event_host_brackets.empty(),
         "no absolute device-event bracket is invented");
}

void ResistsNinetyNineMillisecondQueuedDeliveryBurst() {
  auto capture = MakeCapture(300);
  constexpr std::size_t kBurstBegin = 120;
  constexpr std::size_t kBurstEnd = 143;
  const auto resumed = capture.ideal_receipt_times[kBurstBegin] + std::chrono::milliseconds(99);
  for (std::size_t index = kBurstBegin; index < kBurstEnd; ++index) {
    capture.samples[index].host_received_at =
        resumed + (index - kBurstBegin) * std::chrono::microseconds(1);
  }

  const auto result = FitRobustDeviceClockMapping(capture.samples, kTicksPerSecond);
  Expect(std::abs(result.diagnostics.clock_rate_ratio - kRateRatio) < 1e-8,
         "queued burst does not tilt robust clock rate");
  Expect(result.diagnostics.outlier_count >= 20 && result.diagnostics.outlier_count <= 24,
         "queued frames are explicitly classified as outliers");
  Expect(result.diagnostics.maximum_positive_queue_delay > std::chrono::milliseconds(90),
         "queued-delivery delay is exposed in diagnostics");
  Expect(result.diagnostics.receipt_time_uncertainty < std::chrono::milliseconds(2),
         "queued burst leaves a bounded sub-frame normal receipt uncertainty");
  for (std::size_t index = 0; index < capture.samples.size(); ++index) {
    Expect(std::abs(Seconds(result.mapped_host_receipt_times[index] -
                            capture.ideal_receipt_times[index])) < 2e-4,
           "burst-resistant mapped estimate");
  }
}

void ConvertsExplicitTransportBoundIntoEventBrackets() {
  const auto capture = MakeCapture(64);
  constexpr auto kLatencyBound = std::chrono::milliseconds(10);
  const auto result = FitRobustDeviceClockMapping(
      capture.samples, kTicksPerSecond,
      RobustDeviceClockMappingOptions{.fixed_transport_latency_upper_bound = kLatencyBound});

  Expect(result.diagnostics.fixed_transport_latency_upper_bound == kLatencyBound,
         "caller latency bound retained");
  Expect(result.diagnostics.device_event_time_uncertainty.has_value(),
         "bounded device-event uncertainty reported");
  Expect(result.bounded_device_event_host_midpoints.size() == capture.samples.size(),
         "bounded event midpoint returned for every sample");
  Expect(result.bounded_device_event_host_brackets.size() == capture.samples.size(),
         "bounded event bracket returned for every sample");
  const auto receipt_uncertainty =
      std::chrono::ceil<Clock::duration>(result.diagnostics.receipt_time_uncertainty);
  for (std::size_t index = 0; index < capture.samples.size(); ++index) {
    const auto &bracket = result.bounded_device_event_host_brackets[index];
    const auto actual_event_time = capture.ideal_receipt_times[index] - kFixedLatency;
    Expect(bracket.earliest <= actual_event_time && actual_event_time <= bracket.latest,
           "independently bounded fixed latency contains synthetic device event");
    Expect(result.bounded_device_event_host_midpoints[index] ==
               result.mapped_host_receipt_times[index] - kLatencyBound / 2,
           "event midpoint centers the fixed-latency interval");
    Expect(bracket.earliest ==
                   result.mapped_host_receipt_times[index] - kLatencyBound - receipt_uncertainty &&
               bracket.latest == result.mapped_host_receipt_times[index] + receipt_uncertainty,
           "event bracket includes fit uncertainty and one-sided transport bound");
  }
}

template <typename Function>
void ExpectInvalidArgument(Function &&function, const std::string &message) {
  bool rejected = false;
  try {
    function();
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Expect(rejected, message);
}

void RejectsUnboundedAndMalformedInputs() {
  auto capture = MakeCapture(7);
  ExpectInvalidArgument(
      [&] { (void)FitRobustDeviceClockMapping(capture.samples, kTicksPerSecond); },
      "too few samples rejected");

  capture = MakeCapture(kMaximumRobustDeviceClockSamples + 1U);
  ExpectInvalidArgument(
      [&] { (void)FitRobustDeviceClockMapping(capture.samples, kTicksPerSecond); },
      "sample count beyond fixed production bound rejected");

  capture = MakeCapture(16);
  capture.samples[8].device_timestamp = capture.samples[7].device_timestamp;
  ExpectInvalidArgument(
      [&] { (void)FitRobustDeviceClockMapping(capture.samples, kTicksPerSecond); },
      "nonmonotonic device time rejected");

  capture = MakeCapture(16);
  capture.samples[8].host_received_at =
      capture.samples[7].host_received_at - std::chrono::steady_clock::duration{1};
  ExpectInvalidArgument(
      [&] { (void)FitRobustDeviceClockMapping(capture.samples, kTicksPerSecond); },
      "decreasing host receipt time rejected");

  capture = MakeCapture(16);
  ExpectInvalidArgument([&] { (void)FitRobustDeviceClockMapping(capture.samples, 0); },
                        "zero device frequency rejected");

  ExpectInvalidArgument(
      [&] {
        (void)FitRobustDeviceClockMapping(
            capture.samples, kTicksPerSecond,
            RobustDeviceClockMappingOptions{.fixed_transport_latency_upper_bound =
                                                -std::chrono::milliseconds(1)});
      },
      "negative fixed transport bound rejected");
}

}  // namespace

int main() {
  FitsCleanCaptureAndReportsBoundedStorage();
  ResistsNinetyNineMillisecondQueuedDeliveryBurst();
  ConvertsExplicitTransportBoundIntoEventBrackets();
  RejectsUnboundedAndMalformedInputs();
  return failures == 0 ? 0 : 1;
}
