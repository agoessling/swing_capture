#ifndef SWING_CAPTURE_CAPTURE_AUDIO_PCM_WAV_H_
#define SWING_CAPTURE_CAPTURE_AUDIO_PCM_WAV_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace swing_capture {

// Decoding is intended for short capture evidence, not arbitrarily large RIFF
// containers. The limit includes the canonical 44-byte header.
inline constexpr std::size_t kMaximumMonoPcmS16WavBytes = std::size_t{64} * 1024U * 1024U;

struct DecodedMonoPcmS16Wav {
  std::uint32_t sample_rate_hz = 0;
  std::vector<std::int16_t> samples;
};

// Encodes one signed 16-bit mono PCM stream as a canonical little-endian WAV.
[[nodiscard]] std::vector<std::byte> EncodeMonoPcmS16Wav(std::span<const std::int16_t> samples,
                                                         std::uint32_t sample_rate_hz);

// Accepts only the canonical RIFF/WAVE layout emitted by
// EncodeMonoPcmS16Wav: PCM format, one channel, 16-bit little-endian samples,
// and a single 16-byte "fmt " chunk followed immediately by "data". Throws
// std::invalid_argument for malformed input and std::length_error when input
// exceeds kMaximumMonoPcmS16WavBytes.
[[nodiscard]] DecodedMonoPcmS16Wav DecodeMonoPcmS16Wav(std::span<const std::byte> wav);

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_AUDIO_PCM_WAV_H_
