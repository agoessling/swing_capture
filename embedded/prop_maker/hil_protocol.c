#include "embedded/prop_maker/hil_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "embedded/prop_maker/swing_sequence.h"

typedef struct token {
  const char *begin;
  size_t length;
} token;

typedef struct error_details {
  uint32_t request_id;
  swing_hil_error error;
} error_details;

typedef struct command_details {
  uint32_t request_id;
  swing_hil_command_kind kind;
} command_details;

typedef struct parse_context {
  const token *tokens;
  size_t token_count;
  uint32_t request_id;
} parse_context;

static int token_equals(token value, const char *expected) {
  const size_t expected_length = strlen(expected);
  return value.length == expected_length && memcmp(value.begin, expected, value.length) == 0;
}

static int token_starts_with(token value, const char *prefix) {
  const size_t prefix_length = strlen(prefix);
  return value.length >= prefix_length && memcmp(value.begin, prefix, prefix_length) == 0;
}

static int parse_u32(token value, uint32_t *output) {
  if (value.length == 0U) {
    return 0;
  }

  uint32_t parsed = 0U;
  for (size_t index = 0U; index < value.length; ++index) {
    const char digit = value.begin[index];
    if (digit < '0' || digit > '9') {
      return 0;
    }
    const uint32_t numeric_digit = (uint32_t)(digit - '0');
    if (parsed > (UINT32_MAX - numeric_digit) / 10U) {
      return 0;
    }
    parsed = parsed * 10U + numeric_digit;
  }

  *output = parsed;
  return 1;
}

static int parse_hex_nibble(char value, uint8_t *output) {
  if (value >= '0' && value <= '9') {
    *output = (uint8_t)(value - '0');
    return 1;
  }
  if (value >= 'a' && value <= 'f') {
    *output = (uint8_t)(value - 'a' + 10);
    return 1;
  }
  if (value >= 'A' && value <= 'F') {
    *output = (uint8_t)(value - 'A' + 10);
    return 1;
  }
  return 0;
}

static int parse_hex_bytes(token value, swing_hil_pcm_chunk_command *command) {
  if (value.length == 0U || (value.length % 2U) != 0U ||
      value.length / 2U > SWING_HIL_PCM_MAX_CHUNK_BYTES) {
    return 0;
  }
  command->byte_count = (uint32_t)(value.length / 2U);
  for (size_t index = 0U; index < command->byte_count; ++index) {
    uint8_t high = 0U;
    uint8_t low = 0U;
    if (!parse_hex_nibble(value.begin[index * 2U], &high) ||
        !parse_hex_nibble(value.begin[index * 2U + 1U], &low)) {
      return 0;
    }
    command->bytes[index] = (uint8_t)((high << 4U) | low);
  }
  return 1;
}

static size_t tokenize(const char *line, size_t length, token *tokens, size_t capacity,
                       int *too_many) {
  size_t position = 0U;
  size_t count = 0U;
  *too_many = 0;
  while (position < length) {
    while (position < length && line[position] == ' ') {
      ++position;
    }
    if (position == length) {
      break;
    }

    const size_t begin = position;
    while (position < length && line[position] != ' ') {
      const unsigned char byte = (unsigned char)line[position];
      if (byte < 0x21U || byte > 0x7eU) {
        *too_many = 1;
        return count;
      }
      ++position;
    }

    if (count == capacity) {
      *too_many = 1;
      return count;
    }
    tokens[count++] = (token){.begin = line + begin, .length = position - begin};
  }
  return count;
}

static swing_hil_parser_event error_event(error_details details) {
  swing_hil_parser_event event = {0};
  event.status = SWING_HIL_FEED_ERROR;
  event.error = details.error;
  event.request_id = details.request_id;
  return event;
}

static swing_hil_parser_event command_event(command_details details) {
  swing_hil_parser_event event = {0};
  event.status = SWING_HIL_FEED_COMMAND;
  event.command.request_id = details.request_id;
  event.command.kind = details.kind;
  event.request_id = details.request_id;
  return event;
}

