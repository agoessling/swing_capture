#ifndef SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_CONSENSUS_CONSENSUS_EVALUATOR_H_
#define SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_CONSENSUS_CONSENSUS_EVALUATOR_H_

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "capture/audio/pcm_wav.h"

namespace swing_capture::offline::experiments::consensus {

struct Candidate {
  std::int64_t strike_us = 0;
  std::int64_t confirmation_us = 0;
  std::int64_t peak_ppm = 0;
  std::int64_t threshold_ppm = 0;
};

enum class ConsensusMode {
  kLeaderImmediate,
  kHard,
  kSoftBounded,
};

struct ConsensusPolicy {
  ConsensusMode mode = ConsensusMode::kLeaderImmediate;
  std::int64_t peer_wait_us = 0;
  std::int64_t one_way_network_us = 0;
  std::int64_t pair_tolerance_us = 80'000;
};

struct TerminalSelection {
  bool accepted = false;
  Candidate leader_candidate;
  std::int64_t decision_us = 0;
  std::int64_t added_decision_latency_us = 0;
  bool peer_corroborated = false;
};

struct ShadowSelection {
  bool used_local_candidate = false;
  Candidate candidate;
  std::int64_t selected_timestamp_us = 0;
  std::int64_t decision_us = 0;
  std::string source;
};

// Selects the online terminal event. All timestamps must already be mapped to
// one shared axis. Hard consensus skips an unpaired leader candidate. Soft
// consensus gives a paired candidate until the first candidate's bounded
// deadline to supersede it, then accepts the original candidate.
[[nodiscard]] TerminalSelection SelectTerminal(const std::vector<Candidate> &leader,
                                               const std::vector<Candidate> &peer,
                                               std::int64_t ready_us, std::int64_t end_us,
                                               ConsensusPolicy policy);

// Reproduces the current latest-within-250-ms shadow policy.
[[nodiscard]] ShadowSelection SelectLatestShadowCandidate(const std::vector<Candidate> &shadow,
                                                          const Candidate &leader_candidate,
                                                          std::int64_t request_arrival_us,
                                                          std::int64_t maximum_age_us = 250'000);

// Uses the leader strike timestamp carried in the request, a bounded local
// wait, and a candidate ring. The closest candidate wins; normalized strength
// breaks an exact timing tie.
[[nodiscard]] ShadowSelection SelectClosestShadowCandidate(const std::vector<Candidate> &shadow,
                                                           const Candidate &leader_candidate,
                                                           std::int64_t request_arrival_us,
                                                           std::int64_t post_arrival_wait_us,
                                                           std::int64_t pair_tolerance_us);

struct FieldInputs {
  DecodedMonoPcmS16Wav down_the_line_audio;
  DecodedMonoPcmS16Wav face_on_audio;
  nlohmann::json target_index;
  nlohmann::json face_on_session;
  nlohmann::json production_candidates;
  nlohmann::json face_on_envelope;
  std::string face_on_observations_csv;
};

// Runs the frozen field benchmark, including detector resets for every dynamic
// arm in complete lifecycle replay.
[[nodiscard]] nlohmann::json AnalyzeFieldConsensus(const FieldInputs &inputs);

}  // namespace swing_capture::offline::experiments::consensus

#endif  // SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_CONSENSUS_CONSENSUS_EVALUATOR_H_
