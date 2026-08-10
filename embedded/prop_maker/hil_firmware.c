#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "embedded/prop_maker/board.h"
#include "embedded/prop_maker/hil_protocol.h"
#include "embedded/prop_maker/swing_sequence.h"

// Pico SDK inline helpers use parameters only in assertions, which disappear
// under -c opt. Keep that vendor-header warning from defeating first-party
// -Werror while leaving the warning enabled for this translation unit's code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "audio_i2s.pio.h"
#include "fixture_neopixel.pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/timer.h"
#include "pico/platform.h"
#include "pico/platform/common.h"
#include "pico/stdio.h"
#include "pico/time.h"
#include "pico/types.h"
#pragma GCC diagnostic pop

#define AUDIO_SAMPLE_RATE_HZ 32000U
#define AUDIO_WARMUP_SAMPLES 320U
#define AUDIO_POWER_UP_LEAD_US 15000U
#define AUDIO_MAX_SAMPLES \
  ((SWING_HIL_TONE_MAX_DURATION_US * (uint64_t)AUDIO_SAMPLE_RATE_HZ) / 1000000ULL)

typedef struct audio_output {
  PIO pio;
  uint state_machine;
  uint program_offset;
  int dma_channel;
  dma_channel_config dma_config;
} audio_output;

typedef struct fixture_neopixel_output {
  PIO pio;
  uint state_machine;
  uint program_offset;
} fixture_neopixel_output;

typedef struct calibration_evidence {
  uint64_t scheduled_us[SWING_HIL_CALIBRATION_CANDIDATE_COUNT];
  uint64_t device_us[SWING_HIL_CALIBRATION_CANDIDATE_COUNT];
  uint64_t end_us;
  uint64_t maximum_lateness_us;
} calibration_evidence;

typedef struct swing_evidence {
  uint64_t sequence_start_scheduled_us;
  uint64_t sequence_start_us;
  uint64_t impact_scheduled_us;
  uint64_t impact_us;
  uint64_t white_command_us;
  uint64_t tone_command_us;
  uint64_t tone_end_us;
  uint64_t white_end_us;
  uint64_t post_scheduled_us;
  uint64_t post_start_us;
  uint64_t end_us;
  uint64_t pre_maximum_lateness_us;
  uint64_t post_maximum_lateness_us;
} swing_evidence;

static const uint32_t silent_samples[AUDIO_WARMUP_SAMPLES] = {0};

static uint32_t *tone_sample_storage(void) {
  // DMA requires bounded, writable storage with static lifetime; keeping it
  // function-local avoids exposing mutable state throughout the firmware.
  static uint32_t samples[AUDIO_MAX_SAMPLES];
  return samples;
}

static void initialize_output_pin_low(uint pin) {
  gpio_init(pin);
  gpio_set_dir(pin, (bool)GPIO_OUT);
  gpio_put(pin, false);
}

static void initialize_audio(audio_output *output) {
  initialize_output_pin_low(PROP_MAKER_EXTERNAL_POWER_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_DATA_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_BIT_CLOCK_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_WORD_SELECT_PIN);

  output->pio = pio0;
  output->state_machine = pio_claim_unused_sm(output->pio, true);
  output->program_offset = (uint)pio_add_program(output->pio, &swing_audio_i2s_program);

  gpio_set_function(PROP_MAKER_I2S_DATA_PIN, GPIO_FUNC_PIO0);
  gpio_set_function(PROP_MAKER_I2S_BIT_CLOCK_PIN, GPIO_FUNC_PIO0);
  gpio_set_function(PROP_MAKER_I2S_WORD_SELECT_PIN, GPIO_FUNC_PIO0);
  swing_audio_i2s_program_init(output->pio, output->state_machine, output->program_offset,
                               PROP_MAKER_I2S_DATA_PIN, PROP_MAKER_I2S_BIT_CLOCK_PIN);

  const uint32_t divider_fixed8 = clock_get_hz(clk_sys) * 4U / AUDIO_SAMPLE_RATE_HZ;
  pio_sm_set_clkdiv_int_frac(output->pio, output->state_machine, divider_fixed8 >> 8U,
                             (uint8_t)(divider_fixed8 & 0xffU));

  output->dma_channel = dma_claim_unused_channel(true);
  output->dma_config = dma_channel_get_default_config((uint)output->dma_channel);
  channel_config_set_transfer_data_size(&output->dma_config, DMA_SIZE_32);
  channel_config_set_read_increment(&output->dma_config, true);
  channel_config_set_write_increment(&output->dma_config, false);
  channel_config_set_dreq(&output->dma_config,
                          pio_get_dreq(output->pio, output->state_machine, true));

  pio_sm_set_enabled(output->pio, output->state_machine, false);
  initialize_output_pin_low(PROP_MAKER_I2S_DATA_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_BIT_CLOCK_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_WORD_SELECT_PIN);
}

