#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>  // NOLINT(misc-include-cleaner)
#include <span>
#include <stdexcept>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/offline/experiments/envelope/envelope_experiment.h"

namespace {

// NOLINTBEGIN(misc-include-cleaner)

std::vector<std::byte> ReadBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("could not open WAV: " + path.string());
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 0) {
    throw std::runtime_error("could not determine WAV size");
  }
  input.seekg(0, std::ios::beg);
  std::vector<char> raw_bytes(static_cast<std::size_t>(size));
  input.read(raw_bytes.data(), size);
  if (!input) {
    throw std::runtime_error("could not read WAV");
  }
  std::vector<std::byte> bytes(raw_bytes.size());
  std::ranges::transform(raw_bytes, bytes.begin(), [](char value) {
    return static_cast<std::byte>(static_cast<unsigned char>(value));
  });
  return bytes;
}

nlohmann::json ReadJson(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("could not open JSON: " + path.string());
  }
  return nlohmann::json::parse(input);
}

// NOLINTEND(misc-include-cleaner)

}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 3) {
      std::cerr << "usage: envelope_experiment AUDIO.wav AUDIO_WINDOWS.json\n";
      return 2;
    }
    const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
    const auto audio = swing_capture::DecodeMonoPcmS16Wav(ReadBytes(arguments[1]));
    const auto windows = swing_capture::offline::envelope::ParseArmWindows(ReadJson(arguments[2]));
    std::cout << swing_capture::offline::envelope::Analyze(
                     audio, windows, swing_capture::offline::envelope::ExperimentConfigs())
                     .dump(2)
              << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
