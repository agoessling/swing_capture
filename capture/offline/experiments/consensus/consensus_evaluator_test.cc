#include "capture/offline/experiments/consensus/consensus_evaluator.h"

#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using swing_capture::offline::experiments::consensus::Candidate;
using swing_capture::offline::experiments::consensus::ConsensusMode;
using swing_capture::offline::experiments::consensus::ConsensusPolicy;
using swing_capture::offline::experiments::consensus::SelectClosestShadowCandidate;
using swing_capture::offline::experiments::consensus::SelectLatestShadowCandidate;
using swing_capture::offline::experiments::consensus::SelectTerminal;

void Check(bool condition, std::string_view diagnostic) {
  if (!condition) {
    throw std::runtime_error(std::string(diagnostic));
  }
}

Candidate Event(long strike, long confirmation, long peak = 20'000, long threshold = 10'000) {
  return {.strike_us = strike,
          .confirmation_us = confirmation,
          .peak_ppm = peak,
          .threshold_ppm = threshold};
}

void TestReadinessAndImmediateTerminal() {
  const auto result = SelectTerminal({Event(90, 95), Event(120, 125)}, {}, 100, 200,
                                     {.mode = ConsensusMode::kLeaderImmediate});
  Check(result.accepted, "immediate event accepted");
  Check(result.leader_candidate.strike_us == 120, "pre-readiness event is diagnostic only");
  Check(result.decision_us == 125, "immediate decision uses confirmation time");
}

void TestHardConsensusSkipsUnpairedCandidate() {
  const auto result =
      SelectTerminal({Event(120, 125), Event(180, 185)}, {Event(183, 190)}, 100, 300,
                     {.mode = ConsensusMode::kHard,
                      .peer_wait_us = 30,
                      .one_way_network_us = 5,
                      .pair_tolerance_us = 10});
  Check(result.accepted, "paired later event accepted");
  Check(result.leader_candidate.strike_us == 180, "unpaired event skipped");
  Check(result.peer_corroborated, "peer corroboration retained");
  Check(result.decision_us == 195, "network arrival included in decision time");
}

void TestSoftConsensusBoundedFallbackAndSupersession() {
  const auto fallback = SelectTerminal({Event(120, 125)}, {}, 100, 300,
                                       {.mode = ConsensusMode::kSoftBounded,
                                        .peer_wait_us = 40,
                                        .one_way_network_us = 5,
                                        .pair_tolerance_us = 10});
  Check(fallback.accepted && fallback.leader_candidate.strike_us == 120,
        "soft fallback preserves local recall");
  Check(fallback.decision_us == 165 && fallback.added_decision_latency_us == 40,
        "soft fallback charges bounded wait");

  const auto superseded =
      SelectTerminal({Event(120, 125), Event(150, 155)}, {Event(151, 158)}, 100, 300,
                     {.mode = ConsensusMode::kSoftBounded,
                      .peer_wait_us = 50,
                      .one_way_network_us = 5,
                      .pair_tolerance_us = 10});
  Check(superseded.leader_candidate.strike_us == 150,
        "corroborated candidate supersedes pending unpaired event");
  Check(superseded.decision_us == 163, "peer availability sets decision time");
}

void TestShadowCandidateRing() {
  const std::vector<Candidate> shadow = {Event(700, 705), Event(995, 1'075, 50'000, 10'000)};
  const Candidate leader = Event(1'000, 1'002);
  const auto current = SelectLatestShadowCandidate(shadow, leader, 1'025, 250);
  Check(!current.used_local_candidate, "future confirmation forces arrival fallback");
  const auto ring = SelectClosestShadowCandidate(shadow, leader, 1'025, 55, 80);
  Check(ring.used_local_candidate, "brief wait recovers delayed local evidence");
  Check(ring.selected_timestamp_us == 995, "ring retains accurate strike timestamp");
  Check(ring.decision_us == 1'075, "ring reports actual bounded decision time");
}

void TestValidation() {
  bool threw = false;
  try {
    static_cast<void>(SelectTerminal({Event(20, 30), Event(10, 15)}, {}, 0, 100,
                                     {.mode = ConsensusMode::kLeaderImmediate}));
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  Check(threw, "unsorted candidates rejected");
}

}  // namespace

int main() {
  TestReadinessAndImmediateTerminal();
  TestHardConsensusSkipsUnpairedCandidate();
  TestSoftConsensusBoundedFallbackAndSupersession();
  TestShadowCandidateRing();
  TestValidation();
  return EXIT_SUCCESS;
}