static void initialize_fixture_neopixel(fixture_neopixel_output *output) {
  output->pio = pio1;
  output->state_machine = pio_claim_unused_sm(output->pio, true);
  output->program_offset = (uint)pio_add_program(output->pio, &fixture_neopixel_program);
  pio_gpio_init(output->pio, PROP_MAKER_EXTERNAL_NEOPIXEL_PIN);
  pio_sm_set_consecutive_pindirs(output->pio, output->state_machine,
                                 PROP_MAKER_EXTERNAL_NEOPIXEL_PIN, 1, true);
  pio_sm_config config = fixture_neopixel_program_get_default_config(output->program_offset);
  sm_config_set_sideset_pins(&config, PROP_MAKER_EXTERNAL_NEOPIXEL_PIN);
  sm_config_set_out_shift(&config, false, true, 24);
  sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
  const float divider = (float)clock_get_hz(clk_sys) / (800000.0F * 10.0F);
  sm_config_set_clkdiv(&config, divider);
  pio_sm_init(output->pio, output->state_machine, output->program_offset, &config);
  pio_sm_set_enabled(output->pio, output->state_machine, true);
}

static void fixture_neopixel_set(fixture_neopixel_output *output, swing_hil_rgb8 color) {
  pio_sm_put_blocking(output->pio, output->state_machine,
                      swing_hil_pack_fixture_neopixel_rgb(color));
}

static void fixture_neopixel_off(fixture_neopixel_output *output) {
  fixture_neopixel_set(output, (swing_hil_rgb8){.red = 0U, .green = 0U, .blue = 0U});
  busy_wait_us_32(80U);
}

static void initialize_audio_once(audio_output *output, bool *initialized) {
  if (*initialized) {
    return;
  }
  initialize_audio(output);
  *initialized = true;
}

static void initialize_fixture_neopixel_once(fixture_neopixel_output *output, bool *initialized) {
  if (*initialized) {
    return;
  }
  initialize_fixture_neopixel(output);
  // The PIO sideset pin stalls low with an empty FIFO. Do not clock an OFF
  // word into an unpowered pixel; the prepare path latches OFF immediately
  // after GPIO23 rises.
  *initialized = true;
}

static void audio_start(audio_output *output) {
  gpio_set_function(PROP_MAKER_I2S_DATA_PIN, GPIO_FUNC_PIO0);
  gpio_set_function(PROP_MAKER_I2S_BIT_CLOCK_PIN, GPIO_FUNC_PIO0);
  gpio_set_function(PROP_MAKER_I2S_WORD_SELECT_PIN, GPIO_FUNC_PIO0);
  pio_sm_set_enabled(output->pio, output->state_machine, false);
  pio_sm_clear_fifos(output->pio, output->state_machine);
  pio_sm_restart(output->pio, output->state_machine);
  pio_sm_exec(output->pio, output->state_machine,
              pio_encode_jmp(output->program_offset + swing_audio_i2s_offset_entry_point));
  pio_sm_set_enabled(output->pio, output->state_machine, true);
}

static void audio_transfer(audio_output *output, const uint32_t *samples, uint32_t count) {
  dma_channel_configure((uint)output->dma_channel, &output->dma_config,
                        &output->pio->txf[output->state_machine], samples, count, true);
}

static void audio_wait_until_drained(audio_output *output) {
  dma_channel_wait_for_finish_blocking((uint)output->dma_channel);
  while (!pio_sm_is_tx_fifo_empty(output->pio, output->state_machine)) {
    tight_loop_contents();
  }
  // The FIFO can become empty while one 32-bit stereo sample is still in the
  // output shift register. One hundred microseconds covers more than three
  // complete samples at 32 kHz.
  busy_wait_us_32(100U);
}

static void audio_quiesce(audio_output *output) {
  pio_sm_set_enabled(output->pio, output->state_machine, false);
  initialize_output_pin_low(PROP_MAKER_I2S_DATA_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_BIT_CLOCK_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_WORD_SELECT_PIN);
}

static void audio_power_down(audio_output *output) {
  gpio_put(PROP_MAKER_EXTERNAL_POWER_PIN, false);
  audio_quiesce(output);
}

static void synthetic_fixture_shutdown(audio_output *audio, fixture_neopixel_output *fixture_pixel,
                                       swing_hil_prepare_state *prepare_state) {
  fixture_neopixel_off(fixture_pixel);
  audio_quiesce(audio);
  gpio_put(PROP_MAKER_EXTERNAL_POWER_PIN, false);
  swing_hil_prepare_reset(prepare_state);
}

