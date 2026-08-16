#include "android/dual_hil/audio_evidence_validation.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <nlohmann/json.hpp>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "capture/audio/pcm_wav.h"

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::AudioHilCheck;
using swing_capture::EncodeMonoPcmS16Wav;
using swing_capture::android::dual_hil::AnalyzeRetainedAudioEvidence;
using swing_capture::android::dual_hil::kRetainedAudioEvidenceSampleCount;
using swing_capture::android::dual_hil::kRetainedAudioEvidenceSampleRateHz;
using swing_capture::android::dual_hil::kRetainedAudioEvidenceStrikeSampleIndex;
using swing_capture::android::dual_hil::kRetainedAudioEvidenceWavBytes;
using swing_capture::android::dual_hil::RetainedAudioEvidence;
using swing_capture::android::dual_hil::RetainedAudioEvidenceInspection;

constexpr std::uint64_t kFirstFramePosition = 1000000U;
constexpr std::uint64_t kAcceptedDeviceMicroseconds = 5000000U;
constexpr std::uint64_t kImpactDeviceMicroseconds = 6220000U;

struct Fixture {
  std::string report;
  std::string manifest;
  std::string wav;
};

Json Metadata(std::string path) {
  return {
      {"path", std::move(path)},
      {"bytes", kRetainedAudioEvidenceWavBytes},
      {"sample_rate_hz", kRetainedAudioEvidenceSampleRateHz},
      {"first_frame_position", std::to_string(kFirstFramePosition)},
      {"last_frame_position",
       std::to_string(kFirstFramePosition + kRetainedAudioEvidenceSampleCount - 1U)},
      {"strike_frame_position",
       std::to_string(kFirstFramePosition + kRetainedAudioEvidenceStrikeSampleIndex)},
      {"sample_count", kRetainedAudioEvidenceSampleCount},
      {"strike_sample_index", kRetainedAudioEvidenceStrikeSampleIndex},
  };
}

std::vector<std::int16_t> QuietSamples() {
  std::vector<std::int16_t> samples(kRetainedAudioEvidenceSampleCount);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    samples[index] = index % 2U == 0U ? 80 : -80;
  }
  return samples;
}

void AddTone(std::vector<std::int16_t> *samples, std::uint32_t frequency_hz,
             double amplitude = 9000.0) {
  constexpr std::size_t kToneSamples = kRetainedAudioEvidenceSampleRateHz / 100U;
  for (std::size_t index = 0; index < kToneSamples; ++index) {
    const double phase = 2.0 * std::numbers::pi_v<double> * frequency_hz *
                         static_cast<double>(index) / kRetainedAudioEvidenceSampleRateHz;
    (*samples)[kRetainedAudioEvidenceStrikeSampleIndex + index] =
        static_cast<std::int16_t>(std::sin(phase) * amplitude);
  }
}

Fixture MakeFixture(std::span<const std::int16_t> samples) {
  constexpr std::string_view kSessionId = "audio-hil-session";
  const Json report = {
      {"retained_session",
       {
           {"session_id", kSessionId},
           {"audio_evidence", Metadata("sessions/audio-hil-session/audio_evidence.wav")},
       }},
  };
  const Json manifest = {
      {"session_id", kSessionId},
      {"android_capture", {{"audio_evidence", Metadata("audio_evidence.wav")}}},
  };
  const std::vector<std::byte> wav =
      EncodeMonoPcmS16Wav(samples, kRetainedAudioEvidenceSampleRateHz);
  return {
      .report = report.dump(),
      .manifest = manifest.dump(),
      .wav = std::string(reinterpret_cast<const char *>(wav.data()), wav.size()),
  };
}

RetainedAudioEvidence Analyze(const Fixture &fixture) {
  return AnalyzeRetainedAudioEvidence({
      .report = fixture.report,
      .manifest = fixture.manifest,
      .wav = fixture.wav,
      .feather_accepted_device_microseconds = kAcceptedDeviceMicroseconds,
      .feather_impact_scheduled_device_microseconds = kImpactDeviceMicroseconds,
  });
}

const AudioHilCheck &FindCheck(const RetainedAudioEvidence &evidence, std::string_view name) {
  const auto check = std::ranges::find(evidence.evaluation.checks, name, &AudioHilCheck::name);
  assert(check != evidence.evaluation.checks.end());
  return *check;
}

template <typename Action>
void ExpectFailure(Action action) {
  try {
    action();
  } catch (const std::exception &) {
    return;
  }
  assert(false);
}

