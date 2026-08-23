#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>  // NOLINT(misc-include-cleaner)
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "android/dual_hil/pcm_replay_case.h"

namespace {

using swing_capture::android::dual_hil::ExtractPcmReplayCase;
using swing_capture::android::dual_hil::FindPcmReplayCase;
using swing_capture::android::dual_hil::ParsePcmReplayManifest;

std::vector<char> ReadFile(const std::filesystem::path &path, std::uintmax_t maximum_bytes) {
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error("input is not a regular file: " + path.string());
  }
  const std::uintmax_t size = std::filesystem::file_size(path);
  if (size == 0U || size > maximum_bytes || size > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("input size is invalid: " + path.string());
  }
  std::vector<char> contents(static_cast<std::size_t>(size));
  std::ifstream input(path, std::ios::binary);
  input.read(contents.data(), static_cast<std::streamsize>(size));
  if (!input) {
    throw std::runtime_error("cannot read input: " + path.string());
  }
  return contents;
}

void WritePcm16Le(const std::filesystem::path &path, const std::vector<std::int16_t> &samples) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open PCM output: " + path.string());
  }
  for (const std::int16_t sample : samples) {
    const auto bits = static_cast<std::uint16_t>(sample);
    const std::array<char, 2> bytes = {static_cast<char>(bits & 0xffU),
                                       static_cast<char>(bits >> 8U)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  if (!output) {
    throw std::runtime_error("cannot write PCM output: " + path.string());
  }
}

}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 5) {
      throw std::invalid_argument(
          "usage: pcm_replay_extract <manifest.json> <source.wav> <case-name> <output.pcm>");
    }
    const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
    const std::vector<char> manifest_bytes = ReadFile(arguments[1], 1024ULL * 1024ULL);
    const std::string manifest_text(manifest_bytes.begin(), manifest_bytes.end());
    const auto manifest = ParsePcmReplayManifest(manifest_text);
    const std::vector<char> source_wav = ReadFile(arguments[2], 64ULL * 1024ULL * 1024ULL);
    const auto extracted = ExtractPcmReplayCase(manifest, FindPcmReplayCase(manifest, arguments[3]),
                                                std::as_bytes(std::span(source_wav)));
    const std::filesystem::path output_path(arguments[4]);
    WritePcm16Le(output_path, extracted.samples);
    std::cout << nlohmann::json({
                                    {"case_name", extracted.definition.name},
                                    {"source_id", extracted.source_id},
                                    {"source_start_frame", extracted.definition.start_frame},
                                    {"sample_count", extracted.definition.sample_count},
                                    {"marker_frame", extracted.definition.marker_frame},
                                    {"sample_rate_hz", extracted.sample_rate_hz},
                                    {"source_crc32", extracted.source_crc32},
                                    {"gain_permille", extracted.definition.gain_permille},
                                    {"output", output_path.string()},
                                })
                     .dump(2)
              << '\n';
    return 0;
  } catch (const std::exception &failure) {
    std::cerr << "PCM replay extraction failed: " << failure.what() << '\n';
    return 1;
  }
}
