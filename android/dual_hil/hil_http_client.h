#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_HTTP_CLIENT_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_HTTP_CLIENT_H_

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace swing_capture::android::dual_hil {

struct HilHttpResponse {
  int status = 0;
  std::map<std::string, std::string, std::less<>> headers;
  std::string body;
};

struct HilHttpRequest {
  std::string_view ipv4_host;
  std::uint16_t port = 0;
  std::string_view method;
  std::string_view path;
  std::string_view bearer_token;
  std::string_view body;
  std::map<std::string, std::string, std::less<>> headers;
  std::chrono::steady_clock::time_point deadline;
};

[[nodiscard]] HilHttpResponse ParseHilHttpResponse(std::string_view wire, bool head);

[[nodiscard]] HilHttpResponse RequestHilHttp(const HilHttpRequest &request);

[[nodiscard]] HilHttpResponse RequireHilHttp(const HilHttpRequest &request, int expected_status);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_HTTP_CLIENT_H_
