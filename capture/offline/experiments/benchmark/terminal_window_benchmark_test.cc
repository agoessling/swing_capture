#include "capture/offline/experiments/benchmark/terminal_window_benchmark.h"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

using swing_capture::offline::experiments::AcceptedAudioEvent;
using swing_capture::offline::experiments::BenchmarkTerminalWindows;
using swing_capture::offline::experiments::TerminalOutcome;
using swing_capture::offline::experiments::TerminalWindow;

void ScoresOnlyEventsAfterReadiness() {
  const std::vector<AcceptedAudioEvent> events = {
      {.time_us = 900}, {.time_us = 1'500}, {.time_us = 3'950}, {.time_us = 7'000}};
  const std::vector<TerminalWindow> windows = {
      {.id = "early", .arm_us = 0, .ready_us = 1'000, .target_us = 2'000, .end_us = 3'000},
      {.id = "target", .arm_us = 3'000, .ready_us = 3'500, .target_us = 4'000, .end_us = 5'000},
      {.id = "none", .arm_us = 5'000, .ready_us = 5'500, .target_us = 6'000, .end_us = 6'500},
  };

  const auto result = BenchmarkTerminalWindows(events, windows, 100);

  assert(result.windows.size() == 3);
  assert(result.windows[0].outcome == TerminalOutcome::kFalseEarly);
  assert(result.windows[0].terminal_event_us == 1'500);
  assert(result.windows[1].outcome == TerminalOutcome::kTargetFirst);
  assert(result.windows[1].error_from_target_us == -50);
  assert(result.windows[2].outcome == TerminalOutcome::kNoCandidate);
  assert(result.target_first_count == 1);
  assert(result.false_early_count == 1);
  assert(result.no_candidate_count == 1);
}

void RejectsInvalidInputs() {
  try {
    static_cast<void>(BenchmarkTerminalWindows({{.time_us = 2}, {.time_us = 1}}, {}, 100));
    assert(false);
  } catch (const std::invalid_argument &) {
  }
  try {
    static_cast<void>(BenchmarkTerminalWindows(
        {}, {{.id = "bad", .arm_us = 2, .ready_us = 1, .target_us = 3, .end_us = 4}}, 100));
    assert(false);
  } catch (const std::invalid_argument &) {
  }
}

}  // namespace

int main() {
  ScoresOnlyEventsAfterReadiness();
  RejectsInvalidInputs();
  return 0;
}
