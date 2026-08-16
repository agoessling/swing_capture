#include "android/dual_hil/audio_evidence_validation.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "capture/audio/commanded_tone_analyzer.h"
#include "capture/audio/pcm_wav.h"
#include "embedded/prop_maker/swing_sequence.h"

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr std::string_view kAudioFilename = "audio_evidence.wav";
constexpr std::size_t kAudioMetadataFieldCount = 8U;

[[noreturn]] void Invalid(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

std::uint64_t DecimalPosition(const Json &value, std::string_view field) {
  if (!value.is_string()) {
    Invalid(std::string(field) + " must be a decimal string");
  }
  const std::string text = value.get<std::string>();
  if (text.empty() || !std::ranges::all_of(text, [](char character) {
        return character >= '0' && character <= '9';
      })) {
    Invalid(std::string(field) + " must contain only decimal digits");
  }
  try {
    std::size_t consumed = 0;
    const std::uint64_t parsed = std::stoull(text, &consumed);
    if (consumed != text.size()) {
      Invalid(std::string(field) + " contains trailing characters");
    }
    return parsed;
  } catch (const std::exception &) {
    Invalid(std::string(field) + " is outside the unsigned 64-bit range");
  }
}

std::uint64_t UnsignedNumber(const Json &object, std::string_view field) {
  const auto value = object.find(field);
  if (value == object.end() || !value->is_number_unsigned()) {
    Invalid(std::string(field) + " must be an unsigned integer");
  }
  return value->get<std::uint64_t>();
}

struct AudioMetadata {
  std::string path;
  std::uint64_t bytes = 0;
  std::uint64_t sample_rate_hz = 0;
  std::uint64_t first_frame_position = 0;
  std::uint64_t last_frame_position = 0;
  std::uint64_t strike_frame_position = 0;
  std::uint64_t sample_count = 0;
  std::uint64_t strike_sample_index = 0;
};

struct MetadataExpectation {
  std::string_view path;
  std::string_view owner;
};

AudioMetadata ParseMetadata(const Json &value, const MetadataExpectation &expectation) {
  if (!value.is_object() || value.size() != kAudioMetadataFieldCount) {
    Invalid(std::string(expectation.owner) + " audio_evidence must contain exactly eight fields");
  }
  AudioMetadata metadata;
  metadata.path = value.value("path", "");
  metadata.bytes = UnsignedNumber(value, "bytes");
  metadata.sample_rate_hz = UnsignedNumber(value, "sample_rate_hz");
  metadata.first_frame_position =
      DecimalPosition(value.at("first_frame_position"), "first_frame_position");
  metadata.last_frame_position =
      DecimalPosition(value.at("last_frame_position"), "last_frame_position");
  metadata.strike_frame_position =
      DecimalPosition(value.at("strike_frame_position"), "strike_frame_position");
  metadata.sample_count = UnsignedNumber(value, "sample_count");
  metadata.strike_sample_index = UnsignedNumber(value, "strike_sample_index");
  if (metadata.path != expectation.path) {
    Invalid(std::string(expectation.owner) + " audio_evidence path is not canonical");
  }
  return metadata;
}

void RequireExactMetadata(const AudioMetadata &metadata) {
  if (metadata.bytes != kRetainedAudioEvidenceWavBytes ||
      metadata.sample_rate_hz != kRetainedAudioEvidenceSampleRateHz ||
      metadata.sample_count != kRetainedAudioEvidenceSampleCount ||
      metadata.strike_sample_index != kRetainedAudioEvidenceStrikeSampleIndex) {
    Invalid("retained audio metadata differs from the fixed evidence contract");
  }
  if (metadata.first_frame_position >
          std::numeric_limits<std::uint64_t>::max() - metadata.strike_sample_index ||
      metadata.first_frame_position + metadata.strike_sample_index !=
          metadata.strike_frame_position ||
      metadata.first_frame_position >
          std::numeric_limits<std::uint64_t>::max() - (metadata.sample_count - 1U) ||
      metadata.first_frame_position + metadata.sample_count - 1U != metadata.last_frame_position) {
    Invalid("retained audio absolute frame positions are internally inconsistent");
  }
}

void RequireMatchingMetadata(const AudioMetadata &report, const AudioMetadata &manifest) {
  if (report.bytes != manifest.bytes || report.sample_rate_hz != manifest.sample_rate_hz ||
      report.first_frame_position != manifest.first_frame_position ||
      report.last_frame_position != manifest.last_frame_position ||
      report.strike_frame_position != manifest.strike_frame_position ||
      report.sample_count != manifest.sample_count ||
      report.strike_sample_index != manifest.strike_sample_index) {
    Invalid("report and manifest audio_evidence metadata disagree");
  }
}

std::uint64_t RoundedSamples(std::uint64_t microseconds, std::uint32_t sample_rate_hz) {
  constexpr std::uint64_t kMicrosecondsPerSecond = 1000000U;
  constexpr std::uint64_t kHalfMicrosecondSecond = kMicrosecondsPerSecond / 2U;
  if (microseconds >
      (std::numeric_limits<std::uint64_t>::max() - kHalfMicrosecondSecond) / sample_rate_hz) {
    Invalid("Feather command-to-impact interval overflows sample conversion");
  }
  return (microseconds * sample_rate_hz + kHalfMicrosecondSecond) / kMicrosecondsPerSecond;
}

std::uint64_t DurationSamples(std::chrono::nanoseconds duration, std::uint32_t sample_rate_hz) {
  constexpr std::uint64_t kNanosecondsPerSecond = 1000000000U;
  const auto count = static_cast<std::uint64_t>(duration.count());
  if (count >
      (std::numeric_limits<std::uint64_t>::max() - (kNanosecondsPerSecond - 1U)) / sample_rate_hz) {
    Invalid("audio analysis duration overflows sample conversion");
  }
  return (count * sample_rate_hz + kNanosecondsPerSecond - 1U) / kNanosecondsPerSecond;
}

std::uint64_t RequiredEndSample(const RetainedAudioEvidence &evidence) {
  const std::uint64_t lead =
      DurationSamples(evidence.stimulus.lead, kRetainedAudioEvidenceSampleRateHz);
  const std::uint64_t latency = DurationSamples(evidence.thresholds.maximum_additional_latency,
                                                kRetainedAudioEvidenceSampleRateHz);
  const std::uint64_t duration =
      DurationSamples(evidence.stimulus.duration, kRetainedAudioEvidenceSampleRateHz);
  const std::uint64_t scan_tail = DurationSamples(evidence.thresholds.maximum_active_duration,
                                                  kRetainedAudioEvidenceSampleRateHz) +
                                  DurationSamples(evidence.thresholds.spectral_frame_duration,
                                                  kRetainedAudioEvidenceSampleRateHz);
  const std::uint64_t tail = std::max(duration, scan_tail);
  std::uint64_t required_end = evidence.inferred_command_sample_offset;
  for (const std::uint64_t increment : {lead, latency, tail}) {
    if (increment > std::numeric_limits<std::uint64_t>::max() - required_end) {
      Invalid("required commanded-tone analysis window overflows");
    }
    required_end += increment;
  }
  return required_end;
}

}  // namespace

