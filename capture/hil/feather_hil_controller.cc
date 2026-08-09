#include "capture/hil/feather_hil_controller.h"

#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "capture/hil/feather_hil_protocol.h"
#include "capture/hil/feather_hil_serial.h"

namespace swing_capture::hil {
namespace {

constexpr auto kIoTimeout = std::chrono::milliseconds(500);
constexpr std::uint64_t kMaximumStartLatenessMicroseconds = 5'000;
constexpr std::uint64_t kMaximumLedDurationOvershootMicroseconds = 100;
constexpr std::uint64_t kMaximumToneDurationOvershootMicroseconds = 250;

struct ExpectedStimulus {
  std::string_view subject;
  std::uint32_t lead_microseconds = 0;
  std::uint32_t duration_microseconds = 0;
  std::uint32_t frequency_hz = 0;
  std::uint32_t level_permille = 0;
};

std::uint32_t RequestIdSeed() {
  std::uint64_t value =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  value ^= static_cast<std::uint64_t>(getpid()) * 0x9e3779b97f4a7c15ULL;
  value ^= value >> 30U;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27U;
  value *= 0x94d049bb133111ebULL;
  value ^= value >> 31U;
  auto seed = static_cast<std::uint32_t>(value ^ (value >> 32U));
  if (seed == 0 || seed == std::numeric_limits<std::uint32_t>::max()) {
    seed = 0x53434849U;  // "SCHI"
  }
  return seed;
}

std::chrono::milliseconds CompletionTimeout(std::uint32_t lead_microseconds,
                                            std::uint32_t duration_microseconds) {
  const auto stimulus_time =
      std::chrono::microseconds(static_cast<std::int64_t>(lead_microseconds) +
                                static_cast<std::int64_t>(duration_microseconds));
  return std::chrono::ceil<std::chrono::milliseconds>(stimulus_time) + kIoTimeout;
}

void RequireResponse(const FeatherResponse &response, FeatherResponseKind kind,
                     std::string_view subject, std::string_view state = {}) {
  if (response.kind != kind || response.subject != subject ||
      (!state.empty() && response.state != state)) {
    throw std::runtime_error("unexpected Feather HIL response for " + std::string(subject));
  }
}

void RequireResponseRequest(const FeatherResponse &response, std::uint32_t request_id) {
  const bool boot = response.request_id == 0 && response.kind == FeatherResponseKind::kEvent &&
                    response.subject == "BOOT";
  if (boot) {
    throw std::runtime_error("Feather reset while request " + std::to_string(request_id) +
                             " was active");
  }
  if (response.request_id != request_id) {
    throw std::runtime_error("received out-of-order Feather HIL response for request " +
                             std::to_string(response.request_id));
  }
  if (response.kind == FeatherResponseKind::kError) {
    throw std::runtime_error("Feather HIL request " + std::to_string(request_id) +
                             " failed: " + response.subject);
  }
}

[[noreturn]] void ThrowTransactionFailure(const std::exception &cause,
                                          FeatherHilFailureEvidence evidence) {
  const std::string message = "Feather HIL " + evidence.subject + " transaction failed at " +
                              std::string(FeatherHilTransactionStageName(evidence.stage)) + ": " +
                              cause.what();
  throw FeatherHilTransactionError(message, std::move(evidence));
}

const std::string &RequiredField(const FeatherResponse &response, std::string_view name) {
  const auto found = response.fields.find(name);
  if (found == response.fields.end()) {
    throw std::runtime_error("missing Feather HIL response field: " + std::string(name));
  }
  return found->second;
}

std::uint32_t RequiredUnsigned32Field(const FeatherResponse &response, std::string_view name) {
  const std::uint64_t value = RequiredUnsignedField(response, name);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("Feather HIL field exceeds uint32: " + std::string(name));
  }
  return static_cast<std::uint32_t>(value);
}

std::set<std::string, std::less<>> ParseCapabilities(std::string_view value) {
  std::set<std::string, std::less<>> capabilities;
  std::size_t begin = 0;
  while (begin < value.size()) {
    const std::size_t end = value.find(',', begin);
    const std::string capability(value.substr(begin, end - begin));
    if (capability.empty() || !capabilities.insert(capability).second) {
      throw std::runtime_error("invalid Feather HIL capability list");
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  if (capabilities.empty() || value.ends_with(',')) {
    throw std::runtime_error("invalid Feather HIL capability list");
  }
  return capabilities;
}

void ValidateDeviceInfo(const FeatherDeviceInfo &info) {
  const std::set<std::string, std::less<>> expected_capabilities = {"led", "query", "tone"};
  const bool compatible =
      info.firmware == kFeatherHilFirmware && info.protocol_version == kFeatherHilProtocolVersion &&
      info.capabilities == expected_capabilities &&
      info.maximum_lead_microseconds == kFeatherHilMaximumLeadMicroseconds &&
      info.led_minimum_duration_microseconds == kFeatherHilLedMinimumDurationMicroseconds &&
      info.led_maximum_duration_microseconds == kFeatherHilLedMaximumDurationMicroseconds &&
      info.tone_minimum_lead_microseconds == kFeatherHilToneMinimumLeadMicroseconds &&
      info.tone_minimum_duration_microseconds == kFeatherHilToneMinimumDurationMicroseconds &&
      info.tone_maximum_duration_microseconds == kFeatherHilToneMaximumDurationMicroseconds &&
      info.tone_minimum_frequency_hz == kFeatherHilToneMinimumFrequencyHz &&
      info.tone_maximum_frequency_hz == kFeatherHilToneMaximumFrequencyHz &&
      info.tone_minimum_level_permille == kFeatherHilToneMinimumLevelPermille &&
      info.tone_maximum_level_permille == kFeatherHilToneMaximumLevelPermille;
  if (!compatible) {
    throw std::runtime_error("Feather HIL firmware, protocol, capabilities, or limits mismatch");
  }
}

FeatherDeviceInfo DecodeDeviceInfo(const FeatherResponse &response) {
  FeatherDeviceInfo info;
  info.fields = response.fields;
  info.firmware = RequiredField(response, "firmware");
  info.protocol_version = RequiredUnsigned32Field(response, "protocol");
  info.capabilities = ParseCapabilities(RequiredField(response, "capabilities"));
  info.maximum_lead_microseconds = RequiredUnsigned32Field(response, "lead_max_us");
  info.led_minimum_duration_microseconds = RequiredUnsigned32Field(response, "led_duration_min_us");
  info.led_maximum_duration_microseconds = RequiredUnsigned32Field(response, "led_duration_max_us");
  info.tone_minimum_lead_microseconds = RequiredUnsigned32Field(response, "tone_lead_min_us");
  info.tone_minimum_duration_microseconds =
      RequiredUnsigned32Field(response, "tone_duration_min_us");
  info.tone_maximum_duration_microseconds =
      RequiredUnsigned32Field(response, "tone_duration_max_us");
  info.tone_minimum_frequency_hz = RequiredUnsigned32Field(response, "tone_frequency_min_hz");
  info.tone_maximum_frequency_hz = RequiredUnsigned32Field(response, "tone_frequency_max_hz");
  info.tone_minimum_level_permille = RequiredUnsigned32Field(response, "tone_level_min_permille");
  info.tone_maximum_level_permille = RequiredUnsigned32Field(response, "tone_level_max_permille");
  info.device_microseconds = RequiredUnsignedField(response, "device_us");
  info.response = response;
  return info;
}

std::uint32_t CheckedMicroseconds(std::chrono::microseconds value, std::string_view name) {
  constexpr auto kMaximum =
      static_cast<std::chrono::microseconds::rep>(std::numeric_limits<std::uint32_t>::max());
  if (value < std::chrono::microseconds::zero() || value.count() > kMaximum) {
    throw std::invalid_argument(std::string(name) + " must fit a nonnegative uint32");
  }
  return static_cast<std::uint32_t>(value.count());
}

void PopulateAcknowledgement(const FeatherResponse &response, const ExpectedStimulus &command,
                             FeatherStimulusReceipt *receipt) {
  RequireResponse(response, FeatherResponseKind::kAcknowledgement, command.subject);
  receipt->acknowledgement = response;
  receipt->accepted_device_microseconds = RequiredUnsignedField(response, "accepted_us");
  receipt->scheduled_device_microseconds = RequiredUnsignedField(response, "scheduled_us");
  receipt->requested_lead_microseconds = RequiredUnsigned32Field(response, "lead_us");
  receipt->requested_duration_microseconds = RequiredUnsigned32Field(response, "duration_us");
  if (receipt->requested_lead_microseconds != command.lead_microseconds ||
      receipt->requested_duration_microseconds != command.duration_microseconds ||
      receipt->accepted_device_microseconds >
          std::numeric_limits<std::uint64_t>::max() - command.lead_microseconds ||
      receipt->scheduled_device_microseconds !=
          receipt->accepted_device_microseconds + command.lead_microseconds) {
    throw std::runtime_error("inconsistent Feather HIL acknowledgement for " +
                             std::string(command.subject));
  }

  if (command.subject != "TONE") {
    receipt->quantized_duration_microseconds = command.duration_microseconds;
    return;
  }
  receipt->frequency_hz = RequiredUnsigned32Field(response, "frequency_hz");
  receipt->level_permille = RequiredUnsigned32Field(response, "level_permille");
  receipt->sample_rate_hz = RequiredUnsigned32Field(response, "sample_rate_hz");
  receipt->sample_count = RequiredUnsigned32Field(response, "sample_count");
  const std::uint64_t expected_samples =
      (static_cast<std::uint64_t>(command.duration_microseconds) * kFeatherHilToneSampleRateHz +
       999'999U) /
      1'000'000U;
  if (receipt->frequency_hz != command.frequency_hz ||
      receipt->level_permille != command.level_permille ||
      receipt->sample_rate_hz != kFeatherHilToneSampleRateHz ||
      receipt->sample_count != expected_samples) {
    throw std::runtime_error("inconsistent Feather HIL tone acknowledgement");
  }
  receipt->quantized_duration_microseconds =
      (static_cast<std::uint64_t>(receipt->sample_count) * 1'000'000U + receipt->sample_rate_hz -
       1U) /
      receipt->sample_rate_hz;
}

void PopulateStart(const FeatherResponse &response, FeatherStimulusReceipt *receipt) {
  RequireResponse(response, FeatherResponseKind::kEvent, receipt->subject, "START");
  receipt->started = response;
  const std::uint64_t repeated_schedule = RequiredUnsignedField(response, "scheduled_us");
  receipt->start_device_microseconds = RequiredUnsignedField(response, "device_us");
  receipt->start_lateness_microseconds = RequiredUnsignedField(response, "lateness_us");
  if (repeated_schedule != receipt->scheduled_device_microseconds ||
      receipt->start_device_microseconds < receipt->scheduled_device_microseconds ||
      receipt->start_lateness_microseconds !=
          receipt->start_device_microseconds - receipt->scheduled_device_microseconds ||
      receipt->start_lateness_microseconds > kMaximumStartLatenessMicroseconds) {
    throw std::runtime_error("inconsistent or late Feather HIL START for " + receipt->subject);
  }
}

void PopulateDone(const FeatherResponse &response, FeatherStimulusReceipt *receipt) {
  RequireResponse(response, FeatherResponseKind::kEvent, receipt->subject, "DONE");
  receipt->done = response;
  const std::uint64_t repeated_start = RequiredUnsignedField(response, "start_us");
  receipt->end_device_microseconds = RequiredUnsignedField(response, "end_us");
  receipt->elapsed_device_microseconds = RequiredUnsignedField(response, "elapsed_us");
  const std::uint32_t repeated_duration = RequiredUnsigned32Field(response, "requested_us");
  if (repeated_start != receipt->start_device_microseconds ||
      repeated_duration != receipt->requested_duration_microseconds ||
      receipt->end_device_microseconds < receipt->start_device_microseconds ||
      receipt->elapsed_device_microseconds !=
          receipt->end_device_microseconds - receipt->start_device_microseconds) {
    throw std::runtime_error("inconsistent Feather HIL DONE for " + receipt->subject);
  }

  std::uint64_t maximum_overshoot = kMaximumLedDurationOvershootMicroseconds;
  if (receipt->subject == "TONE") {
    const std::uint32_t repeated_rate = RequiredUnsigned32Field(response, "sample_rate_hz");
    const std::uint32_t repeated_count = RequiredUnsigned32Field(response, "sample_count");
    if (repeated_rate != receipt->sample_rate_hz || repeated_count != receipt->sample_count) {
      throw std::runtime_error("inconsistent Feather HIL tone sample receipt");
    }
    maximum_overshoot = kMaximumToneDurationOvershootMicroseconds;
  }
  if (receipt->elapsed_device_microseconds < receipt->quantized_duration_microseconds ||
      receipt->elapsed_device_microseconds - receipt->quantized_duration_microseconds >
          maximum_overshoot) {
    throw std::runtime_error("Feather HIL stimulus duration outside tolerance for " +
                             receipt->subject);
  }
}

}  // namespace

std::string_view FeatherHilTransactionStageName(FeatherHilTransactionStage stage) noexcept {
  switch (stage) {
    case FeatherHilTransactionStage::kQueryWrite:
      return "query_write";
    case FeatherHilTransactionStage::kQueryResponse:
      return "query_response";
    case FeatherHilTransactionStage::kStimulusWrite:
      return "stimulus_write";
    case FeatherHilTransactionStage::kAcknowledgement:
      return "acknowledgement";
    case FeatherHilTransactionStage::kStart:
      return "start";
    case FeatherHilTransactionStage::kDone:
      return "done";
  }
  return "unknown";
}

FeatherHilTransactionError::FeatherHilTransactionError(const std::string &message,
                                                       FeatherHilFailureEvidence evidence)
    : std::runtime_error(message), evidence_(std::move(evidence)) {}

const FeatherHilFailureEvidence &FeatherHilTransactionError::evidence() const noexcept {
  return evidence_;
}

FeatherHilController::FeatherHilController(FeatherHilSerial &serial)
    : FeatherHilController(serial, RequestIdSeed()) {}

FeatherHilController::FeatherHilController(FeatherHilSerial &serial,
                                           std::uint32_t initial_request_id)
    : serial_(&serial), next_request_id_(initial_request_id) {
  if (initial_request_id == 0 || initial_request_id == std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("Feather HIL initial request ID must be usable and nonzero");
  }
}

FeatherDeviceInfo FeatherHilController::QueryInfo() {
  device_info_.reset();
  const std::uint32_t request_id = NextRequestId();
  FeatherHilFailureEvidence evidence = {
      .request_id = request_id,
      .subject = "QUERY",
      .stage = FeatherHilTransactionStage::kQueryWrite,
      .command_wire = BuildFeatherQueryCommand(request_id),
      .device_info = std::nullopt,
      .stimulus_receipt = std::nullopt,
      .offending_response = std::nullopt,
  };
  const auto write_started = std::chrono::steady_clock::now();
  try {
    serial_->Write(evidence.command_wire, kIoTimeout);
  } catch (const std::exception &error) {
    ThrowTransactionFailure(error, std::move(evidence));
  }
  const auto sent = std::chrono::steady_clock::now();
  evidence.stage = FeatherHilTransactionStage::kQueryResponse;
  try {
    const FeatherResponse response = ReadFor(request_id, kIoTimeout, true);
    const auto received = std::chrono::steady_clock::now();
    evidence.offending_response = response;
    RequireResponseRequest(response, request_id);
    RequireResponse(response, FeatherResponseKind::kOk, "QUERY");
    FeatherDeviceInfo info = DecodeDeviceInfo(response);
    info.host_query_write_started = write_started;
    info.host_query_sent = sent;
    info.host_response_received = received;
    evidence.device_info = info;
    ValidateDeviceInfo(info);
    device_info_ = info;
    return info;
  } catch (const std::exception &error) {
    ThrowTransactionFailure(error, std::move(evidence));
  }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
FeatherStimulusReceipt FeatherHilController::PulseLed(std::chrono::microseconds lead,
                                                      std::chrono::microseconds duration) {
  const FeatherDeviceInfo &info = RequireNegotiated();
  const std::uint32_t lead_us = CheckedMicroseconds(lead, "LED lead");
  const std::uint32_t duration_us = CheckedMicroseconds(duration, "LED duration");
  if (lead_us > info.maximum_lead_microseconds ||
      duration_us < info.led_minimum_duration_microseconds ||
      duration_us > info.led_maximum_duration_microseconds) {
    throw std::invalid_argument("LED parameters exceed negotiated Feather HIL limits");
  }
  const std::uint32_t request_id = NextRequestId();
  return RunStimulus(request_id, {.subject = "LED",
                                  .wire = BuildFeatherLedCommand(request_id, lead_us, duration_us),
                                  .lead_microseconds = lead_us,
                                  .duration_microseconds = duration_us});
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
FeatherStimulusReceipt FeatherHilController::PlayTone(std::chrono::microseconds lead,
                                                      std::chrono::microseconds duration,
                                                      std::uint32_t frequency_hz,
                                                      std::uint32_t level_permille) {
  const FeatherDeviceInfo &info = RequireNegotiated();
  const std::uint32_t lead_us = CheckedMicroseconds(lead, "tone lead");
  const std::uint32_t duration_us = CheckedMicroseconds(duration, "tone duration");
  if (lead_us < info.tone_minimum_lead_microseconds || lead_us > info.maximum_lead_microseconds ||
      duration_us < info.tone_minimum_duration_microseconds ||
      duration_us > info.tone_maximum_duration_microseconds ||
      frequency_hz < info.tone_minimum_frequency_hz ||
      frequency_hz > info.tone_maximum_frequency_hz ||
      level_permille < info.tone_minimum_level_permille ||
      level_permille > info.tone_maximum_level_permille) {
    throw std::invalid_argument("tone parameters exceed negotiated Feather HIL limits");
  }
  const std::uint32_t request_id = NextRequestId();
  return RunStimulus(request_id, {.subject = "TONE",
                                  .wire = BuildFeatherToneCommand(request_id, lead_us, duration_us,
                                                                  frequency_hz, level_permille),
                                  .lead_microseconds = lead_us,
                                  .duration_microseconds = duration_us,
                                  .frequency_hz = frequency_hz,
                                  .level_permille = level_permille});
}

FeatherResponse FeatherHilController::ReadFor(std::uint32_t request_id,
                                              std::chrono::milliseconds timeout,
                                              bool permit_initial_boot) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      throw std::runtime_error("timed out waiting for Feather HIL request " +
                               std::to_string(request_id));
    }
    FeatherResponse response = serial_->Read(remaining);
    const bool boot = response.request_id == 0 && response.kind == FeatherResponseKind::kEvent &&
                      response.subject == "BOOT";
    if (boot && permit_initial_boot) {
      continue;
    }
    return response;
  }
}

FeatherStimulusReceipt FeatherHilController::RunStimulus(std::uint32_t request_id,
                                                         const StimulusCommand &command) {
  FeatherStimulusReceipt receipt;
  receipt.request_id = request_id;
  receipt.subject = command.subject;
  FeatherHilFailureEvidence evidence = {
      .request_id = request_id,
      .subject = command.subject,
      .stage = FeatherHilTransactionStage::kStimulusWrite,
      .command_wire = command.wire,
      .device_info = std::nullopt,
      .stimulus_receipt = receipt,
      .offending_response = std::nullopt,
  };
  receipt.host_command_write_started = std::chrono::steady_clock::now();
  evidence.stimulus_receipt = receipt;
  try {
    serial_->Write(command.wire, kIoTimeout);
  } catch (const std::exception &error) {
    evidence.stimulus_receipt = receipt;
    ThrowTransactionFailure(error, std::move(evidence));
  }
  receipt.host_command_sent = std::chrono::steady_clock::now();

  evidence.stage = FeatherHilTransactionStage::kAcknowledgement;
  evidence.offending_response.reset();
  try {
    const FeatherResponse acknowledgement = ReadFor(request_id, kIoTimeout, false);
    receipt.host_acknowledgement_received = std::chrono::steady_clock::now();
    evidence.offending_response = acknowledgement;
    RequireResponseRequest(acknowledgement, request_id);
    PopulateAcknowledgement(acknowledgement,
                            {.subject = command.subject,
                             .lead_microseconds = command.lead_microseconds,
                             .duration_microseconds = command.duration_microseconds,
                             .frequency_hz = command.frequency_hz,
                             .level_permille = command.level_permille},
                            &receipt);
  } catch (const std::exception &error) {
    evidence.stimulus_receipt = receipt;
    ThrowTransactionFailure(error, std::move(evidence));
  }

  evidence.stage = FeatherHilTransactionStage::kStart;
  evidence.offending_response.reset();
  try {
    const FeatherResponse started =
        ReadFor(request_id,
                CompletionTimeout(command.lead_microseconds, command.duration_microseconds), false);
    receipt.host_start_received = std::chrono::steady_clock::now();
    evidence.offending_response = started;
    RequireResponseRequest(started, request_id);
    PopulateStart(started, &receipt);
  } catch (const std::exception &error) {
    evidence.stimulus_receipt = receipt;
    ThrowTransactionFailure(error, std::move(evidence));
  }

  evidence.stage = FeatherHilTransactionStage::kDone;
  evidence.offending_response.reset();
  try {
    const FeatherResponse done = ReadFor(request_id, kIoTimeout, false);
    receipt.host_done_received = std::chrono::steady_clock::now();
    evidence.offending_response = done;
    RequireResponseRequest(done, request_id);
    PopulateDone(done, &receipt);
    return receipt;
  } catch (const std::exception &error) {
    evidence.stimulus_receipt = receipt;
    ThrowTransactionFailure(error, std::move(evidence));
  }
}

std::uint32_t FeatherHilController::NextRequestId() {
  if (next_request_id_ == 0 || next_request_id_ == std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("Feather HIL request ID space exhausted");
  }
  return next_request_id_++;
}

const FeatherDeviceInfo &FeatherHilController::RequireNegotiated() const {
  if (!device_info_.has_value()) {
    throw std::logic_error("QueryInfo must negotiate the Feather HIL contract before a stimulus");
  }
  return *device_info_;
}

}  // namespace swing_capture::hil