static swing_hil_parser_event parse_query(parse_context context) {
  if (context.token_count != 3U) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_ARGUMENT_COUNT});
  }
  return command_event(
      (command_details){.request_id = context.request_id, .kind = SWING_HIL_COMMAND_QUERY});
}

static swing_hil_parser_event parse_led(parse_context context) {
  if (context.token_count != 5U) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_ARGUMENT_COUNT});
  }

  swing_hil_led_command command = {0};
  if (!parse_u32(context.tokens[3], &command.lead_us) ||
      !parse_u32(context.tokens[4], &command.duration_us)) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_MALFORMED});
  }
  if (command.lead_us > SWING_HIL_MAX_LEAD_US ||
      command.duration_us < SWING_HIL_LED_MIN_DURATION_US ||
      command.duration_us > SWING_HIL_LED_MAX_DURATION_US) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_OUT_OF_RANGE});
  }

  swing_hil_parser_event event = command_event(
      (command_details){.request_id = context.request_id, .kind = SWING_HIL_COMMAND_LED});
  event.command.parameters.led = command;
  return event;
}

static int tone_is_in_range(swing_hil_tone_command command) {
  return command.lead_us >= SWING_HIL_TONE_MIN_LEAD_US &&
         command.lead_us <= SWING_HIL_MAX_LEAD_US &&
         command.duration_us >= SWING_HIL_TONE_MIN_DURATION_US &&
         command.duration_us <= SWING_HIL_TONE_MAX_DURATION_US &&
         command.frequency_hz >= SWING_HIL_TONE_MIN_FREQUENCY_HZ &&
         command.frequency_hz <= SWING_HIL_TONE_MAX_FREQUENCY_HZ &&
         command.level_permille >= SWING_HIL_TONE_MIN_LEVEL_PERMILLE &&
         command.level_permille <= SWING_HIL_TONE_MAX_LEVEL_PERMILLE;
}

static swing_hil_parser_event parse_tone(parse_context context) {
  if (context.token_count != 7U) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_ARGUMENT_COUNT});
  }

  swing_hil_tone_command command = {0};
  if (!parse_u32(context.tokens[3], &command.lead_us) ||
      !parse_u32(context.tokens[4], &command.duration_us) ||
      !parse_u32(context.tokens[5], &command.frequency_hz) ||
      !parse_u32(context.tokens[6], &command.level_permille)) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_MALFORMED});
  }
  if (!tone_is_in_range(command)) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_OUT_OF_RANGE});
  }

  swing_hil_parser_event event = command_event(
      (command_details){.request_id = context.request_id, .kind = SWING_HIL_COMMAND_TONE});
  event.command.parameters.tone = command;
  return event;
}

static swing_hil_parser_event parse_pcm_begin(parse_context context) {
  if (context.token_count != 5U) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_ARGUMENT_COUNT});
  }
  swing_hil_pcm_begin_command command = {0};
  if (!parse_u32(context.tokens[3], &command.sample_count) ||
      !parse_u32(context.tokens[4], &command.crc32)) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_MALFORMED});
  }
  if (command.sample_count == 0U || command.sample_count > SWING_HIL_PCM_MAX_SAMPLES) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_OUT_OF_RANGE});
  }
  swing_hil_parser_event event = command_event(
      (command_details){.request_id = context.request_id, .kind = SWING_HIL_COMMAND_PCM_BEGIN});
  event.command.parameters.pcm_begin = command;
  return event;
}

