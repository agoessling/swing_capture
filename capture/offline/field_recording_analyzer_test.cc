#include "capture/offline/field_recording_analyzer.h"

#include <cassert>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "capture/audio/pcm_wav.h"

namespace {

using swing_capture::DecodedMonoPcmS16Wav;
using swing_capture::offline::AnalyzeFieldRecordingPair;
using swing_capture::offline::FieldRecordingPair;

constexpr std::uint32_t kRate = 48'000;

DecodedMonoPcmS16Wav Audio(const std::vector<double> &impact_seconds) {
  std::vector<std::int16_t> samples(kRate * 10, 50);
  for (const double seconds : impact_seconds) {
    const auto frame = static_cast<std::size_t>(seconds * kRate);
    samples[frame] = 20'000;
    samples[frame + 1] = 30'000;
    samples[frame + 2] = -24'000;
  }
  return {.sample_rate_hz = kRate, .samples = std::move(samples)};
}

nlohmann::json Manifest(std::string role, std::string created_at) {
  return {{"schema_version", 1},
          {"session_kind", "field_recording"},
          {"recording_id", "recording-" + role},
          {"shared_recording_id", "field-pair"},
          {"node_id", "node-" + role},
          {"role", role},
          {"created_at_utc", created_at},
          {"audio",
           {{"encoding", "pcm_s16le"},
            {"channel_count", 1},
            {"sample_rate_hz", kRate},
            {"frames", std::to_string(kRate * 10)}}}};
}

void FindsAndAlignsPairedImpacts() {
  FieldRecordingPair pair = {
      .down_the_line_manifest = Manifest("down_the_line", "2026-08-22T19:15:03.250Z"),
      .down_the_line_audio = Audio({2.0, 3.2, 5.0, 8.0}),
      .face_on_manifest = Manifest("face_on", "2026-08-22T19:15:03.500Z"),
      .face_on_audio = Audio({1.75, 4.75, 6.4, 7.75}),
  };

  const nlohmann::json result = AnalyzeFieldRecordingPair(pair);

  assert(result.at("paired_candidate_count") == 3);
  assert(result.at("paired_candidates").at(0).at("paired_candidate_id") == "P001");
  assert(result.at("paired_candidates").at(2).at("paired_candidate_id") == "P003");
  assert(result.at("streams").at("down_the_line").at("candidate_count") == 4);
  assert(result.at("streams").at("face_on").at("candidate_count") == 4);
  const auto offset = std::stoll(
      result.at("alignment").at("face_on_time_minus_down_the_line_us").get<std::string>());
  assert(offset >= -250'050 && offset <= -249'950);
  assert(result.at("paired_candidates").at(0).at("down_the_line_window_start_us") == "0");
  assert(result.at("paired_candidates").at(2).at("face_on_window_end_us") == "9750020");
  assert(result.at("unmatched_candidates").at("down_the_line").size() == 1);
  assert(result.at("unmatched_candidates").at("face_on").size() == 1);
  assert(result.at("detectors").at("down_the_line").at("minimum_peak_amplitude_ppm") == 6'000);
  const std::string csv = swing_capture::offline::FieldRecordingPairedCandidatesCsv(result);
  assert(csv.starts_with("paired_candidate_id,down_the_line_impact_time_us"));
  assert(csv.find("P001,2000020,1750020") != std::string::npos);
}

void RejectsMismatchedAudioMetadata() {
  FieldRecordingPair pair = {
      .down_the_line_manifest = Manifest("down_the_line", "2026-08-22T19:15:03Z"),
      .down_the_line_audio = Audio({2.0}),
      .face_on_manifest = Manifest("face_on", "2026-08-22T19:15:03Z"),
      .face_on_audio = Audio({2.0}),
  };
  pair.face_on_manifest["audio"]["frames"] = "1";
  bool rejected = false;
  try {
    static_cast<void>(AnalyzeFieldRecordingPair(pair));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

}  // namespace

int main() {
  FindsAndAlignsPairedImpacts();
  RejectsMismatchedAudioMetadata();
  return 0;
}
