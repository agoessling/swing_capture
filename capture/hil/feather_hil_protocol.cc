#include "capture/hil/feather_hil_protocol.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "embedded/prop_maker/swing_sequence.h"

namespace swing_capture::hil {
namespace {

constexpr std::size_t kMaximumLineBytes = 1024;

std::vector<std::string_view> Tokens(std::string_view line) {
  if (!line.empty() && line.back() == '\n') {
    line.remove_suffix(1);
  }
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  if (line.empty() || line.size() > kMaximumLineBytes || line.contains('\n') ||
      line.contains('\r')) {
    throw std::invalid_argument("invalid Feather HIL response line length or framing");
  }

  std::vector<std::string_view> tokens;
  std::size_t start = 0;
  while (start < line.size()) {
    const std::size_t end = line.find(' ', start);
    const std::string_view token = line.substr(start, end - start);
    if (token.empty()) {
      throw std::invalid_argument("Feather HIL response contains empty token");
    }
    tokens.push_back(token);
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return tokens;
}

template <typename Integer>
Integer ParseUnsigned(std::string_view value) {
  static_assert(std::is_unsigned_v<Integer>);
  Integer parsed = 0;
  const auto result = std::from_chars(value.begin(), value.end(), parsed);
  if (result.ec != std::errc() || result.ptr != value.end()) {
    throw std::invalid_argument("Feather HIL value must be an unsigned integer");
  }
  return parsed;
}

void ParseFields(const std::vector<std::string_view> &tokens, std::size_t first,
                 FeatherResponse *response) {
  for (std::size_t index = first; index < tokens.size(); ++index) {
    const std::size_t separator = tokens[index].find('=');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == tokens[index].size()) {
      throw std::invalid_argument("Feather HIL response field must be name=value");
    }
    const std::string name(tokens[index].substr(0, separator));
    const std::string value(tokens[index].substr(separator + 1));
    if (!response->fields.emplace(name, value).second) {
      throw std::invalid_argument("duplicate Feather HIL response field: " + name);
    }
  }
}

void ValidateRequestId(std::uint32_t request_id) {
  if (request_id == 0) {
    throw std::invalid_argument("Feather HIL request ID zero is reserved for device events");
  }
}

void ValidateResponseRequestId(const FeatherResponse &response) {
  const bool boot_event =
      response.kind == FeatherResponseKind::kEvent && response.subject == "BOOT";
  if ((boot_event && response.request_id != 0) ||
      (!boot_event && response.kind != FeatherResponseKind::kError && response.request_id == 0)) {
    throw std::invalid_argument("invalid reserved Feather HIL response request ID");
  }
}

}  // namespace

FeatherResponse ParseFeatherResponse(std::string_view line) {
  const std::string wire_line(line);
  const std::vector<std::string_view> tokens = Tokens(line);
  if (tokens.size() < 3 || tokens[0] != kFeatherHilProtocol) {
    throw std::invalid_argument("unsupported or incomplete Feather HIL response");
  }

  FeatherResponse response;
  response.request_id = ParseUnsigned<std::uint32_t>(tokens[1]);
  std::size_t first_field = 0;
  if (tokens[2] == "OK") {
    if (tokens.size() < 4) {
      throw std::invalid_argument("Feather HIL OK is missing its command");
    }
    response.kind = FeatherResponseKind::kOk;
    response.subject = tokens[3];
    first_field = 4;
  } else if (tokens[2] == "ACK") {
    if (tokens.size() < 4) {
      throw std::invalid_argument("Feather HIL ACK is missing its command");
    }
    response.kind = FeatherResponseKind::kAcknowledgement;
    response.subject = tokens[3];
    first_field = 4;
  } else if (tokens[2] == "EVENT") {
    if (tokens.size() < 4) {
      throw std::invalid_argument("Feather HIL EVENT is missing its subject");
    }
    response.kind = FeatherResponseKind::kEvent;
    response.subject = tokens[3];
    if (response.subject == "BOOT") {
      first_field = 4;
    } else {
      if (tokens.size() < 5) {
        throw std::invalid_argument("Feather HIL EVENT is missing its state");
      }
      response.state = tokens[4];
      first_field = 5;
    }
  } else if (tokens[2] == "ERR") {
    if (tokens.size() < 4) {
      throw std::invalid_argument("Feather HIL ERR is missing its code");
    }
    response.kind = FeatherResponseKind::kError;
    response.subject = tokens[3];
    first_field = 4;
  } else {
    throw std::invalid_argument("unknown Feather HIL response kind");
  }
  ParseFields(tokens, first_field, &response);
  ValidateResponseRequestId(response);
  response.wire_line = wire_line;
  return response;
}

std::uint64_t RequiredUnsignedField(const FeatherResponse &response, std::string_view name) {
  const auto field = response.fields.find(name);
  if (field == response.fields.end()) {
    throw std::invalid_argument("missing Feather HIL response field: " + std::string(name));
  }
  return ParseUnsigned<std::uint64_t>(field->second);
}

std::string BuildFeatherQueryCommand(std::uint32_t request_id) {
  ValidateRequestId(request_id);
  return std::string(kFeatherHilProtocol) + " " + std::to_string(request_id) + " QUERY\n";
}

std::string BuildFeatherLedCommand(std::uint32_t request_id, std::uint64_t lead_microseconds,
                                   std::uint64_t duration_microseconds) {
  ValidateRequestId(request_id);
  if (lead_microseconds > kFeatherHilMaximumLeadMicroseconds ||
      duration_microseconds < kFeatherHilLedMinimumDurationMicroseconds ||
      duration_microseconds > kFeatherHilLedMaximumDurationMicroseconds) {
    throw std::invalid_argument("LED lead or duration is outside the SC-HIL/1 range");
  }
  return std::string(kFeatherHilProtocol) + " " + std::to_string(request_id) + " LED " +
         std::to_string(lead_microseconds) + " " + std::to_string(duration_microseconds) + "\n";
}

std::string BuildFeatherToneCommand(std::uint32_t request_id, std::uint64_t lead_microseconds,
                                    std::uint64_t duration_microseconds, std::uint32_t frequency_hz,
                                    std::uint32_t level_permille) {
  ValidateRequestId(request_id);
  if (lead_microseconds < kFeatherHilToneMinimumLeadMicroseconds ||
      lead_microseconds > kFeatherHilMaximumLeadMicroseconds ||
      duration_microseconds < kFeatherHilToneMinimumDurationMicroseconds ||
      duration_microseconds > kFeatherHilToneMaximumDurationMicroseconds ||
      frequency_hz < kFeatherHilToneMinimumFrequencyHz ||
      frequency_hz > kFeatherHilToneMaximumFrequencyHz ||
      level_permille < kFeatherHilToneMinimumLevelPermille ||
      level_permille > kFeatherHilToneMaximumLevelPermille) {
    throw std::invalid_argument("tone parameters are outside the SC-HIL/1 range");
  }
  return std::string(kFeatherHilProtocol) + " " + std::to_string(request_id) + " TONE " +
         std::to_string(lead_microseconds) + " " + std::to_string(duration_microseconds) + " " +
         std::to_string(frequency_hz) + " " + std::to_string(level_permille) + "\n";
}

std::string BuildFeatherCalibrationCommand(std::uint32_t request_id) {
  ValidateRequestId(request_id);
  return std::string(kFeatherHilProtocol) + " " + std::to_string(request_id) + " CALIBRATE\n";
}

std::string BuildFeatherSwingCommand(std::uint32_t request_id, std::uint32_t brightness) {
  ValidateRequestId(request_id);
  if (!swing_hil_swing_brightness_is_candidate(brightness)) {
    throw std::invalid_argument("swing brightness is not an advertised calibration candidate");
  }
  return std::string(kFeatherHilProtocol) + " " + std::to_string(request_id) + " SWING " +
         std::to_string(brightness) + "\n";
}

}  // namespace swing_capture::hil
