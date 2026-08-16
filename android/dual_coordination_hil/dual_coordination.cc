#include "android/dual_coordination_hil/dual_coordination.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace swing_capture::android::dual_coordination_hil {
namespace {

constexpr std::size_t kMaximumIdentifierLength = 128U;
constexpr std::size_t kMaximumSourceLength = 64U;

bool IsToken(std::string_view value, std::size_t maximum_length) {
  if (value.empty() || value.size() > maximum_length) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' ||
           character == '-';
  });
}

bool IsKnownRole(CaptureRole role) {
  return role == CaptureRole::kDownTheLine || role == CaptureRole::kFaceOn;
}

ClockEstimateResult ClockFailure(ClockEstimateStatus status, std::string detail) {
  return ClockEstimateResult{
      .status = status, .estimate = std::nullopt, .detail = std::move(detail)};
}

PairingResult PairingFailure(PairingStatus status, std::string detail) {
  return PairingResult{.status = status, .record = std::nullopt, .detail = std::move(detail)};
}

bool CheckedAdd(std::int64_t first, std::int64_t second, std::int64_t *result) {
  if ((second > 0 && first > std::numeric_limits<std::int64_t>::max() - second) ||
      (second < 0 && first < std::numeric_limits<std::int64_t>::min() - second)) {
    return false;
  }
  *result = first + second;
  return true;
}

bool CheckedSubtract(std::int64_t first, std::int64_t second, std::int64_t *result) {
  if ((second > 0 && first < std::numeric_limits<std::int64_t>::min() + second) ||
      (second < 0 && first > std::numeric_limits<std::int64_t>::max() + second)) {
    return false;
  }
  *result = first - second;
  return true;
}

struct MappingResult {
  PairingStatus status = PairingStatus::kInvalidClockEvidence;
  std::optional<NodeEvidence> evidence;
  std::string detail;
};

MappingResult MappingFailure(PairingStatus status, std::string detail) {
  return MappingResult{.status = status, .evidence = std::nullopt, .detail = std::move(detail)};
}

MappingResult MapReport(const TriggerWithClock &input) {
  const TriggerReport &report = input.report;
  if (!IsKnownRole(report.role) || !IsToken(report.node_id, kMaximumIdentifierLength) ||
      !IsToken(report.shared_session_id, kMaximumIdentifierLength) ||
      !IsToken(report.local_session_id, kMaximumIdentifierLength) ||
      !IsToken(report.source, kMaximumSourceLength) || report.trigger_timestamp_ns <= 0 ||
      report.trigger_uncertainty_ns < 0) {
    return MappingFailure(PairingStatus::kInvalidReport,
                          "trigger report identity or local timing is invalid");
  }

  if (input.clock.status == ClockEstimateStatus::kInsufficientSamples) {
    return MappingFailure(PairingStatus::kInsufficientClockSamples,
                          "trigger report has fewer than three clock exchanges");
  }
  if (input.clock.status == ClockEstimateStatus::kInconsistentBounds) {
    return MappingFailure(PairingStatus::kInconsistentClocks,
                          "trigger report clock bounds do not intersect");
  }
  if (input.clock.status != ClockEstimateStatus::kReady || !input.clock.estimate.has_value()) {
    return MappingFailure(PairingStatus::kInvalidClockEvidence,
                          "trigger report does not have a valid clock estimate");
  }

  const ClockOffsetEstimate &clock = *input.clock.estimate;
  if (clock.node_id != report.node_id) {
    return MappingFailure(PairingStatus::kClockNodeMismatch,
                          "clock estimate belongs to a different node");
  }
  if (clock.sample_count < kMinimumClockExchangeSamples) {
    return MappingFailure(PairingStatus::kInsufficientClockSamples,
                          "durable timing requires at least three clock exchanges");
  }
  if (clock.sample_count > kMaximumClockExchangeSamples || clock.uncertainty_ns < 0 ||
      clock.minimum_round_trip_ns < 0 ||
      clock.maximum_round_trip_ns < clock.minimum_round_trip_ns) {
    return MappingFailure(PairingStatus::kInvalidClockEvidence,
                          "clock estimate uncertainty, RTT, or sample count is invalid");
  }

  std::int64_t mapped_timestamp_ns = 0;
  std::int64_t mapped_uncertainty_ns = 0;
  if (!CheckedSubtract(report.trigger_timestamp_ns, clock.offset_ns, &mapped_timestamp_ns) ||
      mapped_timestamp_ns < 0 ||
      !CheckedAdd(report.trigger_uncertainty_ns, clock.uncertainty_ns, &mapped_uncertainty_ns)) {
    return MappingFailure(PairingStatus::kInvalidClockEvidence,
                          "mapping the trigger into the coordinator clock overflows");
  }
  if (mapped_uncertainty_ns > kMaximumReportUncertaintyNs) {
    return MappingFailure(PairingStatus::kExcessiveReportUncertainty,
                          "mapped report uncertainty exceeds 10 ms");
  }

  NodeEvidence evidence{
      .role = report.role,
      .node_id = report.node_id,
      .local_session_id = report.local_session_id,
      .trigger_timestamp_ns = report.trigger_timestamp_ns,
      .trigger_uncertainty_ns = report.trigger_uncertainty_ns,
      .mapped_coordinator_timestamp_ns = mapped_timestamp_ns,
      .mapped_coordinator_uncertainty_ns = mapped_uncertainty_ns,
      .clock_offset_ns = clock.offset_ns,
      .clock_uncertainty_ns = clock.uncertainty_ns,
      .minimum_round_trip_ns = clock.minimum_round_trip_ns,
      .maximum_round_trip_ns = clock.maximum_round_trip_ns,
      .clock_sample_count = clock.sample_count,
      .source = report.source,
  };
  return MappingResult{
      .status = PairingStatus::kPaired, .evidence = std::move(evidence), .detail = "mapped"};
}