static void synthetic_fixture_reset_while_unpowered(audio_output *audio,
                                                    swing_hil_prepare_state *prepare_state) {
  audio_quiesce(audio);
  gpio_put(PROP_MAKER_EXTERNAL_POWER_PIN, false);
  swing_hil_prepare_reset(prepare_state);
}

static uint64_t synthetic_fixture_power_on(audio_output *audio,
                                           fixture_neopixel_output *fixture_pixel) {
  audio_start(audio);
  audio_transfer(audio, silent_samples, AUDIO_WARMUP_SAMPLES);
  gpio_put(PROP_MAKER_EXTERNAL_POWER_PIN, true);
  const uint64_t power_on_us = time_us_64();
  fixture_neopixel_off(fixture_pixel);
  audio_wait_until_drained(audio);
  audio_quiesce(audio);
  return power_on_us;
}

static uint32_t prepare_tone(const swing_hil_tone_command *command) {
  uint32_t *samples = tone_sample_storage();
  uint32_t sample_count =
      (uint32_t)((command->duration_us * (uint64_t)AUDIO_SAMPLE_RATE_HZ + 999999ULL) / 1000000ULL);
  if (sample_count > AUDIO_MAX_SAMPLES) {
    sample_count = AUDIO_MAX_SAMPLES;
  }

  const int32_t amplitude = (INT16_MAX * (int32_t)command->level_permille) / 1000;
  int32_t sample = amplitude;
  uint32_t half_cycle_phase = 0U;
  for (uint32_t index = 0U; index < sample_count; ++index) {
    const uint32_t packed_sample = (uint32_t)(uint16_t)(int16_t)sample;
    samples[index] = (packed_sample << 16U) | packed_sample;
    half_cycle_phase += command->frequency_hz;
    if (half_cycle_phase >= AUDIO_SAMPLE_RATE_HZ / 2U) {
      half_cycle_phase -= AUDIO_SAMPLE_RATE_HZ / 2U;
      sample = -sample;
    }
  }
  return sample_count;
}

static unsigned long long printable_time(uint64_t value) { return (unsigned long long)value; }

static uint64_t lateness_us(uint64_t scheduled_us, uint64_t actual_us) {
  return actual_us > scheduled_us ? actual_us - scheduled_us : 0U;
}

static uint64_t absolute_delta_us(uint64_t first_us, uint64_t second_us) {
  return first_us > second_us ? first_us - second_us : second_us - first_us;
}

static void update_maximum(uint64_t value, uint64_t *maximum) {
  if (value > *maximum) {
    *maximum = value;
  }
}

static void respond_error(uint32_t request_id, swing_hil_error error) {
  printf(SWING_HIL_PROTOCOL_PREFIX " %lu ERR %s device_us=%llu\n", (unsigned long)request_id,
         swing_hil_error_code(error), printable_time(time_us_64()));
  stdio_flush();
}

static void respond_runtime_error(uint32_t request_id, const char *code) {
  printf(SWING_HIL_PROTOCOL_PREFIX " %lu ERR %s device_us=%llu\n", (unsigned long)request_id, code,
         printable_time(time_us_64()));
  stdio_flush();
}

static void respond_query(uint32_t request_id) {
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu OK QUERY firmware=" SWING_HIL_FIRMWARE_VERSION
         " protocol=1 capabilities=query,led,tone,calibrate,swing lead_max_us=%u"
         " led_duration_min_us=%u led_duration_max_us=%u"
         " tone_lead_min_us=%u tone_duration_min_us=%u tone_duration_max_us=%u"
         " tone_frequency_min_hz=%u tone_frequency_max_hz=%u"
         " tone_level_min_permille=%u tone_level_max_permille=%u"
         " swing_start_lead_us=%u swing_step_us=%u swing_pre_steps=%u swing_white_us=%u"
         " swing_post_steps=%u swing_tone_duration_us=%u swing_tone_frequency_hz=%u"
         " swing_tone_level_permille=%u swing_lateness_max_us=%u"
         " swing_impact_delta_max_us=%u swing_color_reference_brightness=%u"
         " calibration_step_us=%u calibration_count=%u"
         " calibration_candidates=" SWING_HIL_CALIBRATION_CANDIDATES_TEXT
         " fixture_neopixel_gpio=%u"
         " fixture_neopixel_color_order=" PROP_MAKER_FIXTURE_NEOPIXEL_COLOR_ORDER
         " shared_power_gpio=%u prepare_timeout_us=%llu"
         " device_us=%llu\n",
         (unsigned long)request_id, SWING_HIL_MAX_LEAD_US, SWING_HIL_LED_MIN_DURATION_US,
         SWING_HIL_LED_MAX_DURATION_US, SWING_HIL_TONE_MIN_LEAD_US, SWING_HIL_TONE_MIN_DURATION_US,
         SWING_HIL_TONE_MAX_DURATION_US, SWING_HIL_TONE_MIN_FREQUENCY_HZ,
         SWING_HIL_TONE_MAX_FREQUENCY_HZ, SWING_HIL_TONE_MIN_LEVEL_PERMILLE,
         SWING_HIL_TONE_MAX_LEVEL_PERMILLE, SWING_HIL_SWING_START_LEAD_US, SWING_HIL_SWING_STEP_US,
         SWING_HIL_SWING_PRE_STEPS, SWING_HIL_SWING_IMPACT_WHITE_US, SWING_HIL_SWING_POST_STEPS,
         SWING_HIL_SWING_TONE_DURATION_US, SWING_HIL_SWING_TONE_FREQUENCY_HZ,
         SWING_HIL_SWING_TONE_LEVEL_PERMILLE, SWING_HIL_SWING_MAX_LATENESS_US,
         SWING_HIL_SWING_MAX_IMPACT_COMMAND_DELTA_US, SWING_HIL_SWING_COLOR_REFERENCE_BRIGHTNESS,
         SWING_HIL_CALIBRATION_STEP_US, SWING_HIL_CALIBRATION_CANDIDATE_COUNT,
         PROP_MAKER_EXTERNAL_NEOPIXEL_PIN, PROP_MAKER_EXTERNAL_POWER_PIN,
         printable_time(SWING_HIL_PREPARE_TIMEOUT_US), printable_time(time_us_64()));
  stdio_flush();
}

