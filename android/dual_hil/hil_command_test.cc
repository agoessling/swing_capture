#include "android/dual_hil/hil_command.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;
using swing_capture::android::dual_hil::RunHilCommand;
using swing_capture::android::dual_hil::RunRequiredHilCommand;

void CapturesOutputAndWritesInput(const std::filesystem::path &fixture) {
  const auto result =
      RunHilCommand(fixture, {"echo"}, std::chrono::steady_clock::now() + 1s, "payload");
  assert(result.exit_code == 0);
  assert(!result.timed_out);
  assert(result.output == "stdout:payload:stderr");
}

void PreservesExitCode(const std::filesystem::path &fixture) {
  const auto result = RunHilCommand(fixture, {"fail"}, std::chrono::steady_clock::now() + 1s);
  assert(result.exit_code == 7);
  assert(!result.timed_out);
  assert(result.output == "expected failure");
}

void RequiredCommandRejectsFailure(const std::filesystem::path &fixture) {
  bool rejected = false;
  try {
    static_cast<void>(
        RunRequiredHilCommand(fixture, {"fail"}, std::chrono::steady_clock::now() + 1s));
  } catch (const std::runtime_error &failure) {
    rejected = std::string(failure.what()).contains("failed with exit 7");
  }
  assert(rejected);
}

}  // namespace

int main(int argument_count, char **arguments) {
  assert(argument_count == 2);
  const std::filesystem::path fixture(arguments[1]);
  CapturesOutputAndWritesInput(fixture);
  PreservesExitCode(fixture);
  RequiredCommandRejectsFailure(fixture);
}