static swing_hil_parser_event parse_pcm_chunk(parse_context context) {
  if (context.token_count != 5U) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_ARGUMENT_COUNT});
  }
  swing_hil_pcm_chunk_command command = {0};
  if (!parse_u32(context.tokens[3], &command.byte_offset) ||
      !parse_hex_bytes(context.tokens[4], &command)) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_MALFORMED});
  }
  const uint32_t maximum_bytes = SWING_HIL_PCM_MAX_SAMPLES * 2U;
  if (command.byte_offset > maximum_bytes ||
      command.byte_count > maximum_bytes - command.byte_offset) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_OUT_OF_RANGE});
  }
  swing_hil_parser_event event = command_event(
      (command_details){.request_id = context.request_id, .kind = SWING_HIL_COMMAND_PCM_CHUNK});
  event.command.parameters.pcm_chunk = command;
  return event;
}

static swing_hil_parser_event parse_pcm_simple(parse_context context, swing_hil_command_kind kind) {
  if (context.token_count != 3U) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_ARGUMENT_COUNT});
  }
  return command_event((command_details){.request_id = context.request_id, .kind = kind});
}

static swing_hil_parser_event parse_pcm_play(parse_context context) {
  if (context.token_count != 7U) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_ARGUMENT_COUNT});
  }
  swing_hil_pcm_play_command command = {0};
  if (!parse_u32(context.tokens[3], &command.lead_us) ||
      !parse_u32(context.tokens[4], &command.gain_permille) ||
      !parse_u32(context.tokens[5], &command.brightness) ||
      !parse_u32(context.tokens[6], &command.marker_sample)) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_MALFORMED});
  }
  if (command.lead_us < SWING_HIL_PCM_MIN_LEAD_US || command.lead_us > SWING_HIL_MAX_LEAD_US ||
      command.gain_permille < SWING_HIL_PCM_MIN_GAIN_PERMILLE ||
      command.gain_permille > SWING_HIL_PCM_MAX_GAIN_PERMILLE ||
      command.marker_sample >= SWING_HIL_PCM_MAX_SAMPLES ||
      !swing_hil_swing_brightness_is_candidate(command.brightness)) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_OUT_OF_RANGE});
  }
  swing_hil_parser_event event = command_event(
      (command_details){.request_id = context.request_id, .kind = SWING_HIL_COMMAND_PCM_PLAY});
  event.command.parameters.pcm_play = command;
  return event;
}

static swing_hil_parser_event parse_calibrate(parse_context context) {
  if (context.token_count != 3U) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_ARGUMENT_COUNT});
  }
  return command_event(
      (command_details){.request_id = context.request_id, .kind = SWING_HIL_COMMAND_CALIBRATE});
}

static swing_hil_parser_event parse_swing(parse_context context) {
  if (context.token_count != 4U) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_ARGUMENT_COUNT});
  }
  swing_hil_swing_command command = {0};
  if (!parse_u32(context.tokens[3], &command.brightness)) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_MALFORMED});
  }
  if (!swing_hil_swing_brightness_is_candidate(command.brightness)) {
    return error_event(
        (error_details){.request_id = context.request_id, .error = SWING_HIL_ERROR_OUT_OF_RANGE});
  }
  swing_hil_parser_event event = command_event(
      (command_details){.request_id = context.request_id, .kind = SWING_HIL_COMMAND_SWING});
  event.command.parameters.swing = command;
  return event;
}

