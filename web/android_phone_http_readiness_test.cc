#include "web/android_phone_http_readiness.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using swing_capture::web::AndroidPhoneHttpReadinessOperations;
using swing_capture::web::AndroidPhoneHttpReadinessOptions;
using swing_capture::web::WaitForAndroidPhoneHttpReadiness;

void Check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

struct FakeRuntime {
  std::chrono::steady_clock::time_point now{};
  std::vector<std::function<int()>> responses;
  std::size_t next_response = 0U;
  std::vector<std::chrono::steady_clock::time_point> deadlines;
  std::vector<std::chrono::steady_clock::time_point> sleeps;

  AndroidPhoneHttpReadinessOperations Operations() {
    return {
        .now = [this] { return now; },
        .request_status =
            [this](const std::chrono::steady_clock::time_point deadline) {
              deadlines.push_back(deadline);
              if (next_response >= responses.size()) {
                throw std::runtime_error("connection refused");
              }
              return responses[next_response++]();
            },
        .sleep_until =
            [this](const std::chrono::steady_clock::time_point wake) {
              sleeps.push_back(wake);
              now = wake;
            },
    };
  }
};

std::function<int()> Status(int status) {
  return [status] { return status; };
}

std::function<int()> TransportFailure() {
  return []() -> int { throw std::runtime_error("not listening"); };
}

void RetriesTransportAndServerStartupFailures() {
  FakeRuntime runtime;
  runtime.responses = {TransportFailure(), Status(503), Status(200)};
  const auto evidence = WaitForAndroidPhoneHttpReadiness(
      "down_the_line", "http://10.0.0.1:8088",
      AndroidPhoneHttpReadinessOptions{
          .timeout = 1'000ms, .request_timeout = 250ms, .retry_interval = 100ms},
      runtime.Operations());

  Check(evidence.passed, "eventual readiness was not accepted");
  Check(evidence.attempts.size() == 3U, "readiness did not preserve all attempts");
  Check(evidence.attempts[0].outcome == "transport_error" &&
            !evidence.attempts[0].http_status.has_value(),
        "transport failure evidence is wrong");
  Check(evidence.attempts[1].outcome == "retryable_http_status" &&
            evidence.attempts[1].http_status == 503,
        "retryable status evidence is wrong");
  Check(evidence.attempts[2].outcome == "ready" && evidence.attempts[2].http_status == 200 &&
            evidence.attempts[2].elapsed_ms == 200,
        "success evidence is wrong");
  Check(runtime.sleeps.size() == 2U, "retry interval count is wrong");
  Check(runtime.deadlines.size() == 3U &&
            runtime.deadlines[0] == std::chrono::steady_clock::time_point{} + 250ms &&
            runtime.deadlines[2] == std::chrono::steady_clock::time_point{} + 450ms,
        "per-request deadlines are not bounded");
}

void StopsAtTheOverallDeadline() {
  FakeRuntime runtime;
  const auto evidence = WaitForAndroidPhoneHttpReadiness(
      "face_on", "http://10.0.0.2:8088",
      AndroidPhoneHttpReadinessOptions{
          .timeout = 250ms, .request_timeout = 200ms, .retry_interval = 100ms},
      runtime.Operations());

  Check(!evidence.passed && evidence.attempts.size() == 3U,
        "deadline did not bound readiness attempts");
  Check(runtime.now == std::chrono::steady_clock::time_point{} + 250ms,
        "readiness did not stop at its overall deadline");
  Check(runtime.deadlines.back() == std::chrono::steady_clock::time_point{} + 250ms,
        "last request deadline exceeded the overall deadline");
  Check(evidence.attempts.back().elapsed_ms == 200,
        "timeout evidence did not retain the final attempt time");
}

void RejectsTerminalHttpStatusWithoutRetry() {
  FakeRuntime runtime;
  runtime.responses = {Status(401), Status(200)};
  const auto evidence = WaitForAndroidPhoneHttpReadiness("down_the_line", "http://10.0.0.1:8088",
                                                         {}, runtime.Operations());

  Check(!evidence.passed && evidence.attempts.size() == 1U && runtime.sleeps.empty(),
        "terminal authentication failure was retried");
  Check(evidence.attempts[0].outcome == "terminal_http_status" &&
            evidence.attempts[0].http_status == 401,
        "terminal HTTP evidence is wrong");
}

void RejectsInvalidPolicies() {
  FakeRuntime runtime;
  bool rejected = false;
  try {
    static_cast<void>(WaitForAndroidPhoneHttpReadiness(
        "face_on", "http://10.0.0.2:8088",
        AndroidPhoneHttpReadinessOptions{
            .timeout = 100ms, .request_timeout = 200ms, .retry_interval = 10ms},
        runtime.Operations()));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Check(rejected, "request timeout beyond the readiness window was accepted");
}

}  // namespace

int main() {
  RetriesTransportAndServerStartupFailures();
  StopsAtTheOverallDeadline();
  RejectsTerminalHttpStatusWithoutRetry();
  RejectsInvalidPolicies();
  return 0;
}
