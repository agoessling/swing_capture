#include "capture/hil/feather_hil_controller.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "capture/hil/feather_hil_serial.h"

namespace {

using swing_capture::hil::FeatherHilController;
using swing_capture::hil::FeatherHilFailureEvidence;
using swing_capture::hil::FeatherHilSerial;
using swing_capture::hil::FeatherHilTransactionError;
using swing_capture::hil::FeatherHilTransactionStage;
using swing_capture::hil::FeatherResponseKind;

constexpr std::string_view kQueryFields =
    "firmware=prop-maker-hil-5 protocol=1 capabilities=query,led,tone,calibrate,swing "
    "lead_max_us=2000000 led_duration_min_us=100 led_duration_max_us=1000000 "
    "tone_lead_min_us=20000 tone_duration_min_us=1000 tone_duration_max_us=250000 "
    "tone_frequency_min_hz=100 tone_frequency_max_hz=10000 tone_level_min_permille=1 "
    "tone_level_max_permille=125 swing_start_lead_us=20000 swing_step_us=20000 "
    "swing_pre_steps=60 swing_white_us=20000 swing_post_steps=25 "
    "swing_tone_duration_us=10000 swing_tone_frequency_hz=2000 "
    "swing_tone_level_permille=125 swing_lateness_max_us=2000 "
    "swing_impact_delta_max_us=250 swing_color_reference_brightness=128 "
    "calibration_step_us=70000 calibration_count=8 "
    "calibration_candidates=1,2,3,4,6,8,12,16 fixture_neopixel_gpio=21 "
    "fixture_neopixel_color_order=rgb shared_power_gpio=23 "
    "prepare_timeout_us=10000000 device_us=900000";

class PseudoTerminal final {
 public:
  PseudoTerminal() {
    master_ = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(master_ >= 0);
    assert(grantpt(master_) == 0);
    assert(unlockpt(master_) == 0);
    std::array<char, 256> path = {};
    assert(ptsname_r(master_, path.data(), path.size()) == 0);
    slave_path_ = path.data();
  }

  ~PseudoTerminal() { close(master_); }

  PseudoTerminal(const PseudoTerminal &) = delete;
  PseudoTerminal &operator=(const PseudoTerminal &) = delete;
  PseudoTerminal(PseudoTerminal &&) = delete;
  PseudoTerminal &operator=(PseudoTerminal &&) = delete;

  [[nodiscard]] int master() const { return master_; }
  [[nodiscard]] const std::filesystem::path &slave_path() const { return slave_path_; }

