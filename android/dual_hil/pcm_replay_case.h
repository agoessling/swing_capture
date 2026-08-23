#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_PCM_REPLAY_CASE_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_PCM_REPLAY_CASE_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::android::dual_hil {

enum class PcmReplayExpectation {
  kRequiredPositive,
  kDiagnosticNegative,
};

enum class PcmReplayRunDisposition {
  kRequiredPositiveDetected,
  kRequiredPositiveMissed,
  kExpectedDiagnosticNegative,
  kDiagnosticNegativeFalseTrigger,
};

struct PcmReplayCase {
  std::string name;
  PcmReplayExpectation expectation = PcmReplayExpectation::kDiagnosticNegative;
  std::uint64_t start_frame = 0;
  std::uint32_t sample_count = 0;
  std::uint32_t marker_frame = 0;
  std::uint32_t gain_permille = 0;
  std::uint32_t expected_crc32 = 0;
};

struct PcmReplayManifest {
  std::string source_id;
  std::uint32_t source_sample_rate_hz = 0;
  std::vector<PcmReplayCase> cases;
};

struct ExtractedPcmReplayCase {
  PcmReplayCase definition;
  std::string source_id;
  std::uint32_t sample_rate_hz = 0;
  std::uint32_t source_crc32 = 0;
  std::vector<std::int16_t> samples;
};

enum class PcmReplayPairOutcome {
  kNeither,
  kLeaderOnly,
  kShadowOnly,
  kBoth,
};

[[nodiscard]] constexpr PcmReplayPairOutcome ClassifyPcmReplayPairOutcome(bool leader_detected,
                                                                          bool shadow_detected) {
  if (leader_detected) {
    return shadow_detected ? PcmReplayPairOutcome::kBoth : PcmReplayPairOutcome::kLeaderOnly;
  }
  return shadow_detected ? PcmReplayPairOutcome::kShadowOnly : PcmReplayPairOutcome::kNeither;
}

[[nodiscard]] std::string_view PcmReplayPairOutcomeName(PcmReplayPairOutcome outcome);

// A partial pair must remain unchanged for at least the complete bounded peer-impact delivery
// path before a HIL may issue a missed-shot salvage request. Otherwise salvage can make the peer
// CAPTURE_ACTIVE while the leader's asynchronous impact request is still in flight.
[[nodiscard]] constexpr std::chrono::milliseconds PcmReplayPeerImpactQuiescenceWindow(
    std::chrono::milliseconds connect_timeout, std::chrono::milliseconds read_timeout,
    std::size_t maximum_attempts, std::chrono::milliseconds retry_delay,
    std::chrono::milliseconds scheduling_margin) {
  const std::size_t retry_count = maximum_attempts > 0U ? maximum_attempts - 1U : 0U;
  return (connect_timeout + read_timeout) * maximum_attempts + retry_delay * retry_count +
         scheduling_margin;
}

enum class PcmReplayObservationDecision {
  kContinue,
  kPairedTerminal,
  kNeitherQuiescent,
  kPartialQuiescent,
};

[[nodiscard]] constexpr PcmReplayObservationDecision ClassifyPcmReplayObservationDecision(
    PcmReplayPairOutcome outcome, std::chrono::milliseconds elapsed_since_start,
    std::optional<std::chrono::milliseconds> stable_partial_elapsed,
    std::chrono::milliseconds initial_observation_window,
    std::chrono::milliseconds partial_quiescence_window) {
  if (outcome == PcmReplayPairOutcome::kBoth) {
    return PcmReplayObservationDecision::kPairedTerminal;
  }
  if (outcome == PcmReplayPairOutcome::kNeither) {
    return elapsed_since_start >= initial_observation_window
               ? PcmReplayObservationDecision::kNeitherQuiescent
               : PcmReplayObservationDecision::kContinue;
  }
  return stable_partial_elapsed.has_value() && *stable_partial_elapsed >= partial_quiescence_window
             ? PcmReplayObservationDecision::kPartialQuiescent
             : PcmReplayObservationDecision::kContinue;
}

struct PcmReplayObservation {
  std::string case_name;
  std::uint32_t source_crc32 = 0;
  std::uint32_t gain_permille = 0;
  bool leader_detected = false;
  bool shadow_detected = false;
  std::string leader_source;
  std::string shadow_source;

  [[nodiscard]] constexpr PcmReplayPairOutcome pair_outcome() const {
    return ClassifyPcmReplayPairOutcome(leader_detected, shadow_detected);
  }

  [[nodiscard]] constexpr bool paired_detected() const {
    return pair_outcome() == PcmReplayPairOutcome::kBoth;
  }

  [[nodiscard]] constexpr bool any_detected() const {
    return pair_outcome() != PcmReplayPairOutcome::kNeither;
  }
};

struct PcmReplayScore {
  std::size_t required_positive_count = 0;
  std::size_t required_positive_detected = 0;
  std::size_t diagnostic_negative_count = 0;
  std::size_t diagnostic_negative_detected = 0;
  bool required_gate_passed = false;
};

[[nodiscard]] PcmReplayManifest ParsePcmReplayManifest(std::string_view manifest_json);

[[nodiscard]] const PcmReplayCase &FindPcmReplayCase(const PcmReplayManifest &manifest,
                                                     std::string_view name);

[[nodiscard]] ExtractedPcmReplayCase ExtractPcmReplayCase(const PcmReplayManifest &manifest,
                                                          const PcmReplayCase &replay_case,
                                                          std::span<const std::byte> source_wav);

// Scores required positives as the qualification gate. Diagnostic negatives are always reported
// in the confusion counts but cannot silently turn a known hard negative into a green gate.
[[nodiscard]] PcmReplayScore ScorePcmReplayObservations(
    const PcmReplayManifest &manifest, std::span<const PcmReplayObservation> observations);

[[nodiscard]] constexpr PcmReplayRunDisposition ClassifyPcmReplayRun(
    PcmReplayExpectation expectation, bool detected) {
  if (expectation == PcmReplayExpectation::kRequiredPositive) {
    return detected ? PcmReplayRunDisposition::kRequiredPositiveDetected
                    : PcmReplayRunDisposition::kRequiredPositiveMissed;
  }
  return detected ? PcmReplayRunDisposition::kDiagnosticNegativeFalseTrigger
                  : PcmReplayRunDisposition::kExpectedDiagnosticNegative;
}

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_PCM_REPLAY_CASE_H_
