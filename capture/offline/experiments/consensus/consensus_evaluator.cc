#include "capture/offline/experiments/consensus/consensus_evaluator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/offline/experiments/benchmark/terminal_window_benchmark.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture::offline::experiments::consensus {
namespace {

// NOLINTBEGIN(misc-include-cleaner)

constexpr std::int64_t kFaceAudioMinusVideoUs = 82'393;
constexpr std::int64_t kFaceAudioMinusDownAudioUs = 76'917;
constexpr std::int64_t kVideoStartupUs = 800'000;
constexpr std::int64_t kAudioStartupUs = 2'450'000;
constexpr std::int64_t kPostImpactUs = 1'000'000;
constexpr std::int64_t kRestartUs = 800'000;
constexpr std::int64_t kMaximumArmedUs = 15'000'000;
constexpr std::int64_t kCooldownUs = 2'000'000;
constexpr std::int64_t kRecordingDurationUs = 247'000'000;
constexpr std::int64_t kTargetToleranceUs = 100'000;
constexpr std::int64_t kRetainedHistoryCapacityUs = 3'000'000;

struct Observation {
  std::int64_t timestamp_us;
  double person;
  double address;
  double motion;
};

struct Target {
  std::string id;
  std::int64_t impact_video_us;
  std::int64_t impact_face_audio_us;
  std::int64_t takeaway_video_us;
};

std::int64_t DecimalString(const nlohmann::json &value) {
  const std::string text = value.get<std::string>();
  if (text.empty() || !std::ranges::all_of(text, [](char character) {
        return character >= '0' && character <= '9';
      })) {
    throw std::invalid_argument("expected an unsigned decimal string");
  }
  std::size_t consumed = 0;
  const std::int64_t parsed = std::stoll(text, &consumed);
  if (consumed != text.size()) {
    throw std::invalid_argument("decimal string is outside the supported range");
  }
  return parsed;
}

std::int64_t RoundedMs(double milliseconds) { return std::llround(milliseconds * 1'000.0); }

double CandidateStrength(const Candidate &candidate) {
  return candidate.threshold_ppm == 0 ? 0.0
                                      : static_cast<double>(candidate.peak_ppm) /
                                            static_cast<double>(candidate.threshold_ppm);
}

void ValidateCandidates(const std::vector<Candidate> &candidates) {
  if (!std::ranges::is_sorted(candidates, {}, &Candidate::confirmation_us)) {
    throw std::invalid_argument("candidates must be sorted by confirmation time");
  }
  for (const Candidate &candidate : candidates) {
    if (candidate.strike_us < 0 || candidate.confirmation_us < candidate.strike_us ||
        candidate.peak_ppm < 0 || candidate.threshold_ppm < 0) {
      throw std::invalid_argument("candidate timing or amplitude is invalid");
    }
  }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::optional<Candidate> BestPeerMatch(const std::vector<Candidate> &peer,
                                       const Candidate &leader_candidate,
                                       std::int64_t available_by_us, std::int64_t tolerance_us) {
  std::optional<Candidate> best;
  for (const Candidate &candidate : peer) {
    if (candidate.confirmation_us > available_by_us) {
      break;
    }
    if (std::abs(candidate.strike_us - leader_candidate.strike_us) > tolerance_us) {
      continue;
    }
    if (!best.has_value() || std::tuple(std::abs(candidate.strike_us - leader_candidate.strike_us),
                                        -CandidateStrength(candidate), candidate.confirmation_us) <
                                 std::tuple(std::abs(best->strike_us - leader_candidate.strike_us),
                                            -CandidateStrength(*best), best->confirmation_us)) {
      best = candidate;
    }
  }
  return best;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

std::int64_t PeerAvailableAtLeader(const Candidate &candidate, const ConsensusPolicy &policy) {
  if (candidate.confirmation_us >
      std::numeric_limits<std::int64_t>::max() - policy.one_way_network_us) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return candidate.confirmation_us + policy.one_way_network_us;
}

std::optional<std::pair<Candidate, std::int64_t>> AvailablePeerMatch(
    const std::vector<Candidate> &peer, const Candidate &leader_candidate, std::int64_t deadline_us,
    const ConsensusPolicy &policy) {
  std::optional<std::pair<Candidate, std::int64_t>> best;
  for (const Candidate &candidate : peer) {
    const std::int64_t available_us = PeerAvailableAtLeader(candidate, policy);
    if (available_us > deadline_us) {
      continue;
    }
    if (std::abs(candidate.strike_us - leader_candidate.strike_us) > policy.pair_tolerance_us) {
      continue;
    }
    if (!best.has_value() ||
        std::tuple(std::abs(candidate.strike_us - leader_candidate.strike_us), available_us,
                   -CandidateStrength(candidate)) <
            std::tuple(std::abs(best->first.strike_us - leader_candidate.strike_us), best->second,
                       -CandidateStrength(best->first))) {
      best = std::pair(candidate, available_us);
    }
  }
  return best;
}

std::string ModeName(ConsensusMode mode) {
  switch (mode) {
    case ConsensusMode::kLeaderImmediate:
      return "leader_immediate";
    case ConsensusMode::kHard:
      return "hard_consensus";
    case ConsensusMode::kSoftBounded:
      return "soft_bounded";
  }
  throw std::logic_error("unknown consensus mode");
}

std::int64_t Ppm(float normalized) {
  return std::llround(static_cast<double>(normalized) * 1'000'000.0);
}

ImpactDetectorConfig DetectorConfig(bool face_on) {
  return {
      .threshold_multiplier = 8.0F,
      .minimum_peak_amplitude = face_on ? 0.015F : 0.010F,
      .initial_noise_floor = 0.005F,
      .noise_update_clip_multiplier = 4.0F,
      .noise_floor_time_constant_seconds = 0.5,
      .peak_confirmation_seconds = 0.0015,
      .refractory_period_seconds = 0.25,
  };
}

std::size_t FrameAtOrAfter(std::int64_t time_us, std::uint32_t sample_rate_hz) {
  const auto numerator = static_cast<std::uint64_t>(time_us) * sample_rate_hz;
  return static_cast<std::size_t>((numerator + 999'999) / 1'000'000);
}

std::size_t FrameAtOrBefore(std::int64_t time_us, std::uint32_t sample_rate_hz) {
  return static_cast<std::size_t>(static_cast<std::uint64_t>(time_us) * sample_rate_hz / 1'000'000);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::vector<Candidate> DetectArmedWindow(const DecodedMonoPcmS16Wav &audio,
                                         ImpactDetectorConfig config, std::int64_t local_start_us,
                                         std::int64_t local_end_us,
                                         std::int64_t local_to_shared_us) {
  if (audio.sample_rate_hz == 0 || audio.samples.empty() || local_start_us < 0 ||
      local_end_us <= local_start_us) {
    throw std::invalid_argument("invalid armed audio window");
  }
  const std::size_t start_frame = FrameAtOrAfter(local_start_us, audio.sample_rate_hz);
  const std::size_t end_frame =
      std::min(audio.samples.size(), FrameAtOrBefore(local_end_us, audio.sample_rate_hz));
  if (start_frame >= end_frame) {
    return {};
  }

  ImpactDetector detector(config);
  constexpr std::size_t kBlockFrames = 4096;
  std::array<ImpactEvent, 32> events;
  std::vector<Candidate> candidates;
  const auto origin = std::chrono::steady_clock::time_point(std::chrono::seconds(1));
  for (std::size_t offset = start_frame; offset < end_frame; offset += kBlockFrames) {
    const std::size_t count = std::min(kBlockFrames, end_frame - offset);
    const auto block_start =
        origin +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(offset) / audio.sample_rate_hz));
    const auto result = detector.ProcessBlock(std::span(audio.samples).subspan(offset, count),
                                              block_start, audio.sample_rate_hz, events);
    if (result.events_dropped() != 0) {
      throw std::runtime_error("impact event buffer overflowed");
    }
    for (std::size_t index = 0; index < result.events_written; ++index) {
      const ImpactEvent &event = events[index];
      candidates.push_back(
          {.strike_us =
               std::chrono::duration_cast<std::chrono::microseconds>(event.strike_time - origin)
                   .count() +
               local_to_shared_us,
           .confirmation_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  event.confirmation_time - origin)
                                  .count() +
                              local_to_shared_us,
           .peak_ppm = Ppm(event.peak_amplitude),
           .threshold_ppm = Ppm(event.threshold_at_detection)});
    }
  }
  return candidates;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

std::vector<Observation> ParseObservations(std::string_view csv) {
  std::istringstream input{std::string(csv)};
  std::string line;
  if (!std::getline(input, line) || line !=
                                        "timestamp_us,person_confidence,address_confidence,motion_"
                                        "magnitude,inside_hitting_region") {
    throw std::invalid_argument("unexpected pose observation CSV header");
  }
  std::vector<Observation> observations;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream row(line);
    std::array<std::string, 5> columns;
    for (std::size_t index = 0; index < columns.size(); ++index) {
      if (!std::getline(row, columns[index], index + 1 == columns.size() ? '\n' : ',')) {
        throw std::invalid_argument("malformed pose observation row");
      }
    }
    observations.push_back({.timestamp_us = std::stoll(columns[0]),
                            .person = std::stod(columns[1]),
                            .address = std::stod(columns[2]),
                            .motion = std::stod(columns[3])});
  }
  if (observations.empty() ||
      !std::ranges::is_sorted(observations, {}, &Observation::timestamp_us)) {
    throw std::invalid_argument("pose observations must be present and sorted");
  }
  return observations;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<Target> ParseTargets(const nlohmann::json &target_index,
                                 const nlohmann::json &session) {
  const auto &face_events = target_index.at("views").at("face_on").at("events");
  const auto &session_targets = session.at("targets");
  if (face_events.size() != session_targets.size()) {
    throw std::invalid_argument("target index and pose session disagree");
  }
  std::vector<Target> targets;
  targets.reserve(face_events.size());
  for (std::size_t index = 0; index < face_events.size(); ++index) {
    const auto &event = face_events[index];
    const auto &session_target = session_targets[index];
    const std::string id = event.at("id").get<std::string>();
    if (session_target.at("id") != id) {
      throw std::invalid_argument("target identifiers disagree");
    }
    const std::int64_t video_ready_us =
        DecimalString(session_target.at("arm_ns")) / 1'000 + kVideoStartupUs;
    const std::int64_t takeaway_us =
        video_ready_us + DecimalString(session_target.at("video_lead_before_takeaway_ns")) / 1'000;
    targets.push_back({.id = id,
                       .impact_video_us = RoundedMs(event.at("video_impact_ms").get<double>()),
                       .impact_face_audio_us = RoundedMs(event.at("wav_peak_ms").get<double>()),
                       .takeaway_video_us = takeaway_us});
  }
  return targets;
}

class PoseController {
 public:
  bool Observe(const Observation &observation) {
    if (observation.timestamp_us <= last_timestamp_us_) {
      throw std::invalid_argument("pose timestamps must increase");
    }
    const std::int64_t previous_us = last_timestamp_us_;
    last_timestamp_us_ = observation.timestamp_us;
    switch (state_) {
      case State::kWatching:
        if (Qualifies(observation)) {
          state_ = State::kQualifying;
          qualification_started_us_ = observation.timestamp_us;
          last_qualified_us_ = observation.timestamp_us;
        }
        return false;
      case State::kQualifying:
        if (previous_us >= 0 && observation.timestamp_us - previous_us > 450'000) {
          if (Qualifies(observation)) {
            qualification_started_us_ = observation.timestamp_us;
            last_qualified_us_ = observation.timestamp_us;
          } else {
            ClearQualification();
          }
          return false;
        }
        if (Qualifies(observation)) {
          last_qualified_us_ = observation.timestamp_us;
          if (observation.timestamp_us - qualification_started_us_ >= 400'000) {
            state_ = State::kArmed;
            arm_requested_us_ = observation.timestamp_us;
            qualification_started_us_ = -1;
            last_qualified_us_ = -1;
            return true;
          }
          return false;
        }
        if (observation.timestamp_us - last_qualified_us_ > 250'000) {
          ClearQualification();
        }
        return false;
      case State::kArmed:
        throw std::logic_error("pose observations must be blind during high-speed capture");
      case State::kWaitingForClear:
        reset_observed_ = reset_observed_ || ResetEvidence(observation);
        if (reset_observed_ && observation.timestamp_us >= cooldown_until_us_) {
          state_ = State::kWatching;
          arm_requested_us_ = -1;
          cooldown_until_us_ = -1;
          reset_observed_ = false;
        }
        return false;
    }
    throw std::logic_error("unknown pose controller state");
  }

  void CaptureEnded(std::int64_t timestamp_us) {
    if (state_ != State::kArmed || timestamp_us <= last_timestamp_us_) {
      throw std::logic_error("invalid capture completion transition");
    }
    last_timestamp_us_ = timestamp_us;
    state_ = State::kWaitingForClear;
    cooldown_until_us_ = timestamp_us + kCooldownUs;
    reset_observed_ = false;
  }

  [[nodiscard]] std::int64_t arm_requested_us() const { return arm_requested_us_; }

 private:
  enum class State { kWatching, kQualifying, kArmed, kWaitingForClear };

  static bool Engaged(const Observation &observation) { return observation.person >= 0.55; }
  static bool Qualifies(const Observation &observation) {
    return Engaged(observation) && observation.address >= 0.45 && observation.motion <= 0.45;
  }
  static bool ResetEvidence(const Observation &observation) {
    return observation.person <= 0.25 || observation.address < 0.45 || observation.motion > 0.45;
  }
  void ClearQualification() {
    state_ = State::kWatching;
    qualification_started_us_ = -1;
    last_qualified_us_ = -1;
  }

  State state_ = State::kWatching;
  std::int64_t last_timestamp_us_ = -1;
  std::int64_t qualification_started_us_ = -1;
  std::int64_t last_qualified_us_ = -1;
  std::int64_t arm_requested_us_ = -1;
  std::int64_t cooldown_until_us_ = -1;
  bool reset_observed_ = false;
};

struct AttemptResult {
  std::int64_t arm_video_us;
  std::int64_t ready_video_us;
  std::int64_t terminal_strike_video_us;
  std::int64_t decision_video_us;
  std::int64_t completion_video_us;
  std::string outcome;
  std::string target_id;
  std::int64_t terminal_error_us;
  std::int64_t added_latency_us;
  bool peer_corroborated;
};

struct LifecycleResult {
  std::vector<AttemptResult> attempts;
  std::vector<std::string> captured_targets;
  std::int64_t total_high_speed_us = 0;
  std::int64_t minimum_video_lead_us = std::numeric_limits<std::int64_t>::max();
  std::int64_t minimum_retained_history_us = std::numeric_limits<std::int64_t>::max();
  std::size_t skipped_observations = 0;
};

struct DetectorCache {
  std::map<std::int64_t, std::vector<Candidate>> face;
  std::map<std::int64_t, std::vector<Candidate>> down;
};

std::optional<Target> MatchingTarget(const std::vector<Target> &targets,
                                     std::int64_t strike_face_audio_us) {
  std::optional<Target> best;
  for (const Target &target : targets) {
    if (std::abs(target.impact_face_audio_us - strike_face_audio_us) > kTargetToleranceUs) {
      continue;
    }
    if (!best.has_value() || std::abs(target.impact_face_audio_us - strike_face_audio_us) <
                                 std::abs(best->impact_face_audio_us - strike_face_audio_us)) {
      best = target;
    }
  }
  return best;
}

// This event loop intentionally mirrors the production lifecycle ordering.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LifecycleResult ReplayLifecycle(const FieldInputs &inputs,
                                const std::vector<Observation> &observations,
                                const std::vector<Target> &targets, bool face_is_audio_leader,
                                ConsensusPolicy policy, DetectorCache &cache,
                                const std::vector<Candidate> *leader_override = nullptr) {
  enum class Mode { kStandby, kWaiting, kCompleting, kRestarting };
  PoseController controller;
  LifecycleResult result;
  Mode mode = Mode::kStandby;
  std::size_t observation_index = 0;
  std::size_t target_index = 0;
  std::int64_t standby_after_us = -1;
  std::int64_t no_impact_us = std::numeric_limits<std::int64_t>::max();
  std::int64_t completion_us = std::numeric_limits<std::int64_t>::max();
  std::int64_t restart_us = std::numeric_limits<std::int64_t>::max();
  AttemptResult current{};
  std::vector<Target> current_targets;

  auto start_attempt = [&](std::int64_t arm_video_us) {
    current = {.arm_video_us = arm_video_us,
               .ready_video_us = arm_video_us + kAudioStartupUs,
               .terminal_strike_video_us = -1,
               .decision_video_us = -1,
               .completion_video_us = -1,
               .outcome = "",
               .target_id = "",
               .terminal_error_us = 0,
               .added_latency_us = 0,
               .peer_corroborated = false};
    current_targets.clear();
    no_impact_us = arm_video_us + kMaximumArmedUs;
    const std::int64_t face_start_us = arm_video_us + kFaceAudioMinusVideoUs;
    const std::int64_t face_end_us = no_impact_us + kFaceAudioMinusVideoUs;
    const std::int64_t down_start_us = face_start_us - kFaceAudioMinusDownAudioUs;
    const std::int64_t down_end_us = face_end_us - kFaceAudioMinusDownAudioUs;
    const auto face_inserted = cache.face.try_emplace(face_start_us);
    if (face_inserted.second) {
      face_inserted.first->second = DetectArmedWindow(inputs.face_on_audio, DetectorConfig(true),
                                                      face_start_us, face_end_us, 0);
    }
    const auto down_inserted = cache.down.try_emplace(down_start_us);
    if (down_inserted.second) {
      down_inserted.first->second =
          DetectArmedWindow(inputs.down_the_line_audio, DetectorConfig(false), down_start_us,
                            down_end_us, kFaceAudioMinusDownAudioUs);
    }
    const std::vector<Candidate> &face = face_inserted.first->second;
    const std::vector<Candidate> &down = down_inserted.first->second;
    std::vector<Candidate> overridden_leader;
    if (leader_override != nullptr) {
      std::ranges::copy_if(*leader_override, std::back_inserter(overridden_leader),
                           [face_start_us, face_end_us](const Candidate &candidate) {
                             return candidate.strike_us >= face_start_us &&
                                    candidate.strike_us < face_end_us;
                           });
    }
    const std::vector<Candidate> *leader = &face;
    if (leader_override != nullptr) {
      leader = &overridden_leader;
    } else if (!face_is_audio_leader) {
      leader = &down;
    }
    const auto &peer = face_is_audio_leader ? down : face;
    const TerminalSelection terminal =
        SelectTerminal(*leader, peer, current.ready_video_us + kFaceAudioMinusVideoUs,
                       no_impact_us + kFaceAudioMinusVideoUs, policy);
    if (terminal.accepted) {
      current.terminal_strike_video_us =
          terminal.leader_candidate.strike_us - kFaceAudioMinusVideoUs;
      current.decision_video_us = terminal.decision_us - kFaceAudioMinusVideoUs;
      current.added_latency_us = terminal.added_decision_latency_us;
      current.peer_corroborated = terminal.peer_corroborated;
    }
    mode = Mode::kWaiting;
  };

  while (observation_index < observations.size() || target_index < targets.size() ||
         mode != Mode::kStandby) {
    const std::int64_t observation_us = observation_index < observations.size()
                                            ? observations[observation_index].timestamp_us
                                            : std::numeric_limits<std::int64_t>::max();
    const std::int64_t target_us = target_index < targets.size()
                                       ? targets[target_index].impact_video_us
                                       : std::numeric_limits<std::int64_t>::max();
    const std::int64_t terminal_us = mode == Mode::kWaiting && current.decision_video_us >= 0
                                         ? current.decision_video_us
                                         : std::numeric_limits<std::int64_t>::max();

    if (mode == Mode::kWaiting && no_impact_us <= observation_us && no_impact_us <= target_us &&
        no_impact_us <= terminal_us) {
      current.decision_video_us = no_impact_us;
      current.terminal_strike_video_us = no_impact_us;
      current.outcome = "no_impact_timeout";
      completion_us = no_impact_us + kPostImpactUs;
      mode = Mode::kCompleting;
      continue;
    }
    if (mode == Mode::kWaiting && terminal_us <= observation_us && terminal_us <= target_us) {
      const auto matched =
          MatchingTarget(targets, current.terminal_strike_video_us + kFaceAudioMinusVideoUs);
      current.outcome = matched.has_value() ? "target_terminal" : "false_terminal";
      if (matched.has_value()) {
        current.target_id = matched->id;
        current.terminal_error_us = current.terminal_strike_video_us - matched->impact_video_us;
      }
      completion_us = current.decision_video_us + kPostImpactUs;
      mode = Mode::kCompleting;
      continue;
    }
    if (mode == Mode::kCompleting && completion_us <= observation_us &&
        completion_us <= target_us) {
      const std::int64_t retained_start_us =
          std::max(current.arm_video_us + kVideoStartupUs,
                   current.terminal_strike_video_us - kRetainedHistoryCapacityUs);
      for (const Target &target : current_targets) {
        if (target.impact_video_us < retained_start_us || target.impact_video_us > completion_us ||
            current.arm_video_us + kVideoStartupUs > target.takeaway_video_us ||
            std::ranges::find(result.captured_targets, target.id) !=
                result.captured_targets.end()) {
          continue;
        }
        result.captured_targets.push_back(target.id);
        result.minimum_video_lead_us =
            std::min(result.minimum_video_lead_us,
                     target.takeaway_video_us - (current.arm_video_us + kVideoStartupUs));
        result.minimum_retained_history_us = std::min(result.minimum_retained_history_us,
                                                      target.impact_video_us - retained_start_us);
      }
      controller.CaptureEnded(completion_us);
      current.completion_video_us = completion_us;
      result.total_high_speed_us += completion_us - current.arm_video_us;
      result.attempts.push_back(current);
      restart_us = completion_us + kRestartUs;
      standby_after_us = restart_us;
      mode = Mode::kRestarting;
      continue;
    }
    if (mode == Mode::kRestarting && restart_us <= observation_us && restart_us <= target_us) {
      mode = Mode::kStandby;
      continue;
    }
    if (target_us <= observation_us) {
      const Target &target = targets[target_index++];
      const bool high_speed_retaining = mode == Mode::kWaiting || mode == Mode::kCompleting;
      if (high_speed_retaining && current.ready_video_us <= target.impact_video_us) {
        current_targets.push_back(target);
      }
      continue;
    }
    if (observation_index >= observations.size()) {
      break;
    }
    const Observation &observation = observations[observation_index++];
    if (mode != Mode::kStandby || observation.timestamp_us <= standby_after_us) {
      ++result.skipped_observations;
      continue;
    }
    if (controller.Observe(observation)) {
      start_attempt(controller.arm_requested_us());
    }
  }
  return result;
}

// The nested attribution loops keep all candidates visible in the report.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
nlohmann::json CandidateGenerationReport(const FieldInputs &inputs,
                                         const std::vector<Target> &targets) {
  auto score_stream = [&](std::string_view role) {
    const auto &candidates = inputs.production_candidates.at("streams").at(role).at("candidates");
    std::vector<std::int64_t> target_times;
    target_times.reserve(targets.size());
    for (const Target &target : targets) {
      target_times.push_back(role == "face_on"
                                 ? target.impact_face_audio_us
                                 : target.impact_face_audio_us - kFaceAudioMinusDownAudioUs);
    }
    std::vector<bool> recalled(targets.size());
    std::size_t credited = 0;
    for (const auto &candidate : candidates) {
      const std::int64_t time_us = DecimalString(candidate.at("time_us"));
      std::optional<std::size_t> best;
      for (std::size_t index = 0; index < target_times.size(); ++index) {
        if (std::abs(time_us - target_times[index]) <= kTargetToleranceUs &&
            (!best.has_value() ||
             std::abs(time_us - target_times[index]) < std::abs(time_us - target_times[*best]))) {
          best = index;
        }
      }
      if (best.has_value()) {
        recalled[*best] = true;
        ++credited;
      }
    }
    const std::size_t recall = std::ranges::count(recalled, true);
    const std::size_t non_credited = candidates.size() - credited;
    return nlohmann::json{{"target_recall", recall},
                          {"target_count", targets.size()},
                          {"candidate_count", candidates.size()},
                          {"non_credited_candidate_count", non_credited},
                          {"false_candidates_per_minute", static_cast<double>(non_credited) *
                                                              60'000'000.0 / kRecordingDurationUs}};
  };

  std::vector<bool> paired_recalled(targets.size());
  std::size_t paired_credited = 0;
  const auto &pairs = inputs.production_candidates.at("paired_candidates");
  for (const auto &pair : pairs) {
    const std::int64_t time_us = DecimalString(pair.at("face_on_impact_time_us"));
    std::optional<std::size_t> best;
    for (std::size_t index = 0; index < targets.size(); ++index) {
      if (std::abs(time_us - targets[index].impact_face_audio_us) <= kTargetToleranceUs &&
          (!best.has_value() || std::abs(time_us - targets[index].impact_face_audio_us) <
                                    std::abs(time_us - targets[*best].impact_face_audio_us))) {
        best = index;
      }
    }
    if (best.has_value()) {
      paired_recalled[*best] = true;
      ++paired_credited;
    }
  }
  const std::size_t paired_non_credited = pairs.size() - paired_credited;
  return {{"down_the_line", score_stream("down_the_line")},
          {"face_on", score_stream("face_on")},
          {"paired",
           {{"target_recall", std::ranges::count(paired_recalled, true)},
            {"target_count", targets.size()},
            {"candidate_count", pairs.size()},
            {"non_credited_candidate_count", paired_non_credited},
            {"false_candidates_per_minute",
             static_cast<double>(paired_non_credited) * 60'000'000.0 / kRecordingDurationUs}}}};
}

std::string OutcomeName(TerminalOutcome outcome) {
  switch (outcome) {
    case TerminalOutcome::kTargetFirst:
      return "target_first";
    case TerminalOutcome::kFalseEarly:
      return "false_early";
    case TerminalOutcome::kLate:
      return "late";
    case TerminalOutcome::kNoCandidate:
      return "no_candidate";
  }
  throw std::logic_error("unknown terminal outcome");
}

nlohmann::json ArmedTerminalReport(
    const FieldInputs &inputs, const std::vector<Target> &targets,
    const std::vector<std::pair<std::vector<Candidate>, std::vector<Candidate>>> &candidate_windows,
    bool face_is_audio_leader, ConsensusPolicy policy) {
  std::size_t target_first = 0;
  std::size_t false_early = 0;
  std::size_t late = 0;
  std::size_t no_candidate = 0;
  std::int64_t maximum_added_latency_us = 0;
  std::int64_t maximum_total_latency_us = 0;
  nlohmann::json window_reports = nlohmann::json::array();
  const auto &attempts = inputs.face_on_session.at("attempts");
  for (std::size_t index = 0; index < targets.size(); ++index) {
    const std::int64_t arm_video_us = DecimalString(attempts[index].at("arm_ns")) / 1'000;
    const std::int64_t ready_face_audio_us =
        arm_video_us + kAudioStartupUs + kFaceAudioMinusVideoUs;
    const std::int64_t end_face_audio_us = targets[index].impact_face_audio_us + 150'000;
    const std::int64_t face_start_us = arm_video_us + kFaceAudioMinusVideoUs;
    const std::vector<Candidate> &face = candidate_windows[index].first;
    const std::vector<Candidate> &down = candidate_windows[index].second;
    const TerminalSelection selected =
        SelectTerminal(face_is_audio_leader ? face : down, face_is_audio_leader ? down : face,
                       ready_face_audio_us, end_face_audio_us, policy);
    const std::vector<AcceptedAudioEvent> accepted =
        selected.accepted ? std::vector<AcceptedAudioEvent>{{selected.leader_candidate.strike_us}}
                          : std::vector<AcceptedAudioEvent>{};
    const TerminalBenchmarkResult scored =
        BenchmarkTerminalWindows(accepted,
                                 {{.id = targets[index].id,
                                   .arm_us = face_start_us,
                                   .ready_us = ready_face_audio_us,
                                   .target_us = targets[index].impact_face_audio_us,
                                   .end_us = end_face_audio_us}},
                                 kTargetToleranceUs);
    const TerminalWindowResult &window = scored.windows.front();
    target_first += scored.target_first_count;
    false_early += scored.false_early_count;
    late += scored.late_count;
    no_candidate += scored.no_candidate_count;
    maximum_added_latency_us =
        std::max(maximum_added_latency_us, selected.added_decision_latency_us);
    if (selected.accepted) {
      maximum_total_latency_us = std::max(
          maximum_total_latency_us, selected.decision_us - selected.leader_candidate.strike_us);
    }
    window_reports.push_back(
        {{"id", targets[index].id},
         {"outcome", OutcomeName(window.outcome)},
         {"terminal_event_us", window.terminal_event_us.has_value()
                                   ? nlohmann::json(std::to_string(*window.terminal_event_us))
                                   : nlohmann::json(nullptr)},
         {"terminal_error_us", window.error_from_target_us.has_value()
                                   ? nlohmann::json(std::to_string(*window.error_from_target_us))
                                   : nlohmann::json(nullptr)},
         {"decision_us", selected.accepted ? nlohmann::json(std::to_string(selected.decision_us))
                                           : nlohmann::json(nullptr)},
         {"added_decision_latency_us", selected.added_decision_latency_us},
         {"total_decision_latency_us",
          selected.accepted
              ? nlohmann::json(selected.decision_us - selected.leader_candidate.strike_us)
              : nlohmann::json(nullptr)},
         {"peer_corroborated", selected.peer_corroborated}});
  }
  return {{"leader", face_is_audio_leader ? "face_on" : "down_the_line"},
          {"mode", ModeName(policy.mode)},
          {"network_one_way_us", policy.one_way_network_us},
          {"peer_wait_us", policy.peer_wait_us},
          {"target_first_count", target_first},
          {"false_early_count", false_early},
          {"late_count", late},
          {"no_candidate_count", no_candidate},
          {"maximum_added_decision_latency_us", maximum_added_latency_us},
          {"maximum_total_decision_latency_us", maximum_total_latency_us},
          {"windows", std::move(window_reports)}};
}

std::vector<std::pair<std::vector<Candidate>, std::vector<Candidate>>> BuildArmedWindows(
    const FieldInputs &inputs, const std::vector<Target> &targets) {
  std::vector<std::pair<std::vector<Candidate>, std::vector<Candidate>>> result;
  const auto &attempts = inputs.face_on_session.at("attempts");
  for (std::size_t index = 0; index < targets.size(); ++index) {
    const std::int64_t arm_video_us = DecimalString(attempts[index].at("arm_ns")) / 1'000;
    const std::int64_t face_start_us = arm_video_us + kFaceAudioMinusVideoUs;
    const std::int64_t face_end_us = targets[index].impact_face_audio_us + 150'000;
    const std::int64_t down_start_us = face_start_us - kFaceAudioMinusDownAudioUs;
    result.emplace_back(
        DetectArmedWindow(inputs.face_on_audio, DetectorConfig(true), face_start_us, face_end_us,
                          0),
        DetectArmedWindow(inputs.down_the_line_audio, DetectorConfig(false), down_start_us,
                          face_end_us - kFaceAudioMinusDownAudioUs, kFaceAudioMinusDownAudioUs));
  }
  return result;
}

nlohmann::json LifecycleJson(const LifecycleResult &result, bool face_is_audio_leader,
                             ConsensusPolicy policy) {
  const std::size_t false_attempts =
      std::ranges::count(result.attempts, std::string("false_terminal"), &AttemptResult::outcome);
  const std::size_t no_impact_attempts = std::ranges::count(
      result.attempts, std::string("no_impact_timeout"), &AttemptResult::outcome);
  std::int64_t maximum_added_latency_us = 0;
  std::int64_t maximum_total_latency_us = 0;
  nlohmann::json attempts = nlohmann::json::array();
  for (const AttemptResult &attempt : result.attempts) {
    maximum_added_latency_us = std::max(maximum_added_latency_us, attempt.added_latency_us);
    maximum_total_latency_us = std::max(
        maximum_total_latency_us, attempt.decision_video_us - attempt.terminal_strike_video_us);
    attempts.push_back(
        {{"arm_video_us", std::to_string(attempt.arm_video_us)},
         {"terminal_strike_video_us", std::to_string(attempt.terminal_strike_video_us)},
         {"decision_video_us", std::to_string(attempt.decision_video_us)},
         {"completion_video_us", std::to_string(attempt.completion_video_us)},
         {"outcome", attempt.outcome},
         {"target_id", attempt.target_id},
         {"terminal_error_us", attempt.terminal_error_us},
         {"added_decision_latency_us", attempt.added_latency_us},
         {"total_decision_latency_us",
          attempt.decision_video_us - attempt.terminal_strike_video_us},
         {"peer_corroborated", attempt.peer_corroborated}});
  }
  return {{"leader", face_is_audio_leader ? "face_on" : "down_the_line"},
          {"mode", ModeName(policy.mode)},
          {"network_one_way_us", policy.one_way_network_us},
          {"peer_wait_us", policy.peer_wait_us},
          {"real_swings_captured", result.captured_targets.size()},
          {"captured_target_ids", result.captured_targets},
          {"false_attempts", false_attempts},
          {"no_impact_attempts", no_impact_attempts},
          {"attempt_count", result.attempts.size()},
          {"total_high_speed_us", std::to_string(result.total_high_speed_us)},
          {"high_speed_duty_cycle",
           static_cast<double>(result.total_high_speed_us) / kRecordingDurationUs},
          {"minimum_video_lead_before_takeaway_us",
           result.minimum_video_lead_us == std::numeric_limits<std::int64_t>::max()
               ? nlohmann::json(nullptr)
               : nlohmann::json(std::to_string(result.minimum_video_lead_us))},
          {"minimum_retained_history_at_impact_us",
           result.minimum_retained_history_us == std::numeric_limits<std::int64_t>::max()
               ? nlohmann::json(nullptr)
               : nlohmann::json(std::to_string(result.minimum_retained_history_us))},
          {"maximum_added_decision_latency_us", maximum_added_latency_us},
          {"maximum_total_decision_latency_us", maximum_total_latency_us},
          {"skipped_pose_observations", result.skipped_observations},
          {"attempts", std::move(attempts)}};
}

std::vector<Candidate> ParseEnvelopeCandidates(const nlohmann::json &analysis,
                                               std::string_view config_name) {
  for (const auto &report : analysis.at("reports")) {
    if (report.at("config").at("name") != config_name) {
      continue;
    }
    std::vector<Candidate> candidates;
    for (const auto &candidate : report.at("candidate_generation").at("candidates")) {
      candidates.push_back(
          {.strike_us = DecimalString(candidate.at("strike_us")),
           .confirmation_us = DecimalString(candidate.at("decision_us")),
           .peak_ppm = std::llround(candidate.at("peak").get<double>() * 1'000'000.0),
           .threshold_ppm = 0});
    }
    return candidates;
  }
  throw std::invalid_argument("requested envelope configuration is absent");
}

std::vector<Candidate> ContinuousCandidates(const nlohmann::json &analysis, std::string_view role,
                                            std::int64_t local_to_shared_us) {
  std::vector<Candidate> result;
  for (const auto &candidate : analysis.at("streams").at(role).at("candidates")) {
    const std::int64_t strike_us = DecimalString(candidate.at("time_us")) + local_to_shared_us;
    result.push_back({.strike_us = strike_us,
                      .confirmation_us = strike_us + 1'500,
                      .peak_ppm = candidate.at("peak_amplitude_ppm").get<std::int64_t>(),
                      .threshold_ppm = candidate.at("threshold_ppm").get<std::int64_t>()});
  }
  return result;
}

Candidate NearestTargetCandidate(const std::vector<Candidate> &candidates, std::int64_t target_us) {
  const auto best = std::ranges::min_element(
      candidates, [target_us](const Candidate &left, const Candidate &right) {
        return std::abs(left.strike_us - target_us) < std::abs(right.strike_us - target_us);
      });
  if (best == candidates.end() || std::abs(best->strike_us - target_us) > kTargetToleranceUs) {
    throw std::runtime_error("target has no local candidate");
  }
  return *best;
}

nlohmann::json ShadowPolicyReport(const FieldInputs &inputs, const std::vector<Target> &targets,
                                  std::int64_t network_us, std::int64_t wait_us,
                                  std::int64_t assumed_alignment_error_us) {
  const std::vector<Candidate> leaders =
      ContinuousCandidates(inputs.production_candidates, "face_on", 0);
  const std::vector<Candidate> shadows =
      ContinuousCandidates(inputs.production_candidates, "down_the_line",
                           kFaceAudioMinusDownAudioUs + assumed_alignment_error_us);
  std::size_t current_local = 0;
  std::size_t current_accurate = 0;
  std::size_t ring_local = 0;
  std::size_t ring_accurate = 0;
  std::int64_t current_total_absolute_error_us = 0;
  std::int64_t ring_total_absolute_error_us = 0;
  std::int64_t current_maximum_absolute_error_us = 0;
  std::int64_t ring_maximum_absolute_error_us = 0;
  nlohmann::json events = nlohmann::json::array();
  for (const Target &target : targets) {
    const Candidate leader = NearestTargetCandidate(leaders, target.impact_face_audio_us);
    const std::int64_t arrival_us = leader.confirmation_us + network_us;
    const ShadowSelection current =
        SelectLatestShadowCandidate(shadows, leader, arrival_us, 250'000);
    const ShadowSelection ring =
        SelectClosestShadowCandidate(shadows, leader, arrival_us, wait_us, 80'000);
    const std::int64_t shadow_target_shared_us =
        target.impact_face_audio_us;  // Curated WAV peaks differ by exactly 80 ms.
    const std::int64_t current_actual_timestamp_us =
        current.used_local_candidate ? current.selected_timestamp_us - assumed_alignment_error_us
                                     : current.selected_timestamp_us;
    const std::int64_t ring_actual_timestamp_us =
        ring.used_local_candidate ? ring.selected_timestamp_us - assumed_alignment_error_us
                                  : ring.selected_timestamp_us;
    const std::int64_t current_error_us = current_actual_timestamp_us - shadow_target_shared_us;
    const std::int64_t ring_error_us = ring_actual_timestamp_us - shadow_target_shared_us;
    current_local += current.used_local_candidate ? 1 : 0;
    ring_local += ring.used_local_candidate ? 1 : 0;
    current_accurate += std::abs(current_error_us) <= kTargetToleranceUs ? 1 : 0;
    ring_accurate += std::abs(ring_error_us) <= kTargetToleranceUs ? 1 : 0;
    current_total_absolute_error_us += std::abs(current_error_us);
    ring_total_absolute_error_us += std::abs(ring_error_us);
    current_maximum_absolute_error_us =
        std::max(current_maximum_absolute_error_us, std::abs(current_error_us));
    ring_maximum_absolute_error_us =
        std::max(ring_maximum_absolute_error_us, std::abs(ring_error_us));
    events.push_back({{"id", target.id},
                      {"current_source", current.source},
                      {"current_error_us", current_error_us},
                      {"ring_source", ring.source},
                      {"ring_error_us", ring_error_us},
                      {"ring_added_wait_us", ring.decision_us - arrival_us}});
  }
  return {{"network_one_way_us", network_us},
          {"ring_post_arrival_wait_us", wait_us},
          {"assumed_alignment_error_us", assumed_alignment_error_us},
          {"current_latest",
           {{"local_candidate_count", current_local},
            {"accurate_within_100ms_count", current_accurate},
            {"mean_absolute_error_us", static_cast<double>(current_total_absolute_error_us) /
                                           static_cast<double>(targets.size())},
            {"maximum_absolute_error_us", current_maximum_absolute_error_us}}},
          {"closest_ring",
           {{"local_candidate_count", ring_local},
            {"accurate_within_100ms_count", ring_accurate},
            {"mean_absolute_error_us", static_cast<double>(ring_total_absolute_error_us) /
                                           static_cast<double>(targets.size())},
            {"maximum_absolute_error_us", ring_maximum_absolute_error_us}}},
          {"events", std::move(events)}};
}

}  // namespace

TerminalSelection SelectTerminal(const std::vector<Candidate> &leader,
                                 const std::vector<Candidate> &peer, std::int64_t ready_us,
                                 std::int64_t end_us, ConsensusPolicy policy) {
  ValidateCandidates(leader);
  ValidateCandidates(peer);
  if (ready_us < 0 || end_us <= ready_us || policy.peer_wait_us < 0 ||
      policy.one_way_network_us < 0 || policy.pair_tolerance_us < 0) {
    throw std::invalid_argument("terminal policy bounds are invalid");
  }
  for (std::size_t index = 0; index < leader.size(); ++index) {
    const Candidate &candidate = leader[index];
    if (candidate.confirmation_us < ready_us || candidate.confirmation_us >= end_us) {
      continue;
    }
    if (policy.mode == ConsensusMode::kLeaderImmediate) {
      return {.accepted = true,
              .leader_candidate = candidate,
              .decision_us = candidate.confirmation_us,
              .added_decision_latency_us = 0,
              .peer_corroborated = false};
    }
    // The accepted event remains the leader strike inside the window. A
    // declared look-ahead may decide after the evidence window's end and must
    // report that latency rather than silently turning the event into a miss.
    const std::int64_t deadline = candidate.confirmation_us + policy.peer_wait_us;
    if (const auto match = AvailablePeerMatch(peer, candidate, deadline, policy);
        match.has_value()) {
      return {.accepted = true,
              .leader_candidate = candidate,
              .decision_us = std::max(candidate.confirmation_us, match->second),
              .added_decision_latency_us =
                  std::max(candidate.confirmation_us, match->second) - candidate.confirmation_us,
              .peer_corroborated = true};
    }
    if (policy.mode == ConsensusMode::kHard) {
      continue;
    }

    // While the first unpaired candidate is pending, a later corroborated
    // leader candidate may supersede it. This is the only look-ahead used by
    // soft consensus and is charged to its decision latency.
    for (std::size_t later_index = index + 1; later_index < leader.size(); ++later_index) {
      const Candidate &later = leader[later_index];
      if (later.confirmation_us > deadline) {
        break;
      }
      if (const auto match = AvailablePeerMatch(peer, later, deadline, policy); match.has_value()) {
        const std::int64_t decision = std::max(later.confirmation_us, match->second);
        return {.accepted = true,
                .leader_candidate = later,
                .decision_us = decision,
                .added_decision_latency_us = decision - later.confirmation_us,
                .peer_corroborated = true};
      }
    }
    return {.accepted = true,
            .leader_candidate = candidate,
            .decision_us = deadline,
            .added_decision_latency_us = deadline - candidate.confirmation_us,
            .peer_corroborated = false};
  }
  return {};
}

ShadowSelection SelectLatestShadowCandidate(const std::vector<Candidate> &shadow,
                                            const Candidate &leader_candidate,
                                            std::int64_t request_arrival_us,
                                            std::int64_t maximum_age_us) {
  ValidateCandidates(shadow);
  static_cast<void>(leader_candidate);
  if (request_arrival_us < 0 || maximum_age_us < 0) {
    throw std::invalid_argument("shadow selection bounds are invalid");
  }
  std::optional<Candidate> latest;
  for (const Candidate &candidate : shadow) {
    if (candidate.confirmation_us > request_arrival_us) {
      break;
    }
    latest = candidate;
  }
  if (latest.has_value() && latest->strike_us <= request_arrival_us &&
      request_arrival_us - latest->strike_us <= maximum_age_us) {
    return {.used_local_candidate = true,
            .candidate = *latest,
            .selected_timestamp_us = latest->strike_us,
            .decision_us = request_arrival_us,
            .source = "latest_local_candidate"};
  }
  return {.used_local_candidate = false,
          .candidate = {},
          .selected_timestamp_us = request_arrival_us,
          .decision_us = request_arrival_us,
          .source = "request_arrival_fallback"};
}

ShadowSelection SelectClosestShadowCandidate(const std::vector<Candidate> &shadow,
                                             const Candidate &leader_candidate,
                                             std::int64_t request_arrival_us,
                                             std::int64_t post_arrival_wait_us,
                                             std::int64_t pair_tolerance_us) {
  ValidateCandidates(shadow);
  if (request_arrival_us < 0 || post_arrival_wait_us < 0 || pair_tolerance_us < 0) {
    throw std::invalid_argument("shadow ring selection bounds are invalid");
  }
  const std::int64_t deadline_us = request_arrival_us + post_arrival_wait_us;
  if (const auto best = BestPeerMatch(shadow, leader_candidate, deadline_us, pair_tolerance_us);
      best.has_value()) {
    return {.used_local_candidate = true,
            .candidate = *best,
            .selected_timestamp_us = best->strike_us,
            .decision_us = std::max(request_arrival_us, best->confirmation_us),
            .source = "closest_ring_candidate"};
  }
  return {.used_local_candidate = false,
          .candidate = {},
          .selected_timestamp_us = request_arrival_us,
          .decision_us = deadline_us,
          .source = "request_arrival_fallback"};
}

nlohmann::json AnalyzeFieldConsensus(const FieldInputs &inputs) {
  const std::vector<Observation> observations = ParseObservations(inputs.face_on_observations_csv);
  const std::vector<Target> targets = ParseTargets(inputs.target_index, inputs.face_on_session);
  const auto armed_windows = BuildArmedWindows(inputs, targets);
  const std::vector<Candidate> robust_envelope =
      ParseEnvelopeCandidates(inputs.face_on_envelope, "robust_hp120_x12");
  DetectorCache detector_cache;
  nlohmann::json terminal_reports = nlohmann::json::array();
  nlohmann::json lifecycle_reports = nlohmann::json::array();

  const std::array<std::int64_t, 6> network_delays = {0, 10'000, 25'000, 50'000, 100'000, 200'000};
  const std::array<std::int64_t, 5> peer_waits = {50'000, 100'000, 150'000, 250'000, 350'000};
  for (const bool face_leader : {true, false}) {
    const ConsensusPolicy immediate{.mode = ConsensusMode::kLeaderImmediate};
    terminal_reports.push_back(
        ArmedTerminalReport(inputs, targets, armed_windows, face_leader, immediate));
    lifecycle_reports.push_back(LifecycleJson(
        ReplayLifecycle(inputs, observations, targets, face_leader, immediate, detector_cache),
        face_leader, immediate));
    for (const std::int64_t network_us : network_delays) {
      for (const std::int64_t wait_us : peer_waits) {
        for (const ConsensusMode mode : {ConsensusMode::kHard, ConsensusMode::kSoftBounded}) {
          const ConsensusPolicy policy{.mode = mode,
                                       .peer_wait_us = wait_us,
                                       .one_way_network_us = network_us,
                                       .pair_tolerance_us = 80'000};
          terminal_reports.push_back(
              ArmedTerminalReport(inputs, targets, armed_windows, face_leader, policy));
          const bool lifecycle_grid_point = (wait_us == 150'000 && network_us <= 100'000) ||
                                            (wait_us == 350'000 && network_us == 200'000);
          if (lifecycle_grid_point) {
            lifecycle_reports.push_back(LifecycleJson(
                ReplayLifecycle(inputs, observations, targets, face_leader, policy, detector_cache),
                face_leader, policy));
          }
        }
      }
    }
  }

  nlohmann::json shadow_reports = nlohmann::json::array();
  for (const std::int64_t network_us : network_delays) {
    for (const std::int64_t wait_us : {0LL, 25'000LL, 50'000LL, 75'000LL, 100'000LL}) {
      shadow_reports.push_back(ShadowPolicyReport(inputs, targets, network_us, wait_us, 0));
    }
  }
  nlohmann::json alignment_sensitivity = nlohmann::json::array();
  for (const std::int64_t error_us :
       {-50'000LL, -25'000LL, -10'000LL, 0LL, 10'000LL, 25'000LL, 50'000LL}) {
    alignment_sensitivity.push_back(ShadowPolicyReport(inputs, targets, 25'000, 75'000, error_us));
  }
  const ConsensusPolicy envelope_policy{.mode = ConsensusMode::kLeaderImmediate};
  const nlohmann::json envelope_lifecycle =
      LifecycleJson(ReplayLifecycle(inputs, observations, targets, true, envelope_policy,
                                    detector_cache, &robust_envelope),
                    true, envelope_policy);

  return {
      {"schema_version", 1},
      {"benchmark_contract", "capture/offline/experiments/BENCHMARK.md"},
      {"development_set_warning",
       "Device, view, mounting location, and acoustics are confounded; results rank prototypes "
       "only."},
      {"clock_axes",
       {{"shared_axis", "face_on_wav_us"},
        {"face_audio_minus_video_us", kFaceAudioMinusVideoUs},
        {"face_audio_minus_down_audio_us", kFaceAudioMinusDownAudioUs},
        {"alignment_source", "production_impact_candidates.json measured median"}}},
      {"candidate_generation", CandidateGenerationReport(inputs, targets)},
      {"armed_terminal", std::move(terminal_reports)},
      {"complete_lifecycle", std::move(lifecycle_reports)},
      {"conservative_envelope_lifecycle",
       {{"config", "robust_hp120_x12"},
        {"selection_reason",
         "Conservative robust-envelope neighborhood; excludes development-tuned HF gate."},
        {"result", envelope_lifecycle}}},
      {"shadow_timestamp_selection", std::move(shadow_reports)},
      {"shadow_alignment_sensitivity", std::move(alignment_sensitivity)},
      {"request_arrival_clock_safety",
       {{"safe_for_local_freshness_bound_without_clock_sync", true},
        {"safe_for_cross_phone_candidate_match_without_clock_sync", false},
        {"reason",
         "Arrival and local candidates share the receiver clock, but arrival includes unknown "
         "one-way network delay and cannot identify the corresponding local strike lobe."},
        {"requirement",
         "Carry the leader strike time plus an estimated cross-phone monotonic-clock offset and "
         "uncertainty, or use arrival only as an audited fallback."}}},
  };
}

// NOLINTEND(misc-include-cleaner)

}  // namespace swing_capture::offline::experiments::consensus
