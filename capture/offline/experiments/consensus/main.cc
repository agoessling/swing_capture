#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>  // NOLINT(misc-include-cleaner)
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/offline/experiments/consensus/consensus_evaluator.h"

namespace {

// NOLINTBEGIN(misc-include-cleaner)

std::string ReadText(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("unable to open " + path.string());
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::byte> ReadBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to open " + path.string());
  }
  const std::vector<char> raw{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()};
  const std::span<const std::byte> bytes = std::as_bytes(std::span(raw));
  return {bytes.begin(), bytes.end()};
}

void WriteText(const std::filesystem::path &path, const std::string &contents) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to write " + path.string());
  }
  output << contents;
  if (!output) {
    throw std::runtime_error("failed while writing " + path.string());
  }
}

}  // namespace

int main(int argc, char **argv) {
  const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
  if (argc != 9) {
    std::cerr
        << "usage: consensus_evaluator"
           " DTL.wav ATL.wav target_index.json face_session.json "
           "production_candidates.json face_envelope.json face_observations.csv output.json\n";
    return 2;
  }
  try {
    const swing_capture::offline::experiments::consensus::FieldInputs inputs{
        .down_the_line_audio = swing_capture::DecodeMonoPcmS16Wav(ReadBytes(arguments[1])),
        .face_on_audio = swing_capture::DecodeMonoPcmS16Wav(ReadBytes(arguments[2])),
        .target_index = nlohmann::json::parse(ReadText(arguments[3])),
        .face_on_session = nlohmann::json::parse(ReadText(arguments[4])),
        .production_candidates = nlohmann::json::parse(ReadText(arguments[5])),
        .face_on_envelope = nlohmann::json::parse(ReadText(arguments[6])),
        .face_on_observations_csv = ReadText(arguments[7]),
    };
    const nlohmann::json analysis =
        swing_capture::offline::experiments::consensus::AnalyzeFieldConsensus(inputs);
    WriteText(arguments[8], analysis.dump(2) + "\n");
  } catch (const std::exception &error) {
    std::cerr << "consensus evaluation failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

// NOLINTEND(misc-include-cleaner)
