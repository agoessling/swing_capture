#include "capture/hil/application_audio_stimulus.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "capture/hil/feather_hil_protocol.h"
#include "capture/trigger/impact_detector.h"

namespace {

using Clock = std::chrono::steady_clock;
using swing_capture::ImpactDetector;
using swing_capture::ImpactEvent;
using swing_capture::hil::BuildFeatherToneCommand;
using swing_capture::hil::kApplicationAudioStimulus;

constexpr std::uint32_t kSampleRateHz = 32'000;
constexpr std::size_t kBlockFrames = 256;
constexpr std::size_t kBaselineFrames = kSampleRateHz * 300U / 1'000U;
constexpr std::size_t kStimulusFrames =
    kSampleRateHz * static_cast<std::size_t>(kApplicationAudioStimulus.duration.count()) /
    1'000'000U;
constexpr float kPassingMicrophonePeak = 0.17F;

static_assert(kStimulusFrames == 320U);
static_assert(kApplicationAudioStimulus.duration > std::chrono::microseconds(1'500));

std::int16_t Pcm(float normalized_amplitude) {
  return static_cast<std::int16_t>(std::lround(normalized_amplitude * 32767.0F));
}

std::vector<std::int16_t> FixtureWaveform(float peak_amplitude, std::size_t block_phase = 0) {
  constexpr std::size_t kTrailingFrames = kSampleRateHz * 10U / 1'000U;
  const std::size_t baseline_frames = kBaselineFrames + block_phase;
  std::vector<std::int16_t> samples(baseline_frames + kStimulusFrames + kTrailingFrames);

  // This deterministic background is close to the 0.0122 normalized RMS in
  // the passing station artifact. It exercises the adaptive threshold instead
  // of relying only on minimum_peak_amplitude.
  for (std::size_t index = 0; index < baseline_frames; ++index) {
    samples[index] = Pcm(index % 2U == 0U ? 0.012F : -0.012F);
  }

  // Firmware emits a square wave and toggles its sign every eight samples for
  // 2 kHz at 32 kHz. peak_amplitude models the microphone-domain result after
  // amplifier, speaker, air, and microphone gain rather than the I2S level.
  for (std::size_t index = 0; index < kStimulusFrames; ++index) {
    const bool positive = ((index / 8U) % 2U) == 0U;
    samples[baseline_frames + index] = Pcm(positive ? peak_amplitude : -peak_amplitude);
  }
  return samples;
}

std::vector<ImpactEvent> Detect(std::span<const std::int16_t> samples) {
  ImpactDetector detector;
  std::vector<ImpactEvent> detected;
  const auto origin = Clock::time_point(std::chrono::seconds(10));
  for (std::size_t offset = 0; offset < samples.size(); offset += kBlockFrames) {
    const std::size_t count = std::min(kBlockFrames, samples.size() - offset);
    std::array<ImpactEvent, 4> events;
    const auto block_offset = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(static_cast<double>(offset) / kSampleRateHz));
    const auto block_start = origin + block_offset;
    const auto result =
        detector.ProcessBlock(samples.subspan(offset, count), block_start, kSampleRateHz, events);
    detected.insert(detected.end(), events.begin(), events.begin() + result.events_written);
  }
  return detected;
}

void TenMillisecondFixtureStimulusTriggersAtEveryBlockPhase() {
  for (std::size_t phase = 0; phase < kBlockFrames; ++phase) {
    const std::vector<std::int16_t> samples = FixtureWaveform(kPassingMicrophonePeak, phase);
    const std::vector<ImpactEvent> events = Detect(samples);

    assert(events.size() == 1U);
    assert(events.front().sample_rate_hz == kSampleRateHz);
    assert(events.front().sample_index_in_block == (kBaselineFrames + phase) % kBlockFrames);
    assert(std::abs(events.front().peak_amplitude - kPassingMicrophonePeak) < 0.001F);
    assert(events.front().threshold_at_detection >= 0.05F);
    assert(events.front().peak_amplitude > events.front().threshold_at_detection);
    assert(events.front().confirmation_time - events.front().strike_time >=
           std::chrono::microseconds(1'500));
  }
}

void BelowMinimumPeakDoesNotTrigger() {
  const std::vector<std::int16_t> samples = FixtureWaveform(0.049F);
  assert(Detect(samples).empty());
}

void StimulusUsesNegotiatedFirmwareCommandShape() {
  const std::string command = BuildFeatherToneCommand(
      17, kApplicationAudioStimulus.lead.count(), kApplicationAudioStimulus.duration.count(),
      kApplicationAudioStimulus.frequency_hz, kApplicationAudioStimulus.level_permille);
  assert(command == "SC-HIL/1 17 TONE 100000 10000 2000 10\n");
}

}  // namespace

int main() {
  TenMillisecondFixtureStimulusTriggersAtEveryBlockPhase();
  BelowMinimumPeakDoesNotTrigger();
  StimulusUsesNegotiatedFirmwareCommandShape();
  return 0;
}
