#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "embedded/prop_maker/board.h"
#include "embedded/prop_maker/hil_protocol.h"

// Pico SDK inline helpers use parameters only in assertions, which disappear
// under -c opt. Keep that vendor-header warning from defeating first-party
// -Werror while leaving the warning enabled for this translation unit's code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "audio_i2s.pio.h"
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

static void audio_stop(audio_output *output) {
  gpio_put(PROP_MAKER_EXTERNAL_POWER_PIN, false);
  pio_sm_set_enabled(output->pio, output->state_machine, false);
  initialize_output_pin_low(PROP_MAKER_I2S_DATA_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_BIT_CLOCK_PIN);
  initialize_output_pin_low(PROP_MAKER_I2S_WORD_SELECT_PIN);
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

static void respond_error(uint32_t request_id, swing_hil_error error) {
  printf(SWING_HIL_PROTOCOL_PREFIX " %lu ERR %s device_us=%llu\n", (unsigned long)request_id,
         swing_hil_error_code(error), printable_time(time_us_64()));
  stdio_flush();
}

static void respond_query(uint32_t request_id) {
  printf(SWING_HIL_PROTOCOL_PREFIX
         " %lu OK QUERY firmware=" SWING_HIL_FIRMWARE_VERSION
         " protocol=1 capabilities=query,led,tone lead_max_us=%u"
         " led_duration_min_us=%u led_duration_max_us=%u"
         " tone_lead_min_us=%u tone_duration_min_us=%u tone_duration_max_us=%u"
         " tone_frequency_min_hz=%u tone_frequency_max_hz=%u"
         " tone_level_min_permille=%u tone_level_max_permille=%u device_us=%llu\n",
         (unsigned long)request_id, SWING_HIL_MAX_LEAD_US, SWING_HIL_LED_MIN_DURATION_US,
         SWING_HIL_LED_MAX_DURATION_US, SWING_HIL_TONE_MIN_LEAD_US, SWING_HIL_TONE_MIN_DURATION_US,
         SWING_HIL_TONE_MAX_DURATION_US, SWING_HIL_TONE_MIN_FREQUENCY_HZ,
         SWING_HIL_TONE_MAX_FREQUENCY_HZ, SWING_HIL_TONE_MIN_LEVEL_PERMILLE,
         SWING_HIL_TONE_MAX_LEVEL_PERMILLE, printable_time(time_us_64()));
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
  audio_stop(output);

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

int main(void) {
  initialize_output_pin_low(PICO_DEFAULT_LED_PIN);
  initialize_output_pin_low(PROP_MAKER_EXTERNAL_POWER_PIN);
  stdio_init_all();

  audio_output audio = {0};
  initialize_audio(&audio);

  printf(SWING_HIL_PROTOCOL_PREFIX " 0 EVENT BOOT firmware=" SWING_HIL_FIRMWARE_VERSION
                                   " device_us=%llu\n",
         printable_time(time_us_64()));
  stdio_flush();

  swing_hil_line_parser parser = {0};
  swing_hil_line_parser_init(&parser);
  while (true) {
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
      respond_error(event.request_id, event.error);
      continue;
    }

    switch (event.command.kind) {
      case SWING_HIL_COMMAND_QUERY:
        respond_query(event.command.request_id);
        break;
      case SWING_HIL_COMMAND_LED:
        run_led(&event.command);
        break;
      case SWING_HIL_COMMAND_TONE:
        run_tone(&audio, &event.command);
        break;
    }
  }
}
