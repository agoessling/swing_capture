#include "capture/audio/pcm_wav.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using swing_capture::EncodeMonoPcmS16Wav;

std::string Text(const std::vector<std::byte> &bytes, std::size_t offset, std::size_t size) {
  std::string result;
  result.reserve(size);
  for (std::size_t index = 0; index < size; ++index) {
    result.push_back(std::to_integer<char>(bytes[offset + index]));
  }
  return result;
}

std::uint32_t Uint32(const std::vector<std::byte> &bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

void TestCanonicalMonoWav() {
  constexpr std::array<std::int16_t, 3> kSamples = {0, 32767, -32768};
  const auto wav = EncodeMonoPcmS16Wav(kSamples, 32000);
  assert(wav.size() == 50);
  assert(Text(wav, 0, 4) == "RIFF");
  assert(Uint32(wav, 4) == 42);
  assert(Text(wav, 8, 4) == "WAVE");
  assert(Text(wav, 36, 4) == "data");
  assert(Uint32(wav, 24) == 32000);
  assert(Uint32(wav, 28) == 64000);
  assert(Uint32(wav, 40) == 6);
  assert(wav[44] == std::byte{0x00} && wav[45] == std::byte{0x00});
  assert(wav[46] == std::byte{0xff} && wav[47] == std::byte{0x7f});
  assert(wav[48] == std::byte{0x00} && wav[49] == std::byte{0x80});
}

void TestRejectsZeroRate() {
  bool rejected = false;
  try {
    static_cast<void>(EncodeMonoPcmS16Wav({}, 0));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

}  // namespace

int main() {
  TestCanonicalMonoWav();
  TestRejectsZeroRate();
  return 0;
}
