#ifndef SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_BENCHMARK_TERMINAL_WINDOW_BENCHMARK_H_
#define SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_BENCHMARK_TERMINAL_WINDOW_BENCHMARK_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace swing_capture::offline::experiments {

struct AcceptedAudioEvent {
  std::int64_t time_us = 0;
};

struct TerminalWindow {
  std::string id;
  std::int64_t arm_us = 0;
  std::int64_t ready_us = 0;
  std::int64_t target_us = 0;
  std::int64_t end_us = 0;
};

enum class TerminalOutcome {
  kTargetFirst,
  kFalseEarly,
  kLate,
  kNoCandidate,
};

struct TerminalWindowResult {
  std::string id;
  TerminalOutcome outcome = TerminalOutcome::kNoCandidate;
  std::optional<std::int64_t> terminal_event_us;
  std::optional<std::int64_t> error_from_target_us;
};

struct TerminalBenchmarkResult {
  std::vector<TerminalWindowResult> windows;
  std::size_t target_first_count = 0;
  std::size_t false_early_count = 0;
  std::size_t late_count = 0;
  std::size_t no_candidate_count = 0;
};

// Scores the first accepted event after trigger readiness. Events before
// readiness remain diagnostic evidence but cannot terminate a production
// attempt.
[[nodiscard]] TerminalBenchmarkResult BenchmarkTerminalWindows(
    const std::vector<AcceptedAudioEvent> &events, const std::vector<TerminalWindow> &windows,
    std::int64_t target_tolerance_us = 100'000);

}  // namespace swing_capture::offline::experiments

#endif  // SWING_CAPTURE_CAPTURE_OFFLINE_EXPERIMENTS_BENCHMARK_TERMINAL_WINDOW_BENCHMARK_H_
