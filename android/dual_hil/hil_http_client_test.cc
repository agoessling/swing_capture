#include "android/dual_hil/hil_http_client.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void ExpectRejected(const std::function<void()> &action, std::string_view name) {
  try {
    action();
  } catch (const std::runtime_error &) {
    return;
  }
  throw std::runtime_error("invalid HTTP response was accepted: " + std::string(name));
}

}  // namespace

int main() {
  using swing_capture::android::dual_hil::ParseHilHttpResponse;
  const auto response = ParseHilHttpResponse(
      "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\n"
      "Content-Length: 15\r\n\r\n{\"error\":\"no\"}\n",
      false);
  if (response.status != 409 || response.body != "{\"error\":\"no\"}\n" ||
      response.headers.at("content-type") != "application/json") {
    throw std::runtime_error("valid HTTP response was not preserved");
  }
  const auto head = ParseHilHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 123\r\n\r\n", true);
  if (head.status != 200 || !head.body.empty()) {
    throw std::runtime_error("valid HEAD response was not accepted");
  }
  ExpectRejected([] { static_cast<void>(ParseHilHttpResponse("HTTP/1.1 200 OK\r\n\r\n", false)); },
                 "missing content length");
  ExpectRejected(
      [] {
        static_cast<void>(
            ParseHilHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nx", false));
      },
      "inconsistent content length");
  ExpectRejected([] { static_cast<void>(ParseHilHttpResponse("not HTTP", false)); },
                 "malformed status line");
  return 0;
}
