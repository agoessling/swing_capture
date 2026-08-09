#include "capture/optical/led_schedule_association.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ratio>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "capture/optical/frame_selection.h"
#include "capture/optical/led_pulse.h"

namespace swing_capture::optical {
namespace {

using Clock = std::chrono::steady_clock;

double Milliseconds(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

Clock::duration CheckedCeilClockDuration(std::chrono::duration<double> duration, const char *name) {
  if (!std::isfinite(duration.count()) || duration < std::chrono::duration<double>::zero()) {
    throw std::invalid_argument(std::string(name) + " must be finite and nonnegative");
  }
  return std::chrono::ceil<Clock::duration>(duration);
}

void ValidateOptions(const LedScheduleAssociationOptions &options) {
  if (options.commanded_duration <= Clock::duration::zero()) {
    throw std::invalid_argument("commanded LED duration must be positive");
  }
  if (options.exposure_duration < Clock::duration::zero()) {
    throw std::invalid_argument("camera exposure duration cannot be negative");
  }
  if (options.maximum_schedule_bracket_width < Clock::duration::zero()) {
    throw std::invalid_argument("maximum LED schedule bracket width cannot be negative");
  }
  if (options.frame_delivery_latency_bound < Clock::duration::zero()) {
    throw std::invalid_argument("LED frame-delivery latency bound cannot be negative");
  }
  (void)CheckedCeilClockDuration(options.mapping_uncertainty, "LED mapping uncertainty");
  (void)CheckedCeilClockDuration(options.maximum_mapping_uncertainty,
                                 "maximum LED mapping uncertainty");
}

void ValidateSchedule(const EstimatedStimulusHostSchedule &schedule) {
  if (schedule.latest_start < schedule.earliest_start ||
      schedule.target_start < schedule.earliest_start ||
      schedule.target_start > schedule.latest_start) {
    throw std::invalid_argument("estimated LED host schedule is inconsistent");
  }
}

Clock::duration MedianFrameInterval(std::span<const Clock::time_point> frame_host_times) {
  if (frame_host_times.size() != kLedAnalysisFrameCount) {
    throw std::invalid_argument("LED schedule association requires exactly 48 mapped frames");
  }
  std::vector<Clock::duration> intervals;
  intervals.reserve(frame_host_times.size() - 1U);
  for (std::size_t index = 1; index < frame_host_times.size(); ++index) {
    if (frame_host_times[index] <= frame_host_times[index - 1U]) {
      throw std::invalid_argument("mapped LED frame times must be strictly increasing");
    }
    intervals.push_back(frame_host_times[index] - frame_host_times[index - 1U]);
  }
  std::ranges::sort(intervals);
  const std::size_t middle = intervals.size() / 2U;
  if ((intervals.size() & 1U) != 0U) {
    return intervals[middle];
  }
  return intervals[middle - 1U] + (intervals[middle] - intervals[middle - 1U]) / 2;
}

std::size_t CountActiveFramesInAcceptedSpan(const LedPulseResult &pulse) {
  if (!pulse.detected) {
    throw std::invalid_argument("accepted LED pulse operation requires a detected pulse");
  }
  if (pulse.frames.empty() || pulse.pulse_start_frame_index > pulse.pulse_end_frame_index ||
      pulse.pulse_end_frame_index >= pulse.frames.size()) {
    throw std::invalid_argument("detected LED pulse has invalid accepted-run indices");
  }
  const std::size_t span = pulse.pulse_end_frame_index - pulse.pulse_start_frame_index + 1U;
  if (pulse.pulse_span_frame_count != span || !pulse.frames[pulse.pulse_start_frame_index].active ||
      !pulse.frames[pulse.pulse_end_frame_index].active) {
    throw std::invalid_argument("detected LED pulse has inconsistent accepted-run span");
  }
  const auto begin =
      pulse.frames.begin() + static_cast<std::ptrdiff_t>(pulse.pulse_start_frame_index);
  const auto end =
      pulse.frames.begin() + static_cast<std::ptrdiff_t>(pulse.pulse_end_frame_index + 1U);
  const auto active =
      static_cast<std::size_t>(std::ranges::count_if(begin, end, &LedFrameDiagnostic::active));
  if (active == 0 || active != pulse.pulse_active_frame_count) {
    throw std::invalid_argument("detected LED pulse has inconsistent active-frame count");
  }
  return active;
}

void AddCheck(LedScheduleAssociationResult *result, std::string name, bool passed,
              std::string message) {
  result->checks.push_back(
      {.name = std::move(name), .passed = passed, .message = std::move(message)});
}

bool InsideInclusive(Clock::time_point value, Clock::time_point lower, Clock::time_point upper) {
  return value >= lower && value <= upper;
}

}  // namespace

LedScheduleAssociationResult EvaluateLedScheduleAssociation(
    std::span<const Clock::time_point> selected_frame_host_times, const LedPulseResult &pulse,
    const EstimatedStimulusHostSchedule &schedule, const LedScheduleAssociationOptions &options) {
  ValidateOptions(options);
  ValidateSchedule(schedule);
  if (pulse.frames.size() != selected_frame_host_times.size()) {
    throw std::invalid_argument("LED diagnostics must match the mapped 48-frame timeline");
  }

  LedScheduleAssociationResult result;
  result.nominal_frame_interval = MedianFrameInterval(selected_frame_host_times);
  result.schedule_bracket_width = schedule.latest_start - schedule.earliest_start;
  result.mapping_uncertainty =
      CheckedCeilClockDuration(options.mapping_uncertainty, "LED mapping uncertainty");
  const Clock::duration maximum_mapping_uncertainty = CheckedCeilClockDuration(
      options.maximum_mapping_uncertainty, "maximum LED mapping uncertainty");
  result.frame_delivery_latency_bound = options.frame_delivery_latency_bound;
  result.edge_tolerance = result.nominal_frame_interval + options.exposure_duration +
                          result.mapping_uncertainty + result.frame_delivery_latency_bound;

  const bool bracket_passed =
      result.schedule_bracket_width <= options.maximum_schedule_bracket_width;
  {
    std::ostringstream message;
    message << "schedule bracket width=" << Milliseconds(result.schedule_bracket_width)
            << " ms, maximum=" << Milliseconds(options.maximum_schedule_bracket_width) << " ms";
    AddCheck(&result, "schedule_ack_bracket_width", bracket_passed, message.str());
  }

  const bool mapping_passed = result.mapping_uncertainty <= maximum_mapping_uncertainty;
  {
    std::ostringstream message;
    message << "mapping uncertainty=" << Milliseconds(result.mapping_uncertainty)
            << " ms, maximum=" << Milliseconds(maximum_mapping_uncertainty) << " ms";
    AddCheck(&result, "mapper_uncertainty", mapping_passed, message.str());
  }

  AddCheck(&result, "pulse_detected", pulse.detected,
           pulse.detected ? "LED analyzer selected a pulse run" : pulse.diagnostic);
  if (!pulse.detected) {
    result.passed = false;
    return result;
  }
  (void)CountActiveFramesInAcceptedSpan(pulse);

  result.observed_start = selected_frame_host_times[pulse.pulse_start_frame_index];
  result.observed_end_exclusive =
      selected_frame_host_times[pulse.pulse_end_frame_index] + result.nominal_frame_interval;
  result.allowed_start_earliest = schedule.earliest_start - result.edge_tolerance;
  result.allowed_start_latest = schedule.latest_start + result.edge_tolerance;
  result.allowed_end_earliest =
      schedule.earliest_start + options.commanded_duration - result.edge_tolerance;
  result.allowed_end_latest =
      schedule.latest_start + options.commanded_duration + result.edge_tolerance;

  const bool start_passed = InsideInclusive(result.observed_start, result.allowed_start_earliest,
                                            result.allowed_start_latest);
  {
    std::ostringstream message;
    message << "observed pulse start is " << (start_passed ? "inside" : "outside")
            << " the commanded-start bracket with frame-phase, exposure, mapping, and delivery "
               "tolerance="
            << Milliseconds(result.edge_tolerance) << " ms";
    AddCheck(&result, "pulse_start_associated", start_passed, message.str());
  }

  const bool end_passed = InsideInclusive(result.observed_end_exclusive,
                                          result.allowed_end_earliest, result.allowed_end_latest);
  {
    std::ostringstream message;
    message << "observed pulse end is " << (end_passed ? "inside" : "outside")
            << " the commanded-end bracket with frame-phase, exposure, mapping, and delivery "
               "tolerance="
            << Milliseconds(result.edge_tolerance) << " ms";
    AddCheck(&result, "pulse_end_associated", end_passed, message.str());
  }

  result.passed = std::ranges::all_of(result.checks, &LedScheduleAssociationCheck::passed);
  return result;
}

std::size_t SelectAcceptedLedPeakFrame(const LedPulseResult &pulse) {
  (void)CountActiveFramesInAcceptedSpan(pulse);
  std::size_t selected = pulse.frames.size();
  double selected_response = 0.0;
  for (std::size_t index = pulse.pulse_start_frame_index; index <= pulse.pulse_end_frame_index;
       ++index) {
    const LedFrameDiagnostic &diagnostic = pulse.frames[index];
    if (!diagnostic.active) {
      continue;
    }
    if (selected == pulse.frames.size() || diagnostic.mean_positive_red_delta > selected_response) {
      selected = index;
      selected_response = diagnostic.mean_positive_red_delta;
    }
  }
  if (selected == pulse.frames.size()) {
    throw std::invalid_argument("detected LED pulse has no active peak frame");
  }
  return selected;
}

}  // namespace swing_capture::optical