bool IsValidReport(const TriggerReport &report) {
  return IsKnownRole(report.role) && IsToken(report.node_id, kMaximumIdentifierLength) &&
         IsToken(report.shared_session_id, kMaximumIdentifierLength) &&
         IsToken(report.local_session_id, kMaximumIdentifierLength) &&
         IsToken(report.source, kMaximumSourceLength) && report.trigger_timestamp_ns > 0 &&
         report.trigger_uncertainty_ns >= 0;
}

struct SelectedReports {
  PairingStatus status = PairingStatus::kInvalidReport;
  const TriggerWithClock *down = nullptr;
  const TriggerWithClock *face = nullptr;
  std::string detail;
};

SelectedReports SelectReports(std::span<const TriggerWithClock> reports) {
  if (reports.empty()) {
    return SelectedReports{.status = PairingStatus::kMissingRole,
                           .detail = "both down_the_line and face_on reports are missing"};
  }
  SelectedReports selected;
  for (const TriggerWithClock &input : reports) {
    if (!IsValidReport(input.report)) {
      return SelectedReports{.status = PairingStatus::kInvalidReport,
                             .detail = "trigger report identity or local timing is invalid"};
    }
    if (input.report.shared_session_id != reports.front().report.shared_session_id) {
      return SelectedReports{.status = PairingStatus::kUnrelatedSharedSessionIds,
                             .detail = "trigger reports have unrelated shared_session_id values"};
    }
    const TriggerWithClock **slot =
        input.report.role == CaptureRole::kDownTheLine ? &selected.down : &selected.face;
    if (*slot != nullptr) {
      return SelectedReports{.status = PairingStatus::kDuplicateRole,
                             .detail = "a shared event contains duplicate capture roles"};
    }
    *slot = &input;
  }
  if (selected.down == nullptr || selected.face == nullptr) {
    return SelectedReports{.status = PairingStatus::kMissingRole,
                           .detail = "a shared event is missing one capture role"};
  }
  selected.status = PairingStatus::kPaired;
  selected.detail = "selected";
  return selected;
}

