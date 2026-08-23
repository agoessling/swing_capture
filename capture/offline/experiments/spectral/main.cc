#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>  // NOLINT(misc-include-cleaner)
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/offline/experiments/spectral/spectral_impact_experiment.h"

namespace {

// nlohmann's aggregate alias is provided by the directly included public header, but this clang
// tooling version does not associate the alias with that header.
// NOLINTBEGIN(misc-include-cleaner)

struct Arguments {
  std::filesystem::path down_the_line_wav;
  std::filesystem::path down_the_line_windows;
  std::filesystem::path face_on_wav;
  std::filesystem::path face_on_windows;
  std::filesystem::path output_json;
  std::filesystem::path output_markdown;
};

Arguments ParseArguments(int argc, char **argv) {
  Arguments result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(
        argv[index]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(option));
    }
    const std::filesystem::path value(
        argv[++index]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (option == "--dtl-wav") {
      result.down_the_line_wav = value;
    } else if (option == "--dtl-windows") {
      result.down_the_line_windows = value;
    } else if (option == "--atl-wav") {
      result.face_on_wav = value;
    } else if (option == "--atl-windows") {
      result.face_on_windows = value;
    } else if (option == "--output-json") {
      result.output_json = value;
    } else if (option == "--output-markdown") {
      result.output_markdown = value;
    } else {
      throw std::invalid_argument("unknown option " + std::string(option));
    }
  }
  if (result.down_the_line_wav.empty() || result.down_the_line_windows.empty() ||
      result.face_on_wav.empty() || result.face_on_windows.empty() || result.output_json.empty() ||
      result.output_markdown.empty()) {
    throw std::invalid_argument(
        "required: --dtl-wav PATH --dtl-windows PATH --atl-wav PATH --atl-windows PATH "
        "--output-json PATH --output-markdown PATH");
  }
  return result;
}

std::vector<std::byte> ReadBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    throw std::runtime_error("unable to open " + path.string());
  }
  const std::streamoff length = input.tellg();
  if (length < 0) {
    throw std::runtime_error("unable to measure " + path.string());
  }
  input.seekg(0, std::ios::beg);
  std::vector<char> characters(static_cast<std::size_t>(length));
  input.read(characters.data(), length);
  if (!input) {
    throw std::runtime_error("unable to read " + path.string());
  }
  const auto bytes = std::as_bytes(std::span(characters));
  return {bytes.begin(), bytes.end()};
}

nlohmann::json ReadJson(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("unable to open " + path.string());
  }
  nlohmann::json result;
  input >> result;
  return result;
}

void WriteText(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to open output " + path.string());
  }
  output << contents;
  if (!output) {
    throw std::runtime_error("unable to write output " + path.string());
  }
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Arguments arguments = ParseArguments(argc, argv);
    const std::vector<swing_capture::offline::spectral::RecordingInput> recordings = {
        {.view = "down_the_line",
         .device = "Pixel 5a",
         .audio = swing_capture::DecodeMonoPcmS16Wav(ReadBytes(arguments.down_the_line_wav)),
         .production_windows = ReadJson(arguments.down_the_line_windows)},
        {.view = "face_on",
         .device = "Pixel 6",
         .audio = swing_capture::DecodeMonoPcmS16Wav(ReadBytes(arguments.face_on_wav)),
         .production_windows = ReadJson(arguments.face_on_windows)},
    };
    const nlohmann::json report =
        swing_capture::offline::spectral::EvaluateSpectralImpactExperiment(recordings);
    WriteText(arguments.output_json, report.dump(2) + "\n");
    WriteText(arguments.output_markdown,
              swing_capture::offline::spectral::RenderSpectralImpactExperimentMarkdown(report));
  } catch (const std::exception &error) {
    std::cerr << "spectral impact experiment failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

// NOLINTEND(misc-include-cleaner)
