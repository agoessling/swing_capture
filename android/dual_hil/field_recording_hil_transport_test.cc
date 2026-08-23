#include "android/dual_hil/field_recording_hil_transport.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swing_capture::android::dual_hil {
namespace {

void Check(const bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void ExpectInvalid(const std::function<void()> &action, std::string_view diagnostic_fragment) {
  try {
    action();
  } catch (const std::invalid_argument &failure) {
    Check(std::string_view(failure.what()).contains(diagnostic_fragment),
          "invalid endpoint diagnostic is not actionable");
    return;
  }
  throw std::runtime_error("invalid endpoint was accepted");
}

void TestAdbForwardEndpointIsExplicitlyLoopback() {
  const FieldRecordingHilHttpEndpoint endpoint = MakeFieldRecordingAdbForwardEndpoint(37'123U);
  Check(endpoint.transport == FieldRecordingHilHttpTransport::kAdbForward,
        "ADB endpoint transport");
  Check(endpoint.UsesAdbForward(), "ADB endpoint forward marker");
  Check(endpoint.origin == "http://127.0.0.1:37123", "ADB endpoint origin");
  Check(endpoint.ipv4_address == "127.0.0.1" && endpoint.port == 37'123U,
        "ADB endpoint socket address");
  Check(FieldRecordingHilHttpTransportName(endpoint.transport) == "adb_forward",
        "ADB endpoint evidence name");
  ExpectInvalid([] { static_cast<void>(MakeFieldRecordingAdbForwardEndpoint(0U)); }, "nonzero");
}

void TestDirectLanEndpointIsCanonicalAndFixed() {
  const FieldRecordingHilHttpEndpoint endpoint = ParseFieldRecordingDirectLanOrigin(
      "http://10.168.168.241:8088", "SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN");
  Check(endpoint.transport == FieldRecordingHilHttpTransport::kDirectLan,
        "direct-LAN endpoint transport");
  Check(!endpoint.UsesAdbForward(), "direct-LAN endpoint forward marker");
  Check(endpoint.origin == "http://10.168.168.241:8088", "direct-LAN origin");
  Check(endpoint.ipv4_address == "10.168.168.241" && endpoint.port == 8088U,
        "direct-LAN socket address");
  Check(FieldRecordingHilHttpTransportName(endpoint.transport) == "wifi_lan_direct",
        "direct-LAN evidence name");
}

void TestUnsafeOrAmbiguousOriginsAreRejected() {
  const std::vector<std::string> invalid = {
      "",
      "https://10.0.0.2:8088",
      "http://10.0.0.2",
      "http://10.0.0.2:8080",
      "http://10.0.0.2:8088/",
      "http://10.0.0.2:8088/api",
      "http://10.0.0.2:8088?query",
      "http://10.0.0.2:8088#fragment",
      "http://token@10.0.0.2:8088",
      "http://pixel.test:8088",
      "http://[fe80::1]:8088",
      "http://127.0.0.1:8088",
      "http://0.0.0.0:8088",
      "http://0.1.2.3:8088",
      "http://224.0.0.1:8088",
      "http://255.255.255.255:8088",
      "http://010.000.000.002:8088",
      " http://10.0.0.2:8088",
      "http://10.0.0.2:8088 ",
      "http://10.0.0.2:8088:8088",
      "http://10.0.0.2:08088",
  };
  for (const std::string &origin : invalid) {
    ExpectInvalid(
        [&] {
          static_cast<void>(
              ParseFieldRecordingDirectLanOrigin(origin, "SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN"));
        },
        "SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN");
  }
  ExpectInvalid(
      [] { static_cast<void>(ParseFieldRecordingDirectLanOrigin("http://10.0.0.2:8088", "")); },
      "canonical");
}

void TestDirectLanPairPreservesRolesAndRequiresDistinctPhones() {
  const FieldRecordingHilDirectLanPair pair =
      ParseFieldRecordingDirectLanPair("http://10.168.168.241:8088", "http://10.168.168.111:8088");
  Check(pair.down_the_line.ipv4_address == "10.168.168.241", "down-the-line origin assignment");
  Check(pair.face_on.ipv4_address == "10.168.168.111", "face-on origin assignment");
  ExpectInvalid(
      [] {
        static_cast<void>(ParseFieldRecordingDirectLanPair("http://10.168.168.241:8088",
                                                           "http://10.168.168.241:8088"));
      },
      "two distinct phone");
}

}  // namespace
}  // namespace swing_capture::android::dual_hil

int main() {
  try {
    swing_capture::android::dual_hil::TestAdbForwardEndpointIsExplicitlyLoopback();
    swing_capture::android::dual_hil::TestDirectLanEndpointIsCanonicalAndFixed();
    swing_capture::android::dual_hil::TestUnsafeOrAmbiguousOriginsAreRejected();
    swing_capture::android::dual_hil::TestDirectLanPairPreservesRolesAndRequiresDistinctPhones();
    return 0;
  } catch (const std::exception &failure) {
    std::cerr << "field-recording HIL transport test failed: " << failure.what() << '\n';
    return 1;
  }
}
