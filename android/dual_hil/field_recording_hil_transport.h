#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_HIL_TRANSPORT_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_HIL_TRANSPORT_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace swing_capture::android::dual_hil {

enum class FieldRecordingHilHttpTransport : std::uint8_t {
  kAdbForward,
  kDirectLan,
};

// A fully resolved host-side HTTP endpoint. Direct-LAN endpoints are deliberately
// restricted to canonical IPv4 literals so the HIL cannot silently exercise a
// proxy, loopback alias, DNS rebinding target, or URL with hidden path state.
struct FieldRecordingHilHttpEndpoint {
  FieldRecordingHilHttpTransport transport = FieldRecordingHilHttpTransport::kAdbForward;
  std::string origin;
  std::string ipv4_address;
  std::uint16_t port = 0;

  [[nodiscard]] bool UsesAdbForward() const {
    return transport == FieldRecordingHilHttpTransport::kAdbForward;
  }
};

struct FieldRecordingHilDirectLanPair {
  FieldRecordingHilHttpEndpoint down_the_line;
  FieldRecordingHilHttpEndpoint face_on;
};

[[nodiscard]] FieldRecordingHilHttpEndpoint MakeFieldRecordingAdbForwardEndpoint(
    std::uint16_t host_port);

[[nodiscard]] FieldRecordingHilHttpEndpoint ParseFieldRecordingDirectLanOrigin(
    std::string origin, std::string_view environment_name);

[[nodiscard]] FieldRecordingHilDirectLanPair ParseFieldRecordingDirectLanPair(
    std::string down_the_line_origin, std::string face_on_origin);

[[nodiscard]] std::string_view FieldRecordingHilHttpTransportName(
    FieldRecordingHilHttpTransport transport);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_HIL_TRANSPORT_H_
