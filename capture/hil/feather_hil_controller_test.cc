#include "capture/hil/feather_hil_controller.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
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
    "firmware=prop-maker-hil-1 protocol=1 capabilities=query,led,tone "
    "lead_max_us=2000000 led_duration_min_us=100 led_duration_max_us=1000000 "
    "tone_lead_min_us=20000 tone_duration_min_us=1000 tone_duration_max_us=250000 "
    "tone_frequency_min_hz=100 tone_frequency_max_hz=10000 tone_level_min_permille=1 "
    "tone_level_max_permille=125 device_us=900000";

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
    WriteLine(terminal.master(), "SC-HIL/1 0 EVENT BOOT firmware=prop-maker-hil-1 device_us=10\n");
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
  });

  FeatherHilController controller(serial, 101);
  const auto info = controller.QueryInfo();
  assert(info.firmware == "prop-maker-hil-1");
  assert(info.protocol_version == 1);
  assert(info.capabilities.contains("led"));
  assert(info.maximum_lead_microseconds == 2'000'000);
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
  ExpectQueryRejected(
      "firmware=wrong protocol=1 capabilities=query,led,tone lead_max_us=2000000 "
      "led_duration_min_us=100 led_duration_max_us=1000000 tone_lead_min_us=20000 "
      "tone_duration_min_us=1000 tone_duration_max_us=250000 tone_frequency_min_hz=100 "
      "tone_frequency_max_hz=10000 tone_level_min_permille=1 tone_level_max_permille=125 "
      "device_us=1");
  ExpectQueryRejected(
      "firmware=prop-maker-hil-1 protocol=1 capabilities=query,led lead_max_us=2000000 "
      "led_duration_min_us=100 led_duration_max_us=1000000 tone_lead_min_us=20000 "
      "tone_duration_min_us=1000 tone_duration_max_us=250000 tone_frequency_min_hz=100 "
      "tone_frequency_max_hz=10000 tone_level_min_permille=1 tone_level_max_permille=125 "
      "device_us=1");
  ExpectQueryRejected(
      "firmware=prop-maker-hil-1 protocol=1 capabilities=query,led,tone lead_max_us=1999999 "
      "led_duration_min_us=100 led_duration_max_us=1000000 tone_lead_min_us=20000 "
      "tone_duration_min_us=1000 tone_duration_max_us=250000 tone_frequency_min_hz=100 "
      "tone_frequency_max_hz=10000 tone_level_min_permille=1 tone_level_max_permille=125 "
      "device_us=1");
}

void TestPreservesStructuredQueryFailure() {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  const std::string response = QueryResponse(
      251,
      "firmware=wrong protocol=1 capabilities=query,led,tone lead_max_us=2000000 "
      "led_duration_min_us=100 led_duration_max_us=1000000 tone_lead_min_us=20000 "
      "tone_duration_min_us=1000 tone_duration_max_us=250000 tone_frequency_min_hz=100 "
      "tone_frequency_max_hz=10000 tone_level_min_permille=1 tone_level_max_permille=125 "
      "device_us=1");
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
      "SC-HIL/1 0 EVENT BOOT firmware=prop-maker-hil-1 device_us=1000010\n",
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

void TestPreservesResetAndErrorEvidence() {
  {
    PseudoTerminal terminal;
    FeatherHilSerial serial(terminal.slave_path());
    constexpr std::string_view kBoot =
        "SC-HIL/1 0 EVENT BOOT firmware=prop-maker-hil-1 device_us=1000010\n";
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
  TestPreservesResetAndErrorEvidence();
  TestRejectsParametersBeforeWritingStimulus();
  TestRejectsInvalidRequestSeed();
  return 0;
}
