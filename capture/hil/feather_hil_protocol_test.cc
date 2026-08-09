#include "capture/hil/feather_hil_protocol.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string_view>

namespace {

using swing_capture::hil::BuildFeatherLedCommand;
using swing_capture::hil::BuildFeatherQueryCommand;
using swing_capture::hil::BuildFeatherToneCommand;
using swing_capture::hil::FeatherResponseKind;
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
      "SC-HIL/1 101 OK QUERY firmware=prop-maker-hil-1 protocol=1 "
      "capabilities=query,led,tone lead_max_us=2000000 led_duration_min_us=100 "
      "led_duration_max_us=1000000 tone_lead_min_us=20000 tone_duration_min_us=1000 "
      "tone_duration_max_us=250000 tone_frequency_min_hz=100 "
      "tone_frequency_max_hz=10000 tone_level_min_permille=1 "
      "tone_level_max_permille=125 device_us=900000\r\n";
  const auto info = ParseFeatherResponse(kQuery);
  assert(info.kind == FeatherResponseKind::kOk);
  assert(info.request_id == 101);
  assert(info.fields.at("firmware") == "prop-maker-hil-1");
  assert(info.fields.at("capabilities") == "query,led,tone");
  assert(info.wire_line == kQuery);

  const auto boot =
      ParseFeatherResponse("SC-HIL/1 0 EVENT BOOT firmware=prop-maker-hil-1 device_us=42\n");
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
           "SC-HIL/1 1 EVENT BOOT firmware=prop-maker-hil-1 device_us=1",
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
