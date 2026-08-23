#include "android/dual_hil/timing_calibration_evidence.h"

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace {

using Json = nlohmann::json;
using swing_capture::android::dual_hil::EvaluateTimingCalibrationEvidence;

constexpr std::string_view kDigest =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

const std::filesystem::path &ArtifactRoot() {
  static const std::filesystem::path root = [] {
    const char *temporary = std::getenv("TEST_TMPDIR");
    assert(temporary != nullptr);
    return std::filesystem::path(temporary) / "timing-artifacts";
  }();
  return root;
}

void WriteArtifact(std::string_view relative_path, std::string_view contents = "abc") {
  const std::filesystem::path path = ArtifactRoot() / std::string(relative_path);
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  assert(output);
  output << contents;
  assert(output.good());
}

void PrepareArtifacts() {
  WriteArtifact("physical/pair-reference.bin");
  WriteArtifact("physical/instrument-calibration.bin");
  WriteArtifact("physical/audio-reference.bin");
  for (std::size_t index = 0; index < 20U; ++index) {
    WriteArtifact("pair/archive-" + std::to_string(index) + ".zip");
  }
  for (std::size_t index = 0; index < 10U; ++index) {
    WriteArtifact("audio/trial-" + std::to_string(index) + ".bin");
  }
}

template <typename Action>
void ExpectFailure(Action action) {
  try {
    action();
  } catch (const std::exception &) {
    return;
  }
  assert(false);
}

Json PairClockEvidence() {
  Json trials = Json::array();
  for (std::size_t index = 0; index < 20U; ++index) {
    const bool accepted = index < 15U;
    const bool swapped = index % 2U != 0U;
    trials.push_back({
        {"trial_id", "clock-" + std::to_string(index)},
        {"archive_path", "pair/archive-" + std::to_string(index) + ".zip"},
        {"archive_sha256", std::string(kDigest)},
        {"physical_reference_artifact_path", "physical/pair-reference.bin"},
        {"physical_reference_artifact_sha256", std::string(kDigest)},
        {"network_label", index % 3U == 0U ? "studio" : "field"},
        {"leader_device_model", swapped ? "pixel5a" : "pixel6"},
        {"shadow_device_model", swapped ? "pixel6" : "pixel5a"},
        {"leader_role", "face_on"},
        {"shadow_role", "down_the_line"},
        {"screen_off_duration_seconds", index == 0U ? "1800" : "10"},
        {"network_contention", index % 2U == 0U},
        {"reviewed_mapping_expectation", accepted ? "accept" : "reject"},
        {"measured_alignment_error_ns", accepted ? "5000000" : "40000000"},
        {"alignment_measurement_uncertainty_ns", "1000000"},
        {"mapping_uncertainty_ns", accepted ? "20000000" : "30000000"},
        {"mapping_age_ns", "5000000000"},
        {"minimum_round_trip_ns", "1000000"},
        {"maximum_round_trip_ns", "4000000"},
        {"selected_candidate_residual_ns", accepted ? Json("2000000") : Json(nullptr)},
        {"maximum_trigger_separation_ns", accepted ? Json("10000000") : Json(nullptr)},
    });
  }
  return {
      {"schema_version", 1},
      {"evidence_kind", "pair_clock_threshold"},
      {"candidate_threshold_ns", "25000000"},
      {"maximum_acceptable_alignment_error_ns", "10000000"},
      {"reference",
       {{"instrument_id", "pair-reference-1"},
        {"method", "simultaneous_pair_alignment_reference"},
        {"calibration_record_path", "physical/instrument-calibration.bin"},
        {"calibration_record_sha256", std::string(kDigest)}}},
      {"review",
       {{"state", "approved"},
        {"reviewer", "reviewer-a"},
        {"reviewed_at_utc", "2026-08-23T00:00:00Z"},
        {"reference_setup_verified", true}}},
      {"trials", std::move(trials)},
  };
}

Json AudioLatencyEvidence(std::string_view scope) {
  Json trials = Json::array();
  for (std::size_t index = 0; index < 10U; ++index) {
    const bool pixel6 = index < 5U;
    const bool far = index % 2U != 0U;
    const std::int64_t distance = far ? 1600 : 1000;
    const std::int64_t propagation = (distance * 1'000'000'000LL + 343'000 / 2) / 343'000;
    const std::int64_t reference_event =
        1'000'000'000LL + static_cast<std::int64_t>(index) * 100'000'000LL;
    constexpr std::int64_t kDeviceLatency = 5'000'000LL;
    constexpr std::int64_t kClockOffset = 1'000LL;
    const std::int64_t phone_event = reference_event + propagation + kDeviceLatency + kClockOffset;
    trials.push_back({
        {"trial_id", "latency-" + std::to_string(index)},
        {"raw_trial_artifact_path", "audio/trial-" + std::to_string(index) + ".bin"},
        {"raw_trial_artifact_sha256", std::string(kDigest)},
        {"node_id", pixel6 ? "pixel6-node" : "pixel5a-node"},
        {"device_model", pixel6 ? "pixel6" : "pixel5a"},
        {"role", index % 2U == 0U ? "face_on" : "down_the_line"},
        {"microphone_distance_mm", std::to_string(distance)},
        {"distance_uncertainty_mm", "5"},
        {"speed_of_sound_mm_per_second", "343000"},
        {"speed_of_sound_uncertainty_mm_per_second", "1000"},
        {"reference_event_ns", std::to_string(reference_event)},
        {"reference_uncertainty_ns", "100000"},
        {"phone_audio_event_ns", std::to_string(phone_event)},
        {"phone_audio_uncertainty_ns", "200000"},
        {"phone_minus_reference_clock_offset_ns", std::to_string(kClockOffset)},
        {"clock_mapping_uncertainty_ns", "300000"},
    });
  }
  return {
      {"schema_version", 1},
      {"evidence_kind", "absolute_audio_latency"},
      {"calibration_id", "calibration-a"},
      {"scope", scope},
      {"reference",
       {{"instrument_id", "reference-1"},
        {"method", scope == "instrumented_ball_contact" ? "instrumented_ball_contact_sensor"
                                                        : "physical_acoustic_emission_sensor"},
        {"calibration_record_path", "physical/instrument-calibration.bin"},
        {"calibration_record_sha256", std::string(kDigest)},
        {"raw_reference_artifact_path", "physical/audio-reference.bin"},
        {"raw_reference_artifact_sha256", std::string(kDigest)}}},
      {"review",
       {{"state", "approved"},
        {"reviewer", "reviewer-a"},
        {"reviewed_at_utc", "2026-08-23T00:00:00Z"},
        {"reference_setup_verified", true}}},
      {"trials", std::move(trials)},
  };
}

void ScoresPairClockPolicyOnlyWhenCoverageAndLabelsSupportIt() {
  const Json report = EvaluateTimingCalibrationEvidence(PairClockEvidence().dump(), ArtifactRoot());
  assert(report.at("coverage_complete") == true);
  assert(report.at("threshold_selection_eligible") == true);
  assert(report.at("false_accept_count") == 0U);
  assert(report.at("false_reject_count") == 0U);
  assert(report.at("physical_truth_inferred") == false);

  Json false_accept = PairClockEvidence();
  false_accept["trials"][0]["measured_alignment_error_ns"] = "40000000";
  false_accept["trials"][0]["reviewed_mapping_expectation"] = "reject";
  const Json rejected = EvaluateTimingCalibrationEvidence(false_accept.dump(), ArtifactRoot());
  assert(rejected.at("false_accept_count") == 1U);
  assert(rejected.at("threshold_selection_eligible") == false);

  Json pending = PairClockEvidence();
  pending["review"]["state"] = "needs_review";
  const Json pending_report = EvaluateTimingCalibrationEvidence(pending.dump(), ArtifactRoot());
  assert(pending_report.at("provenance_review_complete") == false);
  assert(pending_report.at("threshold_selection_eligible") == false);
}

void SeparatesComponentCalibrationFromBallImpactCalibration() {
  const Json component = EvaluateTimingCalibrationEvidence(
      AudioLatencyEvidence("source_acoustic_emission").dump(), ArtifactRoot());
  assert(component.at("coverage_complete") == true);
  assert(component.at("component_calibration_eligible") == true);
  assert(component.at("absolute_ball_impact_claim_eligible") == false);
  assert(component.at("physical_truth_inferred") == false);
  for (const Json &device : component.at("device_summaries")) {
    assert(device.at("median_derived_latency_ns") == "5000000");
  }

  const Json ball = EvaluateTimingCalibrationEvidence(
      AudioLatencyEvidence("instrumented_ball_contact").dump(), ArtifactRoot());
  assert(ball.at("absolute_ball_impact_claim_eligible") == true);
  assert(ball.at("physical_truth_inferred") == false);
}

void RejectsUnpinnedOrPhysicallyImpossibleEvidence() {
  Json unpinned = AudioLatencyEvidence("source_acoustic_emission");
  unpinned["trials"][0]["raw_trial_artifact_sha256"] = "not-a-digest";
  ExpectFailure([&] {
    static_cast<void>(EvaluateTimingCalibrationEvidence(unpinned.dump(), ArtifactRoot()));
  });

  Json impossible = AudioLatencyEvidence("source_acoustic_emission");
  impossible["trials"][0]["phone_audio_event_ns"] = "1";
  ExpectFailure([&] {
    static_cast<void>(EvaluateTimingCalibrationEvidence(impossible.dump(), ArtifactRoot()));
  });

  for (const std::string_view noncanonical : {"-01", "-0", "+1", "01"}) {
    Json bad_decimal = AudioLatencyEvidence("source_acoustic_emission");
    bad_decimal["trials"][0]["phone_minus_reference_clock_offset_ns"] = noncanonical;
    ExpectFailure([&] {
      static_cast<void>(EvaluateTimingCalibrationEvidence(bad_decimal.dump(), ArtifactRoot()));
    });
  }

  Json escaped = PairClockEvidence();
  escaped["trials"][0]["archive_path"] = "../outside.zip";
  ExpectFailure([&] {
    static_cast<void>(EvaluateTimingCalibrationEvidence(escaped.dump(), ArtifactRoot()));
  });

  Json missing = PairClockEvidence();
  missing["trials"][0]["archive_path"] = "pair/missing.zip";
  ExpectFailure([&] {
    static_cast<void>(EvaluateTimingCalibrationEvidence(missing.dump(), ArtifactRoot()));
  });

  Json bad_review = AudioLatencyEvidence("source_acoustic_emission");
  bad_review["review"]["state"] = "appproved";
  ExpectFailure([&] {
    static_cast<void>(EvaluateTimingCalibrationEvidence(bad_review.dump(), ArtifactRoot()));
  });
  bad_review = AudioLatencyEvidence("source_acoustic_emission");
  bad_review["review"]["reviewed_at_utc"] = "2026-02-30T00:00:00Z";
  ExpectFailure([&] {
    static_cast<void>(EvaluateTimingCalibrationEvidence(bad_review.dump(), ArtifactRoot()));
  });

  WriteArtifact("pair/archive-0.zip", "tampered");
  ExpectFailure([&] {
    static_cast<void>(
        EvaluateTimingCalibrationEvidence(PairClockEvidence().dump(), ArtifactRoot()));
  });
}

}  // namespace

int main() {
  PrepareArtifacts();
  ScoresPairClockPolicyOnlyWhenCoverageAndLabelsSupportIt();
  SeparatesComponentCalibrationFromBallImpactCalibration();
  RejectsUnpinnedOrPhysicallyImpossibleEvidence();
  return 0;
}
