#include "capture/audio/pcm_wav.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace swing_capture {
namespace {

constexpr std::size_t kWavHeaderBytes = 44;

void WriteText(std::span<std::byte> output, std::size_t offset, std::string_view text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    output[offset + index] = static_cast<std::byte>(text[index]);
  }
}

template <typename Integer>
void WriteLittleEndian(std::span<std::byte> output, std::size_t offset, Integer value) {
  static_assert(std::is_unsigned_v<Integer>);
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    output[offset + index] = static_cast<std::byte>(value & 0xffU);
    value >>= 8U;
  }
}

bool MatchesText(std::span<const std::byte> input, std::size_t offset, std::string_view expected) {
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (input[offset + index] != static_cast<std::byte>(expected[index])) {
      return false;
    }
  }
  return true;
}

template <typename Integer>
Integer ReadLittleEndian(std::span<const std::byte> input, std::size_t offset) {
  static_assert(std::is_unsigned_v<Integer>);
  Integer value = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    value |= std::to_integer<Integer>(input[offset + index]) << (index * 8U);
  }
  return value;
}

}  // namespace

std::vector<std::byte> EncodeMonoPcmS16Wav(std::span<const std::int16_t> samples,
                                           std::uint32_t sample_rate_hz) {
  if (sample_rate_hz == 0) {
    throw std::invalid_argument("WAV sample rate must be positive");
  }
  constexpr std::size_t kBytesPerSample = sizeof(std::int16_t);
  if (sample_rate_hz > std::numeric_limits<std::uint32_t>::max() / kBytesPerSample) {
    throw std::invalid_argument("WAV sample rate overflows the byte rate");
  }
  constexpr std::uint64_t kMaximumDataBytes = std::numeric_limits<std::uint32_t>::max() - 36U;
  if (samples.size() > kMaximumDataBytes / kBytesPerSample) {
    throw std::length_error("PCM capture is too large for a RIFF WAV file");
  }
  const auto data_bytes = static_cast<std::uint32_t>(samples.size() * kBytesPerSample);
  std::vector<std::byte> wav(kWavHeaderBytes + data_bytes);
  WriteText(wav, 0, "RIFF");
  WriteLittleEndian(wav, 4, 36U + data_bytes);
  WriteText(wav, 8, "WAVE");
  WriteText(wav, 12, "fmt ");
  WriteLittleEndian(wav, 16, std::uint32_t{16});
  WriteLittleEndian(wav, 20, std::uint16_t{1});
  WriteLittleEndian(wav, 22, std::uint16_t{1});
  WriteLittleEndian(wav, 24, sample_rate_hz);
  WriteLittleEndian(wav, 28, sample_rate_hz * std::uint32_t{kBytesPerSample});
  WriteLittleEndian(wav, 32, std::uint16_t{kBytesPerSample});
  WriteLittleEndian(wav, 34, std::uint16_t{16});
  WriteText(wav, 36, "data");
  WriteLittleEndian(wav, 40, data_bytes);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    WriteLittleEndian(wav, kWavHeaderBytes + index * kBytesPerSample,
                      std::bit_cast<std::uint16_t>(samples[index]));
  }
  return wav;
}

DecodedMonoPcmS16Wav DecodeMonoPcmS16Wav(std::span<const std::byte> wav) {
  if (wav.size() > kMaximumMonoPcmS16WavBytes) {
    throw std::length_error("WAV input exceeds the decoder size limit");
  }
  if (wav.size() < kWavHeaderBytes) {
    throw std::invalid_argument("WAV input is shorter than the canonical header");
  }
  if (!MatchesText(wav, 0, "RIFF") || !MatchesText(wav, 8, "WAVE") ||
      !MatchesText(wav, 12, "fmt ") || !MatchesText(wav, 36, "data")) {
    throw std::invalid_argument("WAV input does not have the canonical chunk layout");
  }

  const auto riff_bytes = ReadLittleEndian<std::uint32_t>(wav, 4);
  if (riff_bytes != static_cast<std::uint32_t>(wav.size() - 8U)) {
    throw std::invalid_argument("WAV RIFF length does not match the input length");
  }
  if (ReadLittleEndian<std::uint32_t>(wav, 16) != 16U ||
      ReadLittleEndian<std::uint16_t>(wav, 20) != 1U ||
      ReadLittleEndian<std::uint16_t>(wav, 22) != 1U ||
      ReadLittleEndian<std::uint16_t>(wav, 32) != sizeof(std::int16_t) ||
      ReadLittleEndian<std::uint16_t>(wav, 34) != 16U) {
    throw std::invalid_argument("WAV input is not canonical mono PCM S16");
  }

  const auto sample_rate_hz = ReadLittleEndian<std::uint32_t>(wav, 24);
  constexpr auto kBytesPerSample = std::uint32_t{sizeof(std::int16_t)};
  if (sample_rate_hz == 0 ||
      sample_rate_hz > std::numeric_limits<std::uint32_t>::max() / kBytesPerSample ||
      ReadLittleEndian<std::uint32_t>(wav, 28) != sample_rate_hz * kBytesPerSample) {
    throw std::invalid_argument("WAV sample rate or byte rate is invalid");
  }

  const auto data_bytes = ReadLittleEndian<std::uint32_t>(wav, 40);
  if (data_bytes != static_cast<std::uint32_t>(wav.size() - kWavHeaderBytes)) {
    throw std::invalid_argument("WAV data length does not match the input length");
  }
  if (data_bytes % kBytesPerSample != 0U) {
    throw std::invalid_argument("WAV PCM S16 data length must be even");
  }

  DecodedMonoPcmS16Wav decoded;
  decoded.sample_rate_hz = sample_rate_hz;
  decoded.samples.resize(data_bytes / kBytesPerSample);
  for (std::size_t index = 0; index < decoded.samples.size(); ++index) {
    decoded.samples[index] = std::bit_cast<std::int16_t>(
        ReadLittleEndian<std::uint16_t>(wav, kWavHeaderBytes + index * kBytesPerSample));
  }
  return decoded;
}

}  // namespace swing_capture
