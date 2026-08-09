#include "capture/optical/led_schedule_association.h"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "capture/optical/frame_selection.h"
#include "capture/optical/led_pulse.h"

namespace {

using Clock = std::chrono::steady_clock;
using swing_capture::optical::EstimatedStimulusHostSchedule;
using swing_capture::optical::EvaluateLedScheduleAssociation;
using swing_capture::optical::kLedAnalysisFrameCount;
using swing_capture::optical::LedFrameDiagnostic;
using swing_capture::optical::LedPulseResult;
using swing_capture::optical::LedScheduleAssociationOptions;
using swing_capture::optical::LedScheduleAssociationResult;
using swing_capture::optical::SelectAcceptedLedPeakFrame;

constexpr auto kFrameInterval = std::chrono::milliseconds(4);
constexpr auto kCommandedDuration = std::chrono::milliseconds(40);

int failures = 0;

void Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::vector<Clock::time_point> MakeTimes() {
  std::vector<Clock::time_point> times;
  times.reserve(kLedAnalysisFrameCount);
  const auto first = Clock::time_point(std::chrono::seconds(10));
  for (std::size_t index = 0; index < kLedAnalysisFrameCount; ++index) {
    times.push_back(first + index * kFrameInterval);
  }
  return times;
}

LedPulseResult MakePulse(std::size_t start, std::size_t end) {
  LedPulseResult pulse;
  pulse.detected = true;
  pulse.diagnostic = "synthetic detected pulse";
  pulse.pulse_start_frame_index = start;
  pulse.pulse_end_frame_index = end;
  pulse.pulse_span_frame_count = end - start + 1U;
  pulse.pulse_active_frame_count = pulse.pulse_span_frame_count;
  pulse.frames.resize(kLedAnalysisFrameCount);
  for (std::size_t index = 0; index < pulse.frames.size(); ++index) {
    LedFrameDiagnostic &frame = pulse.frames[index];
    frame.frame_id = 100U + index;
    frame.device_timestamp = 1'000'000U + index * 4'000U;
    frame.baseline_frame = index < 8;
    frame.active = index >= start && index <= end;
    frame.mean_positive_red_delta = frame.active ? 50.0 : 0.0;
  }
  return pulse;
}

EstimatedStimulusHostSchedule ScheduleAt(Clock::time_point target,
                                         Clock::duration half_width = Clock::duration::zero()) {
  return {
      .earliest_start = target - half_width,
      .latest_start = target + half_width,
      .target_start = target,
  };
}

LedScheduleAssociationOptions Options() {
  return {
      .commanded_duration = kCommandedDuration,
      .exposure_duration = std::chrono::microseconds(500),
      .mapping_uncertainty = std::chrono::microseconds(500),
      .maximum_mapping_uncertainty = std::chrono::milliseconds(1),
      .frame_delivery_latency_bound = std::chrono::milliseconds(1),
      .maximum_schedule_bracket_width = std::chrono::milliseconds(4),
  };
}

const swing_capture::optical::LedScheduleAssociationCheck &FindCheck(
    const LedScheduleAssociationResult &result, std::string_view name) {
  for (const auto &check : result.checks) {
    if (check.name == name) {
      return check;
    }
  }
  throw std::runtime_error("missing check: " + std::string(name));
}

void AcceptsAssociatedPulseAndSelectsPeakInsideRun() {
  const auto times = MakeTimes();
  auto pulse = MakePulse(16, 25);
  pulse.frames[20].mean_positive_red_delta = 90.0;
  pulse.frames[30].mean_positive_red_delta = 250.0;
  const auto result =
      EvaluateLedScheduleAssociation(times, pulse, ScheduleAt(times[16]), Options());

  Expect(result.passed, "aligned ten-frame pulse passes schedule association");
  Expect(result.nominal_frame_interval == kFrameInterval, "mapped frame interval recorded");
  Expect(result.edge_tolerance == std::chrono::milliseconds(6),
         "edge tolerance records frame phase, exposure, mapping, and delivery bounds");
  Expect(result.observed_start == times[16], "observed start recorded");
  Expect(result.observed_end_exclusive == times[26], "observed exclusive end recorded");
  Expect(SelectAcceptedLedPeakFrame(pulse) == 20,
         "peak selection ignores stronger inactive outlier outside accepted run");
}

void AppliesExactlyOneFrameOfTimestampTolerance() {
  const auto times = MakeTimes();
  auto options = Options();
  options.exposure_duration = Clock::duration::zero();
  options.mapping_uncertainty = std::chrono::duration<double>::zero();
  options.frame_delivery_latency_bound = Clock::duration::zero();

  const auto one_frame_late =
      EvaluateLedScheduleAssociation(times, MakePulse(17, 26), ScheduleAt(times[16]), options);
  Expect(one_frame_late.passed, "one-frame edge displacement is accepted inclusively");

  const auto two_frames_late =
      EvaluateLedScheduleAssociation(times, MakePulse(18, 27), ScheduleAt(times[16]), options);
  Expect(!two_frames_late.passed, "two-frame edge displacement is rejected");
  Expect(!FindCheck(two_frames_late, "pulse_start_associated").passed,
         "late pulse start has an explicit failed check");
  Expect(!FindCheck(two_frames_late, "pulse_end_associated").passed,
         "late pulse end has an explicit failed check");
}

void FrameDeliveryLatencyBoundIsExplicit() {
  const auto times = MakeTimes();
  auto options = Options();
  options.exposure_duration = Clock::duration::zero();
  options.mapping_uncertainty = std::chrono::duration<double>::zero();
  const auto schedule =
      ScheduleAt(times[16] - kFrameInterval - options.frame_delivery_latency_bound);
  const auto bounded_delivery =
      EvaluateLedScheduleAssociation(times, MakePulse(16, 25), schedule, options);
  Expect(bounded_delivery.passed, "declared frame-delivery latency is accepted inclusively");

  options.frame_delivery_latency_bound = Clock::duration::zero();
  const auto unbounded_delivery =
      EvaluateLedScheduleAssociation(times, MakePulse(16, 25), schedule, options);
  Expect(!unbounded_delivery.passed, "same receipt bias fails without a declared delivery bound");
}

void ExposureToleranceIsExplicit() {
  const auto times = MakeTimes();
  auto options = Options();
  options.mapping_uncertainty = std::chrono::duration<double>::zero();
  options.frame_delivery_latency_bound = Clock::duration::zero();
  const auto schedule = ScheduleAt(times[16] + kFrameInterval + options.exposure_duration);
  const auto with_exposure =
      EvaluateLedScheduleAssociation(times, MakePulse(16, 25), schedule, options);
  Expect(with_exposure.passed, "one frame plus exposure displacement is accepted");

  options.exposure_duration = Clock::duration::zero();
  const auto without_exposure =
      EvaluateLedScheduleAssociation(times, MakePulse(16, 25), schedule, options);
  Expect(!without_exposure.passed, "same displacement fails without exposure tolerance");
}

void RejectsWideScheduleAndExcessiveMappingUncertainty() {
  const auto times = MakeTimes();
  auto options = Options();
  const auto wide = EvaluateLedScheduleAssociation(
      times, MakePulse(16, 25), ScheduleAt(times[16], std::chrono::milliseconds(3)), options);
  Expect(!wide.passed, "six-millisecond ACK bracket exceeds configured limit");
  Expect(!FindCheck(wide, "schedule_ack_bracket_width").passed,
         "wide ACK bracket has an explicit failed check");

  options.mapping_uncertainty = std::chrono::milliseconds(2);
  const auto uncertain =
      EvaluateLedScheduleAssociation(times, MakePulse(16, 25), ScheduleAt(times[16]), options);
  Expect(!uncertain.passed, "excessive mapper uncertainty is rejected");
  Expect(!FindCheck(uncertain, "mapper_uncertainty").passed,
         "mapper uncertainty has an explicit failed check");
}

void ReportsNoDetectionWithoutInventingEdges() {
  const auto times = MakeTimes();
  LedPulseResult pulse;
  pulse.frames.resize(kLedAnalysisFrameCount);
  pulse.diagnostic = "no frame crossed the brightness threshold";
  const auto result =
      EvaluateLedScheduleAssociation(times, pulse, ScheduleAt(times[16]), Options());

  Expect(!result.passed, "missing optical pulse fails association");
  const auto &check = FindCheck(result, "pulse_detected");
  Expect(!check.passed && check.message == pulse.diagnostic,
         "LED analyzer diagnostic is preserved");
  Expect(result.checks.size() == 3, "edge checks are not fabricated without a detection");
}

template <typename Function>
void ExpectInvalidArgument(Function &&function, const std::string &message) {
  bool rejected = false;
  try {
    function();
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Expect(rejected, message);
}

void RejectsMalformedEvidence() {
  auto times = MakeTimes();
  const auto schedule = ScheduleAt(times[16]);
  auto pulse = MakePulse(16, 25);

  times.pop_back();
  ExpectInvalidArgument(
      [&] { (void)EvaluateLedScheduleAssociation(times, pulse, schedule, Options()); },
      "non-48-frame mapped timeline rejected");

  times = MakeTimes();
  times[20] = times[19];
  ExpectInvalidArgument(
      [&] { (void)EvaluateLedScheduleAssociation(times, pulse, schedule, Options()); },
      "nonmonotonic mapped timeline rejected");

  pulse.pulse_end_frame_index = pulse.frames.size();
  ExpectInvalidArgument([&] { (void)SelectAcceptedLedPeakFrame(pulse); },
                        "out-of-range accepted pulse rejected");

  pulse = MakePulse(16, 25);
  pulse.pulse_active_frame_count -= 1U;
  ExpectInvalidArgument([&] { (void)SelectAcceptedLedPeakFrame(pulse); },
                        "inconsistent active count rejected");
}

}  // namespace

int main() {
  AcceptsAssociatedPulseAndSelectsPeakInsideRun();
  AppliesExactlyOneFrameOfTimestampTolerance();
  FrameDeliveryLatencyBoundIsExplicit();
  ExposureToleranceIsExplicit();
  RejectsWideScheduleAndExcessiveMappingUncertainty();
  ReportsNoDetectionWithoutInventingEdges();
  RejectsMalformedEvidence();
  return failures == 0 ? 0 : 1;
}