static void run_led(const swing_hil_command *command) {
  const uint64_t accepted_us = time_us_64();
  const uint64_t scheduled_us = accepted_us + command->parameters.led.lead_us;
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu ACK LED accepted_us=%llu scheduled_us=%llu lead_us=%lu duration_us=%lu\n",
         (unsigned long)command->request_id, printable_time(accepted_us),
         printable_time(scheduled_us), (unsigned long)command->parameters.led.lead_us,
         (unsigned long)command->parameters.led.duration_us);
  stdio_flush();

  sleep_until(from_us_since_boot(scheduled_us));
  const uint64_t start_us = time_us_64();
  gpio_put(PICO_DEFAULT_LED_PIN, true);
  busy_wait_until(from_us_since_boot(start_us + command->parameters.led.duration_us));
  gpio_put(PICO_DEFAULT_LED_PIN, false);
  const uint64_t end_us = time_us_64();

  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu EVENT LED START scheduled_us=%llu device_us=%llu lateness_us=%llu\n",
         (unsigned long)command->request_id, printable_time(scheduled_us), printable_time(start_us),
         printable_time(lateness_us(scheduled_us, start_us)));
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu EVENT LED DONE start_us=%llu end_us=%llu elapsed_us=%llu requested_us=%lu\n",
         (unsigned long)command->request_id, printable_time(start_us), printable_time(end_us),
         printable_time(end_us - start_us), (unsigned long)command->parameters.led.duration_us);
  stdio_flush();
}

static void run_tone(audio_output *output, const swing_hil_command *command) {
  const uint32_t sample_count = prepare_tone(&command->parameters.tone);
  const uint64_t accepted_us = time_us_64();
  const uint64_t scheduled_us = accepted_us + command->parameters.tone.lead_us;
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu ACK TONE accepted_us=%llu scheduled_us=%llu lead_us=%lu duration_us=%lu"
         " frequency_hz=%lu level_permille=%lu sample_rate_hz=%u sample_count=%lu\n",
         (unsigned long)command->request_id, printable_time(accepted_us),
         printable_time(scheduled_us), (unsigned long)command->parameters.tone.lead_us,
         (unsigned long)command->parameters.tone.duration_us,
         (unsigned long)command->parameters.tone.frequency_hz,
         (unsigned long)command->parameters.tone.level_permille, AUDIO_SAMPLE_RATE_HZ,
         (unsigned long)sample_count);
  stdio_flush();

  sleep_until(from_us_since_boot(scheduled_us - AUDIO_POWER_UP_LEAD_US));
  audio_start(output);
  audio_transfer(output, silent_samples, AUDIO_WARMUP_SAMPLES);
  gpio_put(PROP_MAKER_EXTERNAL_POWER_PIN, true);
  audio_wait_until_drained(output);
  sleep_until(from_us_since_boot(scheduled_us));

  const uint64_t start_us = time_us_64();
  audio_transfer(output, tone_sample_storage(), sample_count);
  audio_wait_until_drained(output);
  const uint64_t end_us = time_us_64();
  audio_power_down(output);

  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu EVENT TONE START scheduled_us=%llu device_us=%llu lateness_us=%llu\n",
         (unsigned long)command->request_id, printable_time(scheduled_us), printable_time(start_us),
         printable_time(lateness_us(scheduled_us, start_us)));
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu EVENT TONE DONE start_us=%llu end_us=%llu elapsed_us=%llu requested_us=%lu"
         " sample_rate_hz=%u sample_count=%lu\n",
         (unsigned long)command->request_id, printable_time(start_us), printable_time(end_us),
         printable_time(end_us - start_us), (unsigned long)command->parameters.tone.duration_us,
         AUDIO_SAMPLE_RATE_HZ, (unsigned long)sample_count);
  stdio_flush();
}

