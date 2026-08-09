#ifndef SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_PROTOCOL_H_
#define SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_PROTOCOL_H_

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace swing_capture::hil {

inline constexpr std::string_view kFeatherHilProtocol = "SC-HIL/1";
inline constexpr std::string_view kFeatherHilFirmware = "prop-maker-hil-1";
inline constexpr std::uint32_t kFeatherHilProtocolVersion = 1;
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

}  // namespace swing_capture::hil

#endif  // SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_PROTOCOL_H_
