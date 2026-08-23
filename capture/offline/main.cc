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
#include "capture/offline/field_recording_analyzer.h"

namespace {

// nlohmann's aggregate alias is provided by the directly included public header, but this clang
// tooling version does not associate the alias with that header.
// NOLINTBEGIN(misc-include-cleaner)

std::vector<std::byte> ReadBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to open " + path.string());
  }
  input.seekg(0, std::ios::end);
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
  const std::span<const std::byte> bytes = std::as_bytes(std::span(characters));
  return {bytes.begin(), bytes.end()};
}

nlohmann::json ReadJson(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("unable to open " + path.string());
  }
  return nlohmann::json::parse(input);
}

struct Arguments {
  std::filesystem::path down_manifest;
  std::filesystem::path down_wav;
  std::filesystem::path face_manifest;
  std::filesystem::path face_wav;
  std::filesystem::path output;
  std::filesystem::path csv_output;
  bool production_detector = false;
};

Arguments ParseArguments(int argc, char **argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(
        argv[index]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (option == "--production-detector") {
      arguments.production_detector = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(option));
    }
    const std::filesystem::path value(
        argv[++index]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (option == "--down-the-line-manifest") {
      arguments.down_manifest = value;
    } else if (option == "--down-the-line-wav") {
      arguments.down_wav = value;
    } else if (option == "--face-on-manifest") {
      arguments.face_manifest = value;
    } else if (option == "--face-on-wav") {
      arguments.face_wav = value;
    } else if (option == "--output") {
      arguments.output = value;
    } else if (option == "--csv-output") {
      arguments.csv_output = value;
    } else {
      throw std::invalid_argument("unknown option " + std::string(option));
    }
  }
  if (arguments.down_manifest.empty() || arguments.down_wav.empty() ||
      arguments.face_manifest.empty() || arguments.face_wav.empty()) {
    throw std::invalid_argument(
        "required: --down-the-line-manifest PATH --down-the-line-wav PATH "
        "--face-on-manifest PATH --face-on-wav PATH [--output PATH] [--csv-output PATH] "
        "[--production-detector]");
  }
  if (!arguments.output.empty() && arguments.csv_output.empty()) {
    arguments.csv_output = arguments.output;
    arguments.csv_output.replace_extension(".csv");
  }
  return arguments;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Arguments arguments = ParseArguments(argc, argv);
    const swing_capture::offline::FieldRecordingPair pair = {
        .down_the_line_manifest = ReadJson(arguments.down_manifest),
        .down_the_line_audio = swing_capture::DecodeMonoPcmS16Wav(ReadBytes(arguments.down_wav)),
        .face_on_manifest = ReadJson(arguments.face_manifest),
        .face_on_audio = swing_capture::DecodeMonoPcmS16Wav(ReadBytes(arguments.face_wav)),
    };
    swing_capture::offline::FieldRecordingAnalysisConfig config;
    if (arguments.production_detector) {
      config.down_the_line_detector = {
          .threshold_multiplier = 8.0F,
          .minimum_peak_amplitude = 0.010F,
          .initial_noise_floor = 0.005F,
          .noise_update_clip_multiplier = 4.0F,
          .noise_floor_time_constant_seconds = 0.5,
          .peak_confirmation_seconds = 0.0015,
          .refractory_period_seconds = 0.25,
      };
      config.face_on_detector = config.down_the_line_detector;
      config.face_on_detector.minimum_peak_amplitude = 0.015F;
    }
    const nlohmann::json analysis = swing_capture::offline::AnalyzeFieldRecordingPair(pair, config);
    const std::string encoded = analysis.dump(2) + "\n";
    if (arguments.output.empty()) {
      std::cout << encoded;
    } else {
      std::ofstream output(arguments.output);
      if (!output) {
        throw std::runtime_error("unable to open output " + arguments.output.string());
      }
      output << encoded;
      if (!output) {
        throw std::runtime_error("unable to write output " + arguments.output.string());
      }
    }
    if (!arguments.csv_output.empty()) {
      std::ofstream csv_output(arguments.csv_output);
      if (!csv_output) {
        throw std::runtime_error("unable to open CSV output " + arguments.csv_output.string());
      }
      csv_output << swing_capture::offline::FieldRecordingPairedCandidatesCsv(analysis);
      if (!csv_output) {
        throw std::runtime_error("unable to write CSV output " + arguments.csv_output.string());
      }
    }
  } catch (const std::exception &error) {
    std::cerr << "field recording analysis failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

// NOLINTEND(misc-include-cleaner)