static void run_calibration(audio_output *audio, fixture_neopixel_output *fixture_pixel,
                            swing_hil_prepare_state *prepare_state,
                            const swing_hil_command *command) {
  const uint64_t accepted_us = time_us_64();
  const uint64_t start_scheduled_us = accepted_us + SWING_HIL_SWING_START_LEAD_US;
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu ACK CALIBRATE accepted_us=%llu start_scheduled_us=%llu start_lead_us=%u"
         " step_us=%u step_count=%u duration_us=%u"
         " candidates=" SWING_HIL_CALIBRATION_CANDIDATES_TEXT
         " fixture_neopixel_gpio=%u shared_power_gpio=%u prepare_timeout_us=%llu\n",
         (unsigned long)command->request_id, printable_time(accepted_us),
         printable_time(start_scheduled_us), SWING_HIL_SWING_START_LEAD_US,
         SWING_HIL_CALIBRATION_STEP_US, SWING_HIL_CALIBRATION_CANDIDATE_COUNT,
         SWING_HIL_CALIBRATION_DURATION_US, PROP_MAKER_EXTERNAL_NEOPIXEL_PIN,
         PROP_MAKER_EXTERNAL_POWER_PIN, printable_time(SWING_HIL_PREPARE_TIMEOUT_US));
  stdio_flush();

  const uint64_t power_on_us = synthetic_fixture_power_on(audio, fixture_pixel);

  calibration_evidence evidence = {0};
  for (uint32_t step = 0U; step < SWING_HIL_CALIBRATION_CANDIDATE_COUNT; ++step) {
    evidence.scheduled_us[step] =
        start_scheduled_us + (uint64_t)step * SWING_HIL_CALIBRATION_STEP_US;
    sleep_until(from_us_since_boot(evidence.scheduled_us[step]));
    evidence.device_us[step] = time_us_64();
    const uint64_t step_lateness =
        lateness_us(evidence.scheduled_us[step], evidence.device_us[step]);
    update_maximum(step_lateness, &evidence.maximum_lateness_us);
    if (step_lateness > SWING_HIL_SWING_MAX_LATENESS_US) {
      synthetic_fixture_shutdown(audio, fixture_pixel, prepare_state);
      respond_runtime_error(command->request_id, "timing");
      return;
    }
    const uint8_t brightness = swing_hil_calibration_candidate(step);
    fixture_neopixel_set(
        fixture_pixel,
        (swing_hil_rgb8){.red = brightness, .green = brightness, .blue = brightness});
  }
  busy_wait_until(
      from_us_since_boot(evidence.device_us[0] + (uint64_t)SWING_HIL_CALIBRATION_STEP_US *
                                                     SWING_HIL_CALIBRATION_CANDIDATE_COUNT));
  fixture_neopixel_off(fixture_pixel);
  evidence.end_us = time_us_64();
  swing_hil_prepare_mark(prepare_state, power_on_us);

  for (uint32_t step = 0U; step < SWING_HIL_CALIBRATION_CANDIDATE_COUNT; ++step) {
    printf(SWING_HIL_PROTOCOL_PREFIX
           " %lu EVENT CALIBRATE STEP index=%lu brightness=%u scheduled_us=%llu"
           " device_us=%llu lateness_us=%llu\n",
           (unsigned long)command->request_id, (unsigned long)step,
           swing_hil_calibration_candidate(step), printable_time(evidence.scheduled_us[step]),
           printable_time(evidence.device_us[step]),
           printable_time(lateness_us(evidence.scheduled_us[step], evidence.device_us[step])));
  }
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu EVENT CALIBRATE DONE start_us=%llu end_us=%llu elapsed_us=%llu requested_us=%u"
         " max_step_lateness_us=%llu power_on_us=%llu prepared_until_us=%llu"
         " pixel_off=1 i2s_inactive=1 rail_powered=1 prepared=1"
         " fixture_neopixel_gpio=%u shared_power_gpio=%u\n",
         (unsigned long)command->request_id, printable_time(evidence.device_us[0]),
         printable_time(evidence.end_us), printable_time(evidence.end_us - evidence.device_us[0]),
         SWING_HIL_CALIBRATION_DURATION_US, printable_time(evidence.maximum_lateness_us),
         printable_time(power_on_us), printable_time(prepare_state->expires_us),
         PROP_MAKER_EXTERNAL_NEOPIXEL_PIN, PROP_MAKER_EXTERNAL_POWER_PIN);
  stdio_flush();
}

