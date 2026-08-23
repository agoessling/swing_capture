#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_CLEANUP_EVIDENCE_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_CLEANUP_EVIDENCE_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::android::dual_hil {

struct CleanupAttempt {
  std::string operation;
  std::string target;
  bool restored = false;
  std::string diagnostic;
};

struct CleanupObligation {
  std::string operation;
  std::string target;
  std::size_t attempt_count = 0U;
  std::size_t failed_attempt_count = 0U;
  bool restored = false;
  std::string last_failure;
};

class HilCleanupEvidence {
 public:
  void Register(std::string operation, std::string target);
  void Cancel(std::string_view operation, std::string_view target);
  void RecordRestored(std::string_view operation, std::string_view target);
  void RecordFailed(std::string_view operation, std::string_view target, std::string diagnostic);
  void Finalize();

  [[nodiscard]] bool finalized() const;
  [[nodiscard]] bool passed() const;
  [[nodiscard]] std::size_t restored_count() const;
  [[nodiscard]] std::size_t failed_count() const;
  [[nodiscard]] const std::vector<CleanupObligation> &obligations() const;
  [[nodiscard]] const std::vector<CleanupAttempt> &attempts() const;

 private:
  CleanupObligation &FindOrRegister(std::string_view operation, std::string_view target);

  std::vector<CleanupObligation> obligations_;
  std::vector<CleanupAttempt> attempts_;
  bool finalized_ = false;
};

[[nodiscard]] std::string SerializeHilCleanupEvidence(const HilCleanupEvidence &evidence);

struct HilPrimaryOutcome {
  bool passed = false;
  int exit_code = 1;
  std::string diagnostic;
};

struct HilCombinedOutcome {
  bool passed = false;
  int exit_code = 1;
  std::string diagnostic;
  bool primary_passed = false;
  bool cleanup_passed = false;
};

[[nodiscard]] HilCombinedOutcome CombineHilPrimaryAndCleanup(HilPrimaryOutcome primary,
                                                             bool cleanup_passed);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_CLEANUP_EVIDENCE_H_
