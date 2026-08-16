#include "capture/audio/pcm_wav.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using swing_capture::DecodeMonoPcmS16Wav;
using swing_capture::EncodeMonoPcmS16Wav;
using swing_capture::kMaximumMonoPcmS16WavBytes;

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

template <typename Integer>
void PutLittleEndian(std::vector<std::byte> &bytes, std::size_t offset, Integer value) {
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    bytes[offset + index] = static_cast<std::byte>(value & 0xffU);
    value >>= 8U;
  }
}

void ExpectDecodeRejected(std::span<const std::byte> wav) {
  bool rejected = false;
  try {
    static_cast<void>(DecodeMonoPcmS16Wav(wav));
  } catch (const std::invalid_argument &) {
    rejected = true;
  } catch (const std::length_error &) {
    rejected = true;
  }
  assert(rejected);
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

void TestDecodeRoundTrip() {
  constexpr std::array<std::int16_t, 6> kSamples = {
      std::numeric_limits<std::int16_t>::min(), -12345, -1, 0, 12345,
      std::numeric_limits<std::int16_t>::max(),
  };
  const auto decoded = DecodeMonoPcmS16Wav(EncodeMonoPcmS16Wav(kSamples, 48000));
  assert(decoded.sample_rate_hz == 48000);
  assert(decoded.samples == std::vector<std::int16_t>(kSamples.begin(), kSamples.end()));

  const auto empty = DecodeMonoPcmS16Wav(EncodeMonoPcmS16Wav({}, 1));
  assert(empty.sample_rate_hz == 1);
  assert(empty.samples.empty());
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

void TestRejectsTruncatedAndExtendedInput() {
  constexpr std::array<std::int16_t, 3> kSamples = {100, -200, 300};
  const auto wav = EncodeMonoPcmS16Wav(kSamples, 48000);
  for (std::size_t size = 0; size < wav.size(); ++size) {
    ExpectDecodeRejected(std::span<const std::byte>(wav.data(), size));
  }

  auto extended = wav;
  extended.push_back(std::byte{0});
  ExpectDecodeRejected(extended);
}

void TestRejectsNoncanonicalHeaders() {
  const auto canonical = EncodeMonoPcmS16Wav(std::array<std::int16_t, 1>{123}, 48000);
  for (const std::size_t offset : {0U, 8U, 12U, 36U}) {
    auto malformed = canonical;
    malformed[offset] = std::byte{0x58};
    ExpectDecodeRejected(malformed);
  }

  auto malformed = canonical;
  PutLittleEndian(malformed, 4, std::uint32_t{0});
  ExpectDecodeRejected(malformed);

  malformed = canonical;
  PutLittleEndian(malformed, 16, std::uint32_t{18});
  ExpectDecodeRejected(malformed);

  for (const std::pair<std::size_t, std::uint16_t> &field : {
           std::pair<std::size_t, std::uint16_t>{20U, 3U},
           {22U, 2U},
           {32U, 4U},
           {34U, 24U},
       }) {
    malformed = canonical;
    PutLittleEndian(malformed, field.first, field.second);
    ExpectDecodeRejected(malformed);
  }

  malformed = canonical;
  PutLittleEndian(malformed, 24, std::uint32_t{0});
  ExpectDecodeRejected(malformed);

  malformed = canonical;
  PutLittleEndian(malformed, 24, std::numeric_limits<std::uint32_t>::max());
  ExpectDecodeRejected(malformed);

  malformed = canonical;
  PutLittleEndian(malformed, 28, std::uint32_t{95999});
  ExpectDecodeRejected(malformed);

  malformed = canonical;
  PutLittleEndian(malformed, 40, std::uint32_t{0});
  ExpectDecodeRejected(malformed);
}

void TestRejectsOddDataAndOversizedInput() {
  auto odd = EncodeMonoPcmS16Wav({}, 48000);
  odd.push_back(std::byte{0x7f});
  PutLittleEndian(odd, 4, std::uint32_t{37});
  PutLittleEndian(odd, 40, std::uint32_t{1});
  ExpectDecodeRejected(odd);

  const std::vector<std::byte> oversized(kMaximumMonoPcmS16WavBytes + 1U);
  ExpectDecodeRejected(oversized);
}

}  // namespace

int main() {
  TestCanonicalMonoWav();
  TestDecodeRoundTrip();
  TestRejectsZeroRate();
  TestRejectsTruncatedAndExtendedInput();
  TestRejectsNoncanonicalHeaders();
  TestRejectsOddDataAndOversizedInput();
  return 0;
}