RetainedAudioEvidence AnalyzeRetainedAudioEvidence(
    const RetainedAudioEvidenceInspection &inspection) {
  try {
    const Json report = Json::parse(inspection.report);
    const Json manifest = Json::parse(inspection.manifest);
    const Json &retained = report.at("retained_session");
    const std::string session_id = retained.at("session_id").get<std::string>();
    if (session_id.empty() || session_id.contains("..") ||
        manifest.at("session_id").get<std::string>() != session_id) {
      Invalid("audio evidence session identity is invalid");
    }
    const AudioMetadata report_metadata = ParseMetadata(
        retained.at("audio_evidence"),
        {.path = "sessions/" + session_id + "/" + std::string(kAudioFilename), .owner = "report"});
    const AudioMetadata manifest_metadata =
        ParseMetadata(manifest.at("android_capture").at("audio_evidence"),
                      {.path = kAudioFilename, .owner = "manifest"});
    RequireExactMetadata(report_metadata);
    RequireExactMetadata(manifest_metadata);
    RequireMatchingMetadata(report_metadata, manifest_metadata);
    if (inspection.wav.size() != report_metadata.bytes) {
      Invalid("retained WAV byte count differs from audio_evidence metadata");
    }
    const auto wav_characters = std::span<const char>(inspection.wav.data(), inspection.wav.size());
    const DecodedMonoPcmS16Wav decoded = DecodeMonoPcmS16Wav(std::as_bytes(wav_characters));
    if (decoded.sample_rate_hz != report_metadata.sample_rate_hz ||
        decoded.samples.size() != report_metadata.sample_count) {
      Invalid("decoded WAV rate or sample count differs from audio_evidence metadata");
    }
    if (inspection.feather_impact_scheduled_device_microseconds <
        inspection.feather_accepted_device_microseconds) {
      Invalid("Feather impact schedule precedes command acceptance");
    }

    RetainedAudioEvidence evidence;
    evidence.first_frame_position = report_metadata.first_frame_position;
    evidence.last_frame_position = report_metadata.last_frame_position;
    evidence.strike_frame_position = report_metadata.strike_frame_position;
    evidence.sample_count = report_metadata.sample_count;
    evidence.strike_sample_index = report_metadata.strike_sample_index;
    evidence.feather_command_to_impact_microseconds =
        inspection.feather_impact_scheduled_device_microseconds -
        inspection.feather_accepted_device_microseconds;
    if (evidence.feather_command_to_impact_microseconds >
        static_cast<std::uint64_t>(std::chrono::microseconds::max().count())) {
      Invalid("Feather command-to-impact interval exceeds the supported duration");
    }
    const std::uint64_t command_to_impact_samples = RoundedSamples(
        evidence.feather_command_to_impact_microseconds, kRetainedAudioEvidenceSampleRateHz);
    if (command_to_impact_samples > evidence.strike_sample_index) {
      Invalid("inferred Feather command predates retained audio evidence");
    }
    evidence.inferred_command_sample_offset =
        evidence.strike_sample_index - command_to_impact_samples;
    evidence.stimulus = {
        .lead = std::chrono::microseconds(evidence.feather_command_to_impact_microseconds),
        .duration = std::chrono::microseconds(SWING_HIL_SWING_TONE_DURATION_US),
        .frequency_hz = SWING_HIL_SWING_TONE_FREQUENCY_HZ,
    };
    // The Pixel 5a's qualified microphone path produces roughly 0.005 normalized RMS for the
    // maximum-safe fixture tone. Frequency, SNR, duration, and clipping remain independent gates,
    // so this floor only avoids rejecting a clearly identified low-gain recording on amplitude.
    evidence.thresholds.minimum_event_rms_normalized_amplitude = 0.004;
    evidence.thresholds.minimum_active_duration = std::chrono::milliseconds(6);
    evidence.thresholds.maximum_active_duration = std::chrono::milliseconds(20);
    evidence.guarded_background_required_samples =
        DurationSamples(evidence.thresholds.background_duration,
                        kRetainedAudioEvidenceSampleRateHz) +
        DurationSamples(evidence.thresholds.background_guard, kRetainedAudioEvidenceSampleRateHz);
    if (evidence.inferred_command_sample_offset < evidence.guarded_background_required_samples) {
      Invalid("retained audio lacks the full guarded 250 ms pre-command background");
    }
    evidence.required_end_sample_offset = RequiredEndSample(evidence);
    if (evidence.required_end_sample_offset > decoded.samples.size()) {
      Invalid("retained audio lacks the required post-tone analysis window");
    }
    evidence.evaluation = AnalyzeCommandedTone(
        {
            .mono_samples = decoded.samples,
            .sample_rate_hz = decoded.sample_rate_hz,
            .commanded_sample_offset = evidence.inferred_command_sample_offset,
        },
        evidence.stimulus, evidence.thresholds);
    return evidence;
  } catch (const nlohmann::json::exception &failure) {
    throw std::runtime_error(std::string("cannot parse retained audio evidence: ") +
                             failure.what());
  }
}

}  // namespace swing_capture::android::dual_hil
