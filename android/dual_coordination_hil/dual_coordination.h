#ifndef SWING_CAPTURE_ANDROID_DUAL_COORDINATION_HIL_DUAL_COORDINATION_H_
#define SWING_CAPTURE_ANDROID_DUAL_COORDINATION_HIL_DUAL_COORDINATION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace swing_capture::android::dual_coordination_hil {

inline constexpr std::size_t kMinimumClockExchangeSamples = 3U;
inline constexpr std::size_t kMaximumClockExchangeSamples = 64U;
inline constexpr std::int64_t kMaximumReportUncertaintyNs = 10'000'000;
inline constexpr std::int64_t kMaximumPairUncertaintyNs = 20'000'000;
inline constexpr std::int64_t kMaximumTriggerSeparationNs = 50'000'000;

enum class CaptureRole {
  kDownTheLine,
  kFaceOn,
};

[[nodiscard]] std::string_view RoleWireName(CaptureRole role);

// Coordinator timestamps are t1/t4; node timestamps are t2/t3. The offset is
// node clock minus coordinator clock.
struct FourTimestampClockExchange {
  std::int64_t coordinator_send_ns = 0;
  std::int64_t node_receive_ns = 0;
  std::int64_t node_send_ns = 0;
  std::int64_t coordinator_receive_ns = 0;

  bool operator==(const FourTimestampClockExchange &) const = default;
};

struct ClockOffsetEstimate {
  std::string node_id;
  std::int64_t offset_ns = 0;
  std::int64_t uncertainty_ns = 0;
  std::int64_t minimum_round_trip_ns = 0;
  std::int64_t maximum_round_trip_ns = 0;
  std::size_t sample_count = 0;

  bool operator==(const ClockOffsetEstimate &) const = default;
};

enum class ClockEstimateStatus {
  kReady,
  kInvalidNodeId,
  kInsufficientSamples,
  kTooManySamples,
  kInvalidExchange,
  kInconsistentBounds,
};

struct ClockEstimateResult {
  ClockEstimateStatus status = ClockEstimateStatus::kInvalidExchange;
  std::optional<ClockOffsetEstimate> estimate;
  std::string detail;
};

[[nodiscard]] ClockEstimateResult EstimateClockOffset(
    std::string node_id, std::span<const FourTimestampClockExchange> exchanges);

struct TriggerReport {
  CaptureRole role = CaptureRole::kDownTheLine;
  std::string node_id;
  std::string shared_session_id;
  std::string local_session_id;
  std::int64_t trigger_timestamp_ns = 0;
  std::int64_t trigger_uncertainty_ns = 0;
  std::string source;

  bool operator==(const TriggerReport &) const = default;
};

struct TriggerWithClock {
  TriggerReport report;
  ClockEstimateResult clock;
};

// Field-for-field counterpart of PairedCoordinationRecord.NodeEvidence.
struct NodeEvidence {
  CaptureRole role = CaptureRole::kDownTheLine;
  std::string node_id;
  std::string local_session_id;
  std::int64_t trigger_timestamp_ns = 0;
  std::int64_t trigger_uncertainty_ns = 0;
  std::int64_t mapped_coordinator_timestamp_ns = 0;
  std::int64_t mapped_coordinator_uncertainty_ns = 0;
  std::int64_t clock_offset_ns = 0;
  std::int64_t clock_uncertainty_ns = 0;
  std::int64_t minimum_round_trip_ns = 0;
  std::int64_t maximum_round_trip_ns = 0;
  std::size_t clock_sample_count = 0;
  std::string source;

  bool operator==(const NodeEvidence &) const = default;
};

struct PairedCoordinationRecord {
  std::string shared_session_id;
  std::int64_t recorded_at_epoch_ms = 0;
  NodeEvidence down_the_line;
  NodeEvidence face_on;
  std::int64_t minimum_trigger_separation_ns = 0;
  std::int64_t maximum_trigger_separation_ns = 0;

  bool operator==(const PairedCoordinationRecord &) const = default;
};

enum class PairingStatus {
  kPaired,
  kInvalidRecordTimestamp,
  kInvalidReport,
  kMissingRole,
  kDuplicateRole,
  kSameNode,
  kUnrelatedSharedSessionIds,
  kInsufficientClockSamples,
  kInconsistentClocks,
  kInvalidClockEvidence,
  kClockNodeMismatch,
  kExcessiveReportUncertainty,
  kExcessivePairUncertainty,
  kExcessiveSeparation,
};

struct PairingResult {
  PairingStatus status = PairingStatus::kInvalidReport;
  std::optional<PairedCoordinationRecord> record;
  std::string detail;
};

// Produces one terminal decision from all reports for a shared event. Exactly
// one report for each role is required.
[[nodiscard]] PairingResult BuildPairedCoordinationRecord(std::span<const TriggerWithClock> reports,
                                                          std::int64_t recorded_at_epoch_ms);

// Serializes schema v1 in the same field order and with the same JSON types as
// PairedCoordinationRecord.toJson(). All signed 64-bit values are decimal JSON
// strings; schema_version and clock_sample_count remain JSON numbers.
[[nodiscard]] std::string ToCanonicalJson(const PairedCoordinationRecord &record);

}  // namespace swing_capture::android::dual_coordination_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_COORDINATION_HIL_DUAL_COORDINATION_H_