static swing_hil_parser_event parse_line(const char *line, size_t length) {
  if (length > 0U && line[length - 1U] == '\r') {
    --length;
  }
  if (length == 0U) {
    return error_event((error_details){.request_id = 0U, .error = SWING_HIL_ERROR_EMPTY});
  }

  token tokens[8] = {0};
  int malformed = 0;
  const size_t token_count = tokenize(line, length, tokens, 8U, &malformed);
  if (malformed || token_count == 0U) {
    return error_event((error_details){.request_id = 0U, .error = SWING_HIL_ERROR_MALFORMED});
  }

  uint32_t request_id = 0U;
  const int request_id_valid =
      token_count >= 2U && parse_u32(tokens[1], &request_id) && request_id != 0U;
  if (!token_equals(tokens[0], SWING_HIL_PROTOCOL_PREFIX)) {
    swing_hil_error error = SWING_HIL_ERROR_MALFORMED;
    if (token_starts_with(tokens[0], "SC-HIL/")) {
      error = SWING_HIL_ERROR_UNSUPPORTED_VERSION;
    }
    uint32_t error_request_id = 0U;
    if (request_id_valid) {
      error_request_id = request_id;
    }
    return error_event((error_details){.request_id = error_request_id, .error = error});
  }
  if (!request_id_valid) {
    return error_event(
        (error_details){.request_id = 0U, .error = SWING_HIL_ERROR_INVALID_REQUEST_ID});
  }
  if (token_count < 3U) {
    return error_event(
        (error_details){.request_id = request_id, .error = SWING_HIL_ERROR_ARGUMENT_COUNT});
  }

  const parse_context context = {
      .tokens = tokens,
      .token_count = token_count,
      .request_id = request_id,
  };
  if (token_equals(tokens[2], "QUERY")) {
    return parse_query(context);
  }
  if (token_equals(tokens[2], "LED")) {
    return parse_led(context);
  }
  if (token_equals(tokens[2], "TONE")) {
    return parse_tone(context);
  }
  if (token_equals(tokens[2], "PCM_BEGIN")) {
    return parse_pcm_begin(context);
  }
  if (token_equals(tokens[2], "PCM_CHUNK")) {
    return parse_pcm_chunk(context);
  }
  if (token_equals(tokens[2], "PCM_COMMIT")) {
    return parse_pcm_simple(context, SWING_HIL_COMMAND_PCM_COMMIT);
  }
  if (token_equals(tokens[2], "PCM_ABORT")) {
    return parse_pcm_simple(context, SWING_HIL_COMMAND_PCM_ABORT);
  }
  if (token_equals(tokens[2], "PCM_PLAY")) {
    return parse_pcm_play(context);
  }
  if (token_equals(tokens[2], "CALIBRATE")) {
    return parse_calibrate(context);
  }
  if (token_equals(tokens[2], "SWING")) {
    return parse_swing(context);
  }

  return error_event(
      (error_details){.request_id = request_id, .error = SWING_HIL_ERROR_UNKNOWN_COMMAND});
}

void swing_hil_line_parser_init(swing_hil_line_parser *parser) {
  parser->length = 0U;
  parser->discarding = false;
}

swing_hil_parser_event swing_hil_line_parser_feed(swing_hil_line_parser *parser, char byte) {
  swing_hil_parser_event event = {0};
  event.status = SWING_HIL_FEED_NEED_MORE;

  if (byte != '\n') {
    if (!parser->discarding) {
      if (parser->length < SWING_HIL_MAX_LINE_BYTES) {
        parser->bytes[parser->length++] = byte;
      } else {
        parser->discarding = true;
      }
    }
    return event;
  }

  if (parser->discarding) {
    swing_hil_line_parser_init(parser);
    return error_event((error_details){.request_id = 0U, .error = SWING_HIL_ERROR_LINE_TOO_LONG});
  }

  event = parse_line(parser->bytes, parser->length);
  swing_hil_line_parser_init(parser);
  return event;
}

const char *swing_hil_error_code(swing_hil_error error) {
  switch (error) {
    case SWING_HIL_ERROR_NONE:
      return "none";
    case SWING_HIL_ERROR_EMPTY:
      return "empty";
    case SWING_HIL_ERROR_LINE_TOO_LONG:
      return "line_too_long";
    case SWING_HIL_ERROR_MALFORMED:
      return "malformed";
    case SWING_HIL_ERROR_UNSUPPORTED_VERSION:
      return "unsupported_version";
    case SWING_HIL_ERROR_INVALID_REQUEST_ID:
      return "invalid_request_id";
    case SWING_HIL_ERROR_UNKNOWN_COMMAND:
      return "unknown_command";
    case SWING_HIL_ERROR_ARGUMENT_COUNT:
      return "argument_count";
    case SWING_HIL_ERROR_OUT_OF_RANGE:
      return "out_of_range";
  }
  return "internal";
}
