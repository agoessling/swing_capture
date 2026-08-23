#include "android/dual_hil/hil_cleanup_evidence.h"

#include <array>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::android::dual_hil::CombineHilPrimaryAndCleanup;
using swing_capture::android::dual_hil::HilCleanupEvidence;
using swing_capture::android::dual_hil::HilPrimaryOutcome;
using swing_capture::android::dual_hil::SerializeHilCleanupEvidence;

void Check(bool condition) {
  if (!condition) {
    throw std::runtime_error("HIL cleanup evidence assertion failed");
  }
}

void CleanupEvidenceRetainsFailedAttemptFollowedBySuccessfulRetry() {
  HilCleanupEvidence evidence;
  evidence.Register("configuration_restore", "face_on");
  evidence.Register("configuration_restore", "down_the_line");
  evidence.RecordFailed("configuration_restore", "face_on", "first restore failed");
  evidence.RecordRestored("configuration_restore", "face_on");
  evidence.RecordRestored("configuration_restore", "down_the_line");
  evidence.Finalize();

  Check(evidence.passed());
  Check(evidence.restored_count() == 2U);
  Check(evidence.failed_count() == 0U);
  const Json report = Json::parse(SerializeHilCleanupEvidence(evidence));
  Check(report.at("finalized").get<bool>());
  Check(report.at("passed").get<bool>());
  Check(report.at("attempt_count") == 3U);
  Check(report.at("failed_count") == 0U);
  Check(report.at("obligations").at(0).at("failed_attempt_count") == 1U);
  Check(report.at("attempts").at(0).at("outcome") == "failed");
  Check(report.at("attempts").at(1).at("outcome") == "restored");
}

void PrimaryAndRestorationOutcomesAreFullyCrossed() {
  struct Case {
    bool primary_passed;
    bool cleanup_passed;
    bool expected_passed;
    int expected_exit_code;
    std::string expected_diagnostic;
  };
  const std::array cases = {
      Case{.primary_passed = true,
           .cleanup_passed = true,
           .expected_passed = true,
           .expected_exit_code = 0,
           .expected_diagnostic = {}},
      Case{.primary_passed = true,
           .cleanup_passed = false,
           .expected_passed = false,
           .expected_exit_code = 1,
           .expected_diagnostic = "dual-phone HIL cleanup or configuration restoration failed"},
      Case{.primary_passed = false,
           .cleanup_passed = true,
           .expected_passed = false,
           .expected_exit_code = 7,
           .expected_diagnostic = "primary capture failure"},
      Case{.primary_passed = false,
           .cleanup_passed = false,
           .expected_passed = false,
           .expected_exit_code = 7,
           .expected_diagnostic = "primary capture failure"},
  };
  for (const Case &test_case : cases) {
    const auto result = CombineHilPrimaryAndCleanup(
        HilPrimaryOutcome{.passed = test_case.primary_passed,
                          .exit_code = test_case.primary_passed ? 0 : 7,
                          .diagnostic = test_case.primary_passed ? "" : "primary capture failure"},
        test_case.cleanup_passed);
    Check(result.primary_passed == test_case.primary_passed);
    Check(result.cleanup_passed == test_case.cleanup_passed);
    Check(result.passed == test_case.expected_passed);
    Check(result.exit_code == test_case.expected_exit_code);
    Check(result.diagnostic == test_case.expected_diagnostic);
  }
}

void UnresolvedObligationFailsFinalizedEvidence() {
  HilCleanupEvidence evidence;
  evidence.Register("wifi_enable", "down_the_line");
  evidence.RecordFailed("wifi_enable", "down_the_line", "adb timed out");
  evidence.Finalize();
  Check(!evidence.passed());
  Check(evidence.failed_count() == 1U);
  const Json report = Json::parse(SerializeHilCleanupEvidence(evidence));
  Check(report.at("failed_count") == 1U);
  Check(report.at("obligations").at(0).at("last_failure") == "adb timed out");
}

}  // namespace

int main() {
  try {
    CleanupEvidenceRetainsFailedAttemptFollowedBySuccessfulRetry();
    PrimaryAndRestorationOutcomesAreFullyCrossed();
    UnresolvedObligationFailsFinalizedEvidence();
    return 0;
  } catch (...) {
    return 1;
  }
}