static const char *run_swing_pre_sequence(fixture_neopixel_output *fixture_pixel,
                                          const swing_hil_command *command,
                                          swing_evidence *evidence) {
  for (uint32_t step = 0U; step < SWING_HIL_SWING_PRE_STEPS; ++step) {
    const uint64_t scheduled_us =
        evidence->sequence_start_scheduled_us + (uint64_t)step * SWING_HIL_SWING_STEP_US;
    sleep_until(from_us_since_boot(scheduled_us));
    const uint64_t device_us = time_us_64();
    const uint64_t step_lateness = lateness_us(scheduled_us, device_us);
    update_maximum(step_lateness, &evidence->pre_maximum_lateness_us);
    if (step_lateness > SWING_HIL_SWING_MAX_LATENESS_US) {
      return "timing";
    }
    if (step == 0U) {
      evidence->sequence_start_us = device_us;
    }
    fixture_neopixel_set(fixture_pixel, swing_hil_swing_pre_color(
                                            step, (uint8_t)command->parameters.swing.brightness));
  }
  return NULL;
}

static const char *run_swing_impact(audio_output *audio, fixture_neopixel_output *fixture_pixel,
                                    const swing_hil_command *command, uint32_t tone_sample_count,
                                    swing_evidence *evidence) {
  sleep_until(from_us_since_boot(evidence->impact_scheduled_us - AUDIO_POWER_UP_LEAD_US));
  audio_start(audio);
  audio_transfer(audio, silent_samples, AUDIO_WARMUP_SAMPLES);
  audio_wait_until_drained(audio);
  sleep_until(from_us_since_boot(evidence->impact_scheduled_us));

  evidence->white_command_us = time_us_64();
  evidence->impact_us = evidence->white_command_us;
  fixture_neopixel_set(fixture_pixel,
                       swing_hil_swing_impact_color((uint8_t)command->parameters.swing.brightness));
  evidence->tone_command_us = time_us_64();
  if (lateness_us(evidence->impact_scheduled_us, evidence->impact_us) >
      SWING_HIL_SWING_MAX_LATENESS_US) {
    return "timing";
  }
  if (absolute_delta_us(evidence->white_command_us, evidence->tone_command_us) >
      SWING_HIL_SWING_MAX_IMPACT_COMMAND_DELTA_US) {
    return "impact_delta";
  }
  audio_transfer(audio, tone_sample_storage(), tone_sample_count);
  audio_wait_until_drained(audio);
  evidence->tone_end_us = time_us_64();
  audio_quiesce(audio);
  busy_wait_until(from_us_since_boot(evidence->white_command_us + SWING_HIL_SWING_IMPACT_WHITE_US));
  evidence->white_end_us = time_us_64();
  return NULL;
}

static const char *run_swing_post_sequence(fixture_neopixel_output *fixture_pixel,
                                           const swing_hil_command *command,
                                           swing_evidence *evidence) {
  evidence->post_start_us = evidence->white_end_us;
  for (uint32_t step = 0U; step < SWING_HIL_SWING_POST_STEPS; ++step) {
    const uint64_t scheduled_us =
        evidence->post_start_us + (uint64_t)step * SWING_HIL_SWING_STEP_US;
    busy_wait_until(from_us_since_boot(scheduled_us));
    const uint64_t device_us = time_us_64();
    const uint64_t step_lateness = lateness_us(scheduled_us, device_us);
    update_maximum(step_lateness, &evidence->post_maximum_lateness_us);
    if (step_lateness > SWING_HIL_SWING_MAX_LATENESS_US) {
      return "timing";
    }
    fixture_neopixel_set(fixture_pixel, swing_hil_swing_post_color(
                                            step, (uint8_t)command->parameters.swing.brightness));
  }
  busy_wait_until(from_us_since_boot(evidence->post_start_us + (uint64_t)SWING_HIL_SWING_STEP_US *
                                                                   SWING_HIL_SWING_POST_STEPS));
  return NULL;
}