 private:
  int master_ = -1;
  std::filesystem::path slave_path_;
};

std::string ReadLine(int descriptor) {
  std::string line;
  char character = '\0';
  while (character != '\n') {
    assert(read(descriptor, &character, 1) == 1);
    line.push_back(character);
  }
  return line;
}

void WriteLine(int descriptor, std::string_view line) {
  assert(write(descriptor, line.data(), line.size()) == static_cast<ssize_t>(line.size()));
}

std::string QueryResponse(std::uint32_t request_id, std::string_view fields = kQueryFields) {
  return "SC-HIL/1 " + std::to_string(request_id) + " OK QUERY " + std::string(fields) + "\n";
}

std::string ReplacedQueryField(std::string_view from, std::string_view to) {
  std::string fields(kQueryFields);
  const std::size_t position = fields.find(from);
  assert(position != std::string::npos);
  fields.replace(position, from.size(), to);
  return fields;
}

std::string CalibrationAcknowledgement(std::uint32_t request_id) {
  return "SC-HIL/1 " + std::to_string(request_id) +
         " ACK CALIBRATE accepted_us=3000000 start_scheduled_us=3020000 "
         "start_lead_us=20000 step_us=70000 step_count=8 duration_us=560000 "
         "candidates=1,2,3,4,6,8,12,16 fixture_neopixel_gpio=21 "
         "shared_power_gpio=23 prepare_timeout_us=10000000\n";
}

void WriteCalibrationSteps(int descriptor, std::uint32_t request_id) {
  constexpr std::array<std::uint32_t, 8> kBrightness = {1, 2, 3, 4, 6, 8, 12, 16};
  for (std::size_t index = 0; index < kBrightness.size(); ++index) {
    const std::uint64_t scheduled = 3'020'000U + index * 70'000U;
    WriteLine(descriptor, "SC-HIL/1 " + std::to_string(request_id) +
                              " EVENT CALIBRATE STEP index=" + std::to_string(index) +
                              " brightness=" + std::to_string(kBrightness[index]) +
                              " scheduled_us=" + std::to_string(scheduled) +
                              " device_us=" + std::to_string(scheduled + 2U) + " lateness_us=2\n");
  }
}

std::string SwingAcknowledgement(std::uint32_t request_id, std::string_view prepared = "1") {
  return "SC-HIL/1 " + std::to_string(request_id) +
         " ACK SWING accepted_us=4000000 sequence_start_scheduled_us=4020000 "
         "impact_scheduled_us=5220000 post_scheduled_us=5240000 end_scheduled_us=5740000 "
         "start_lead_us=20000 step_us=20000 pre_steps=60 white_us=20000 post_steps=25 "
         "tone_duration_us=10000 tone_frequency_hz=2000 tone_level_permille=125 "
         "tone_sample_rate_hz=32000 tone_sample_count=320 brightness=12 prepared=" +
         std::string(prepared) + " rail_powered=1 fixture_neopixel_gpio=21 shared_power_gpio=23\n";
}

void WriteValidSwingEvidenceBeforeDone(int descriptor, std::uint32_t request_id) {
  const std::string prefix = "SC-HIL/1 " + std::to_string(request_id);
  WriteLine(descriptor, prefix +
                            " EVENT SWING PHASE phase=pre scheduled_us=4020000 "
                            "device_us=4020002 lateness_us=2 max_step_lateness_us=3 "
                            "step_us=20000 step_count=60\n");
  WriteLine(descriptor, prefix +
                            " EVENT SWING IMPACT scheduled_us=5220000 device_us=5220003 "
                            "lateness_us=3 white_command_us=5220003 tone_command_us=5220010 "
                            "command_delta_us=7 white_end_us=5240003 tone_end_us=5230110 "
                            "white_us=20000 tone_duration_us=10000 tone_sample_count=320 "
                            "brightness=12\n");
  WriteLine(descriptor, prefix +
                            " EVENT SWING PHASE phase=post scheduled_us=5240000 "
                            "device_us=5240003 lateness_us=3 max_step_lateness_us=4 "
                            "step_us=20000 step_count=25\n");
}

bool Throws(const std::function<void()> &operation) {
  try {
    operation();
  } catch (const std::exception &) {
    return true;
  }
  return false;
}

FeatherHilFailureEvidence CaptureFailure(const std::function<void()> &operation) {
  try {
    operation();
  } catch (const FeatherHilTransactionError &error) {
    return error.evidence();
  }
  assert(false);
  return {};
}

void TestExactFirmwareInfoLedAndToneReceipts() {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  std::jthread device([&terminal] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 101 QUERY\n");
    WriteLine(terminal.master(), "SC-HIL/1 0 EVENT BOOT firmware=prop-maker-hil-5 device_us=10\n");
    WriteLine(terminal.master(), QueryResponse(101));

    assert(ReadLine(terminal.master()) == "SC-HIL/1 102 LED 100000 44053\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 102 ACK LED accepted_us=1000000 scheduled_us=1100000 lead_us=100000 "
              "duration_us=44053\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 102 EVENT LED START scheduled_us=1100000 device_us=1100002 "
              "lateness_us=2\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 102 EVENT LED DONE start_us=1100002 end_us=1144055 elapsed_us=44053 "
              "requested_us=44053\n");

    assert(ReadLine(terminal.master()) == "SC-HIL/1 103 TONE 100000 20000 2000 125\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 103 ACK TONE accepted_us=2000000 scheduled_us=2100000 lead_us=100000 "
              "duration_us=20000 frequency_hz=2000 level_permille=125 sample_rate_hz=32000 "
              "sample_count=640\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 103 EVENT TONE START scheduled_us=2100000 device_us=2100003 "
              "lateness_us=3\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 103 EVENT TONE DONE start_us=2100003 end_us=2120103 "
              "elapsed_us=20100 requested_us=20000 sample_rate_hz=32000 sample_count=640\n");

    assert(ReadLine(terminal.master()) == "SC-HIL/1 104 CALIBRATE\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 104 ACK CALIBRATE accepted_us=3000000 start_scheduled_us=3020000 "
              "start_lead_us=20000 step_us=70000 step_count=8 duration_us=560000 "
              "candidates=1,2,3,4,6,8,12,16 fixture_neopixel_gpio=21 "
              "shared_power_gpio=23 prepare_timeout_us=10000000\n");
    constexpr std::array<std::uint32_t, 8> kBrightness = {1, 2, 3, 4, 6, 8, 12, 16};
    for (std::size_t index = 0; index < kBrightness.size(); ++index) {
      const std::uint64_t scheduled = 3'020'000U + index * 70'000U;
      WriteLine(terminal.master(),
                "SC-HIL/1 104 EVENT CALIBRATE STEP index=" + std::to_string(index) +
                    " brightness=" + std::to_string(kBrightness[index]) +
                    " scheduled_us=" + std::to_string(scheduled) +
                    " device_us=" + std::to_string(scheduled + 2U) + " lateness_us=2\n");
    }
    WriteLine(terminal.master(),
              "SC-HIL/1 104 EVENT CALIBRATE DONE start_us=3020002 end_us=3580082 "
              "elapsed_us=560080 requested_us=560000 max_step_lateness_us=2 "
              "power_on_us=3000100 prepared_until_us=13000100 pixel_off=1 i2s_inactive=1 "
              "rail_powered=1 prepared=1 fixture_neopixel_gpio=21 shared_power_gpio=23\n");

    assert(ReadLine(terminal.master()) == "SC-HIL/1 105 SWING 12\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 105 ACK SWING accepted_us=4000000 "
              "sequence_start_scheduled_us=4020000 impact_scheduled_us=5220000 "
              "post_scheduled_us=5240000 end_scheduled_us=5740000 start_lead_us=20000 "
              "step_us=20000 pre_steps=60 white_us=20000 post_steps=25 "
              "tone_duration_us=10000 tone_frequency_hz=2000 tone_level_permille=125 "
              "tone_sample_rate_hz=32000 tone_sample_count=320 brightness=12 prepared=1 "
              "rail_powered=1 fixture_neopixel_gpio=21 shared_power_gpio=23\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 105 EVENT SWING PHASE phase=pre scheduled_us=4020000 "
              "device_us=4020002 lateness_us=2 max_step_lateness_us=3 step_us=20000 "
              "step_count=60\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 105 EVENT SWING IMPACT scheduled_us=5220000 device_us=5220003 "
              "lateness_us=3 white_command_us=5220003 tone_command_us=5220010 "
              "command_delta_us=7 white_end_us=5240003 tone_end_us=5230110 white_us=20000 "
              "tone_duration_us=10000 tone_sample_count=320 brightness=12\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 105 EVENT SWING PHASE phase=post scheduled_us=5240000 "
              "device_us=5240003 lateness_us=3 max_step_lateness_us=4 step_us=20000 "
              "step_count=25\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 105 EVENT SWING DONE sequence_start_us=4020002 impact_us=5220003 "
              "post_start_us=5240003 end_us=5740083 elapsed_us=1720081 pre_us=1200000 "
              "post_us=500000 outputs_inactive=1 prepared=0 pixel_off=1 i2s_inactive=1 "
              "rail_powered=0 "
              "fixture_neopixel_gpio=21 shared_power_gpio=23\n");
  });

