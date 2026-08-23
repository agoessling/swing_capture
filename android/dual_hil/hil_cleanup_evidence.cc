#include "android/dual_hil/hil_cleanup_evidence.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

void ValidateIdentity(std::string_view operation, std::string_view target) {
  if (operation.empty() || target.empty()) {
    throw std::invalid_argument("HIL cleanup operation and target must be nonempty");
  }
}

}  // namespace

void HilCleanupEvidence::Register(std::string operation, std::string target) {
  ValidateIdentity(operation, target);
  const auto existing =
      std::ranges::find_if(obligations_, [&](const CleanupObligation &obligation) {
        return obligation.operation == operation && obligation.target == target;
      });
  if (existing == obligations_.end()) {
    obligations_.push_back(CleanupObligation{.operation = std::move(operation),
                                             .target = std::move(target),
                                             .attempt_count = 0U,
                                             .failed_attempt_count = 0U,
                                             .restored = false,
                                             .last_failure = {}});
  }
}

void HilCleanupEvidence::Cancel(std::string_view operation, std::string_view target) {
  ValidateIdentity(operation, target);
  const auto obligation =
      std::ranges::find_if(obligations_, [&](const CleanupObligation &candidate) {
        return candidate.operation == operation && candidate.target == target;
      });
  if (obligation == obligations_.end()) {
    throw std::logic_error("cannot cancel an unregistered HIL cleanup obligation");
  }
  obligations_.erase(obligation);
}

CleanupObligation &HilCleanupEvidence::FindOrRegister(std::string_view operation,
                                                      std::string_view target) {
  ValidateIdentity(operation, target);
  auto obligation = std::ranges::find_if(obligations_, [&](const CleanupObligation &candidate) {
    return candidate.operation == operation && candidate.target == target;
  });
  if (obligation == obligations_.end()) {
    Register(std::string(operation), std::string(target));
    obligation = std::prev(obligations_.end());
  }
  return *obligation;
}

void HilCleanupEvidence::RecordRestored(std::string_view operation, std::string_view target) {
  CleanupObligation &obligation = FindOrRegister(operation, target);
  ++obligation.attempt_count;
  obligation.restored = true;
  attempts_.push_back(CleanupAttempt{
      .operation = std::string(operation),
      .target = std::string(target),
      .restored = true,
      .diagnostic = {},
  });
}

void HilCleanupEvidence::RecordFailed(std::string_view operation, std::string_view target,
                                      std::string diagnostic) {
  if (diagnostic.empty()) {
    throw std::invalid_argument("failed HIL cleanup attempt requires a diagnostic");
  }
  CleanupObligation &obligation = FindOrRegister(operation, target);
  ++obligation.attempt_count;
  ++obligation.failed_attempt_count;
  obligation.restored = false;
  obligation.last_failure = diagnostic;
  attempts_.push_back(CleanupAttempt{.operation = std::string(operation),
                                     .target = std::string(target),
                                     .restored = false,
                                     .diagnostic = std::move(diagnostic)});
}

void HilCleanupEvidence::Finalize() { finalized_ = true; }

bool HilCleanupEvidence::finalized() const { return finalized_; }

bool HilCleanupEvidence::passed() const { return finalized_ && failed_count() == 0U; }

std::size_t HilCleanupEvidence::restored_count() const {
  return static_cast<std::size_t>(std::ranges::count_if(
      obligations_, [](const CleanupObligation &obligation) { return obligation.restored; }));
}

std::size_t HilCleanupEvidence::failed_count() const {
  return obligations_.size() - restored_count();
}

const std::vector<CleanupObligation> &HilCleanupEvidence::obligations() const {
  return obligations_;
}

const std::vector<CleanupAttempt> &HilCleanupEvidence::attempts() const { return attempts_; }

std::string SerializeHilCleanupEvidence(const HilCleanupEvidence &evidence) {
  Json obligations = Json::array();
  for (const CleanupObligation &obligation : evidence.obligations()) {
    Json item = {
        {"operation", obligation.operation},
        {"target", obligation.target},
        {"attempted", obligation.attempt_count > 0U},
        {"attempt_count", obligation.attempt_count},
        {"failed_attempt_count", obligation.failed_attempt_count},
        {"restored", obligation.restored},
    };
    if (!obligation.last_failure.empty()) {
      item["last_failure"] = obligation.last_failure;
    }
    obligations.push_back(std::move(item));
  }
  Json attempts = Json::array();
  for (std::size_t index = 0U; index < evidence.attempts().size(); ++index) {
    const CleanupAttempt &attempt = evidence.attempts()[index];
    Json item = {
        {"sequence", index + 1U},
        {"operation", attempt.operation},
        {"target", attempt.target},
        {"outcome", attempt.restored ? "restored" : "failed"},
    };
    if (!attempt.diagnostic.empty()) {
      item["diagnostic"] = attempt.diagnostic;
    }
    attempts.push_back(std::move(item));
  }
  const Json report = {
      {"schema_version", 1},
      {"report_type", "android_dual_phone_hil_cleanup"},
      {"finalized", evidence.finalized()},
      {"passed", evidence.passed()},
      {"obligation_count", evidence.obligations().size()},
      {"attempt_count", evidence.attempts().size()},
      {"restored_count", evidence.restored_count()},
      {"failed_count", evidence.failed_count()},
      {"obligations", std::move(obligations)},
      {"attempts", std::move(attempts)},
  };
  return report.dump(2) + "\n";
}

HilCombinedOutcome CombineHilPrimaryAndCleanup(HilPrimaryOutcome primary, bool cleanup_passed) {
  if (primary.passed && primary.exit_code != 0) {
    throw std::invalid_argument("passing HIL primary outcome must use exit code zero");
  }
  if (!primary.passed && primary.exit_code == 0) {
    throw std::invalid_argument("failing HIL primary outcome must use a nonzero exit code");
  }
  HilCombinedOutcome combined{
      .passed = primary.passed && cleanup_passed,
      .exit_code = primary.exit_code,
      .diagnostic = std::move(primary.diagnostic),
      .primary_passed = primary.passed,
      .cleanup_passed = cleanup_passed,
  };
  if (primary.passed && !cleanup_passed) {
    combined.exit_code = 1;
    combined.diagnostic = "dual-phone HIL cleanup or configuration restoration failed";
  }
  return combined;
}

}  // namespace swing_capture::android::dual_hil
