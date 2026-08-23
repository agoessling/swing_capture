#include "android/dual_hil/pcm_replay_case.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capture/audio/pcm_wav.h"

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

PcmReplayExpectation ParseExpectation(std::string_view value) {
  if (value == "required_positive") {
    return PcmReplayExpectation::kRequiredPositive;
  }
  if (value == "diagnostic_negative") {
    return PcmReplayExpectation::kDiagnosticNegative;
  }
  throw std::invalid_argument("PCM replay case has an unsupported expectation");
}

std::uint32_t PcmCrc32(std::span<const std::int16_t> samples) {
  std::uint32_t checksum = std::numeric_limits<std::uint32_t>::max();
  for (const std::int16_t sample : samples) {
    const auto bits = static_cast<std::uint16_t>(sample);
    for (const std::uint8_t byte :
         {static_cast<std::uint8_t>(bits & 0xffU), static_cast<std::uint8_t>(bits >> 8U)}) {
      checksum ^= byte;
      for (int bit = 0; bit < 8; ++bit) {
        checksum = (checksum >> 1U) ^ (0xedb88320U & static_cast<std::uint32_t>(-(checksum & 1U)));
      }
    }
  }
  return checksum ^ std::numeric_limits<std::uint32_t>::max();
}

}  // namespace

std::string_view PcmReplayPairOutcomeName(PcmReplayPairOutcome outcome) {
  switch (outcome) {
    case PcmReplayPairOutcome::kNeither:
      return "neither";
    case PcmReplayPairOutcome::kLeaderOnly:
      return "leader_only";
    case PcmReplayPairOutcome::kShadowOnly:
      return "shadow_only";
    case PcmReplayPairOutcome::kBoth:
      return "both";
  }
  throw std::invalid_argument("unknown PCM replay pair outcome");
}

PcmReplayManifest ParsePcmReplayManifest(std::string_view manifest_json) {
  try {
    const Json parsed = Json::parse(manifest_json);
    if (parsed.value("schema_version", 0) != 1 || parsed.value("source_id", "").empty() ||
        parsed.value("source_sample_rate_hz", 0U) != 48'000U || !parsed.at("cases").is_array() ||
        parsed.at("cases").empty()) {
      throw std::invalid_argument("PCM replay manifest identity or format is invalid");
    }
    PcmReplayManifest manifest{
        .source_id = parsed.at("source_id").get<std::string>(),
        .source_sample_rate_hz = parsed.at("source_sample_rate_hz").get<std::uint32_t>(),
        .cases = {},
    };
    std::set<std::string, std::less<>> names;
    for (const Json &item : parsed.at("cases")) {
      PcmReplayCase replay_case{
          .name = item.value("name", ""),
          .expectation = ParseExpectation(item.value("expectation", "")),
          .start_frame = item.value("start_frame", std::numeric_limits<std::uint64_t>::max()),
          .sample_count = item.value("sample_count", 0U),
          .marker_frame = item.value("marker_frame", std::numeric_limits<std::uint32_t>::max()),
          .gain_permille = item.value("gain_permille", 0U),
          .expected_crc32 = item.value("expected_crc32", 0U),
      };
      if (replay_case.name.empty() || !names.insert(replay_case.name).second ||
          replay_case.sample_count == 0U || replay_case.sample_count > 12'000U ||
          replay_case.marker_frame >= replay_case.sample_count || replay_case.gain_permille == 0U ||
          replay_case.gain_permille > 1'000U || replay_case.expected_crc32 == 0U) {
        throw std::invalid_argument("PCM replay case bounds or identity are invalid");
      }
      manifest.cases.push_back(std::move(replay_case));
    }
    return manifest;
  } catch (const nlohmann::json::exception &failure) {
    throw std::invalid_argument(std::string("cannot parse PCM replay manifest: ") + failure.what());
  }
}

const PcmReplayCase &FindPcmReplayCase(const PcmReplayManifest &manifest, std::string_view name) {
  const auto found = std::ranges::find(manifest.cases, name, &PcmReplayCase::name);
  if (found == manifest.cases.end()) {
    throw std::invalid_argument("PCM replay case name is not in the manifest");
  }
  return *found;
}

ExtractedPcmReplayCase ExtractPcmReplayCase(const PcmReplayManifest &manifest,
                                            const PcmReplayCase &replay_case,
                                            std::span<const std::byte> source_wav) {
  const DecodedMonoPcmS16Wav decoded = DecodeMonoPcmS16Wav(source_wav);
  if (decoded.sample_rate_hz != manifest.source_sample_rate_hz) {
    throw std::invalid_argument("PCM replay source WAV does not have the manifested sample rate");
  }
  if (replay_case.start_frame > decoded.samples.size() ||
      replay_case.sample_count > decoded.samples.size() - replay_case.start_frame) {
    throw std::invalid_argument("PCM replay case extends beyond the source WAV");
  }
  const auto start = decoded.samples.begin() + static_cast<std::ptrdiff_t>(replay_case.start_frame);
  std::vector<std::int16_t> samples(start, start + replay_case.sample_count);
  const std::uint32_t source_crc32 = PcmCrc32(samples);
  if (source_crc32 != replay_case.expected_crc32) {
    throw std::invalid_argument("PCM replay source WAV does not match the manifested case CRC32");
  }
  return {
      .definition = replay_case,
      .source_id = manifest.source_id,
      .sample_rate_hz = decoded.sample_rate_hz,
      .source_crc32 = source_crc32,
      .samples = std::move(samples),
  };
}

PcmReplayScore ScorePcmReplayObservations(const PcmReplayManifest &manifest,
                                          std::span<const PcmReplayObservation> observations) {
  std::set<std::string, std::less<>> observed;
  PcmReplayScore score;
  score.required_positive_count = static_cast<std::size_t>(std::ranges::count(
      manifest.cases, PcmReplayExpectation::kRequiredPositive, &PcmReplayCase::expectation));
  for (const PcmReplayObservation &observation : observations) {
    if (!observed.insert(observation.case_name).second) {
      throw std::invalid_argument("PCM replay observations contain a duplicate case");
    }
    const PcmReplayCase &replay_case = FindPcmReplayCase(manifest, observation.case_name);
    if (observation.source_crc32 != replay_case.expected_crc32 ||
        observation.gain_permille != replay_case.gain_permille) {
      throw std::invalid_argument("PCM replay observation stimulus identity does not match case");
    }
    if (observation.leader_detected != !observation.leader_source.empty() ||
        observation.shadow_detected != !observation.shadow_source.empty() ||
        (observation.leader_detected && observation.leader_source != "local_audio") ||
        (observation.shadow_detected &&
         observation.shadow_source != "peer_audio_clock_candidate")) {
      throw std::invalid_argument(
          "PCM replay per-role outcome and production trigger sources disagree");
    }
    if (replay_case.expectation == PcmReplayExpectation::kRequiredPositive) {
      score.required_positive_detected += observation.paired_detected() ? 1U : 0U;
    } else {
      ++score.diagnostic_negative_count;
      score.diagnostic_negative_detected += observation.any_detected() ? 1U : 0U;
    }
  }
  score.required_gate_passed = score.required_positive_count > 0U &&
                               score.required_positive_detected == score.required_positive_count &&
                               score.diagnostic_negative_detected == 0U;
  return score;
}

}  // namespace swing_capture::android::dual_hil
