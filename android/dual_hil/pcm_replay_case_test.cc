#include "android/dual_hil/pcm_replay_case.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include "capture/audio/pcm_wav.h"

namespace {

using swing_capture::EncodeMonoPcmS16Wav;
using swing_capture::android::dual_hil::ClassifyPcmReplayObservationDecision;
using swing_capture::android::dual_hil::ClassifyPcmReplayPairOutcome;
using swing_capture::android::dual_hil::ClassifyPcmReplayRun;
using swing_capture::android::dual_hil::ExtractPcmReplayCase;
using swing_capture::android::dual_hil::FindPcmReplayCase;
using swing_capture::android::dual_hil::ParsePcmReplayManifest;
using swing_capture::android::dual_hil::PcmReplayExpectation;
using swing_capture::android::dual_hil::PcmReplayObservation;
using swing_capture::android::dual_hil::PcmReplayObservationDecision;
using swing_capture::android::dual_hil::PcmReplayPairOutcome;
using swing_capture::android::dual_hil::PcmReplayPairOutcomeName;
using swing_capture::android::dual_hil::PcmReplayPeerImpactQuiescenceWindow;
using swing_capture::android::dual_hil::PcmReplayRunDisposition;
using swing_capture::android::dual_hil::ScorePcmReplayObservations;

constexpr std::string_view kManifest = R"({
  "schema_version": 1,
  "source_id": "synthetic-fixture",
  "source_sample_rate_hz": 48000,
  "cases": [
    {"name":"positive","expectation":"required_positive","start_frame":2,
     "sample_count":4,"marker_frame":1,"gain_permille":1000,"expected_crc32":1401342376},
    {"name":"hard-negative","expectation":"diagnostic_negative","start_frame":6,
     "sample_count":3,"marker_frame":1,"gain_permille":100,"expected_crc32":848687119}
  ]
})";

template <typename Action>
void ExpectFailure(Action action) {
  try {
    action();
  } catch (const std::exception &) {
    return;
  }
  assert(false);
}

