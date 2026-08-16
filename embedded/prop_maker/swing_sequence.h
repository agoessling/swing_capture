#ifndef SWING_CAPTURE_EMBEDDED_PROP_MAKER_SWING_SEQUENCE_H_
#define SWING_CAPTURE_EMBEDDED_PROP_MAKER_SWING_SEQUENCE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWING_HIL_SWING_START_LEAD_US 20000U
#define SWING_HIL_SWING_STEP_US 20000U
#define SWING_HIL_SWING_PRE_STEPS 60U
#define SWING_HIL_SWING_PRE_DURATION_US (SWING_HIL_SWING_STEP_US * SWING_HIL_SWING_PRE_STEPS)
#define SWING_HIL_SWING_IMPACT_WHITE_US 20000U
#define SWING_HIL_SWING_POST_STEPS 25U
#define SWING_HIL_SWING_POST_DURATION_US (SWING_HIL_SWING_STEP_US * SWING_HIL_SWING_POST_STEPS)
#define SWING_HIL_SWING_TONE_DURATION_US 10000U
#define SWING_HIL_SWING_TONE_FREQUENCY_HZ 2000U
#define SWING_HIL_SWING_TONE_LEVEL_PERMILLE 125U
#define SWING_HIL_SWING_MAX_LATENESS_US 2000U
#define SWING_HIL_SWING_MAX_IMPACT_COMMAND_DELTA_US 250U
#define SWING_HIL_SWING_COLOR_REFERENCE_BRIGHTNESS 128U
#define SWING_HIL_CALIBRATION_STEP_US 70000U
#define SWING_HIL_CALIBRATION_CANDIDATE_COUNT 8U
#define SWING_HIL_CALIBRATION_CANDIDATES_TEXT "1,2,3,4,6,8,12,16"
#define SWING_HIL_CALIBRATION_DURATION_US \
  (SWING_HIL_CALIBRATION_STEP_US * SWING_HIL_CALIBRATION_CANDIDATE_COUNT)
#define SWING_HIL_PREPARE_TIMEOUT_US 10000000ULL

typedef struct swing_hil_rgb8 {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
} swing_hil_rgb8;

typedef struct swing_hil_prepare_state {
  uint64_t expires_us;
  bool prepared;
} swing_hil_prepare_state;

swing_hil_rgb8 swing_hil_swing_pre_color(uint32_t step, uint8_t brightness);
swing_hil_rgb8 swing_hil_swing_post_color(uint32_t step, uint8_t brightness);
swing_hil_rgb8 swing_hil_swing_impact_color(uint8_t brightness);
uint32_t swing_hil_pack_fixture_neopixel_rgb(swing_hil_rgb8 color);
uint8_t swing_hil_calibration_candidate(uint32_t index);
bool swing_hil_swing_brightness_is_candidate(uint32_t brightness);
void swing_hil_prepare_reset(swing_hil_prepare_state *state);
void swing_hil_prepare_mark(swing_hil_prepare_state *state, uint64_t power_on_us);
bool swing_hil_prepare_consume(swing_hil_prepare_state *state, uint64_t now_us);
bool swing_hil_prepare_expire(swing_hil_prepare_state *state, uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif  // SWING_CAPTURE_EMBEDDED_PROP_MAKER_SWING_SEQUENCE_H_