static void emit_swing_evidence(const swing_hil_command *command, const swing_evidence *evidence,
                                uint32_t tone_sample_count) {
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu EVENT SWING PHASE phase=pre scheduled_us=%llu device_us=%llu lateness_us=%llu"
         " max_step_lateness_us=%llu step_us=%u step_count=%u\n",
         (unsigned long)command->request_id, printable_time(evidence->sequence_start_scheduled_us),
         printable_time(evidence->sequence_start_us),
         printable_time(
             lateness_us(evidence->sequence_start_scheduled_us, evidence->sequence_start_us)),
         printable_time(evidence->pre_maximum_lateness_us), SWING_HIL_SWING_STEP_US,
         SWING_HIL_SWING_PRE_STEPS);
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu EVENT SWING IMPACT scheduled_us=%llu device_us=%llu lateness_us=%llu"
         " white_command_us=%llu tone_command_us=%llu command_delta_us=%llu"
         " white_end_us=%llu tone_end_us=%llu white_us=%u tone_duration_us=%u"
         " tone_sample_count=%lu brightness=%lu\n",
         (unsigned long)command->request_id, printable_time(evidence->impact_scheduled_us),
         printable_time(evidence->impact_us),
         printable_time(lateness_us(evidence->impact_scheduled_us, evidence->impact_us)),
         printable_time(evidence->white_command_us), printable_time(evidence->tone_command_us),
         printable_time(absolute_delta_us(evidence->white_command_us, evidence->tone_command_us)),
         printable_time(evidence->white_end_us), printable_time(evidence->tone_end_us),
         SWING_HIL_SWING_IMPACT_WHITE_US, SWING_HIL_SWING_TONE_DURATION_US,
         (unsigned long)tone_sample_count, (unsigned long)command->parameters.swing.brightness);
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu EVENT SWING PHASE phase=post scheduled_us=%llu device_us=%llu lateness_us=%llu"
         " max_step_lateness_us=%llu step_us=%u step_count=%u\n",
         (unsigned long)command->request_id, printable_time(evidence->post_scheduled_us),
         printable_time(evidence->post_start_us),
         printable_time(lateness_us(evidence->post_scheduled_us, evidence->post_start_us)),
         printable_time(evidence->post_maximum_lateness_us), SWING_HIL_SWING_STEP_US,
         SWING_HIL_SWING_POST_STEPS);
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu EVENT SWING DONE sequence_start_us=%llu impact_us=%llu post_start_us=%llu"
         " end_us=%llu elapsed_us=%llu pre_us=%u post_us=%u outputs_inactive=1"
         " pixel_off=1 i2s_inactive=1 rail_powered=0 prepared=0"
         " fixture_neopixel_gpio=%u shared_power_gpio=%u\n",
         (unsigned long)command->request_id, printable_time(evidence->sequence_start_us),
         printable_time(evidence->impact_us), printable_time(evidence->post_start_us),
         printable_time(evidence->end_us),
         printable_time(evidence->end_us - evidence->sequence_start_us),
         SWING_HIL_SWING_PRE_DURATION_US, SWING_HIL_SWING_POST_DURATION_US,
         PROP_MAKER_EXTERNAL_NEOPIXEL_PIN, PROP_MAKER_EXTERNAL_POWER_PIN);
  stdio_flush();
}

static void run_swing(audio_output *audio, fixture_neopixel_output *fixture_pixel,
                      swing_hil_prepare_state *prepare_state, const swing_hil_command *command) {
  if (!swing_hil_prepare_consume(prepare_state, time_us_64())) {
    synthetic_fixture_reset_while_unpowered(audio, prepare_state);
    respond_runtime_error(command->request_id, "not_prepared");
    return;
  }
  const swing_hil_tone_command tone = {
      .lead_us = SWING_HIL_SWING_START_LEAD_US,
      .duration_us = SWING_HIL_SWING_TONE_DURATION_US,
      .frequency_hz = SWING_HIL_SWING_TONE_FREQUENCY_HZ,
      .level_permille = SWING_HIL_SWING_TONE_LEVEL_PERMILLE,
  };
  const uint32_t tone_sample_count = prepare_tone(&tone);
  const uint64_t accepted_us = time_us_64();
  swing_evidence evidence = {0};
  evidence.sequence_start_scheduled_us = accepted_us + SWING_HIL_SWING_START_LEAD_US;
  evidence.impact_scheduled_us = evidence.sequence_start_scheduled_us +
                                 (uint64_t)SWING_HIL_SWING_STEP_US * SWING_HIL_SWING_PRE_STEPS;
  evidence.post_scheduled_us = evidence.impact_scheduled_us + SWING_HIL_SWING_IMPACT_WHITE_US;
  const uint64_t end_scheduled_us =
      evidence.post_scheduled_us + (uint64_t)SWING_HIL_SWING_STEP_US * SWING_HIL_SWING_POST_STEPS;
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu ACK SWING accepted_us=%llu sequence_start_scheduled_us=%llu"
         " impact_scheduled_us=%llu post_scheduled_us=%llu end_scheduled_us=%llu"
         " start_lead_us=%u step_us=%u pre_steps=%u white_us=%u post_steps=%u"
         " tone_duration_us=%u tone_frequency_hz=%u tone_level_permille=%u"
         " tone_sample_rate_hz=%u tone_sample_count=%lu brightness=%lu"
         " prepared=1 rail_powered=1 fixture_neopixel_gpio=%u shared_power_gpio=%u\n",
         (unsigned long)command->request_id, printable_time(accepted_us),
         printable_time(evidence.sequence_start_scheduled_us),
         printable_time(evidence.impact_scheduled_us), printable_time(evidence.post_scheduled_us),
         printable_time(end_scheduled_us), SWING_HIL_SWING_START_LEAD_US, SWING_HIL_SWING_STEP_US,
         SWING_HIL_SWING_PRE_STEPS, SWING_HIL_SWING_IMPACT_WHITE_US, SWING_HIL_SWING_POST_STEPS,
         SWING_HIL_SWING_TONE_DURATION_US, SWING_HIL_SWING_TONE_FREQUENCY_HZ,
         SWING_HIL_SWING_TONE_LEVEL_PERMILLE, AUDIO_SAMPLE_RATE_HZ,
         (unsigned long)tone_sample_count, (unsigned long)command->parameters.swing.brightness,
         PROP_MAKER_EXTERNAL_NEOPIXEL_PIN, PROP_MAKER_EXTERNAL_POWER_PIN);
  stdio_flush();

  const char *error = run_swing_pre_sequence(fixture_pixel, command, &evidence);
  if (error == NULL) {
    error = run_swing_impact(audio, fixture_pixel, command, tone_sample_count, &evidence);
  }
  if (error == NULL) {
    error = run_swing_post_sequence(fixture_pixel, command, &evidence);
  }
  if (error != NULL) {
    synthetic_fixture_shutdown(audio, fixture_pixel, prepare_state);
    respond_runtime_error(command->request_id, error);
    return;
  }
  synthetic_fixture_shutdown(audio, fixture_pixel, prepare_state);
  evidence.end_us = time_us_64();
  emit_swing_evidence(command, &evidence, tone_sample_count);
}