PairingResult FinishPair(std::string shared_session_id, std::int64_t recorded_at_epoch_ms,
                         NodeEvidence down, NodeEvidence face) {
  std::int64_t combined_uncertainty_ns = 0;
  if (!CheckedAdd(down.mapped_coordinator_uncertainty_ns, face.mapped_coordinator_uncertainty_ns,
                  &combined_uncertainty_ns)) {
    return PairingFailure(PairingStatus::kInvalidClockEvidence,
                          "combined mapped uncertainty overflows");
  }
  if (combined_uncertainty_ns > kMaximumPairUncertaintyNs) {
    return PairingFailure(PairingStatus::kExcessivePairUncertainty,
                          "combined mapped uncertainty exceeds 20 ms");
  }

  const std::int64_t earlier_ns =
      std::min(down.mapped_coordinator_timestamp_ns, face.mapped_coordinator_timestamp_ns);
  const std::int64_t later_ns =
      std::max(down.mapped_coordinator_timestamp_ns, face.mapped_coordinator_timestamp_ns);
  const std::int64_t center_separation_ns = later_ns - earlier_ns;
  const std::int64_t minimum_separation_ns = center_separation_ns > combined_uncertainty_ns
                                                 ? center_separation_ns - combined_uncertainty_ns
                                                 : 0;
  std::int64_t maximum_separation_ns = 0;
  if (!CheckedAdd(center_separation_ns, combined_uncertainty_ns, &maximum_separation_ns)) {
    return PairingFailure(PairingStatus::kInvalidClockEvidence,
                          "maximum mapped trigger separation overflows");
  }
  if (maximum_separation_ns > kMaximumTriggerSeparationNs) {
    return PairingFailure(PairingStatus::kExcessiveSeparation,
                          "conservative mapped trigger separation exceeds 50 ms");
  }

  PairedCoordinationRecord record{
      .shared_session_id = std::move(shared_session_id),
      .recorded_at_epoch_ms = recorded_at_epoch_ms,
      .down_the_line = std::move(down),
      .face_on = std::move(face),
      .minimum_trigger_separation_ns = minimum_separation_ns,
      .maximum_trigger_separation_ns = maximum_separation_ns,
  };
  return PairingResult{
      .status = PairingStatus::kPaired, .record = std::move(record), .detail = "paired"};
}

TriggerWithClock InputFromEvidence(const NodeEvidence &evidence,
                                   std::string_view shared_session_id) {
  TriggerReport report{
      .role = evidence.role,
      .node_id = evidence.node_id,
      .shared_session_id = std::string(shared_session_id),
      .local_session_id = evidence.local_session_id,
      .trigger_timestamp_ns = evidence.trigger_timestamp_ns,
      .trigger_uncertainty_ns = evidence.trigger_uncertainty_ns,
      .source = evidence.source,
  };
  ClockOffsetEstimate estimate{
      .node_id = evidence.node_id,
      .offset_ns = evidence.clock_offset_ns,
      .uncertainty_ns = evidence.clock_uncertainty_ns,
      .minimum_round_trip_ns = evidence.minimum_round_trip_ns,
      .maximum_round_trip_ns = evidence.maximum_round_trip_ns,
      .sample_count = evidence.clock_sample_count,
  };
  return TriggerWithClock{
      .report = std::move(report),
      .clock = ClockEstimateResult{.status = ClockEstimateStatus::kReady,
                                   .estimate = std::move(estimate),
                                   .detail = "ready"},
  };
}

