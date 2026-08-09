#ifndef SWING_CAPTURE_CAPTURE_AUDIO_PCM_WAV_H_
#define SWING_CAPTURE_CAPTURE_AUDIO_PCM_WAV_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace swing_capture {

// Encodes one signed 16-bit mono PCM stream as a canonical little-endian WAV.
[[nodiscard]] std::vector<std::byte> EncodeMonoPcmS16Wav(std::span<const std::int16_t> samples,
                                                         std::uint32_t sample_rate_hz);

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_AUDIO_PCM_WAV_H_