int main(void) {
  initialize_output_pin_low(PICO_DEFAULT_LED_PIN);
  initialize_output_pin_low(PROP_MAKER_EXTERNAL_POWER_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_DATA_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_BIT_CLOCK_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_WORD_SELECT_PIN);
  stdio_init_all();

  fixture_neopixel_output fixture_neopixel = {0};
  bool fixture_neopixel_initialized = false;

  audio_output audio = {0};
  bool audio_initialized = false;
  swing_hil_prepare_state prepare_state = {0};
  swing_hil_prepare_reset(&prepare_state);

  printf(SWING_HIL_PROTOCOL_PREFIX " 0 EVENT BOOT firmware=" SWING_HIL_FIRMWARE_VERSION
                                   " device_us=%llu\n",
         printable_time(time_us_64()));
  stdio_flush();

  swing_hil_line_parser parser = {0};
  swing_hil_line_parser_init(&parser);
  while (true) {
    if (swing_hil_prepare_expire(&prepare_state, time_us_64())) {
      synthetic_fixture_shutdown(&audio, &fixture_neopixel, &prepare_state);
    }
    const int character = getchar_timeout_us(1000U);
    if (character < 0) {
      continue;
    }

    const swing_hil_parser_event event =
        swing_hil_line_parser_feed(&parser, (char)(unsigned char)character);
    if (event.status == SWING_HIL_FEED_NEED_MORE) {
      continue;
    }
    if (event.status == SWING_HIL_FEED_ERROR) {
      if (prepare_state.prepared) {
        synthetic_fixture_shutdown(&audio, &fixture_neopixel, &prepare_state);
      }
      respond_error(event.request_id, event.error);
      continue;
    }

    switch (event.command.kind) {
      case SWING_HIL_COMMAND_QUERY:
        respond_query(event.command.request_id);
        break;
      case SWING_HIL_COMMAND_LED:
        if (prepare_state.prepared) {
          synthetic_fixture_shutdown(&audio, &fixture_neopixel, &prepare_state);
        }
        run_led(&event.command);
        break;
      case SWING_HIL_COMMAND_TONE:
        if (prepare_state.prepared) {
          synthetic_fixture_shutdown(&audio, &fixture_neopixel, &prepare_state);
        }
        initialize_audio_once(&audio, &audio_initialized);
        run_tone(&audio, &event.command);
        break;
      case SWING_HIL_COMMAND_CALIBRATE:
        initialize_audio_once(&audio, &audio_initialized);
        initialize_fixture_neopixel_once(&fixture_neopixel, &fixture_neopixel_initialized);
        run_calibration(&audio, &fixture_neopixel, &prepare_state, &event.command);
        break;
      case SWING_HIL_COMMAND_SWING:
        initialize_audio_once(&audio, &audio_initialized);
        initialize_fixture_neopixel_once(&fixture_neopixel, &fixture_neopixel_initialized);
        run_swing(&audio, &fixture_neopixel, &prepare_state, &event.command);
        break;
    }
  }
}