void AppendName(std::string *json, std::string_view name) {
  if (json->back() != '{') {
    json->push_back(',');
  }
  json->push_back('"');
  json->append(name);
  json->append("\":");
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void AppendString(std::string *json, std::string_view name, std::string_view value) {
  AppendName(json, name);
  json->push_back('"');
  json->append(value);
  json->push_back('"');
}

void AppendNumber(std::string *json, std::string_view name, std::size_t value) {
  AppendName(json, name);
  json->append(std::to_string(value));
}

void AppendSignedString(std::string *json, std::string_view name, std::int64_t value) {
  AppendString(json, name, std::to_string(value));
}

void AppendNode(std::string *json, std::string_view name, const NodeEvidence &evidence) {
  AppendName(json, name);
  json->push_back('{');
  AppendString(json, "role", RoleWireName(evidence.role));
  AppendString(json, "node_id", evidence.node_id);
  AppendString(json, "local_session_id", evidence.local_session_id);
  AppendSignedString(json, "trigger_timestamp_ns", evidence.trigger_timestamp_ns);
  AppendSignedString(json, "trigger_uncertainty_ns", evidence.trigger_uncertainty_ns);
  AppendSignedString(json, "mapped_coordinator_timestamp_ns",
                     evidence.mapped_coordinator_timestamp_ns);
  AppendSignedString(json, "mapped_coordinator_uncertainty_ns",
                     evidence.mapped_coordinator_uncertainty_ns);
  AppendSignedString(json, "clock_offset_ns", evidence.clock_offset_ns);
  AppendSignedString(json, "clock_uncertainty_ns", evidence.clock_uncertainty_ns);
  AppendSignedString(json, "minimum_round_trip_ns", evidence.minimum_round_trip_ns);
  AppendSignedString(json, "maximum_round_trip_ns", evidence.maximum_round_trip_ns);
  AppendNumber(json, "clock_sample_count", evidence.clock_sample_count);
  AppendString(json, "source", evidence.source);
  json->push_back('}');
}

}  // namespace

std::string_view RoleWireName(CaptureRole role) {
  switch (role) {
    case CaptureRole::kDownTheLine:
      return "down_the_line";
    case CaptureRole::kFaceOn:
      return "face_on";
  }
  throw std::invalid_argument("unknown capture role");
}

ClockEstimateResult EstimateClockOffset(std::string node_id,
                                        std::span<const FourTimestampClockExchange> exchanges) {
  if (!IsToken(node_id, kMaximumIdentifierLength)) {
    return ClockFailure(ClockEstimateStatus::kInvalidNodeId,
                        "clock estimate node_id is not a bounded identifier");
  }
  if (exchanges.size() < kMinimumClockExchangeSamples) {
    return ClockFailure(ClockEstimateStatus::kInsufficientSamples,
                        "at least three clock exchanges are required");
  }
  if (exchanges.size() > kMaximumClockExchangeSamples) {
    return ClockFailure(ClockEstimateStatus::kTooManySamples,
                        "at most 64 clock exchanges are retained");
  }

  std::int64_t lower_bound_ns = std::numeric_limits<std::int64_t>::min();
  std::int64_t upper_bound_ns = std::numeric_limits<std::int64_t>::max();
  std::int64_t minimum_round_trip_ns = std::numeric_limits<std::int64_t>::max();
  std::int64_t maximum_round_trip_ns = 0;
  std::int64_t previous_receive_ns = -1;
  for (std::size_t index = 0; index < exchanges.size(); ++index) {
    const FourTimestampClockExchange &exchange = exchanges[index];
    if (exchange.coordinator_send_ns < 0 || exchange.node_receive_ns < 0 ||
        exchange.node_send_ns < exchange.node_receive_ns ||
        exchange.coordinator_receive_ns < exchange.coordinator_send_ns ||
        exchange.coordinator_receive_ns <= previous_receive_ns) {
      return ClockFailure(ClockEstimateStatus::kInvalidExchange,
                          "clock exchange " + std::to_string(index) +
                              " has negative, reversed, or nonmonotonic timestamps");
    }
    const std::int64_t coordinator_elapsed_ns =
        exchange.coordinator_receive_ns - exchange.coordinator_send_ns;
    const std::int64_t node_processing_ns = exchange.node_send_ns - exchange.node_receive_ns;
    if (node_processing_ns > coordinator_elapsed_ns) {
      return ClockFailure(ClockEstimateStatus::kInvalidExchange,
                          "clock exchange " + std::to_string(index) +
                              " has node processing longer than total exchange time");
    }
    const std::int64_t round_trip_ns = coordinator_elapsed_ns - node_processing_ns;
    const std::int64_t sample_lower_ns = exchange.node_send_ns - exchange.coordinator_receive_ns;
    const std::int64_t sample_upper_ns = exchange.node_receive_ns - exchange.coordinator_send_ns;
    lower_bound_ns = std::max(lower_bound_ns, sample_lower_ns);
    upper_bound_ns = std::min(upper_bound_ns, sample_upper_ns);
    minimum_round_trip_ns = std::min(minimum_round_trip_ns, round_trip_ns);
    maximum_round_trip_ns = std::max(maximum_round_trip_ns, round_trip_ns);
    previous_receive_ns = exchange.coordinator_receive_ns;
  }

  if (lower_bound_ns > upper_bound_ns) {
    return ClockFailure(ClockEstimateStatus::kInconsistentBounds,
                        "repeated clock offset bounds do not intersect");
  }
  const std::int64_t midpoint_ns = std::midpoint(lower_bound_ns, upper_bound_ns);
  const std::int64_t uncertainty_ns =
      std::max(midpoint_ns - lower_bound_ns, upper_bound_ns - midpoint_ns);
  ClockOffsetEstimate estimate{
      .node_id = std::move(node_id),
      .offset_ns = midpoint_ns,
      .uncertainty_ns = uncertainty_ns,
      .minimum_round_trip_ns = minimum_round_trip_ns,
      .maximum_round_trip_ns = maximum_round_trip_ns,
      .sample_count = exchanges.size(),
  };
  return ClockEstimateResult{
      .status = ClockEstimateStatus::kReady, .estimate = std::move(estimate), .detail = "ready"};
}

