#include "android/dual_hil/field_recording_hil_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace swing_capture::android::dual_hil {
namespace {

constexpr std::uint16_t kNodeHttpPort = 8088;
constexpr std::string_view kDownTheLineOriginEnvironment = "SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN";
constexpr std::string_view kFaceOnOriginEnvironment = "SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN";

[[noreturn]] void InvalidOrigin(std::string_view environment_name) {
  throw std::invalid_argument(std::string(environment_name) +
                              " must be a canonical path-free non-loopback HTTP IPv4 origin "
                              "on port 8088");
}

bool IsDirectUnicastAddress(const in_addr &address) {
  const std::uint32_t host = ntohl(address.s_addr);
  const auto first_octet = static_cast<std::uint8_t>(host >> 24U);
  return first_octet != 0U && first_octet != 127U && first_octet < 224U;
}

}  // namespace

FieldRecordingHilHttpEndpoint MakeFieldRecordingAdbForwardEndpoint(const std::uint16_t host_port) {
  if (host_port == 0U) {
    throw std::invalid_argument("ADB-forward HTTP port must be nonzero");
  }
  return {
      .transport = FieldRecordingHilHttpTransport::kAdbForward,
      .origin = "http://127.0.0.1:" + std::to_string(host_port),
      .ipv4_address = "127.0.0.1",
      .port = host_port,
  };
}

FieldRecordingHilHttpEndpoint ParseFieldRecordingDirectLanOrigin(
    std::string origin, const std::string_view environment_name) {
  constexpr std::string_view kPrefix = "http://";
  if (environment_name.empty() || !origin.starts_with(kPrefix)) {
    InvalidOrigin(environment_name);
  }
  const std::string_view authority = std::string_view(origin).substr(kPrefix.size());
  if (authority.empty() || authority.contains('/') || authority.contains('?') ||
      authority.contains('#') || authority.contains('@')) {
    InvalidOrigin(environment_name);
  }
  const std::size_t separator = authority.rfind(':');
  if (separator == std::string_view::npos || separator == 0U ||
      separator + 1U == authority.size()) {
    InvalidOrigin(environment_name);
  }
  const std::string address(authority.substr(0U, separator));
  const std::string_view port_text = authority.substr(separator + 1U);
  in_addr parsed_address = {};
  std::array<char, INET_ADDRSTRLEN> canonical = {};
  if (port_text != "8088" || inet_pton(AF_INET, address.c_str(), &parsed_address) != 1 ||
      inet_ntop(AF_INET, &parsed_address, canonical.data(), canonical.size()) == nullptr ||
      address != canonical.data() || !IsDirectUnicastAddress(parsed_address)) {
    InvalidOrigin(environment_name);
  }
  return {
      .transport = FieldRecordingHilHttpTransport::kDirectLan,
      .origin = std::move(origin),
      .ipv4_address = address,
      .port = kNodeHttpPort,
  };
}

FieldRecordingHilDirectLanPair ParseFieldRecordingDirectLanPair(std::string down_the_line_origin,
                                                                std::string face_on_origin) {
  FieldRecordingHilDirectLanPair pair = {
      .down_the_line = ParseFieldRecordingDirectLanOrigin(std::move(down_the_line_origin),
                                                          kDownTheLineOriginEnvironment),
      .face_on =
          ParseFieldRecordingDirectLanOrigin(std::move(face_on_origin), kFaceOnOriginEnvironment),
  };
  if (pair.down_the_line.ipv4_address == pair.face_on.ipv4_address) {
    throw std::invalid_argument(
        "direct-LAN field-recording HIL requires two distinct phone IPv4 addresses");
  }
  return pair;
}

std::string_view FieldRecordingHilHttpTransportName(
    const FieldRecordingHilHttpTransport transport) {
  switch (transport) {
    case FieldRecordingHilHttpTransport::kAdbForward:
      return "adb_forward";
    case FieldRecordingHilHttpTransport::kDirectLan:
      return "wifi_lan_direct";
  }
  throw std::invalid_argument("field-recording HIL HTTP transport is unknown");
}

}  // namespace swing_capture::android::dual_hil
