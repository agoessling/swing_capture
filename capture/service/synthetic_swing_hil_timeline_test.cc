#include "capture/service/synthetic_swing_hil_timeline.h"

#include <cassert>
#include <chrono>
#include <cstdint>

#include "capture/hil/feather_hil_controller.h"
#include "capture/optical/rgb_swing.h"
#include "embedded/prop_maker/swing_sequence.h"

namespace {

using namespace std::chrono_literals;

void TestHostMappingUsesBracketMidpoint() {
  const auto origin = std::chrono::steady_clock::time_point{} + 10s;
  const auto mapped =
      swing_capture::service::EstimateFeatherHostTime(1'000'000, origin, origin + 2ms, 1'020'000);
  assert(mapped == origin + 21ms);
}

void TestBuildsCalibrationSchedule() {
  swing_capture::hil::FeatherCalibrationReceipt receipt;
  receipt.accepted_device_microseconds = 1'000'000;
  receipt.step_microseconds = 70'000;
  receipt.candidates = {16, 24};
  receipt.steps.resize(2);
  receipt.steps[0].index = 0;
  receipt.steps[0].brightness = 16;
  receipt.steps[0].scheduled_device_microseconds = 1'020'000;
  receipt.steps[1].index = 1;
  receipt.steps[1].brightness = 24;
  receipt.steps[1].scheduled_device_microseconds = 1'090'000;
  receipt.host_command_sent = std::chrono::steady_clock::time_point{} + 10s;
  receipt.host_acknowledgement_received = std::chrono::steady_clock::time_point{} + 10s + 2ms;
  const auto schedule = swing_capture::service::BuildCalibrationRgbSchedule(
      receipt, std::chrono::steady_clock::time_point{} + 9s);
  assert(schedule.size() == 3U);
  assert(schedule[0].phase == swing_capture::optical::RgbSwingPhase::kBaseline);
  assert(schedule[1].brightness == 16U);
  const swing_capture::optical::Rgb8 white{255, 255, 255};
  assert(schedule[1].color == white);
  assert(schedule[2].color == white);
  assert(schedule[1].start == std::chrono::steady_clock::time_point{} + 10s + 21ms);
  assert(schedule[2].start - schedule[1].start == 70ms);
}

void TestBuildsScaledSwingSchedule() {
  swing_capture::hil::FeatherSwingReceipt receipt;
  receipt.brightness = 24;
  receipt.accepted_device_microseconds = 2'000'000;
  receipt.sequence_start_scheduled_device_microseconds = 2'020'000;
  receipt.impact_scheduled_device_microseconds = 2'020'000 + SWING_HIL_SWING_PRE_DURATION_US;
  receipt.post_scheduled_device_microseconds =
      2'020'000 + SWING_HIL_SWING_PRE_DURATION_US + SWING_HIL_SWING_IMPACT_WHITE_US;
  receipt.host_command_sent = std::chrono::steady_clock::time_point{} + 20s;
  receipt.host_acknowledgement_received = std::chrono::steady_clock::time_point{} + 20s + 2ms;
  const auto schedule = swing_capture::service::BuildSyntheticSwingRgbSchedule(
      receipt, std::chrono::steady_clock::time_point{} + 19s);
  assert(schedule.size() == 1U + SWING_HIL_SWING_PRE_STEPS + 1U + SWING_HIL_SWING_POST_STEPS);
  assert(schedule[1].phase == swing_capture::optical::RgbSwingPhase::kPreImpact);
  assert(schedule[1].brightness == 24U);
  assert(schedule[1].color.red == 255U || schedule[1].color.green == 255U ||
         schedule[1].color.blue == 255U);
  const std::size_t impact_index = 1U + SWING_HIL_SWING_PRE_STEPS;
  assert(schedule[impact_index].phase == swing_capture::optical::RgbSwingPhase::kImpact);
  const swing_capture::optical::Rgb8 expected_white{255, 255, 255};
  assert(schedule[impact_index].color == expected_white);
  assert(schedule.back().phase == swing_capture::optical::RgbSwingPhase::kPostImpact);
  assert(schedule.back().color.red == 255U || schedule.back().color.green == 255U ||
         schedule.back().color.blue == 255U);
}

}  // namespace

int main() {
  TestHostMappingUsesBracketMidpoint();
  TestBuildsCalibrationSchedule();
  TestBuildsScaledSwingSchedule();
  return 0;
}