PairingResult BuildPairedCoordinationRecord(std::span<const TriggerWithClock> reports,
                                            std::int64_t recorded_at_epoch_ms) {
  if (recorded_at_epoch_ms <= 0) {
    return PairingFailure(PairingStatus::kInvalidRecordTimestamp,
                          "recorded_at_epoch_ms must be positive");
  }
  const SelectedReports selected = SelectReports(reports);
  if (selected.status != PairingStatus::kPaired) {
    return PairingFailure(selected.status, selected.detail);
  }
  if (selected.down->report.node_id == selected.face->report.node_id) {
    return PairingFailure(PairingStatus::kSameNode, "one node cannot provide both capture roles");
  }

  MappingResult down_mapping = MapReport(*selected.down);
  if (!down_mapping.evidence.has_value()) {
    return PairingFailure(down_mapping.status, std::move(down_mapping.detail));
  }
  MappingResult face_mapping = MapReport(*selected.face);
  if (!face_mapping.evidence.has_value()) {
    return PairingFailure(face_mapping.status, std::move(face_mapping.detail));
  }
  return FinishPair(selected.down->report.shared_session_id, recorded_at_epoch_ms,
                    std::move(*down_mapping.evidence), std::move(*face_mapping.evidence));
}

std::string ToCanonicalJson(const PairedCoordinationRecord &record) {
  const std::array inputs = {
      InputFromEvidence(record.down_the_line, record.shared_session_id),
      InputFromEvidence(record.face_on, record.shared_session_id),
  };
  const PairingResult rebuilt = BuildPairedCoordinationRecord(inputs, record.recorded_at_epoch_ms);
  if (rebuilt.status != PairingStatus::kPaired || !rebuilt.record.has_value() ||
      *rebuilt.record != record) {
    throw std::invalid_argument("paired coordination record is inconsistent: " + rebuilt.detail);
  }

  std::string json;
  json.reserve(1024U);
  json.push_back('{');
  AppendNumber(&json, "schema_version", 1U);
  AppendString(&json, "shared_session_id", record.shared_session_id);
  AppendString(&json, "status", "paired");
  AppendSignedString(&json, "recorded_at_epoch_ms", record.recorded_at_epoch_ms);
  AppendNode(&json, "down_the_line", record.down_the_line);
  AppendNode(&json, "face_on", record.face_on);
  AppendSignedString(&json, "minimum_trigger_separation_ns", record.minimum_trigger_separation_ns);
  AppendSignedString(&json, "maximum_trigger_separation_ns", record.maximum_trigger_separation_ns);
  json.push_back('}');
  return json;
}

}  // namespace swing_capture::android::dual_coordination_hil
