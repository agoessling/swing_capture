#include "capture/trigger/impact_detector.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using swing_capture::ImpactDetector;
using swing_capture::ImpactDetectorConfig;
using swing_capture::ImpactEvent;

constexpr std::int16_t Pcm(float normalized_amplitude) {
  return static_cast<std::int16_t>(normalized_amplitude * 32767.0F);
}

Clock::time_point SampleTime(Clock::time_point block_start, std::size_t sample_index,
                             std::uint32_t sample_rate_hz) {
  return block_start +
         std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
             static_cast<double>(sample_index) / static_cast<double>(sample_rate_hz)));
}

void SilenceDoesNotTrigger() {
  ImpactDetector detector;
  constexpr std::uint32_t kSampleRate = 48000;
  const std::vector<std::int16_t> silence(kSampleRate * 2, 0);
  std::array<ImpactEvent, 2> events;

  const auto result = detector.ProcessBlock(silence, Clock::time_point(std::chrono::seconds(1)),
                                            kSampleRate, events);

  assert(result.events_detected == 0);
  assert(result.events_written == 0);
  assert(!detector.peak_confirmation_pending());
}

void ReportsPeakSampleTimeWithinBlock() {
  ImpactDetectorConfig config;
  config.peak_confirmation_seconds = 0.0005;
  ImpactDetector detector(config);
  constexpr std::uint32_t kSampleRate = 48000;
  constexpr std::size_t kPeakIndex = 123;
  std::array<std::int16_t, 240> samples{};
  samples[kPeakIndex - 1] = Pcm(0.45F);
  samples[kPeakIndex] = Pcm(-0.90F);
  samples[kPeakIndex + 1] = Pcm(0.60F);
  std::array<ImpactEvent, 2> events;
  const auto block_start = Clock::time_point(std::chrono::seconds(7));

  const auto result = detector.ProcessBlock(samples, block_start, kSampleRate, events);

  assert(result.events_detected == 1);
  assert(result.events_written == 1);
  assert(events[0].sample_index_in_block == kPeakIndex);
  assert(events[0].source_block_start == block_start);
  assert(events[0].sample_rate_hz == kSampleRate);
  assert(events[0].strike_time == SampleTime(block_start, kPeakIndex, kSampleRate));
  assert(events[0].confirmation_time == SampleTime(block_start, kPeakIndex + 24, kSampleRate));
  assert(events[0].confirmation_time >= events[0].strike_time);
  assert(std::abs(events[0].peak_amplitude - 0.90F) < 0.001F);
}

void AdaptsThresholdToSteadyBackgroundNoise() {
  ImpactDetectorConfig config;
  config.minimum_peak_amplitude = 0.04F;
  config.threshold_multiplier = 5.0F;
  config.initial_noise_floor = 0.002F;
  config.noise_floor_time_constant_seconds = 0.1;
  ImpactDetector detector(config);
  constexpr std::uint32_t kSampleRate = 16000;
  std::vector<std::int16_t> background(kSampleRate);
  for (std::size_t index = 0; index < background.size(); ++index) {
    background[index] = index % 2 == 0 ? Pcm(0.03F) : Pcm(-0.03F);
  }
  std::array<ImpactEvent, 4> events;
  const auto origin = Clock::time_point(std::chrono::seconds(20));

  const auto background_result = detector.ProcessBlock(background, origin, kSampleRate, events);

  assert(background_result.events_detected == 0);
  assert(detector.noise_floor() > 0.028F);
  assert(detector.detection_threshold() > 0.14F);

  std::array<std::int16_t, 128> impact{};
  impact[40] = Pcm(0.75F);
  const auto impact_start = origin + std::chrono::seconds(1);
  const auto impact_result = detector.ProcessBlock(impact, impact_start, kSampleRate, events);

  assert(impact_result.events_detected == 1);
  assert(events[0].sample_index_in_block == 40);
}

void SuppressesImpactsDuringRefractoryPeriod() {
  ImpactDetectorConfig config;
  config.peak_confirmation_seconds = 0.00025;
  config.refractory_period_seconds = 0.1;
  ImpactDetector detector(config);
  constexpr std::uint32_t kSampleRate = 48000;
  std::vector<std::int16_t> samples(8000);
  samples[100] = Pcm(0.9F);
  samples[1000] = Pcm(0.95F);
  samples[6000] = Pcm(0.85F);
  std::array<ImpactEvent, 4> events;

  const auto result = detector.ProcessBlock(samples, Clock::time_point(std::chrono::seconds(30)),
                                            kSampleRate, events);

  assert(result.events_detected == 2);
  assert(result.events_written == 2);
  assert(events[0].sample_index_in_block == 100);
  assert(events[1].sample_index_in_block == 6000);
}