  FeatherHilController controller(serial, 101);
  const auto info = controller.QueryInfo();
  assert(info.firmware == "prop-maker-hil-5");
  assert(info.protocol_version == 1);
  assert(info.capabilities.contains("led"));
  assert(info.maximum_lead_microseconds == 2'000'000);
  assert(info.swing_tone_level_permille == 125U);
  assert(info.swing_color_reference_brightness == 128U);
  assert(info.fixture_neopixel_gpio == 21U);
  assert(info.fixture_neopixel_color_order == "rgb");
  assert(info.shared_power_gpio == 23U);
  assert(info.prepare_timeout_microseconds == 10'000'000U);
  assert(info.device_microseconds == 900'000);
  assert(info.response.wire_line == QueryResponse(101));

  const auto led =
      controller.PulseLed(std::chrono::microseconds(100'000), std::chrono::microseconds(44'053));
  assert(led.request_id == 102);
  assert(led.subject == "LED");
  assert(led.requested_lead_microseconds == 100'000);
  assert(led.requested_duration_microseconds == 44'053);
  assert(led.start_lateness_microseconds == 2);
  assert(led.elapsed_device_microseconds == 44'053);
  assert(led.acknowledgement.fields.at("duration_us") == "44053");
  assert(led.started.fields.at("lateness_us") == "2");
  assert(led.done.fields.at("requested_us") == "44053");

  const auto tone = controller.PlayTone(std::chrono::microseconds(100'000),
                                        std::chrono::microseconds(20'000), 2'000, 125);
  assert(tone.request_id == 103);
  assert(tone.frequency_hz == 2'000);
  assert(tone.level_permille == 125);
  assert(tone.sample_rate_hz == 32'000);
  assert(tone.sample_count == 640);
  assert(tone.quantized_duration_microseconds == 20'000);
  assert(tone.elapsed_device_microseconds == 20'100);
  assert(tone.host_command_write_started <= tone.host_command_sent);
  assert(tone.host_command_sent <= tone.host_acknowledgement_received);
  assert(tone.host_acknowledgement_received <= tone.host_start_received);
  assert(tone.host_start_received <= tone.host_done_received);

  const auto calibration = controller.CalibrateSwingBrightness();
  assert(calibration.request_id == 104);
  assert(calibration.candidates.size() == 8U);
  assert(calibration.steps.size() == calibration.candidates.size());
  assert(calibration.steps.front().brightness == 1U);
  assert(calibration.steps.back().brightness == 16U);
  assert(calibration.maximum_step_lateness_microseconds == 2U);
  assert(calibration.elapsed_device_microseconds == 560'080U);
  assert(calibration.fixture_neopixel_gpio == 21U);
  assert(calibration.shared_power_gpio == 23U);
  assert(calibration.prepare_timeout_microseconds == 10'000'000U);
  assert(calibration.power_on_device_microseconds == 3'000'100U);
  assert(calibration.prepared_until_device_microseconds == 13'000'100U);
  assert(calibration.pixel_off && calibration.i2s_inactive && calibration.rail_powered &&
         calibration.prepared);

  const auto swing = controller.RunSyntheticSwing(12U);
  assert(swing.request_id == 105);
  assert(swing.brightness == 12U);
  assert(swing.pre.step_count == 60U);
  assert(swing.impact.command_delta_microseconds == 7U);
  assert(swing.impact.white_end_device_microseconds -
             swing.impact.white_command_device_microseconds ==
         20'000U);
  assert(swing.post.step_count == 25U);
  assert(swing.elapsed_device_microseconds == 1'720'081U);
  assert(swing.fixture_neopixel_gpio == 21U);
  assert(swing.shared_power_gpio == 23U);
  assert(swing.prepared_at_acknowledgement && swing.rail_powered_at_acknowledgement);
  assert(swing.outputs_inactive_at_completion && !swing.prepared_at_completion);
  assert(swing.pixel_off_at_completion && swing.i2s_inactive_at_completion &&
         !swing.rail_powered_at_completion);
}

void TestRequiresSuccessfulNegotiationBeforeStimulus() {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  FeatherHilController controller(serial, 1);
  assert(Throws([&controller] {
    static_cast<void>(
        controller.PulseLed(std::chrono::microseconds(100'000), std::chrono::microseconds(44'053)));
  }));
  assert(Throws([&controller] {
    static_cast<void>(controller.PlayTone(std::chrono::microseconds(100'000),
                                          std::chrono::microseconds(20'000), 2'000, 125));
  }));
  assert(Throws([&controller] { static_cast<void>(controller.CalibrateSwingBrightness()); }));
  assert(Throws([&controller] { static_cast<void>(controller.RunSyntheticSwing(12U)); }));
}

void ExpectQueryRejected(std::string_view fields) {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  std::jthread device([&terminal, fields] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 201 QUERY\n");
    WriteLine(terminal.master(), QueryResponse(201, fields));
  });
  FeatherHilController controller(serial, 201);
  assert(Throws([&controller] { static_cast<void>(controller.QueryInfo()); }));
}

void TestRejectsIncompatibleNegotiation() {
  ExpectQueryRejected(ReplacedQueryField("prop-maker-hil-5", "prop-maker-hil-4"));
  ExpectQueryRejected(
      ReplacedQueryField("swing_tone_level_permille=125", "swing_tone_level_permille=10"));
  ExpectQueryRejected(ReplacedQueryField("calibration_candidates=1,2,3,4,6,8,12,16",
                                         "calibration_candidates=16,24,32,48,64,80,96,128"));
  ExpectQueryRejected(ReplacedQueryField("fixture_neopixel_gpio=21 ", ""));
  ExpectQueryRejected(ReplacedQueryField("fixture_neopixel_gpio=21", "fixture_neopixel_gpio=4"));
  ExpectQueryRejected(ReplacedQueryField("fixture_neopixel_color_order=rgb ", ""));
  ExpectQueryRejected(
      ReplacedQueryField("fixture_neopixel_color_order=rgb", "fixture_neopixel_color_order=grb"));
  ExpectQueryRejected(ReplacedQueryField("shared_power_gpio=23", "shared_power_gpio=22"));
  ExpectQueryRejected(
      ReplacedQueryField("prepare_timeout_us=10000000", "prepare_timeout_us=9999999"));
}

void TestPreservesStructuredQueryFailure() {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  std::string wrong_fields(kQueryFields);
  const std::size_t firmware = wrong_fields.find("prop-maker-hil-5");
  assert(firmware != std::string::npos);
  wrong_fields.replace(firmware, std::string_view("prop-maker-hil-5").size(), "wrong");
  const std::string response = QueryResponse(251, wrong_fields);
  std::jthread device([&terminal, &response] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 251 QUERY\n");
    WriteLine(terminal.master(), response);
  });
  FeatherHilController controller(serial, 251);
  const FeatherHilFailureEvidence evidence =
      CaptureFailure([&controller] { static_cast<void>(controller.QueryInfo()); });
  assert(evidence.request_id == 251);
  assert(evidence.subject == "QUERY");
  assert(evidence.stage == FeatherHilTransactionStage::kQueryResponse);
  assert(evidence.command_wire == "SC-HIL/1 251 QUERY\n");
  assert(evidence.offending_response.has_value());
  assert(evidence.offending_response->wire_line == response);
  assert(evidence.device_info.has_value());
  assert(evidence.device_info->firmware == "wrong");
  assert(!evidence.stimulus_receipt.has_value());
}

bool RunRejectedLedTranscript(const std::vector<std::string> &responses) {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  std::jthread device([&terminal, &responses] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 301 QUERY\n");
    WriteLine(terminal.master(), QueryResponse(301));
    assert(ReadLine(terminal.master()) == "SC-HIL/1 302 LED 100000 44053\n");
    for (const auto &response : responses) {
      WriteLine(terminal.master(), response);
    }
  });
  FeatherHilController controller(serial, 301);
  static_cast<void>(controller.QueryInfo());
  return Throws([&controller] {
    static_cast<void>(
        controller.PulseLed(std::chrono::microseconds(100'000), std::chrono::microseconds(44'053)));
  });
}

bool RunRejectedToneTranscript(const std::vector<std::string> &responses) {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  std::jthread device([&terminal, &responses] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 501 QUERY\n");
    WriteLine(terminal.master(), QueryResponse(501));
    assert(ReadLine(terminal.master()) == "SC-HIL/1 502 TONE 100000 20000 2000 125\n");
    for (const auto &response : responses) {
      WriteLine(terminal.master(), response);
    }
  });
  FeatherHilController controller(serial, 501);
  static_cast<void>(controller.QueryInfo());
  return Throws([&controller] {
    static_cast<void>(controller.PlayTone(std::chrono::microseconds(100'000),
                                          std::chrono::microseconds(20'000), 2'000, 125));
  });
}

void TestRejectsInconsistentTimingAndReset() {
  assert(RunRejectedLedTranscript({
      "SC-HIL/1 302 ACK LED accepted_us=1000000 scheduled_us=1000200 lead_us=100000 "
      "duration_us=44053\n",
  }));
  assert(RunRejectedLedTranscript({
      "SC-HIL/1 302 ACK LED accepted_us=1000000 scheduled_us=1100000 lead_us=100000 "
      "duration_us=44053\n",
      "SC-HIL/1 302 EVENT LED START scheduled_us=1100000 device_us=1100002 lateness_us=1\n",
  }));
  assert(RunRejectedLedTranscript({
      "SC-HIL/1 302 ACK LED accepted_us=1000000 scheduled_us=1100000 lead_us=100000 "
      "duration_us=44053\n",
      "SC-HIL/1 0 EVENT BOOT firmware=prop-maker-hil-5 device_us=1000010\n",
  }));
  assert(RunRejectedToneTranscript({
      "SC-HIL/1 502 ACK TONE accepted_us=2000000 scheduled_us=2100000 lead_us=100000 "
      "duration_us=20000 frequency_hz=2000 level_permille=125 sample_rate_hz=32000 "
      "sample_count=640\n",
      "SC-HIL/1 502 EVENT TONE START scheduled_us=2100000 device_us=2100003 "
      "lateness_us=3\n",
      "SC-HIL/1 502 EVENT TONE DONE start_us=2100003 end_us=2120103 elapsed_us=20100 "
      "requested_us=20000 sample_rate_hz=32000 sample_count=639\n",
  }));
}

void TestPreservesOutOfToleranceDurationEvidence() {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  constexpr std::string_view kDone =
      "SC-HIL/1 602 EVENT LED DONE start_us=1100002 end_us=1144202 elapsed_us=44200 "
      "requested_us=44053\n";
  std::jthread device([&terminal, kDone] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 601 QUERY\n");
    WriteLine(terminal.master(), QueryResponse(601));
    assert(ReadLine(terminal.master()) == "SC-HIL/1 602 LED 100000 44053\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 602 ACK LED accepted_us=1000000 scheduled_us=1100000 lead_us=100000 "
              "duration_us=44053\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 602 EVENT LED START scheduled_us=1100000 device_us=1100002 "
              "lateness_us=2\n");
    WriteLine(terminal.master(), kDone);
  });
  FeatherHilController controller(serial, 601);
  static_cast<void>(controller.QueryInfo());
  const FeatherHilFailureEvidence evidence = CaptureFailure([&controller] {
    static_cast<void>(
        controller.PulseLed(std::chrono::microseconds(100'000), std::chrono::microseconds(44'053)));
  });
  assert(evidence.stage == FeatherHilTransactionStage::kDone);
  assert(evidence.command_wire == "SC-HIL/1 602 LED 100000 44053\n");
  assert(evidence.offending_response.has_value());
  assert(evidence.offending_response->wire_line == kDone);
  assert(evidence.stimulus_receipt.has_value());
  const auto &receipt = *evidence.stimulus_receipt;
  assert(receipt.request_id == 602);
  assert(receipt.acknowledgement.fields.at("accepted_us") == "1000000");
  assert(receipt.started.fields.at("lateness_us") == "2");
  assert(receipt.done.wire_line == kDone);
  assert(receipt.elapsed_device_microseconds == 44'200);
}

void TestPreservesOutOfToleranceToneDurationEvidence() {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  constexpr std::string_view kDone =
      "SC-HIL/1 612 EVENT TONE DONE start_us=2100003 end_us=2120254 elapsed_us=20251 "
      "requested_us=20000 sample_rate_hz=32000 sample_count=640\n";
  std::jthread device([&terminal, kDone] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 611 QUERY\n");
    WriteLine(terminal.master(), QueryResponse(611));
    assert(ReadLine(terminal.master()) == "SC-HIL/1 612 TONE 100000 20000 2000 125\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 612 ACK TONE accepted_us=2000000 scheduled_us=2100000 lead_us=100000 "
              "duration_us=20000 frequency_hz=2000 level_permille=125 sample_rate_hz=32000 "
              "sample_count=640\n");
    WriteLine(terminal.master(),
              "SC-HIL/1 612 EVENT TONE START scheduled_us=2100000 device_us=2100003 "
              "lateness_us=3\n");
    WriteLine(terminal.master(), kDone);
  });
  FeatherHilController controller(serial, 611);
  static_cast<void>(controller.QueryInfo());
  const FeatherHilFailureEvidence evidence = CaptureFailure([&controller] {
    static_cast<void>(controller.PlayTone(std::chrono::microseconds(100'000),
                                          std::chrono::microseconds(20'000), 2'000, 125));
  });
  assert(evidence.stage == FeatherHilTransactionStage::kDone);
  assert(evidence.offending_response->wire_line == kDone);
  assert(evidence.stimulus_receipt->done.wire_line == kDone);
  assert(evidence.stimulus_receipt->quantized_duration_microseconds == 20'000);
  assert(evidence.stimulus_receipt->elapsed_device_microseconds == 20'251);
  assert(evidence.stimulus_receipt->sample_count == 640);
}

void TestRejectsMalformedCalibrationAndSwingEvidence() {
  {
    PseudoTerminal terminal;
    FeatherHilSerial serial(terminal.slave_path());
    std::jthread device([&terminal] {
      assert(ReadLine(terminal.master()) == "SC-HIL/1 901 QUERY\n");
      WriteLine(terminal.master(), QueryResponse(901));
      assert(ReadLine(terminal.master()) == "SC-HIL/1 902 CALIBRATE\n");
      WriteLine(terminal.master(),
                "SC-HIL/1 902 ACK CALIBRATE accepted_us=3000000 start_scheduled_us=3020000 "
                "start_lead_us=20000 step_us=70000 step_count=8 duration_us=560000 "
                "candidates=1,2,3,4,6,8,12,16 fixture_neopixel_gpio=21 "
                "shared_power_gpio=23 prepare_timeout_us=10000000\n");
      WriteLine(terminal.master(),
                "SC-HIL/1 902 EVENT CALIBRATE STEP index=0 brightness=17 "
                "scheduled_us=3020000 device_us=3020002 lateness_us=2\n");
    });
    FeatherHilController controller(serial, 901);
    static_cast<void>(controller.QueryInfo());
    const FeatherHilFailureEvidence evidence =
        CaptureFailure([&controller] { static_cast<void>(controller.CalibrateSwingBrightness()); });
    assert(evidence.stage == FeatherHilTransactionStage::kCalibrationStep);
    assert(evidence.calibration_receipt.has_value());
    assert(evidence.calibration_receipt->acknowledgement.subject == "CALIBRATE");
    assert(evidence.calibration_receipt->steps.empty());
    assert(evidence.offending_response->fields.at("brightness") == "17");
  }

  {
    PseudoTerminal terminal;
    FeatherHilSerial serial(terminal.slave_path());
    std::jthread device([&terminal] {
      assert(ReadLine(terminal.master()) == "SC-HIL/1 911 QUERY\n");
      WriteLine(terminal.master(), QueryResponse(911));
      assert(ReadLine(terminal.master()) == "SC-HIL/1 912 SWING 12\n");
      WriteLine(terminal.master(),
                "SC-HIL/1 912 ACK SWING accepted_us=4000000 "
                "sequence_start_scheduled_us=4020000 impact_scheduled_us=5220000 "
                "post_scheduled_us=5240000 end_scheduled_us=5740000 start_lead_us=20000 "
                "step_us=20000 pre_steps=60 white_us=20000 post_steps=25 "
                "tone_duration_us=10000 tone_frequency_hz=2000 tone_level_permille=125 "
                "tone_sample_rate_hz=32000 tone_sample_count=320 brightness=12 prepared=1 "
                "rail_powered=1 fixture_neopixel_gpio=21 shared_power_gpio=23\n");
      WriteLine(terminal.master(),
                "SC-HIL/1 912 EVENT SWING PHASE phase=pre scheduled_us=4020000 "
                "device_us=4020002 lateness_us=2 max_step_lateness_us=3 step_us=20000 "
                "step_count=60\n");
      WriteLine(terminal.master(),
                "SC-HIL/1 912 EVENT SWING IMPACT scheduled_us=5220000 device_us=5220003 "
                "lateness_us=3 white_command_us=5220003 tone_command_us=5220303 "
                "command_delta_us=300 white_end_us=5240003 tone_end_us=5230403 "
                "white_us=20000 tone_duration_us=10000 tone_sample_count=320 brightness=12\n");
    });
    FeatherHilController controller(serial, 911);
    static_cast<void>(controller.QueryInfo());
    const FeatherHilFailureEvidence evidence =
        CaptureFailure([&controller] { static_cast<void>(controller.RunSyntheticSwing(12U)); });
    assert(evidence.stage == FeatherHilTransactionStage::kImpact);
    assert(evidence.swing_receipt.has_value());
    assert(evidence.swing_receipt->pre.phase == "pre");
    assert(evidence.offending_response->fields.at("command_delta_us") == "300");
  }
}

bool RunRejectedCalibrationCompletion(std::string completion_state) {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  std::jthread device([&terminal, completion_state = std::move(completion_state)] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 921 QUERY\n");
    WriteLine(terminal.master(), QueryResponse(921));
    assert(ReadLine(terminal.master()) == "SC-HIL/1 922 CALIBRATE\n");
    WriteLine(terminal.master(), CalibrationAcknowledgement(922));
    WriteCalibrationSteps(terminal.master(), 922);
    WriteLine(terminal.master(),
              "SC-HIL/1 922 EVENT CALIBRATE DONE start_us=3020002 end_us=3580082 "
              "elapsed_us=560080 requested_us=560000 max_step_lateness_us=2 " +
                  completion_state + "\n");
  });
  FeatherHilController controller(serial, 921);
  static_cast<void>(controller.QueryInfo());
  return Throws([&controller] { static_cast<void>(controller.CalibrateSwingBrightness()); });
}

bool RunRejectedSwingAcknowledgement(std::string prepared) {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  std::jthread device([&terminal, prepared = std::move(prepared)] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 931 QUERY\n");
    WriteLine(terminal.master(), QueryResponse(931));
    assert(ReadLine(terminal.master()) == "SC-HIL/1 932 SWING 12\n");
    WriteLine(terminal.master(), SwingAcknowledgement(932, prepared));
  });
  FeatherHilController controller(serial, 931);
  static_cast<void>(controller.QueryInfo());
  return Throws([&controller] { static_cast<void>(controller.RunSyntheticSwing(12U)); });
}

bool RunRejectedSwingCompletion(std::string completion_state) {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  std::jthread device([&terminal, completion_state = std::move(completion_state)] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 941 QUERY\n");
    WriteLine(terminal.master(), QueryResponse(941));
    assert(ReadLine(terminal.master()) == "SC-HIL/1 942 SWING 12\n");
    WriteLine(terminal.master(), SwingAcknowledgement(942));
    WriteValidSwingEvidenceBeforeDone(terminal.master(), 942);
    WriteLine(terminal.master(),
              "SC-HIL/1 942 EVENT SWING DONE sequence_start_us=4020002 impact_us=5220003 "
              "post_start_us=5240003 end_us=5740083 elapsed_us=1720081 pre_us=1200000 "
              "post_us=500000 " +
                  completion_state + "\n");
  });
  FeatherHilController controller(serial, 941);
  static_cast<void>(controller.QueryInfo());
  return Throws([&controller] { static_cast<void>(controller.RunSyntheticSwing(12U)); });
}

void TestRejectsFixtureStateContradictions() {
  assert(RunRejectedCalibrationCompletion(
      "power_on_us=3000100 prepared_until_us=13000100 pixel_off=0 i2s_inactive=1 "
      "rail_powered=1 prepared=1 fixture_neopixel_gpio=21 shared_power_gpio=23"));
  assert(RunRejectedCalibrationCompletion(
      "power_on_us=3000100 prepared_until_us=13000100 pixel_off=1 i2s_inactive=0 "
      "rail_powered=1 prepared=1 fixture_neopixel_gpio=21 shared_power_gpio=23"));
  assert(RunRejectedCalibrationCompletion(
      "power_on_us=3000100 prepared_until_us=13000100 pixel_off=1 i2s_inactive=1 "
      "rail_powered=0 prepared=1 fixture_neopixel_gpio=21 shared_power_gpio=23"));
  assert(RunRejectedCalibrationCompletion(
      "power_on_us=3000100 prepared_until_us=13000100 pixel_off=1 i2s_inactive=1 "
      "rail_powered=1 prepared=0 fixture_neopixel_gpio=21 shared_power_gpio=23"));
  assert(RunRejectedCalibrationCompletion(
      "power_on_us=3000100 prepared_until_us=13000099 pixel_off=1 i2s_inactive=1 "
      "rail_powered=1 prepared=1 fixture_neopixel_gpio=21 shared_power_gpio=23"));
  assert(RunRejectedCalibrationCompletion(
      "power_on_us=3000100 prepared_until_us=13000100 pixel_off=1 i2s_inactive=1 "
      "rail_powered=1 prepared=1 fixture_neopixel_gpio=4 shared_power_gpio=23"));

  assert(RunRejectedSwingAcknowledgement("0"));
  assert(RunRejectedSwingCompletion(
      "outputs_inactive=1 prepared=0 pixel_off=0 i2s_inactive=1 rail_powered=0 "
      "fixture_neopixel_gpio=21 "
      "shared_power_gpio=23"));
  assert(RunRejectedSwingCompletion(
      "outputs_inactive=1 prepared=0 pixel_off=1 i2s_inactive=0 rail_powered=0 "
      "fixture_neopixel_gpio=21 "
      "shared_power_gpio=23"));
  assert(RunRejectedSwingCompletion(
      "outputs_inactive=1 prepared=0 pixel_off=1 i2s_inactive=1 rail_powered=1 "
      "fixture_neopixel_gpio=21 "
      "shared_power_gpio=23"));
  assert(RunRejectedSwingCompletion(
      "outputs_inactive=1 prepared=0 pixel_off=1 i2s_inactive=1 rail_powered=0 "
      "fixture_neopixel_gpio=21 "
      "shared_power_gpio=22"));
  assert(RunRejectedSwingCompletion(
      "outputs_inactive=0 prepared=0 pixel_off=1 i2s_inactive=1 rail_powered=0 "
      "fixture_neopixel_gpio=21 shared_power_gpio=23"));
  assert(RunRejectedSwingCompletion(
      "outputs_inactive=1 prepared=1 pixel_off=1 i2s_inactive=1 rail_powered=0 "
      "fixture_neopixel_gpio=21 shared_power_gpio=23"));
}

void TestPreservesResetAndErrorEvidence() {
  {
    PseudoTerminal terminal;
    FeatherHilSerial serial(terminal.slave_path());
    constexpr std::string_view kBoot =
        "SC-HIL/1 0 EVENT BOOT firmware=prop-maker-hil-5 device_us=1000010\n";
    std::jthread device([&terminal, kBoot] {
      assert(ReadLine(terminal.master()) == "SC-HIL/1 701 QUERY\n");
      WriteLine(terminal.master(), QueryResponse(701));
      assert(ReadLine(terminal.master()) == "SC-HIL/1 702 LED 100000 44053\n");
      WriteLine(terminal.master(),
                "SC-HIL/1 702 ACK LED accepted_us=1000000 scheduled_us=1100000 lead_us=100000 "
                "duration_us=44053\n");
      WriteLine(terminal.master(), kBoot);
    });
    FeatherHilController controller(serial, 701);
    static_cast<void>(controller.QueryInfo());
    const FeatherHilFailureEvidence evidence = CaptureFailure([&controller] {
      static_cast<void>(controller.PulseLed(std::chrono::microseconds(100'000),
                                            std::chrono::microseconds(44'053)));
    });
    assert(evidence.stage == FeatherHilTransactionStage::kStart);
    assert(evidence.offending_response->wire_line == kBoot);
    assert(evidence.stimulus_receipt->acknowledgement.subject == "LED");
    assert(evidence.stimulus_receipt->started.wire_line.empty());
  }

  {
    PseudoTerminal terminal;
    FeatherHilSerial serial(terminal.slave_path());
    constexpr std::string_view kError = "SC-HIL/1 802 ERR out_of_range device_us=2000000\n";
    std::jthread device([&terminal, kError] {
      assert(ReadLine(terminal.master()) == "SC-HIL/1 801 QUERY\n");
      WriteLine(terminal.master(), QueryResponse(801));
      assert(ReadLine(terminal.master()) == "SC-HIL/1 802 LED 100000 44053\n");
      WriteLine(terminal.master(), kError);
    });
    FeatherHilController controller(serial, 801);
    static_cast<void>(controller.QueryInfo());
    const FeatherHilFailureEvidence evidence = CaptureFailure([&controller] {
      static_cast<void>(controller.PulseLed(std::chrono::microseconds(100'000),
                                            std::chrono::microseconds(44'053)));
    });
    assert(evidence.stage == FeatherHilTransactionStage::kAcknowledgement);
    assert(evidence.offending_response->kind == FeatherResponseKind::kError);
    assert(evidence.offending_response->subject == "out_of_range");
    assert(evidence.offending_response->wire_line == kError);
    assert(evidence.stimulus_receipt->request_id == 802);
  }
}

void TestRejectsParametersBeforeWritingStimulus() {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  std::jthread device([&terminal] {
    assert(ReadLine(terminal.master()) == "SC-HIL/1 401 QUERY\n");
    WriteLine(terminal.master(), QueryResponse(401));
  });
  FeatherHilController controller(serial, 401);
  static_cast<void>(controller.QueryInfo());
  assert(Throws([&controller] {
    static_cast<void>(controller.PlayTone(std::chrono::microseconds(19'999),
                                          std::chrono::microseconds(20'000), 2'000, 125));
  }));
  assert(Throws([&controller] {
    static_cast<void>(controller.PlayTone(std::chrono::microseconds(100'000),
                                          std::chrono::microseconds(20'000), 2'000, 126));
  }));
  assert(Throws([&controller] { static_cast<void>(controller.RunSyntheticSwing(127U)); }));
}

void TestRejectsInvalidRequestSeed() {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  assert(Throws([&serial] { FeatherHilController controller(serial, 0); }));
  assert(Throws([&serial] {
    FeatherHilController controller(serial, std::numeric_limits<std::uint32_t>::max());
  }));
}

}  // namespace

int main() {
  TestExactFirmwareInfoLedAndToneReceipts();
  TestRequiresSuccessfulNegotiationBeforeStimulus();
  TestRejectsIncompatibleNegotiation();
  TestPreservesStructuredQueryFailure();
  TestRejectsInconsistentTimingAndReset();
  TestPreservesOutOfToleranceDurationEvidence();
  TestPreservesOutOfToleranceToneDurationEvidence();
  TestRejectsMalformedCalibrationAndSwingEvidence();
  TestRejectsFixtureStateContradictions();
  TestPreservesResetAndErrorEvidence();
  TestRejectsParametersBeforeWritingStimulus();
  TestRejectsInvalidRequestSeed();
  return 0;
}
