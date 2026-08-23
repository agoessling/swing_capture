#ifndef SWING_CAPTURE_EMBEDDED_PROP_MAKER_HIL_PROTOCOL_H_
#define SWING_CAPTURE_EMBEDDED_PROP_MAKER_HIL_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWING_HIL_PROTOCOL_PREFIX "SC-HIL/1"
#define SWING_HIL_FIRMWARE_VERSION "prop-maker-hil-9"
#define SWING_HIL_MAX_LINE_BYTES 160U

#define SWING_HIL_MAX_LEAD_US 2000000U
#define SWING_HIL_LED_MIN_DURATION_US 100U
#define SWING_HIL_LED_MAX_DURATION_US 1000000U
#define SWING_HIL_TONE_MIN_LEAD_US 20000U
#define SWING_HIL_TONE_MIN_DURATION_US 1000U
#define SWING_HIL_TONE_MAX_DURATION_US 250000U
#define SWING_HIL_TONE_MIN_FREQUENCY_HZ 100U
#define SWING_HIL_TONE_MAX_FREQUENCY_HZ 10000U
#define SWING_HIL_TONE_MIN_LEVEL_PERMILLE 1U
#define SWING_HIL_TONE_MAX_LEVEL_PERMILLE 125U
#define SWING_HIL_PCM_SAMPLE_RATE_HZ 48000U
#define SWING_HIL_PCM_MAX_SAMPLES 12000U
#define SWING_HIL_PCM_MAX_CHUNK_BYTES 48U
#define SWING_HIL_PCM_MIN_LEAD_US 20000U
#define SWING_HIL_PCM_MIN_GAIN_PERMILLE 1U
#define SWING_HIL_PCM_MAX_GAIN_PERMILLE 1000U
#define SWING_HIL_PCM_WHITE_US 20000U

typedef enum swing_hil_command_kind {
  SWING_HIL_COMMAND_QUERY = 0,
  SWING_HIL_COMMAND_LED,
  SWING_HIL_COMMAND_TONE,
  SWING_HIL_COMMAND_PCM_BEGIN,
  SWING_HIL_COMMAND_PCM_CHUNK,
  SWING_HIL_COMMAND_PCM_COMMIT,
  SWING_HIL_COMMAND_PCM_ABORT,
  SWING_HIL_COMMAND_PCM_PLAY,
  SWING_HIL_COMMAND_CALIBRATE,
  SWING_HIL_COMMAND_SWING,
} swing_hil_command_kind;

typedef struct swing_hil_led_command {
  uint32_t lead_us;
  uint32_t duration_us;
} swing_hil_led_command;

typedef struct swing_hil_tone_command {
  uint32_t lead_us;
  uint32_t duration_us;
  uint32_t frequency_hz;
  uint32_t level_permille;
} swing_hil_tone_command;

typedef struct swing_hil_pcm_begin_command {
  uint32_t sample_count;
  uint32_t crc32;
} swing_hil_pcm_begin_command;

typedef struct swing_hil_pcm_chunk_command {
  uint32_t byte_offset;
  uint32_t byte_count;
  uint8_t bytes[SWING_HIL_PCM_MAX_CHUNK_BYTES];
} swing_hil_pcm_chunk_command;

typedef struct swing_hil_pcm_play_command {
  uint32_t lead_us;
  uint32_t gain_permille;
  uint32_t brightness;
  uint32_t marker_sample;
} swing_hil_pcm_play_command;

typedef struct swing_hil_swing_command {
  uint32_t brightness;
} swing_hil_swing_command;

typedef struct swing_hil_command {
  uint32_t request_id;
  swing_hil_command_kind kind;
  union {
    swing_hil_led_command led;
    swing_hil_tone_command tone;
    swing_hil_pcm_begin_command pcm_begin;
    swing_hil_pcm_chunk_command pcm_chunk;
    swing_hil_pcm_play_command pcm_play;
    swing_hil_swing_command swing;
  } parameters;
} swing_hil_command;

typedef enum swing_hil_error {
  SWING_HIL_ERROR_NONE = 0,
  SWING_HIL_ERROR_EMPTY,
  SWING_HIL_ERROR_LINE_TOO_LONG,
  SWING_HIL_ERROR_MALFORMED,
  SWING_HIL_ERROR_UNSUPPORTED_VERSION,
  SWING_HIL_ERROR_INVALID_REQUEST_ID,
  SWING_HIL_ERROR_UNKNOWN_COMMAND,
  SWING_HIL_ERROR_ARGUMENT_COUNT,
  SWING_HIL_ERROR_OUT_OF_RANGE,
} swing_hil_error;

typedef enum swing_hil_feed_status {
  SWING_HIL_FEED_NEED_MORE = 0,
  SWING_HIL_FEED_COMMAND,
  SWING_HIL_FEED_ERROR,
} swing_hil_feed_status;

typedef struct swing_hil_parser_event {
  swing_hil_feed_status status;
  swing_hil_command command;
  swing_hil_error error;
  // Zero means no valid request ID could be recovered from the input.
  uint32_t request_id;
} swing_hil_parser_event;

typedef struct swing_hil_line_parser {
  char bytes[SWING_HIL_MAX_LINE_BYTES];
  size_t length;
  bool discarding;
} swing_hil_line_parser;

void swing_hil_line_parser_init(swing_hil_line_parser *parser);
swing_hil_parser_event swing_hil_line_parser_feed(swing_hil_line_parser *parser, char byte);
const char *swing_hil_error_code(swing_hil_error error);

#ifdef __cplusplus
}
#endif

#endif  // SWING_CAPTURE_EMBEDDED_PROP_MAKER_HIL_PROTOCOL_H_
