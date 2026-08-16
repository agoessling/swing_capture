#include "android/dual_coordination_hil/dual_coordination.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace coordination = swing_capture::android::dual_coordination_hil;
using coordination::BuildPairedCoordinationRecord;
using coordination::CaptureRole;
using coordination::ClockEstimateResult;
using coordination::ClockEstimateStatus;
using coordination::ClockOffsetEstimate;
using coordination::EstimateClockOffset;
using coordination::FourTimestampClockExchange;
using coordination::PairedCoordinationRecord;
using coordination::PairingResult;
using coordination::PairingStatus;
using coordination::ToCanonicalJson;
using coordination::TriggerReport;
using coordination::TriggerWithClock;
using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr std::int64_t kRecordedAtEpochMs = 1'700'000'000'000;

ClockEstimateResult ReadyClock(std::string node_id, std::int64_t offset_ns,
                               std::int64_t uncertainty_ns, std::int64_t minimum_round_trip_ns,
                               std::int64_t maximum_round_trip_ns, std::size_t sample_count = 3U) {
  ClockOffsetEstimate estimate{
      .node_id = std::move(node_id),
      .offset_ns = offset_ns,
      .uncertainty_ns = uncertainty_ns,
      .minimum_round_trip_ns = minimum_round_trip_ns,
      .maximum_round_trip_ns = maximum_round_trip_ns,
      .sample_count = sample_count,
  };
  return ClockEstimateResult{
      .status = ClockEstimateStatus::kReady, .estimate = std::move(estimate), .detail = "ready"};
}

std::array<TriggerWithClock, 2> JavaFixtureInputs() {
  return {
      TriggerWithClock{
          .report =
              TriggerReport{
                  .role = CaptureRole::kDownTheLine,
                  .node_id = "dtl-node",
                  .shared_session_id = "shared-42",
                  .local_session_id = "dtl-local",
                  .trigger_timestamp_ns = 10'000'000,
                  .trigger_uncertainty_ns = 200,
                  .source = "local_audio",
              },
          .clock = ReadyClock("dtl-node", 1'000'000, 300, 400, 800),
      },
      TriggerWithClock{
          .report =
              TriggerReport{
                  .role = CaptureRole::kFaceOn,
                  .node_id = "face-node",
                  .shared_session_id = "shared-42",
                  .local_session_id = "face-local",
                  .trigger_timestamp_ns = 20'001'000,
                  .trigger_uncertainty_ns = 250,
                  .source = "local_audio",
              },
          .clock = ReadyClock("face-node", 11'000'000, 250, 500, 900),
      },
  };
}

PairingResult PairJavaFixture() {
  const auto inputs = JavaFixtureInputs();
  return BuildPairedCoordinationRecord(inputs, kRecordedAtEpochMs);
}

void ExpectStatus(const PairingResult &result, PairingStatus expected) {
  assert(result.status == expected);
  assert(!result.detail.empty());
  assert((expected == PairingStatus::kPaired) == result.record.has_value());
}

