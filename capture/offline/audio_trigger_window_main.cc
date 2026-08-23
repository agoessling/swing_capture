#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/offline/audio_trigger_window_analyzer.h"
#include "capture/trigger/impact_detector.h"

namespace {

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

struct Arguments {
  std::filesystem::path wav;
  std::filesystem::path output;
  std::string starts_ms;
  std::string targets_ms;
  std::string device;
};

Arguments ParseArguments(int argc, char **argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(
        argv[index]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(option));
    }
    const std::string value(
        argv[++index]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (option == "--wav") {
      arguments.wav = value;
    } else if (option == "--starts-ms") {
      arguments.starts_ms = value;
    } else if (option == "--targets-ms") {
      arguments.targets_ms = value;
    } else if (option == "--device") {
      arguments.device = value;
    } else if (option == "--output") {
      arguments.output = value;
    } else {
      throw std::invalid_argument("unknown option " + std::string(option));
    }
  }
  if (arguments.wav.empty() || arguments.starts_ms.empty() || arguments.targets_ms.empty() ||
      (arguments.device != "pixel5a" && arguments.device != "pixel6")) {
    throw std::invalid_argument(
        "required: --wav PATH --starts-ms CSV --targets-ms CSV "
        "--device pixel5a|pixel6 [--output PATH]");
  }
  return arguments;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<std::int64_t> ParseMilliseconds(std::string_view encoded, std::string_view name) {
  std::vector<std::int64_t> values;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t separator = encoded.find(',', start);
    const std::string field(encoded.substr(start, separator - start));
    std::size_t consumed = 0;
    const double milliseconds = std::stod(field, &consumed);
    if (consumed != field.size() || !std::isfinite(milliseconds) || milliseconds < 0) {
      throw std::invalid_argument(std::string(name) + " contains an invalid timestamp");
    }
    values.push_back(std::llround(milliseconds * 1'000.0));
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return values;
}

swing_capture::ImpactDetectorConfig DetectorConfig(std::string_view device) {
  swing_capture::ImpactDetectorConfig config = {
      .threshold_multiplier = 8.0F,
      .minimum_peak_amplitude = 0.015F,
      .initial_noise_floor = 0.005F,
      .noise_update_clip_multiplier = 4.0F,
      .noise_floor_time_constant_seconds = 0.5,
      .peak_confirmation_seconds = 0.0015,
      .refractory_period_seconds = 0.25,
  };
  if (device == "pixel5a") {
    config.minimum_peak_amplitude = 0.010F;
  }
  return config;
}

std::vector<swing_capture::offline::AudioTriggerWindow> Windows(
    const std::vector<std::int64_t> &starts, const std::vector<std::int64_t> &targets) {
  if (starts.size() != targets.size()) {
    throw std::invalid_argument("starts-ms and targets-ms must have equal lengths");
  }
  std::vector<swing_capture::offline::AudioTriggerWindow> windows;
  windows.reserve(starts.size());
  for (std::size_t index = 0; index < starts.size(); ++index) {
    const std::size_t ordinal = index + 1;
    const std::string id =
        ordinal < 10 ? "S0" + std::to_string(ordinal) : "S" + std::to_string(ordinal);
    windows.push_back({.id = id,
                       .start_us = starts[index],
                       .target_us = targets[index],
                       .end_us = targets[index] + 150'000});
  }
  return windows;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Arguments arguments = ParseArguments(argc, argv);
    const auto audio = swing_capture::DecodeMonoPcmS16Wav(ReadBytes(arguments.wav));
    const auto starts = ParseMilliseconds(arguments.starts_ms, "starts-ms");
    const auto targets = ParseMilliseconds(arguments.targets_ms, "targets-ms");
    const auto result = swing_capture::offline::AnalyzeAudioTriggerWindows(
        audio, DetectorConfig(arguments.device), Windows(starts, targets));
    const std::string encoded = result.dump(2) + "\n";
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
  } catch (const std::exception &error) {
    std::cerr << "audio trigger window analysis failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