void CommandedTenMillisecondTonePasses() {
  auto samples = QuietSamples();
  AddTone(&samples, 2000U);
  const RetainedAudioEvidence evidence = Analyze(MakeFixture(samples));

  assert(evidence.evaluation.passed);
  assert(evidence.thresholds.minimum_event_rms_normalized_amplitude == 0.004);
  assert(FindCheck(evidence, "tone_energy").passed);
  assert(FindCheck(evidence, "tone_frequency").passed);
  assert(FindCheck(evidence, "tone_duration").passed);
  assert(FindCheck(evidence, "event_clipping").passed);
  assert(evidence.feather_command_to_impact_microseconds == 1220000U);
  assert(evidence.inferred_command_sample_offset == 13440U);
  assert(evidence.guarded_background_required_samples == 12480U);
  assert(evidence.required_end_sample_offset <= evidence.sample_count);
  assert(evidence.evaluation.measurements.estimated_frequency_hz >= 1970.0);
  assert(evidence.evaluation.measurements.estimated_frequency_hz <= 2030.0);
  assert(evidence.evaluation.measurements.measured_active_duration_seconds >= 0.006);
  assert(evidence.evaluation.measurements.measured_active_duration_seconds <= 0.020);
}

void Pixel5aLevelTonePassesEnergyAndIdentityGates() {
  auto samples = QuietSamples();
  for (std::int16_t &sample : samples) {
    sample = sample >= 0 ? 10 : -10;
  }
  AddTone(&samples, 2000U, 210.0);
  const RetainedAudioEvidence evidence = Analyze(MakeFixture(samples));
  assert(evidence.evaluation.measurements.event_rms_normalized_amplitude >= 0.004);
  assert(evidence.evaluation.measurements.event_rms_normalized_amplitude < 0.005);
  assert(evidence.evaluation.passed);
  assert(FindCheck(evidence, "tone_energy").passed);
  assert(FindCheck(evidence, "tone_frequency").passed);
}

void BroadbandImpulseIsRejected() {
  auto samples = QuietSamples();
  std::uint32_t state = 0x12345678U;
  for (std::size_t index = 0; index < 480U; ++index) {
    state = state * 1664525U + 1013904223U;
    samples[kRetainedAudioEvidenceStrikeSampleIndex + index] =
        static_cast<std::int16_t>((state >> 16U) - 32768U);
  }
  const RetainedAudioEvidence evidence = Analyze(MakeFixture(samples));
  assert(!evidence.evaluation.passed);
  assert(!FindCheck(evidence, "tone_frequency").passed);
}

void WrongFrequencyIsRejected() {
  auto samples = QuietSamples();
  AddTone(&samples, 3000U);
  const RetainedAudioEvidence evidence = Analyze(MakeFixture(samples));
  assert(!evidence.evaluation.passed);
  assert(!FindCheck(evidence, "tone_frequency").passed);
  assert(evidence.evaluation.measurements.frequency_error_hz > 800.0);
}

void TruncatedWavIsRejected() {
  auto samples = QuietSamples();
  AddTone(&samples, 2000U);
  Fixture fixture = MakeFixture(samples);
  fixture.wav.resize(fixture.wav.size() - 2U);
  ExpectFailure([&] { static_cast<void>(Analyze(fixture)); });
}

void PositionAndWindowContractIsRejected() {
  auto samples = QuietSamples();
  AddTone(&samples, 2000U);
  Fixture fixture = MakeFixture(samples);
  Json manifest = Json::parse(fixture.manifest);
  manifest["android_capture"]["audio_evidence"]["strike_frame_position"] = "1000001";
  fixture.manifest = manifest.dump();
  ExpectFailure([&] { static_cast<void>(Analyze(fixture)); });

  fixture = MakeFixture(samples);
  ExpectFailure([&] {
    static_cast<void>(AnalyzeRetainedAudioEvidence({
        .report = fixture.report,
        .manifest = fixture.manifest,
        .wav = fixture.wav,
        .feather_accepted_device_microseconds = 0U,
        .feather_impact_scheduled_device_microseconds = 1500000U,
    }));
  });
}

}  // namespace

int main() {
  CommandedTenMillisecondTonePasses();
  Pixel5aLevelTonePassesEnergyAndIdentityGates();
  BroadbandImpulseIsRejected();
  WrongFrequencyIsRejected();
  TruncatedWavIsRejected();
  PositionAndWindowContractIsRejected();
  return 0;
}