void TestIntersectsRepeatedClockBounds() {
  constexpr std::array kExchanges = {
      FourTimestampClockExchange{10'000, 11'200, 11'250, 10'650},
      FourTimestampClockExchange{20'000, 21'100, 21'150, 20'250},
      FourTimestampClockExchange{30'000, 31'150, 31'200, 30'250},
  };
  const ClockEstimateResult result = EstimateClockOffset("face-node", kExchanges);
  assert(result.status == ClockEstimateStatus::kReady);
  const ClockOffsetEstimate &estimate = result.estimate.value();
  assert(estimate.node_id == "face-node");
  assert(estimate.offset_ns == 1'025);
  assert(estimate.uncertainty_ns == 75);
  assert(estimate.offset_ns - estimate.uncertainty_ns == 950);
  assert(estimate.offset_ns + estimate.uncertainty_ns == 1'100);
  assert(estimate.minimum_round_trip_ns == 200);
  assert(estimate.maximum_round_trip_ns == 600);
  assert(estimate.sample_count == 3U);
}

void TestRejectsInsufficientInvalidAndInconsistentClocks() {
  constexpr std::array kTwoExchanges = {
      FourTimestampClockExchange{1'000, 2'050, 2'060, 1'110},
      FourTimestampClockExchange{2'000, 3'050, 3'060, 2'110},
  };
  assert(EstimateClockOffset("node", kTwoExchanges).status ==
         ClockEstimateStatus::kInsufficientSamples);

  constexpr std::array kInconsistent = {
      FourTimestampClockExchange{1'000, 2'100, 2'100, 1'200},
      FourTimestampClockExchange{2'000, 3'400, 3'400, 2'200},
      FourTimestampClockExchange{3'000, 4'400, 4'400, 3'200},
  };
  assert(EstimateClockOffset("node", kInconsistent).status ==
         ClockEstimateStatus::kInconsistentBounds);

  auto invalid = std::array{
      FourTimestampClockExchange{100, 1'000, 1'200, 200},
      FourTimestampClockExchange{300, 1'300, 1'310, 410},
      FourTimestampClockExchange{500, 1'500, 1'510, 610},
  };
  assert(EstimateClockOffset("node", invalid).status == ClockEstimateStatus::kInvalidExchange);

  invalid = {
      FourTimestampClockExchange{100, 1'050, 1'060, 210},
      FourTimestampClockExchange{110, 1'060, 1'070, 205},
      FourTimestampClockExchange{300, 1'250, 1'260, 410},
  };
  assert(EstimateClockOffset("node", invalid).status == ClockEstimateStatus::kInvalidExchange);

  std::vector<FourTimestampClockExchange> too_many;
  too_many.reserve(coordination::kMaximumClockExchangeSamples + 1U);
  for (std::size_t index = 0; index <= coordination::kMaximumClockExchangeSamples; ++index) {
    const auto base = static_cast<std::int64_t>(index * 1'000U);
    too_many.push_back(FourTimestampClockExchange{base, base + 100, base + 110, base + 210});
  }
  assert(EstimateClockOffset("node", too_many).status == ClockEstimateStatus::kTooManySamples);
  assert(EstimateClockOffset("bad/node", kInconsistent).status ==
         ClockEstimateStatus::kInvalidNodeId);
}

void TestBuildsCanonicalJavaCompatibleRecord() {
  const PairingResult result = PairJavaFixture();
  ExpectStatus(result, PairingStatus::kPaired);
  const PairedCoordinationRecord &record = result.record.value();
  assert(record.down_the_line.mapped_coordinator_timestamp_ns == 9'000'000);
  assert(record.face_on.mapped_coordinator_timestamp_ns == 9'001'000);
  assert(record.down_the_line.mapped_coordinator_uncertainty_ns == 500);
  assert(record.face_on.mapped_coordinator_uncertainty_ns == 500);
  assert(record.minimum_trigger_separation_ns == 0);
  assert(record.maximum_trigger_separation_ns == 2'000);

  constexpr std::string_view kJavaGolden =
      R"({"schema_version":1,"shared_session_id":"shared-42","status":"paired","recorded_at_epoch_ms":"1700000000000","down_the_line":{"role":"down_the_line","node_id":"dtl-node","local_session_id":"dtl-local","trigger_timestamp_ns":"10000000","trigger_uncertainty_ns":"200","mapped_coordinator_timestamp_ns":"9000000","mapped_coordinator_uncertainty_ns":"500","clock_offset_ns":"1000000","clock_uncertainty_ns":"300","minimum_round_trip_ns":"400","maximum_round_trip_ns":"800","clock_sample_count":3,"source":"local_audio"},"face_on":{"role":"face_on","node_id":"face-node","local_session_id":"face-local","trigger_timestamp_ns":"20001000","trigger_uncertainty_ns":"250","mapped_coordinator_timestamp_ns":"9001000","mapped_coordinator_uncertainty_ns":"500","clock_offset_ns":"11000000","clock_uncertainty_ns":"250","minimum_round_trip_ns":"500","maximum_round_trip_ns":"900","clock_sample_count":3,"source":"local_audio"},"minimum_trigger_separation_ns":"0","maximum_trigger_separation_ns":"2000"})";
  const std::string json = ToCanonicalJson(record);
  assert(json == kJavaGolden);

  const Json parsed = Json::parse(json);
  assert(parsed.size() == 8U);
  assert(parsed.at("schema_version").is_number_integer());
  assert(parsed.at("schema_version") == 1);
  assert(parsed.at("recorded_at_epoch_ms").is_string());
  assert(parsed.at("recorded_at_epoch_ms") == "1700000000000");
  assert(parsed.at("down_the_line").size() == 13U);
  assert(parsed.at("down_the_line").at("trigger_timestamp_ns").is_string());
  assert(parsed.at("down_the_line").at("clock_sample_count").is_number_integer());
  assert(parsed.at("down_the_line").at("clock_sample_count") == 3);
  assert(Json::parse(kJavaGolden) == parsed);
}

void TestRejectsRoleIdentityAndClockFailures() {
  const auto nominal = JavaFixtureInputs();

  const std::array missing = {nominal[0]};
  ExpectStatus(BuildPairedCoordinationRecord(missing, kRecordedAtEpochMs),
               PairingStatus::kMissingRole);

  auto duplicate = nominal;
  duplicate[1].report.role = CaptureRole::kDownTheLine;
  ExpectStatus(BuildPairedCoordinationRecord(duplicate, kRecordedAtEpochMs),
               PairingStatus::kDuplicateRole);

  auto same_node = nominal;
  same_node[1].report.node_id = same_node[0].report.node_id;
  same_node[1].clock.estimate->node_id = same_node[0].report.node_id;
  ExpectStatus(BuildPairedCoordinationRecord(same_node, kRecordedAtEpochMs),
               PairingStatus::kSameNode);

  auto unrelated = nominal;
  unrelated[1].report.shared_session_id = "shared-43";
  ExpectStatus(BuildPairedCoordinationRecord(unrelated, kRecordedAtEpochMs),
               PairingStatus::kUnrelatedSharedSessionIds);

  auto inconsistent = nominal;
  inconsistent[0].clock = ClockEstimateResult{.status = ClockEstimateStatus::kInconsistentBounds,
                                              .estimate = std::nullopt,
                                              .detail = "no intersection"};
  ExpectStatus(BuildPairedCoordinationRecord(inconsistent, kRecordedAtEpochMs),
               PairingStatus::kInconsistentClocks);

  auto insufficient = nominal;
  insufficient[0].clock = ClockEstimateResult{.status = ClockEstimateStatus::kInsufficientSamples,
                                              .estimate = std::nullopt,
                                              .detail = "two samples"};
  ExpectStatus(BuildPairedCoordinationRecord(insufficient, kRecordedAtEpochMs),
               PairingStatus::kInsufficientClockSamples);

  auto wrong_node = nominal;
  wrong_node[0].clock.estimate->node_id = "another-node";
  ExpectStatus(BuildPairedCoordinationRecord(wrong_node, kRecordedAtEpochMs),
               PairingStatus::kClockNodeMismatch);

  auto invalid = nominal;
  invalid[0].report.local_session_id = "bad/session";
  ExpectStatus(BuildPairedCoordinationRecord(invalid, kRecordedAtEpochMs),
               PairingStatus::kInvalidReport);
  ExpectStatus(BuildPairedCoordinationRecord(nominal, 0), PairingStatus::kInvalidRecordTimestamp);
}

void TestEnforcesUncertaintyAndSeparationGates() {
  auto excessive_report = JavaFixtureInputs();
  excessive_report[0].report.trigger_uncertainty_ns = coordination::kMaximumReportUncertaintyNs;
  excessive_report[0].clock.estimate->uncertainty_ns = 1;
  ExpectStatus(BuildPairedCoordinationRecord(excessive_report, kRecordedAtEpochMs),
               PairingStatus::kExcessiveReportUncertainty);

  auto exact_uncertainty = JavaFixtureInputs();
  exact_uncertainty[0].report.trigger_timestamp_ns = 10'000'000;
  exact_uncertainty[0].report.trigger_uncertainty_ns = coordination::kMaximumReportUncertaintyNs;
  exact_uncertainty[0].clock = ReadyClock("dtl-node", 1'000'000, 0, 0, 0);
  exact_uncertainty[1].report.trigger_timestamp_ns = 20'000'000;
  exact_uncertainty[1].report.trigger_uncertainty_ns = coordination::kMaximumReportUncertaintyNs;
  exact_uncertainty[1].clock = ReadyClock("face-node", 11'000'000, 0, 0, 0);
  const PairingResult uncertainty_boundary =
      BuildPairedCoordinationRecord(exact_uncertainty, kRecordedAtEpochMs);
  ExpectStatus(uncertainty_boundary, PairingStatus::kPaired);
  assert(uncertainty_boundary.record->maximum_trigger_separation_ns ==
         coordination::kMaximumPairUncertaintyNs);

  auto exact_separation = JavaFixtureInputs();
  exact_separation[0].report.trigger_uncertainty_ns = 0;
  exact_separation[0].clock = ReadyClock("dtl-node", 1'000'000, 0, 0, 0);
  exact_separation[1].report.trigger_timestamp_ns = 70'000'000;
  exact_separation[1].report.trigger_uncertainty_ns = 0;
  exact_separation[1].clock = ReadyClock("face-node", 11'000'000, 0, 0, 0);
  const PairingResult separation_boundary =
      BuildPairedCoordinationRecord(exact_separation, kRecordedAtEpochMs);
  ExpectStatus(separation_boundary, PairingStatus::kPaired);
  assert(separation_boundary.record->maximum_trigger_separation_ns ==
         coordination::kMaximumTriggerSeparationNs);

  exact_separation[1].report.trigger_timestamp_ns += 1;
  ExpectStatus(BuildPairedCoordinationRecord(exact_separation, kRecordedAtEpochMs),
               PairingStatus::kExcessiveSeparation);
}

void TestSerializerRejectsInconsistentDerivedEvidence() {
  PairedCoordinationRecord record = PairJavaFixture().record.value();
  ++record.maximum_trigger_separation_ns;
  bool rejected = false;
  try {
    static_cast<void>(ToCanonicalJson(record));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

}  // namespace

int main() {
  TestIntersectsRepeatedClockBounds();
  TestRejectsInsufficientInvalidAndInconsistentClocks();
  TestBuildsCanonicalJavaCompatibleRecord();
  TestRejectsRoleIdentityAndClockFailures();
  TestEnforcesUncertaintyAndSeparationGates();
  TestSerializerRejectsInconsistentDerivedEvidence();
  return 0;
}
