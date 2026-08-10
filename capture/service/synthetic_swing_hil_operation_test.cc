#include "capture/service/synthetic_swing_hil_operation.h"

#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using swing_capture::service::SyntheticSwingCaptureState;
using swing_capture::service::SyntheticSwingCaptureStatus;
using swing_capture::service::SyntheticSwingHilOperation;
using swing_capture::service::SyntheticSwingHilOperationConfig;
using swing_capture::service::SyntheticSwingHilOperationHooks;
using swing_capture::service::SyntheticSwingHilStage;

class FakeOperation final {
 public:
  SyntheticSwingHilOperationHooks Hooks() {
    return {
        .calibrate_brightness =
            [this](const std::stop_token &) {
              ++calibrations;
              if (fail_calibration) {
                throw std::runtime_error("injected calibration failure");
              }
              return static_cast<std::uint8_t>(24);
            },
        .arm_capture =
            [this] {
              ++arms;
              Set({.state = SyntheticSwingCaptureState::kArmed,
                   .session_id = std::nullopt,
                   .error = {}});
            },
        .capture_status = [this] { return Get(); },
        .run_stimulus =
            [this](std::uint8_t brightness, const std::stop_token &) {
              assert(brightness == 24U);
              ++stimuli;
              if (block_stimulus) {
                while (!release_stimulus.load()) {
                  std::this_thread::yield();
                }
              }
              Set({.state = SyntheticSwingCaptureState::kReady,
                   .session_id = "session-1",
                   .error = {}});
            },
        .validate_completion =
            [this](const SyntheticSwingCaptureStatus &status) {
              ++validations;
              assert(status.session_id == "session-1");
              if (fail_validation) {
                throw std::runtime_error("injected optical validation failure");
              }
            },
        .disarm_capture =
            [this] {
              ++disarms;
              Set({.state = SyntheticSwingCaptureState::kSetup,
                   .session_id = std::nullopt,
                   .error = {}});
            },
    };
  }

  SyntheticSwingCaptureStatus Get() const {
    const std::scoped_lock lock(mutex);
    return capture;
  }

  void Set(SyntheticSwingCaptureStatus next) {
    const std::scoped_lock lock(mutex);
    capture = std::move(next);
  }

  mutable std::mutex mutex;
  SyntheticSwingCaptureStatus capture;
  std::atomic<int> calibrations = 0;
  std::atomic<int> arms = 0;
  std::atomic<int> stimuli = 0;
  std::atomic<int> validations = 0;
  std::atomic<int> disarms = 0;
  std::atomic<bool> release_stimulus = false;
  bool fail_calibration = false;
  bool fail_validation = false;
  bool block_stimulus = false;
};

SyntheticSwingHilOperation MakeOperation(FakeOperation &fake) {
  return SyntheticSwingHilOperation(
      SyntheticSwingHilOperationConfig{
          .arm_timeout = 100ms, .completion_timeout = 100ms, .poll_interval = 1ms},
      fake.Hooks());
}

void WaitUntilDone(SyntheticSwingHilOperation &operation) {
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (operation.Status().busy && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  assert(!operation.Status().busy);
}

void TestSuccessfulLifecycle() {
  FakeOperation fake;
  auto operation = MakeOperation(fake);
  operation.Start();
  WaitUntilDone(operation);
  const auto status = operation.Status();
  assert(status.stage == SyntheticSwingHilStage::kReady);
  assert(status.session_id == "session-1");
  assert(status.last_session_id == "session-1");
  assert(status.last_stage == SyntheticSwingHilStage::kReady);
  assert(status.selected_brightness == 24U);
  assert(status.error.empty());
  assert(fake.calibrations == 1);
  assert(fake.arms == 1);
  assert(fake.stimuli == 1);
  assert(fake.validations == 1);
  assert(fake.disarms == 0);
}

void TestBusyRunIsRejected() {
  FakeOperation fake;
  fake.block_stimulus = true;
  auto operation = MakeOperation(fake);
  operation.Start();
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (fake.stimuli == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  assert(fake.stimuli == 1);
  bool rejected = false;
  try {
    operation.Start();
  } catch (const std::logic_error &) {
    rejected = true;
  }
  assert(rejected);
  fake.release_stimulus = true;
  WaitUntilDone(operation);
}

void TestConcurrentStartsAdmitExactlyOneRun() {
  FakeOperation fake;
  fake.block_stimulus = true;
  auto operation = MakeOperation(fake);
  std::barrier start_gate(3);
  std::atomic<int> accepted = 0;
  std::atomic<int> rejected = 0;
  std::atomic<int> unexpected = 0;
  auto start = [&] {
    start_gate.arrive_and_wait();
    try {
      operation.Start();
      ++accepted;
    } catch (const std::logic_error &) {
      ++rejected;
    } catch (...) {
      ++unexpected;
    }
  };
  std::jthread first(start);
  std::jthread second(start);
  start_gate.arrive_and_wait();
  first.join();
  second.join();
  assert(accepted == 1);
  assert(rejected == 1);
  assert(unexpected == 0);
  fake.release_stimulus = true;
  WaitUntilDone(operation);
}

void TestFailureDisarmsAndCanRetry() {
  FakeOperation fake;
  fake.fail_calibration = true;
  auto operation = MakeOperation(fake);
  operation.Start();
  WaitUntilDone(operation);
  auto status = operation.Status();
  assert(status.stage == SyntheticSwingHilStage::kError);
  assert(status.error == "injected calibration failure");
  assert(fake.disarms == 1);

  fake.fail_calibration = false;
  operation.Start();
  WaitUntilDone(operation);
  status = operation.Status();
  assert(status.stage == SyntheticSwingHilStage::kReady);
  assert(status.last_error.empty());
  assert(fake.calibrations == 2);
}

void TestPublishedValidationFailureRetainsSessionIdentity() {
  FakeOperation fake;
  fake.fail_validation = true;
  auto operation = MakeOperation(fake);
  operation.Start();
  WaitUntilDone(operation);
  const auto status = operation.Status();
  assert(status.stage == SyntheticSwingHilStage::kError);
  assert(status.error == "injected optical validation failure");
  assert(status.last_session_id == "session-1");
  assert(fake.validations == 1);
  assert(fake.disarms == 1);
}

void TestIncompatibleCaptureStateRejectsBeforeWorker() {
  FakeOperation fake;
  fake.Set({.state = SyntheticSwingCaptureState::kArmed, .session_id = std::nullopt, .error = {}});
  auto operation = MakeOperation(fake);
  bool rejected = false;
  try {
    operation.Start();
  } catch (const std::logic_error &) {
    rejected = true;
  }
  assert(rejected);
  assert(fake.calibrations == 0);
}

}  // namespace

int main() {
  TestSuccessfulLifecycle();
  TestBusyRunIsRejected();
  TestConcurrentStartsAdmitExactlyOneRun();
  TestFailureDisarmsAndCanRetry();
  TestPublishedValidationFailureRetainsSessionIdentity();
  TestIncompatibleCaptureStateRejectsBeforeWorker();
  return 0;
}
