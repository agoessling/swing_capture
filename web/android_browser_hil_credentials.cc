#include "web/android_browser_hil_credentials.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace swing_capture::web {

std::string ExtractAndroidControlToken(std::string_view preferences_xml) {
  constexpr std::string_view kPrefix = "<string name=\"control_token\">";
  constexpr std::string_view kSuffix = "</string>";
  const std::size_t start = preferences_xml.find(kPrefix);
  const std::size_t end = start == std::string_view::npos
                              ? std::string_view::npos
                              : preferences_xml.find(kSuffix, start + kPrefix.size());
  if (start == std::string_view::npos || end == std::string_view::npos) {
    throw std::runtime_error("Android node does not have a persisted control credential");
  }
  std::string token(preferences_xml.substr(start + kPrefix.size(), end - start - kPrefix.size()));
  if (token.size() != 32U || !std::ranges::all_of(token, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' || character == '-';
      })) {
    throw std::runtime_error("Android node control credential has an invalid format");
  }
  return token;
}

void ValidateDirectLanNodeOrigin(std::string_view origin) {
  constexpr std::string_view kScheme = "http://";
  if (!origin.starts_with(kScheme) || origin.ends_with('/') ||
      origin.find_first_of("?#@", 0U) != std::string_view::npos) {
    throw std::invalid_argument("Android browser HIL node URL must be a direct HTTP LAN origin");
  }
  const std::string_view authority = origin.substr(kScheme.size());
  const std::size_t separator = authority.rfind(':');
  if (separator == std::string_view::npos || authority.contains('/')) {
    throw std::invalid_argument("Android browser HIL node URL must include an IPv4 port");
  }
  const std::string address(authority.substr(0U, separator));
  const std::string_view port_text = authority.substr(separator + 1U);
  unsigned int port = 0;
  const auto [end, error] = std::from_chars(port_text.begin(), port_text.end(), port);
  in_addr parsed = {};
  if (error != std::errc() || end != port_text.end() || port != 8088U ||
      inet_pton(AF_INET, address.c_str(), &parsed) != 1 || (ntohl(parsed.s_addr) >> 24U) == 127U ||
      parsed.s_addr == htonl(INADDR_ANY)) {
    throw std::invalid_argument(
        "Android browser HIL node URL must use a non-loopback IPv4 address on port 8088");
  }
}

}  // namespace swing_capture::web
