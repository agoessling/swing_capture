#ifndef SWING_CAPTURE_WEB_ANDROID_PHONE_HTTP_READINESS_H_
#define SWING_CAPTURE_WEB_ANDROID_PHONE_HTTP_READINESS_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::web {

struct AndroidPhoneHttpReadinessAttempt {
  std::size_t sequence = 0U;
  std::int64_t elapsed_ms = 0;
  std::optional<int> http_status;
  std::string outcome;
};

struct AndroidPhoneHttpReadinessEvidence {
  std::string role;
  std::string origin;
  std::int64_t timeout_ms = 0;
  bool passed = false;
  std::vector<AndroidPhoneHttpReadinessAttempt> attempts;
};

struct AndroidPhoneHttpReadinessOptions {
  std::chrono::milliseconds timeout{3'000};
  std::chrono::milliseconds request_timeout{500};
  std::chrono::milliseconds retry_interval{100};
};

struct AndroidPhoneHttpReadinessOperations {
  std::function<std::chrono::steady_clock::time_point()> now;
  std::function<int(std::chrono::steady_clock::time_point)> request_status;
  std::function<void(std::chrono::steady_clock::time_point)> sleep_until;
};

/** Runs the bounded retry policy with injectable time and transport for deterministic coverage. */
[[nodiscard]] AndroidPhoneHttpReadinessEvidence WaitForAndroidPhoneHttpReadiness(
    std::string_view role, std::string_view origin, const AndroidPhoneHttpReadinessOptions &options,
    const AndroidPhoneHttpReadinessOperations &operations);

/** Probes authenticated GET /api/v1/node over the configured direct-LAN phone origin. */
[[nodiscard]] AndroidPhoneHttpReadinessEvidence ProbeAndroidPhoneHttpReadiness(
    std::string_view role, std::string_view origin, std::string_view bearer_token,
    const AndroidPhoneHttpReadinessOptions &options = {});

}  // namespace swing_capture::web

#endif  // SWING_CAPTURE_WEB_ANDROID_PHONE_HTTP_READINESS_H_
