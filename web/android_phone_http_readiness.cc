#include "web/android_phone_http_readiness.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "android/dual_hil/hil_http_client.h"
#include "web/android_browser_hil_credentials.h"

namespace swing_capture::web {
namespace {

using swing_capture::android::dual_hil::HilHttpRequest;
using swing_capture::android::dual_hil::RequestHilHttp;

constexpr std::string_view kScheme = "http://";
constexpr std::string_view kPort = ":8088";
constexpr std::string_view kProbePath = "/api/v1/node";

std::int64_t Milliseconds(std::chrono::steady_clock::duration duration,
                          std::chrono::milliseconds maximum) {
  return std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(duration),
                    std::chrono::milliseconds::zero(), maximum)
      .count();
}

void ValidateOptions(const AndroidPhoneHttpReadinessOptions &options,
                     const AndroidPhoneHttpReadinessOperations &operations, std::string_view role,
                     std::string_view origin) {
  if (role.empty() || origin.empty() || options.timeout <= std::chrono::milliseconds::zero() ||
      options.request_timeout <= std::chrono::milliseconds::zero() ||
      options.retry_interval <= std::chrono::milliseconds::zero() ||
      options.request_timeout > options.timeout || !operations.now || !operations.request_status ||
      !operations.sleep_until) {
    throw std::invalid_argument("Android phone HTTP readiness policy is incomplete");
  }
}

bool IsRetryableStatus(int status) { return status >= 500 && status <= 599; }

}  // namespace

AndroidPhoneHttpReadinessEvidence WaitForAndroidPhoneHttpReadiness(
    const std::string_view role, const std::string_view origin,
    const AndroidPhoneHttpReadinessOptions &options,
    const AndroidPhoneHttpReadinessOperations &operations) {
  ValidateOptions(options, operations, role, origin);
  AndroidPhoneHttpReadinessEvidence evidence = {
      .role = std::string(role),
      .origin = std::string(origin),
      .timeout_ms = options.timeout.count(),
      .passed = false,
      .attempts = {},
  };
  const std::chrono::steady_clock::time_point start = operations.now();
  const std::chrono::steady_clock::time_point deadline = start + options.timeout;
  const std::size_t maximum_attempts =
      static_cast<std::size_t>(options.timeout / options.retry_interval) + 1U;

  while (operations.now() < deadline && evidence.attempts.size() < maximum_attempts) {
    const std::chrono::steady_clock::time_point attempt_start = operations.now();
    const std::chrono::steady_clock::time_point request_deadline =
        std::min(deadline, attempt_start + options.request_timeout);
    AndroidPhoneHttpReadinessAttempt attempt = {
        .sequence = evidence.attempts.size() + 1U,
        .elapsed_ms = 0,
        .http_status = std::nullopt,
        .outcome = "transport_error",
    };
    try {
      const int status = operations.request_status(request_deadline);
      attempt.http_status = status;
      if (status == 200) {
        attempt.outcome = "ready";
      } else if (IsRetryableStatus(status)) {
        attempt.outcome = "retryable_http_status";
      } else {
        attempt.outcome = "terminal_http_status";
      }
    } catch (const std::exception &) {
      // Transport diagnostics are deliberately categorical so credentials and response
      // contents can never enter durable HIL evidence.
      attempt.outcome = "transport_error";
    }
    attempt.elapsed_ms = Milliseconds(operations.now() - start, options.timeout);
    evidence.attempts.push_back(std::move(attempt));
    const AndroidPhoneHttpReadinessAttempt &recorded = evidence.attempts.back();
    if (recorded.outcome == "ready") {
      evidence.passed = true;
      return evidence;
    }
    if (recorded.outcome == "terminal_http_status") {
      return evidence;
    }
    const std::chrono::steady_clock::time_point current = operations.now();
    if (current >= deadline) {
      break;
    }
    operations.sleep_until(std::min(deadline, current + options.retry_interval));
  }
  return evidence;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters) -- preserve the public probe API.
AndroidPhoneHttpReadinessEvidence ProbeAndroidPhoneHttpReadiness(
    const std::string_view role, const std::string_view origin, const std::string_view bearer_token,
    const AndroidPhoneHttpReadinessOptions &options) {
  ValidateDirectLanNodeOrigin(origin);
  if (bearer_token.empty()) {
    throw std::invalid_argument("Android phone HTTP readiness requires a bearer credential");
  }
  const std::string host(
      origin.substr(kScheme.size(), origin.size() - kScheme.size() - kPort.size()));
  return WaitForAndroidPhoneHttpReadiness(
      role, origin, options,
      {
          .now = [] { return std::chrono::steady_clock::now(); },
          .request_status =
              [host, token = std::string(bearer_token)](
                  const std::chrono::steady_clock::time_point deadline) {
                return RequestHilHttp(HilHttpRequest{.ipv4_host = host,
                                                     .port = 8088,
                                                     .method = "GET",
                                                     .path = kProbePath,
                                                     .bearer_token = token,
                                                     .body = {},
                                                     .headers = {},
                                                     .deadline = deadline})
                    .status;
              },
          .sleep_until =
              [](const std::chrono::steady_clock::time_point wake) {
                std::this_thread::sleep_until(wake);
              },
      });
}
// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace swing_capture::web
