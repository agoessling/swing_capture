#include "capture/hil/feather_hil_protocol.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using swing_capture::hil::BuildFeatherCalibrationCommand;
using swing_capture::hil::BuildFeatherLedCommand;
using swing_capture::hil::BuildFeatherPcmAbortCommand;
using swing_capture::hil::BuildFeatherPcmBeginCommand;
using swing_capture::hil::BuildFeatherPcmChunkCommand;
using swing_capture::hil::BuildFeatherPcmCommitCommand;
using swing_capture::hil::BuildFeatherPcmPlayCommand;
using swing_capture::hil::BuildFeatherQueryCommand;
using swing_capture::hil::BuildFeatherSwingCommand;
using swing_capture::hil::BuildFeatherToneCommand;
using swing_capture::hil::FeatherPcm16LeBytes;
using swing_capture::hil::FeatherPcmCrc32;
using swing_capture::hil::FeatherResponseKind;
using swing_capture::hil::FeatherScaledPcmCrc32;
using swing_capture::hil::ParseFeatherResponse;
using swing_capture::hil::RequiredUnsignedField;

bool Rejects(const std::function<void()> &operation) {
  try {
    operation();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

void TestParsesExactFirmwareTranscripts() {
  constexpr std::string_view kQuery =
      "SC-HIL/1 101 OK QUERY firmware=prop-maker-hil-9 protocol=1 "
      "capabilities=query,led,tone,pcm,calibrate,swing lead_max_us=2000000 led_duration_min_us=100 "
      "led_duration_max_us=1000000 tone_lead_min_us=20000 tone_duration_min_us=1000 "
      "tone_duration_max_us=250000 tone_frequency_min_hz=100 "
      "tone_frequency_max_hz=10000 tone_level_min_permille=1 "
      "tone_level_max_permille=125 fixture_neopixel_gpio=21 "
      "fixture_neopixel_color_order=rgb device_us=900000\r\n";
  const auto info = ParseFeatherResponse(kQuery);
  assert(info.kind == FeatherResponseKind::kOk);
  assert(info.request_id == 101);
  assert(info.fields.at("firmware") == "prop-maker-hil-9");
  assert(info.fields.at("capabilities") == "query,led,tone,pcm,calibrate,swing");
  assert(info.fields.at("fixture_neopixel_color_order") == "rgb");
  assert(info.wire_line == kQuery);

  const auto boot =
      ParseFeatherResponse("SC-HIL/1 0 EVENT BOOT firmware=prop-maker-hil-9 device_us=42\n");
  assert(boot.kind == FeatherResponseKind::kEvent);
  assert(boot.subject == "BOOT");
  assert(boot.state.empty());

  const auto led_acknowledgement = ParseFeatherResponse(
      "SC-HIL/1 102 ACK LED accepted_us=1000000 scheduled_us=1100000 lead_us=100000 "
      "duration_us=44053\n");
  assert(led_acknowledgement.kind == FeatherResponseKind::kAcknowledgement);
  assert(RequiredUnsignedField(led_acknowledgement, "scheduled_us") == 1'100'000);
  const auto led_started = ParseFeatherResponse(
      "SC-HIL/1 102 EVENT LED START scheduled_us=1100000 device_us=1100002 lateness_us=2\n");
  assert(led_started.state == "START");
  const auto led_done = ParseFeatherResponse(
      "SC-HIL/1 102 EVENT LED DONE start_us=1100002 end_us=1144055 elapsed_us=44053 "
      "requested_us=44053\n");
  assert(led_done.state == "DONE");

  const auto tone_acknowledgement = ParseFeatherResponse(
      "SC-HIL/1 103 ACK TONE accepted_us=2000000 scheduled_us=2100000 lead_us=100000 "
      "duration_us=20000 frequency_hz=2000 level_permille=125 sample_rate_hz=32000 "
      "sample_count=640\n");
  assert(tone_acknowledgement.subject == "TONE");
  const auto tone_started = ParseFeatherResponse(
      "SC-HIL/1 103 EVENT TONE START scheduled_us=2100000 device_us=2100003 lateness_us=3\n");
  assert(tone_started.state == "START");
  const auto tone_done = ParseFeatherResponse(
      "SC-HIL/1 103 EVENT TONE DONE start_us=2100003 end_us=2120103 elapsed_us=20100 "
      "requested_us=20000 sample_rate_hz=32000 sample_count=640\n");
  assert(tone_done.state == "DONE");

  const auto calibration_step = ParseFeatherResponse(
      "SC-HIL/1 104 EVENT CALIBRATE STEP index=0 brightness=1 scheduled_us=100 "
      "device_us=102 lateness_us=2\n");
  assert(calibration_step.subject == "CALIBRATE");
  assert(calibration_step.state == "STEP");
  const auto swing_phase = ParseFeatherResponse(
      "SC-HIL/1 105 EVENT SWING PHASE phase=pre scheduled_us=100 device_us=101 "
      "lateness_us=1 max_step_lateness_us=1 step_us=20000 step_count=60\n");
  assert(swing_phase.subject == "SWING");
  assert(swing_phase.state == "PHASE");

  const auto error = ParseFeatherResponse("SC-HIL/1 104 ERR out_of_range device_us=2200000\n");
  assert(error.kind == FeatherResponseKind::kError);
  assert(error.subject == "out_of_range");
  const auto unidentified_error =
      ParseFeatherResponse("SC-HIL/1 0 ERR malformed device_us=2200001\n");
  assert(unidentified_error.request_id == 0);
}

void TestBuildsOnlyFirmwareAcceptedCommands() {
  assert(BuildFeatherQueryCommand(1) == "SC-HIL/1 1 QUERY\n");
  assert(BuildFeatherLedCommand(2, 100000, 44053) == "SC-HIL/1 2 LED 100000 44053\n");
  assert(BuildFeatherToneCommand(3, 100000, 20000, 2000, 125) ==
         "SC-HIL/1 3 TONE 100000 20000 2000 125\n");
  const std::vector<std::int16_t> samples = {0, 32767, -32768, 0x1234};
  const auto bytes = FeatherPcm16LeBytes(samples);
  assert(bytes == std::vector<std::uint8_t>({0x00, 0x00, 0xff, 0x7f, 0x00, 0x80, 0x34, 0x12}));
  assert(FeatherPcmCrc32(bytes) == 0x53c29a84U);
  assert(FeatherScaledPcmCrc32(samples, 125U) == 0xfe5f190bU);
  assert(BuildFeatherPcmBeginCommand(6, 4, 0x53c29a84U) == "SC-HIL/1 6 PCM_BEGIN 4 1405262468\n");
  assert(BuildFeatherPcmChunkCommand(6, 0, bytes) == "SC-HIL/1 6 PCM_CHUNK 0 0000ff7f00803412\n");
  assert(BuildFeatherPcmCommitCommand(6) == "SC-HIL/1 6 PCM_COMMIT\n");
  assert(BuildFeatherPcmAbortCommand(6) == "SC-HIL/1 6 PCM_ABORT\n");
  assert(BuildFeatherPcmPlayCommand(7, 100000, 125, 12, 480) ==
         "SC-HIL/1 7 PCM_PLAY 100000 125 12 480\n");
  assert(BuildFeatherPcmPlayCommand(7, 20'000, 1, 1, 0) == "SC-HIL/1 7 PCM_PLAY 20000 1 1 0\n");
  assert(BuildFeatherPcmPlayCommand(7, 20'000, 1, 1, 11'999) ==
         "SC-HIL/1 7 PCM_PLAY 20000 1 1 11999\n");
  assert(BuildFeatherCalibrationCommand(4) == "SC-HIL/1 4 CALIBRATE\n");
  assert(BuildFeatherSwingCommand(5, 1) == "SC-HIL/1 5 SWING 1\n");
  assert(BuildFeatherSwingCommand(5, 12) == "SC-HIL/1 5 SWING 12\n");
  assert(BuildFeatherSwingCommand(5, 16) == "SC-HIL/1 5 SWING 16\n");

  assert(Rejects([] { static_cast<void>(BuildFeatherQueryCommand(0)); }));
  assert(Rejects([] { static_cast<void>(BuildFeatherLedCommand(1, 2'000'001, 44'053)); }));
  assert(Rejects([] { static_cast<void>(BuildFeatherLedCommand(1, 0, 99)); }));
  assert(Rejects([] { static_cast<void>(BuildFeatherLedCommand(1, 0, 1'000'001)); }));
  assert(
      Rejects([] { static_cast<void>(BuildFeatherToneCommand(1, 19'999, 20'000, 2'000, 125)); }));
  assert(Rejects(
      [] { static_cast<void>(BuildFeatherToneCommand(1, 2'000'001, 20'000, 2'000, 125)); }));
  assert(Rejects([] { static_cast<void>(BuildFeatherToneCommand(1, 20'000, 999, 2'000, 125)); }));
  assert(
      Rejects([] { static_cast<void>(BuildFeatherToneCommand(1, 20'000, 250'001, 2'000, 125)); }));
  assert(Rejects([] { static_cast<void>(BuildFeatherToneCommand(1, 20'000, 20'000, 99, 125)); }));
  assert(
      Rejects([] { static_cast<void>(BuildFeatherToneCommand(1, 20'000, 20'000, 10'001, 125)); }));
  assert(Rejects([] { static_cast<void>(BuildFeatherToneCommand(1, 20'000, 20'000, 2'000, 0)); }));
  assert(
      Rejects([] { static_cast<void>(BuildFeatherToneCommand(1, 20'000, 20'000, 2'000, 126)); }));
  assert(Rejects([] { static_cast<void>(BuildFeatherCalibrationCommand(0)); }));
  assert(Rejects([] { static_cast<void>(BuildFeatherSwingCommand(1, 24)); }));
  assert(Rejects([] { static_cast<void>(FeatherPcm16LeBytes({})); }));
  assert(Rejects([] { static_cast<void>(BuildFeatherPcmBeginCommand(1, 0, 0)); }));
  assert(Rejects([] {
    std::vector<std::uint8_t> too_large(49);
    static_cast<void>(BuildFeatherPcmChunkCommand(1, 0, too_large));
  }));
  assert(Rejects([] {
    const std::uint8_t byte = 0;
    static_cast<void>(BuildFeatherPcmChunkCommand(1, 24'000, std::span(&byte, 1U)));
  }));
  assert(Rejects([] { static_cast<void>(BuildFeatherPcmPlayCommand(1, 19'999, 125, 12, 0)); }));
  assert(Rejects([] { static_cast<void>(BuildFeatherPcmPlayCommand(1, 20'000, 1'001, 12, 0)); }));
  assert(
      Rejects([] { static_cast<void>(BuildFeatherPcmPlayCommand(1, 20'000, 125, 12, 12'000)); }));
}

void TestRejectsMalformedResponses() {
  for (const std::string_view response : {
           "",
           "SC-HIL/2 1 OK QUERY protocol=2",
           "SC-HIL/1 nope OK QUERY protocol=1",
           "SC-HIL/1 1 ACK",
           "SC-HIL/1 1 EVENT LED",
           "SC-HIL/1 1 ERR",
           "SC-HIL/1 1 ACK LED broken",
           "SC-HIL/1 1 ACK LED value=1 value=2",
           "SC-HIL/1 1  OK QUERY protocol=1",
           "SC-HIL/1 0 OK QUERY protocol=1",
           "SC-HIL/1 0 ACK LED accepted_us=1",
           "SC-HIL/1 0 EVENT LED START device_us=1",
           "SC-HIL/1 1 EVENT BOOT firmware=prop-maker-hil-9 device_us=1",
       }) {
    assert(Rejects([response] { static_cast<void>(ParseFeatherResponse(response)); }));
  }
  const auto response = ParseFeatherResponse("SC-HIL/1 1 OK QUERY protocol=overflow\n");
  assert(Rejects([&response] { static_cast<void>(RequiredUnsignedField(response, "protocol")); }));
  assert(Rejects([&response] { static_cast<void>(RequiredUnsignedField(response, "missing")); }));
}

}  // namespace

int main() {
  TestParsesExactFirmwareTranscripts();
  TestBuildsOnlyFirmwareAcceptedCommands();
  TestRejectsMalformedResponses();
  return 0;
}
