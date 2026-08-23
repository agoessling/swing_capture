#include "capture/offline/experiments/benchmark/terminal_window_benchmark.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace swing_capture::offline::experiments {
namespace {

void Validate(const std::vector<AcceptedAudioEvent> &events,
              const std::vector<TerminalWindow> &windows, std::int64_t target_tolerance_us) {
  if (target_tolerance_us < 0) {
    throw std::invalid_argument("target tolerance cannot be negative");
  }
  if (!std::ranges::is_sorted(events, {}, &AcceptedAudioEvent::time_us)) {
    throw std::invalid_argument("accepted events must be sorted");
  }
  std::int64_t previous_target_us = -1;
  for (const TerminalWindow &window : windows) {
    if (window.id.empty() || window.arm_us < 0 || window.ready_us < window.arm_us ||
        window.target_us <= window.ready_us || window.end_us <= window.target_us ||
        window.target_us <= previous_target_us) {
      throw std::invalid_argument("terminal window is invalid");
    }
    previous_target_us = window.target_us;
  }
}

TerminalOutcome Classify(std::int64_t error_us, std::int64_t tolerance_us) {
  if (std::abs(error_us) <= tolerance_us) {
    return TerminalOutcome::kTargetFirst;
  }
  return error_us < 0 ? TerminalOutcome::kFalseEarly : TerminalOutcome::kLate;
}

void Count(TerminalOutcome outcome, TerminalBenchmarkResult &result) {
  switch (outcome) {
    case TerminalOutcome::kTargetFirst:
      ++result.target_first_count;
      break;
    case TerminalOutcome::kFalseEarly:
      ++result.false_early_count;
      break;
    case TerminalOutcome::kLate:
      ++result.late_count;
      break;
    case TerminalOutcome::kNoCandidate:
      ++result.no_candidate_count;
      break;
  }
}

}  // namespace

TerminalBenchmarkResult BenchmarkTerminalWindows(const std::vector<AcceptedAudioEvent> &events,
                                                 const std::vector<TerminalWindow> &windows,
                                                 std::int64_t target_tolerance_us) {
  Validate(events, windows, target_tolerance_us);
  TerminalBenchmarkResult result;
  result.windows.reserve(windows.size());
  for (const TerminalWindow &window : windows) {
    const auto terminal =
        std::ranges::lower_bound(events, window.ready_us, {}, &AcceptedAudioEvent::time_us);
    if (terminal == events.end() || terminal->time_us >= window.end_us) {
      result.windows.push_back({.id = window.id,
                                .outcome = TerminalOutcome::kNoCandidate,
                                .terminal_event_us = std::nullopt,
                                .error_from_target_us = std::nullopt});
      Count(TerminalOutcome::kNoCandidate, result);
      continue;
    }
    const std::int64_t error_us = terminal->time_us - window.target_us;
    const TerminalOutcome outcome = Classify(error_us, target_tolerance_us);
    result.windows.push_back({.id = window.id,
                              .outcome = outcome,
                              .terminal_event_us = terminal->time_us,
                              .error_from_target_us = error_us});
    Count(outcome, result);
  }
  return result;
}

}  // namespace swing_capture::offline::experiments
