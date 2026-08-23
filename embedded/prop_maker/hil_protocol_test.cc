#include "embedded/prop_maker/hil_protocol.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <string_view>

#include "embedded/prop_maker/swing_sequence.h"

namespace {

swing_hil_parser_event FeedLine(swing_hil_line_parser *parser, std::string_view line) {
  swing_hil_parser_event event{};
  for (const char byte : line) {
    event = swing_hil_line_parser_feed(parser, byte);
    if (byte != '\n') {
      assert(event.status == SWING_HIL_FEED_NEED_MORE);
    }
  }
  return event;
}

void ParsesEveryCommand() {
  static_assert(std::string_view(SWING_HIL_FIRMWARE_VERSION) == "prop-maker-hil-9");
  static_assert(SWING_HIL_PREPARE_TIMEOUT_US == 10'000'000ULL);
  swing_hil_line_parser parser{};
  swing_hil_line_parser_init(&parser);

  auto event = FeedLine(&parser, "SC-HIL/1 7 QUERY\r\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.request_id == 7U);
  assert(event.command.kind == SWING_HIL_COMMAND_QUERY);

  event = FeedLine(&parser, "SC-HIL/1 8 LED 100000 44053\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.kind == SWING_HIL_COMMAND_LED);
  assert(event.command.parameters.led.lead_us == 100000U);
  assert(event.command.parameters.led.duration_us == 44053U);

  event = FeedLine(&parser, "SC-HIL/1 9 TONE 100000 20000 2000 125\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.kind == SWING_HIL_COMMAND_TONE);
  assert(event.command.parameters.tone.lead_us == 100000U);
  assert(event.command.parameters.tone.duration_us == 20000U);
  assert(event.command.parameters.tone.frequency_hz == 2000U);
  assert(event.command.parameters.tone.level_permille == 125U);

  event = FeedLine(&parser, "SC-HIL/1 91 PCM_BEGIN 4 138745022\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.kind == SWING_HIL_COMMAND_PCM_BEGIN);
  assert(event.command.parameters.pcm_begin.sample_count == 4U);
  assert(event.command.parameters.pcm_begin.crc32 == 138745022U);

  event = FeedLine(&parser, "SC-HIL/1 91 PCM_CHUNK 0 0000ff7f00803412\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.kind == SWING_HIL_COMMAND_PCM_CHUNK);
  assert(event.command.parameters.pcm_chunk.byte_offset == 0U);
  assert(event.command.parameters.pcm_chunk.byte_count == 8U);
  assert(event.command.parameters.pcm_chunk.bytes[3] == 0x7fU);

  event = FeedLine(&parser, "SC-HIL/1 91 PCM_COMMIT\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.kind == SWING_HIL_COMMAND_PCM_COMMIT);

  event = FeedLine(&parser, "SC-HIL/1 91 PCM_ABORT\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.kind == SWING_HIL_COMMAND_PCM_ABORT);

  event = FeedLine(&parser, "SC-HIL/1 92 PCM_PLAY 100000 125 12 480\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.kind == SWING_HIL_COMMAND_PCM_PLAY);
  assert(event.command.parameters.pcm_play.lead_us == 100000U);
  assert(event.command.parameters.pcm_play.gain_permille == 125U);
  assert(event.command.parameters.pcm_play.brightness == 12U);
  assert(event.command.parameters.pcm_play.marker_sample == 480U);

  event = FeedLine(&parser, "SC-HIL/1 93 PCM_PLAY 100000 1000 12 480\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.kind == SWING_HIL_COMMAND_PCM_PLAY);
  assert(event.command.parameters.pcm_play.gain_permille == 1000U);

  event = FeedLine(&parser, "SC-HIL/1 10 CALIBRATE\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.kind == SWING_HIL_COMMAND_CALIBRATE);

  event = FeedLine(&parser, "SC-HIL/1 11 SWING 12\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.kind == SWING_HIL_COMMAND_SWING);
  assert(event.command.parameters.swing.brightness == 12U);
}

void EnforcesStimulusSafetyBounds() {
  swing_hil_line_parser parser{};
  swing_hil_line_parser_init(&parser);

  auto event = FeedLine(&parser, "SC-HIL/1 1 LED 0 99\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);
  assert(event.request_id == 1U);

  event = FeedLine(&parser, "SC-HIL/1 2 TONE 19999 20000 2000 125\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);

  event = FeedLine(&parser, "SC-HIL/1 3 TONE 20000 20000 2000 126\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);

  event = FeedLine(&parser, "SC-HIL/1 30 PCM_PLAY 20000 1001 12 0\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);

  event = FeedLine(&parser, "SC-HIL/1 4 LED 2000001 44053\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);

  event = FeedLine(&parser, "SC-HIL/1 5 SWING 24\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);

  event = FeedLine(&parser, "SC-HIL/1 6 CALIBRATE extra\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_ARGUMENT_COUNT);

  event = FeedLine(&parser, "SC-HIL/1 7 PCM_BEGIN 0 0\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);

  event = FeedLine(&parser, "SC-HIL/1 7 PCM_BEGIN 12001 0\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);

  event = FeedLine(&parser, "SC-HIL/1 7 PCM_CHUNK 0 000\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_MALFORMED);

  event = FeedLine(&parser, "SC-HIL/1 7 PCM_CHUNK 23999 0000\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);

  event = FeedLine(&parser, "SC-HIL/1 7 PCM_PLAY 19999 125 12 0\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);

  event = FeedLine(&parser, "SC-HIL/1 7 PCM_PLAY 20000 1001 12 0\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);

  event = FeedLine(&parser, "SC-HIL/1 7 PCM_PLAY 20000 125 12 12000\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_OUT_OF_RANGE);
}

void AcceptsExactlyTheV5CalibrationCandidates() {
  constexpr std::array<std::string_view, SWING_HIL_CALIBRATION_CANDIDATE_COUNT> kCommands = {
      "SC-HIL/1 11 SWING 1\n",  "SC-HIL/1 12 SWING 2\n",  "SC-HIL/1 13 SWING 3\n",
      "SC-HIL/1 14 SWING 4\n",  "SC-HIL/1 15 SWING 6\n",  "SC-HIL/1 16 SWING 8\n",
      "SC-HIL/1 17 SWING 12\n", "SC-HIL/1 18 SWING 16\n",
  };
  swing_hil_line_parser parser{};
  swing_hil_line_parser_init(&parser);
  for (std::size_t index = 0; index < kCommands.size(); ++index) {
    const auto event = FeedLine(&parser, kCommands[index]);
    assert(event.status == SWING_HIL_FEED_COMMAND);
    assert(event.command.parameters.swing.brightness == swing_hil_calibration_candidate(index));
  }
}

void DiagnosesFramingAndVersionErrors() {
  swing_hil_line_parser parser{};
  swing_hil_line_parser_init(&parser);

  auto event = FeedLine(&parser, "SC-HIL/2 41 QUERY\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_UNSUPPORTED_VERSION);
  assert(event.request_id == 41U);

  event = FeedLine(&parser, "SC-HIL/1 0 QUERY\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_INVALID_REQUEST_ID);

  event = FeedLine(&parser, "SC-HIL/1 42 QUERY extra\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_ARGUMENT_COUNT);
  assert(event.request_id == 42U);

  event = FeedLine(&parser, "SC-HIL/1 43 BLINK 1000 1000\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_UNKNOWN_COMMAND);
}

void RecoversAfterAnOversizedLine() {
  swing_hil_line_parser parser{};
  swing_hil_line_parser_init(&parser);

  for (size_t index = 0; index <= SWING_HIL_MAX_LINE_BYTES; ++index) {
    const auto event = swing_hil_line_parser_feed(&parser, 'x');
    assert(event.status == SWING_HIL_FEED_NEED_MORE);
  }
  auto event = swing_hil_line_parser_feed(&parser, '\n');
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_LINE_TOO_LONG);

  event = FeedLine(&parser, "SC-HIL/1 44 QUERY\n");
  assert(event.status == SWING_HIL_FEED_COMMAND);
  assert(event.command.request_id == 44U);
}

void RejectsNumericOverflowAndControlCharacters() {
  swing_hil_line_parser parser{};
  swing_hil_line_parser_init(&parser);

  auto event = FeedLine(&parser, "SC-HIL/1 4294967296 QUERY\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_INVALID_REQUEST_ID);

  event = FeedLine(&parser, "SC-HIL/1\t45 QUERY\n");
  assert(event.status == SWING_HIL_FEED_ERROR);
  assert(event.error == SWING_HIL_ERROR_MALFORMED);
}

}  // namespace

int main() {
  ParsesEveryCommand();
  EnforcesStimulusSafetyBounds();
  AcceptsExactlyTheV5CalibrationCandidates();
  DiagnosesFramingAndVersionErrors();
  RecoversAfterAnOversizedLine();
  RejectsNumericOverflowAndControlCharacters();
  return 0;
}
