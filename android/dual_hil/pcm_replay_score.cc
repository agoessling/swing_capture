#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "android/dual_hil/pcm_replay_case.h"

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::android::dual_hil::FindPcmReplayCase;
using swing_capture::android::dual_hil::ParsePcmReplayManifest;
using swing_capture::android::dual_hil::PcmReplayExpectation;
using swing_capture::android::dual_hil::PcmReplayObservation;
using swing_capture::android::dual_hil::PcmReplayPairOutcomeName;
using swing_capture::android::dual_hil::ScorePcmReplayObservations;

std::string ReadText(const std::filesystem::path &path) {
  constexpr std::uintmax_t kMaximumInputBytes = 4ULL * 1024ULL * 1024ULL;
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error("input is not a regular file: " + path.string());
  }
  const std::uintmax_t size = std::filesystem::file_size(path);
  if (size == 0U || size > kMaximumInputBytes || size > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("input size is invalid: " + path.string());
  }
  std::string contents(static_cast<std::size_t>(size), '\0');
  std::ifstream input(path, std::ios::binary);
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!input) {
    throw std::runtime_error("cannot read input: " + path.string());
  }
  return contents;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 3) {
      throw std::invalid_argument(
          "usage: pcm_replay_score <case-manifest.json> <observations.json>");
    }
    const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
    const auto manifest = ParsePcmReplayManifest(ReadText(arguments[1]));
    const Json input = Json::parse(ReadText(arguments[2]));
    if (input.value("schema_version", 0) != 2 ||
        input.value("source_id", "") != manifest.source_id ||
        !input.at("observations").is_array()) {
      throw std::invalid_argument("PCM replay observations have the wrong identity or schema");
    }
    std::vector<PcmReplayObservation> observations;
    Json rows = Json::array();
    for (const Json &item : input.at("observations")) {
      const Json &leader = item.at("leader");
      const Json &shadow = item.at("shadow");
      const auto source = [](const Json &role) {
        return role.at("source").is_null() ? std::string() : role.at("source").get<std::string>();
      };
      PcmReplayObservation observation{
          .case_name = item.value("case_name", ""),
          .source_crc32 = item.value("source_crc32", 0U),
          .gain_permille = item.value("gain_permille", 0U),
          .leader_detected = leader.value("automatic_trigger_observed", false),
          .shadow_detected = shadow.value("automatic_trigger_observed", false),
          .leader_source = source(leader),
          .shadow_source = source(shadow),
      };
      if (item.value("pair_outcome", "") != PcmReplayPairOutcomeName(observation.pair_outcome()) ||
          item.value("paired_automatic_trigger_observed", !observation.paired_detected()) !=
              observation.paired_detected() ||
          item.value("any_automatic_trigger_observed", !observation.any_detected()) !=
              observation.any_detected()) {
        throw std::invalid_argument("PCM replay aggregate and per-role outcomes disagree");
      }
      const auto &definition = FindPcmReplayCase(manifest, observation.case_name);
      rows.push_back({
          {"case_name", observation.case_name},
          {"expectation", definition.expectation == PcmReplayExpectation::kRequiredPositive
                              ? "required_positive"
                              : "diagnostic_negative"},
          {"pair_outcome", PcmReplayPairOutcomeName(observation.pair_outcome())},
          {"paired_automatic_trigger_observed", observation.paired_detected()},
          {"any_automatic_trigger_observed", observation.any_detected()},
          {"source_id", manifest.source_id},
          {"source_crc32", observation.source_crc32},
          {"gain_permille", observation.gain_permille},
          {"leader",
           {{"automatic_trigger_observed", observation.leader_detected},
            {"source",
             observation.leader_source.empty() ? Json(nullptr) : Json(observation.leader_source)}}},
          {"shadow",
           {{"automatic_trigger_observed", observation.shadow_detected},
            {"source",
             observation.shadow_source.empty() ? Json(nullptr) : Json(observation.shadow_source)}}},
      });
      observations.push_back(std::move(observation));
    }
    const auto score = ScorePcmReplayObservations(manifest, observations);
    const std::size_t required_missed =
        score.required_positive_count - score.required_positive_detected;
    const std::size_t diagnostic_not_detected =
        score.diagnostic_negative_count - score.diagnostic_negative_detected;
    std::cout << Json({
                          {"schema_version", 1},
                          {"source_id", manifest.source_id},
                          {"required_gate_passed", score.required_gate_passed},
                          {"confusion_matrix",
                           {{"required_positive_detected", score.required_positive_detected},
                            {"required_positive_missed", required_missed},
                            {"diagnostic_negative_detected", score.diagnostic_negative_detected},
                            {"diagnostic_negative_not_detected", diagnostic_not_detected}}},
                          {"observations", std::move(rows)},
                      })
                     .dump(2)
              << '\n';
    return score.required_gate_passed ? 0 : 2;
  } catch (const std::exception &failure) {
    std::cerr << "PCM replay scoring failed: " << failure.what() << '\n';
    return 1;
  }
}