void ConfirmsPeakAcrossBlockBoundary() {
  ImpactDetectorConfig config;
  config.peak_confirmation_seconds = 0.001;
  ImpactDetector detector(config);
  constexpr std::uint32_t kSampleRate = 8000;
  std::array<std::int16_t, 64> first{};
  first.back() = Pcm(-0.8F);
  std::array<std::int16_t, 32> second{};
  std::array<ImpactEvent, 2> events;
  const auto first_start = Clock::time_point(std::chrono::seconds(40));

  const auto first_result = detector.ProcessBlock(first, first_start, kSampleRate, events);
  assert(first_result.events_detected == 0);
  assert(detector.peak_confirmation_pending());

  const auto second_start = SampleTime(first_start, first.size(), kSampleRate);
  const auto second_result = detector.ProcessBlock(second, second_start, kSampleRate, events);

  assert(second_result.events_detected == 1);
  assert(events[0].source_block_start == first_start);
  assert(events[0].sample_index_in_block == first.size() - 1);
  assert(events[0].strike_time == SampleTime(first_start, first.size() - 1, kSampleRate));
  assert(events[0].confirmation_time == SampleTime(second_start, 7, kSampleRate));
}

void LongBlockGapDoesNotMergeDistinctImpacts() {
  ImpactDetectorConfig config;
  config.peak_confirmation_seconds = 0.001;
  config.refractory_period_seconds = 0.1;
  ImpactDetector detector(config);
  constexpr std::uint32_t kSampleRate = 8000;
  const std::array<std::int16_t, 1> first = {Pcm(0.60F)};
  std::array<std::int16_t, 32> second{};
  second[0] = Pcm(-0.95F);
  std::array<ImpactEvent, 3> events;
  const auto first_start = Clock::time_point(std::chrono::seconds(45));

  const auto first_result = detector.ProcessBlock(first, first_start, kSampleRate, events);
  assert(first_result.events_detected == 0);
  assert(detector.peak_confirmation_pending());

  const auto second_start = first_start + std::chrono::seconds(1);
  const auto second_result = detector.ProcessBlock(second, second_start, kSampleRate, events);

  assert(second_result.events_detected == 2);
  assert(second_result.events_written == 2);
  assert(events[0].source_block_start == first_start);
  assert(events[0].sample_index_in_block == 0);
  assert(events[0].confirmation_time == second_start);
  assert(std::abs(events[0].peak_amplitude - 0.60F) < 0.001F);
  assert(events[1].source_block_start == second_start);
  assert(events[1].sample_index_in_block == 0);
  assert(events[1].confirmation_time > events[1].strike_time);
  assert(std::abs(events[1].peak_amplitude - 0.95F) < 0.001F);
}

void PreservesTimingAtDifferentSampleRates() {
  const auto run = [](std::uint32_t sample_rate_hz, std::size_t peak_index) {
    ImpactDetectorConfig config;
    config.peak_confirmation_seconds = 0.00025;
    ImpactDetector detector(config);
    std::vector<std::int16_t> samples(peak_index + sample_rate_hz / 100 + 2);
    samples[peak_index] = Pcm(0.85F);
    std::array<ImpactEvent, 1> events;
    const auto start = Clock::time_point(std::chrono::seconds(50));

    const auto result = detector.ProcessBlock(samples, start, sample_rate_hz, events);
    assert(result.events_detected == 1);
    assert(events[0].sample_rate_hz == sample_rate_hz);
    assert(events[0].sample_index_in_block == peak_index);
    assert(events[0].strike_time == SampleTime(start, peak_index, sample_rate_hz));
    assert(events[0].confirmation_time >= events[0].strike_time);
  };

  run(8000, 37);
  run(96000, 731);
}

}  // namespace

int main() {
  SilenceDoesNotTrigger();
  ReportsPeakSampleTimeWithinBlock();
  AdaptsThresholdToSteadyBackgroundNoise();
  SuppressesImpactsDuringRefractoryPeriod();
  ConfirmsPeakAcrossBlockBoundary();
  LongBlockGapDoesNotMergeDistinctImpacts();
  PreservesTimingAtDifferentSampleRates();
  return 0;
}