void ExtractsExactNativeFramesAndStableCrc() {
  const auto manifest = ParsePcmReplayManifest(kManifest);
  const std::vector<std::int16_t> source = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  const std::vector<std::byte> wav = EncodeMonoPcmS16Wav(source, 48'000);
  const auto extracted =
      ExtractPcmReplayCase(manifest, FindPcmReplayCase(manifest, "positive"), wav);
  assert((extracted.samples == std::vector<std::int16_t>{2, 3, 4, 5}));
  assert(extracted.definition.marker_frame == 1U);
  assert(extracted.source_crc32 == 0x5386c9a8U);
  ExpectFailure([&] {
    static_cast<void>(ExtractPcmReplayCase(manifest, FindPcmReplayCase(manifest, "positive"),
                                           EncodeMonoPcmS16Wav(source, 32'000)));
  });
  std::vector<std::int16_t> wrong_source = source;
  wrong_source[3] = 99;
  ExpectFailure([&] {
    static_cast<void>(ExtractPcmReplayCase(manifest, FindPcmReplayCase(manifest, "positive"),
                                           EncodeMonoPcmS16Wav(wrong_source, 48'000)));
  });
}

void ScoresRequiredPositivesWithoutGreenwashingNegatives() {
  const auto manifest = ParsePcmReplayManifest(kManifest);
  const std::vector<PcmReplayObservation> observations = {
      {.case_name = "positive",
       .source_crc32 = 1401342376,
       .gain_permille = 1000,
       .leader_detected = true,
       .shadow_detected = true,
       .leader_source = "local_audio",
       .shadow_source = "peer_audio_clock_candidate"},
      {.case_name = "hard-negative",
       .source_crc32 = 848687119,
       .gain_permille = 100,
       .leader_detected = true,
       .shadow_detected = false,
       .leader_source = "local_audio",
       .shadow_source = {}},
  };
  const auto score = ScorePcmReplayObservations(manifest, observations);
  assert(!score.required_gate_passed);
  assert(score.required_positive_detected == 1U);
  assert(score.diagnostic_negative_detected == 1U);
  std::vector<PcmReplayObservation> correct_negative = observations;
  correct_negative.back().leader_detected = false;
  correct_negative.back().leader_source.clear();
  const auto qualified = ScorePcmReplayObservations(manifest, correct_negative);
  assert(qualified.required_gate_passed);
  assert(qualified.diagnostic_negative_detected == 0U);
  std::vector<PcmReplayObservation> leader_only_positive = correct_negative;
  leader_only_positive.front().shadow_detected = false;
  leader_only_positive.front().shadow_source.clear();
  assert(leader_only_positive.front().pair_outcome() == PcmReplayPairOutcome::kLeaderOnly);
  const auto leader_only_score = ScorePcmReplayObservations(manifest, leader_only_positive);
  assert(!leader_only_score.required_gate_passed);
  assert(leader_only_score.required_positive_detected == 0U);
  std::vector<PcmReplayObservation> shadow_only_positive = correct_negative;
  shadow_only_positive.front().leader_detected = false;
  shadow_only_positive.front().leader_source.clear();
  assert(shadow_only_positive.front().pair_outcome() == PcmReplayPairOutcome::kShadowOnly);
  const auto shadow_only_score = ScorePcmReplayObservations(manifest, shadow_only_positive);
  assert(!shadow_only_score.required_gate_passed);
  assert(shadow_only_score.required_positive_detected == 0U);
  const auto missing = ScorePcmReplayObservations(manifest, std::span(observations).subspan(1));
  assert(!missing.required_gate_passed);
  assert(missing.required_positive_count == 1U);
  assert(missing.required_positive_detected == 0U);
  ExpectFailure([&] {
    const std::vector<PcmReplayObservation> duplicate = {observations.front(),
                                                         observations.front()};
    static_cast<void>(ScorePcmReplayObservations(manifest, duplicate));
  });
  ExpectFailure([&] {
    std::vector<PcmReplayObservation> wrong_stimulus = observations;
    ++wrong_stimulus.front().source_crc32;
    static_cast<void>(ScorePcmReplayObservations(manifest, wrong_stimulus));
  });
  ExpectFailure([&] {
    std::vector<PcmReplayObservation> source_without_outcome = observations;
    source_without_outcome.front().leader_detected = false;
    static_cast<void>(ScorePcmReplayObservations(manifest, source_without_outcome));
  });
  ExpectFailure([&] {
    std::vector<PcmReplayObservation> wrong_role_source = observations;
    wrong_role_source.front().shadow_source = "local_audio";
    static_cast<void>(ScorePcmReplayObservations(manifest, wrong_role_source));
  });
}

void ClassifiesEveryPhysicalReplayOutcome() {
  assert(ClassifyPcmReplayPairOutcome(false, false) == PcmReplayPairOutcome::kNeither);
  assert(PcmReplayPairOutcomeName(ClassifyPcmReplayPairOutcome(false, false)) == "neither");
  assert(ClassifyPcmReplayPairOutcome(true, false) == PcmReplayPairOutcome::kLeaderOnly);
  assert(PcmReplayPairOutcomeName(ClassifyPcmReplayPairOutcome(true, false)) == "leader_only");
  assert(ClassifyPcmReplayPairOutcome(false, true) == PcmReplayPairOutcome::kShadowOnly);
  assert(PcmReplayPairOutcomeName(ClassifyPcmReplayPairOutcome(false, true)) == "shadow_only");
  assert(ClassifyPcmReplayPairOutcome(true, true) == PcmReplayPairOutcome::kBoth);
  assert(PcmReplayPairOutcomeName(ClassifyPcmReplayPairOutcome(true, true)) == "both");
  assert(ClassifyPcmReplayRun(PcmReplayExpectation::kRequiredPositive, true) ==
         PcmReplayRunDisposition::kRequiredPositiveDetected);
  assert(ClassifyPcmReplayRun(PcmReplayExpectation::kRequiredPositive, false) ==
         PcmReplayRunDisposition::kRequiredPositiveMissed);
  assert(ClassifyPcmReplayRun(PcmReplayExpectation::kDiagnosticNegative, false) ==
         PcmReplayRunDisposition::kExpectedDiagnosticNegative);
  assert(ClassifyPcmReplayRun(PcmReplayExpectation::kDiagnosticNegative, true) ==
         PcmReplayRunDisposition::kDiagnosticNegativeFalseTrigger);
}

void PreservesTheCompletePeerDeliveryBudgetBeforeSalvage() {
  using namespace std::chrono_literals;
  // HttpURLConnection applies the 1.5-second bound separately to connect and read. The peer's
  // 150ms local-candidate wait is enclosed by the read timeout, and a schema-400 fallback is an
  // immediate response rather than another timeout-length attempt.
  assert(PcmReplayPeerImpactQuiescenceWindow(1500ms, 1500ms, 3U, 50ms, 1s) == 10100ms);
  assert(PcmReplayPeerImpactQuiescenceWindow(1500ms, 1500ms, 1U, 50ms, 1s) == 4s);

  constexpr auto kInitialObservation = 750ms;
  constexpr auto kPartialQuiescence = 10100ms;
  assert(ClassifyPcmReplayObservationDecision(PcmReplayPairOutcome::kBoth, 0ms, std::nullopt,
                                              kInitialObservation, kPartialQuiescence) ==
         PcmReplayObservationDecision::kPairedTerminal);
  assert(ClassifyPcmReplayObservationDecision(PcmReplayPairOutcome::kNeither, 749ms, std::nullopt,
                                              kInitialObservation, kPartialQuiescence) ==
         PcmReplayObservationDecision::kContinue);
  assert(ClassifyPcmReplayObservationDecision(PcmReplayPairOutcome::kNeither, 750ms, std::nullopt,
                                              kInitialObservation, kPartialQuiescence) ==
         PcmReplayObservationDecision::kNeitherQuiescent);
  assert(ClassifyPcmReplayObservationDecision(PcmReplayPairOutcome::kLeaderOnly, 750ms, 10099ms,
                                              kInitialObservation, kPartialQuiescence) ==
         PcmReplayObservationDecision::kContinue);
  assert(ClassifyPcmReplayObservationDecision(PcmReplayPairOutcome::kLeaderOnly, 750ms, 10100ms,
                                              kInitialObservation, kPartialQuiescence) ==
         PcmReplayObservationDecision::kPartialQuiescent);
}

}  // namespace

int main() {
  ExtractsExactNativeFramesAndStableCrc();
  ScoresRequiredPositivesWithoutGreenwashingNegatives();
  ClassifiesEveryPhysicalReplayOutcome();
  PreservesTheCompletePeerDeliveryBudgetBeforeSalvage();
  return 0;
}
