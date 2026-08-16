#include "embedded/prop_maker/swing_sequence.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>

#include "embedded/prop_maker/board.h"

namespace {

std::uint8_t MaximumChannel(swing_hil_rgb8 color) {
  return std::max(color.red, std::max(color.green, color.blue));
}

void AssertAtSelectedBrightness(swing_hil_rgb8 color, std::uint8_t brightness) {
  assert(color.red <= brightness);
  assert(color.green <= brightness);
  assert(color.blue <= brightness);
  assert(MaximumChannel(color) == brightness);
}

void TestExactTimingContract() {
  static_assert(SWING_HIL_SWING_PRE_DURATION_US == 1'200'000U);
  static_assert(SWING_HIL_SWING_IMPACT_WHITE_US == 20'000U);
  static_assert(SWING_HIL_SWING_POST_DURATION_US == 500'000U);
  static_assert(SWING_HIL_SWING_TONE_DURATION_US == 10'000U);
  static_assert(SWING_HIL_SWING_TONE_FREQUENCY_HZ == 2'000U);
  static_assert(SWING_HIL_SWING_TONE_LEVEL_PERMILLE == 125U);
  static_assert(SWING_HIL_SWING_COLOR_REFERENCE_BRIGHTNESS == 128U);
  static_assert(SWING_HIL_CALIBRATION_DURATION_US == 560'000U);
  static_assert(SWING_HIL_PREPARE_TIMEOUT_US == 10'000'000ULL);
  static_assert(PROP_MAKER_EXTERNAL_NEOPIXEL_PIN == 21);
  static_assert(std::string_view(PROP_MAKER_FIXTURE_NEOPIXEL_COLOR_ORDER) == "rgb");
  static_assert(PROP_MAKER_EXTERNAL_POWER_PIN == 23);
  static_assert(PROP_MAKER_EXTERNAL_NEOPIXEL_PIN != PICO_DEFAULT_WS2812_PIN);
}

void TestFixtureNeoPixelRgbWirePacking() {
  assert(swing_hil_pack_fixture_neopixel_rgb({.red = 0x12U, .green = 0x34U, .blue = 0x56U}) ==
         0x12345600U);
  assert(swing_hil_pack_fixture_neopixel_rgb({.red = 0xffU, .green = 0U, .blue = 0U}) ==
         0xff000000U);
  assert(swing_hil_pack_fixture_neopixel_rgb({.red = 0U, .green = 0xffU, .blue = 0U}) ==
         0x00ff0000U);
  assert(swing_hil_pack_fixture_neopixel_rgb({.red = 0U, .green = 0U, .blue = 0xffU}) ==
         0x0000ff00U);
}

void TestExactPreImpactColors() {
  const swing_hil_rgb8 first = swing_hil_swing_pre_color(0, 128U);
  const swing_hil_rgb8 first_transition = swing_hil_swing_pre_color(20, 128U);
  const swing_hil_rgb8 second_transition = swing_hil_swing_pre_color(40, 128U);
  const swing_hil_rgb8 last = swing_hil_swing_pre_color(59, 128U);
  assert(first.red == 128U && first.green == 0U && first.blue == 0U);
  assert(first_transition.red == 0U && first_transition.green == 128U &&
         first_transition.blue == 0U);
  assert(second_transition.red == 0U && second_transition.green == 0U &&
         second_transition.blue == 128U);
  assert(last.red == 128U && last.green == 0U && last.blue == 13U);
  for (std::uint32_t step = 1; step < 10U; ++step) {
    assert(swing_hil_swing_pre_color(step, 128U).green >
           swing_hil_swing_pre_color(step - 1U, 128U).green);
  }
  const swing_hil_rgb8 after = swing_hil_swing_pre_color(SWING_HIL_SWING_PRE_STEPS, 128U);
  assert(after.red == 0U && after.green == 0U && after.blue == 0U);
}

void TestImpactAndPostImpactColors() {
  const swing_hil_rgb8 impact = swing_hil_swing_impact_color(16U);
  assert(impact.red == 16U);
  assert(impact.red == impact.green && impact.green == impact.blue);
  const swing_hil_rgb8 first = swing_hil_swing_post_color(0, 128U);
  const swing_hil_rgb8 last = swing_hil_swing_post_color(24, 128U);
  assert(first.red == 0U && first.green == 64U && first.blue == 128U);
  assert(last.red == 0U && last.green == 128U && last.blue == 38U);
  const swing_hil_rgb8 after = swing_hil_swing_post_color(SWING_HIL_SWING_POST_STEPS, 128U);
  assert(after.red == 0U && after.green == 0U && after.blue == 0U);
}

void TestEverySequenceColorHonorsSelectedBrightness() {
  for (std::uint32_t candidate_index = 0; candidate_index < SWING_HIL_CALIBRATION_CANDIDATE_COUNT;
       ++candidate_index) {
    const std::uint8_t brightness = swing_hil_calibration_candidate(candidate_index);
    for (std::uint32_t step = 0; step < SWING_HIL_SWING_PRE_STEPS; ++step) {
      const swing_hil_rgb8 color = swing_hil_swing_pre_color(step, brightness);
      AssertAtSelectedBrightness(color, brightness);
    }
    const swing_hil_rgb8 impact = swing_hil_swing_impact_color(brightness);
    AssertAtSelectedBrightness(impact, brightness);
    for (std::uint32_t step = 0; step < SWING_HIL_SWING_POST_STEPS; ++step) {
      const swing_hil_rgb8 color = swing_hil_swing_post_color(step, brightness);
      AssertAtSelectedBrightness(color, brightness);
    }
  }
  const swing_hil_rgb8 scaled_pre = swing_hil_swing_pre_color(0, 8U);
  const swing_hil_rgb8 scaled_post = swing_hil_swing_post_color(0, 8U);
  assert(scaled_pre.red == 8U);
  assert(scaled_post.green == 4U && scaled_post.blue == 8U);
}

void TestCalibrationCandidateContract() {
  constexpr std::array<std::uint8_t, SWING_HIL_CALIBRATION_CANDIDATE_COUNT> kExpected = {
      1U, 2U, 3U, 4U, 6U, 8U, 12U, 16U,
  };
  for (std::uint32_t index = 0; index < kExpected.size(); ++index) {
    assert(swing_hil_calibration_candidate(index) == kExpected[index]);
    assert(swing_hil_swing_brightness_is_candidate(kExpected[index]));
  }
  assert(swing_hil_calibration_candidate(kExpected.size()) == 0U);
  constexpr std::array<std::uint32_t, 7> kInvalidBrightnesses = {
      0U, 5U, 7U, 9U, 17U, 127U, 255U,
  };
  for (const std::uint32_t brightness : kInvalidBrightnesses) {
    assert(!swing_hil_swing_brightness_is_candidate(brightness));
  }
}

void TestPreparedRailIsBoundedAndConsumedOnce() {
  swing_hil_prepare_state state{};
  swing_hil_prepare_reset(&state);
  assert(!state.prepared);
  assert(!swing_hil_prepare_consume(&state, 99U));

  swing_hil_prepare_mark(&state, 1'000U);
  assert(state.prepared);
  assert(state.expires_us == 10'001'000U);
  assert(!swing_hil_prepare_expire(&state, state.expires_us - 1U));
  assert(state.prepared);
  assert(swing_hil_prepare_consume(&state, state.expires_us - 1U));
  assert(!state.prepared);
  assert(!swing_hil_prepare_consume(&state, state.expires_us - 1U));

  swing_hil_prepare_mark(&state, 2'000U);
  assert(!swing_hil_prepare_consume(&state, state.expires_us));
  assert(!state.prepared);

  swing_hil_prepare_mark(&state, 3'000U);
  assert(swing_hil_prepare_expire(&state, state.expires_us));
  assert(!state.prepared);
  assert(!swing_hil_prepare_expire(&state, state.expires_us + 1U));
}

}  // namespace

int main() {
  TestExactTimingContract();
  TestExactPreImpactColors();
  TestImpactAndPostImpactColors();
  TestFixtureNeoPixelRgbWirePacking();
  TestEverySequenceColorHonorsSelectedBrightness();
  TestCalibrationCandidateContract();
  TestPreparedRailIsBoundedAndConsumedOnce();
  return 0;
}
