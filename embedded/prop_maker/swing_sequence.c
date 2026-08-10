#include "embedded/prop_maker/swing_sequence.h"

#include <stdint.h>

static swing_hil_rgb8 off(void) { return (swing_hil_rgb8){.red = 0U, .green = 0U, .blue = 0U}; }

static uint8_t wheel_level(uint32_t tenths, uint8_t brightness) {
  return (uint8_t)(((uint32_t)brightness * tenths + 5U) / 10U);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static swing_hil_rgb8 hue_wheel_color(uint32_t step, uint8_t brightness) {
  const uint32_t wheel_step = step % 60U;
  const uint32_t segment = wheel_step / 10U;
  const uint32_t position = wheel_step % 10U;
  const uint8_t rising = wheel_level(position, brightness);
  const uint8_t falling = wheel_level(10U - position, brightness);
  switch (segment) {
    case 0U:
      return (swing_hil_rgb8){.red = brightness, .green = rising, .blue = 0U};
    case 1U:
      return (swing_hil_rgb8){.red = falling, .green = brightness, .blue = 0U};
    case 2U:
      return (swing_hil_rgb8){.red = 0U, .green = brightness, .blue = rising};
    case 3U:
      return (swing_hil_rgb8){.red = 0U, .green = falling, .blue = brightness};
    case 4U:
      return (swing_hil_rgb8){.red = rising, .green = 0U, .blue = brightness};
    case 5U:
      return (swing_hil_rgb8){.red = brightness, .green = 0U, .blue = falling};
    default:
      return off();
  }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
swing_hil_rgb8 swing_hil_swing_pre_color(uint32_t step, uint8_t brightness) {
  if (step >= SWING_HIL_SWING_PRE_STEPS) {
    return off();
  }
  return hue_wheel_color(step, brightness);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
swing_hil_rgb8 swing_hil_swing_post_color(uint32_t step, uint8_t brightness) {
  if (step >= SWING_HIL_SWING_POST_STEPS) {
    return off();
  }
  return hue_wheel_color((35U + step * 2U) % 60U, brightness);
}

swing_hil_rgb8 swing_hil_swing_impact_color(uint8_t brightness) {
  return (swing_hil_rgb8){
      .red = brightness,
      .green = brightness,
      .blue = brightness,
  };
}

uint32_t swing_hil_pack_fixture_neopixel_rgb(swing_hil_rgb8 color) {
  return ((uint32_t)color.red << 24U) | ((uint32_t)color.green << 16U) |
         ((uint32_t)color.blue << 8U);
}

uint8_t swing_hil_calibration_candidate(uint32_t index) {
  static const uint8_t candidates[SWING_HIL_CALIBRATION_CANDIDATE_COUNT] = {
      1U, 2U, 3U, 4U, 6U, 8U, 12U, 16U,
  };
  return index < SWING_HIL_CALIBRATION_CANDIDATE_COUNT ? candidates[index] : 0U;
}

bool swing_hil_swing_brightness_is_candidate(uint32_t brightness) {
  for (uint32_t index = 0U; index < SWING_HIL_CALIBRATION_CANDIDATE_COUNT; ++index) {
    if (brightness == swing_hil_calibration_candidate(index)) {
      return true;
    }
  }
  return false;
}

void swing_hil_prepare_reset(swing_hil_prepare_state *state) {
  state->expires_us = 0U;
  state->prepared = false;
}

void swing_hil_prepare_mark(swing_hil_prepare_state *state, uint64_t power_on_us) {
  state->expires_us = power_on_us + SWING_HIL_PREPARE_TIMEOUT_US;
  state->prepared = true;
}

bool swing_hil_prepare_consume(swing_hil_prepare_state *state, uint64_t now_us) {
  if (!state->prepared || now_us >= state->expires_us) {
    swing_hil_prepare_reset(state);
    return false;
  }
  swing_hil_prepare_reset(state);
  return true;
}

bool swing_hil_prepare_expire(swing_hil_prepare_state *state, uint64_t now_us) {
  if (!state->prepared || now_us < state->expires_us) {
    return false;
  }
  swing_hil_prepare_reset(state);
  return true;
}
