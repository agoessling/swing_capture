#ifndef SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_PROTOCOL_H_
#define SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_PROTOCOL_H_

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::hil {

inline constexpr std::string_view kFeatherHilProtocol = "SC-HIL/1";
inline constexpr std::string_view kFeatherHilFirmware = "prop-maker-hil-9";
inline constexpr std::uint32_t kFeatherHilProtocolVersion = 1;
inline constexpr std::uint32_t kFeatherHilFixtureNeopixelGpio = 21;
inline constexpr std::string_view kFeatherHilFixtureNeopixelColorOrder = "rgb";
inline constexpr std::uint32_t kFeatherHilSharedPowerGpio = 23;
inline constexpr std::uint64_t kFeatherHilPrepareTimeoutMicroseconds = 10'000'000;
inline constexpr std::uint32_t kFeatherHilMaximumLeadMicroseconds = 2'000'000;
inline constexpr std::uint32_t kFeatherHilLedMinimumDurationMicroseconds = 100;
inline constexpr std::uint32_t kFeatherHilLedMaximumDurationMicroseconds = 1'000'000;
inline constexpr std::uint32_t kFeatherHilToneMinimumLeadMicroseconds = 20'000;
inline constexpr std::uint32_t kFeatherHilToneMinimumDurationMicroseconds = 1'000;
inline constexpr std::uint32_t kFeatherHilToneMaximumDurationMicroseconds = 250'000;
inline constexpr std::uint32_t kFeatherHilToneMinimumFrequencyHz = 100;
inline constexpr std::uint32_t kFeatherHilToneMaximumFrequencyHz = 10'000;
inline constexpr std::uint32_t kFeatherHilToneMinimumLevelPermille = 1;
inline constexpr std::uint32_t kFeatherHilToneMaximumLevelPermille = 125;
inline constexpr std::uint32_t kFeatherHilToneSampleRateHz = 32'000;
inline constexpr std::uint32_t kFeatherHilPcmSampleRateHz = 48'000;
inline constexpr std::uint32_t kFeatherHilPcmMaximumSamples = 12'000;
inline constexpr std::uint32_t kFeatherHilPcmMaximumChunkBytes = 48;
inline constexpr std::uint32_t kFeatherHilPcmMinimumLeadMicroseconds = 20'000;
inline constexpr std::uint32_t kFeatherHilPcmMinimumGainPermille = 1;
inline constexpr std::uint32_t kFeatherHilPcmMaximumGainPermille = 1'000;
inline constexpr std::uint32_t kFeatherHilPcmWhiteMicroseconds = 20'000;

enum class FeatherResponseKind {
  kOk,
  kAcknowledgement,
  kEvent,
  kError,
};

struct FeatherResponse {
  std::uint32_t request_id = 0;
  FeatherResponseKind kind = FeatherResponseKind::kError;
  std::string subject;
  std::string state;
  std::map<std::string, std::string, std::less<>> fields;
  // Exact bytes received for this response, including its line ending.
  std::string wire_line;
};

// Parses one newline-delimited USB CDC response. The returned object owns all
// text, so it remains valid after the receive buffer is reused.
[[nodiscard]] FeatherResponse ParseFeatherResponse(std::string_view line);

[[nodiscard]] std::uint64_t RequiredUnsignedField(const FeatherResponse &response,
                                                  std::string_view name);

[[nodiscard]] std::string BuildFeatherQueryCommand(std::uint32_t request_id);
[[nodiscard]] std::string BuildFeatherLedCommand(std::uint32_t request_id,
                                                 std::uint64_t lead_microseconds,
                                                 std::uint64_t duration_microseconds);
[[nodiscard]] std::string BuildFeatherToneCommand(std::uint32_t request_id,
                                                  std::uint64_t lead_microseconds,
                                                  std::uint64_t duration_microseconds,
                                                  std::uint32_t frequency_hz,
                                                  std::uint32_t level_permille);
[[nodiscard]] std::uint32_t FeatherPcmCrc32(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<std::uint8_t> FeatherPcm16LeBytes(std::span<const std::int16_t> samples);
[[nodiscard]] std::uint32_t FeatherScaledPcmCrc32(std::span<const std::int16_t> samples,
                                                  std::uint32_t gain_permille);
[[nodiscard]] std::string BuildFeatherPcmBeginCommand(std::uint32_t request_id,
                                                      std::uint32_t sample_count,
                                                      std::uint32_t crc32);
[[nodiscard]] std::string BuildFeatherPcmChunkCommand(std::uint32_t request_id,
                                                      std::uint32_t byte_offset,
                                                      std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string BuildFeatherPcmCommitCommand(std::uint32_t request_id);
[[nodiscard]] std::string BuildFeatherPcmAbortCommand(std::uint32_t request_id);
[[nodiscard]] std::string BuildFeatherPcmPlayCommand(std::uint32_t request_id,
                                                     std::uint64_t lead_microseconds,
                                                     std::uint32_t gain_permille,
                                                     std::uint32_t brightness,
                                                     std::uint32_t marker_sample);
[[nodiscard]] std::string BuildFeatherCalibrationCommand(std::uint32_t request_id);
[[nodiscard]] std::string BuildFeatherSwingCommand(std::uint32_t request_id,
                                                   std::uint32_t brightness);

}  // namespace swing_capture::hil

#endif  // SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_PROTOCOL_H_
