#include "capture/hil/feather_hil_controller.h"

#include <unistd.h>

#include <algorithm>
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
#include <vector>

#include "capture/hil/feather_hil_protocol.h"
#include "capture/hil/feather_hil_serial.h"
#include "embedded/prop_maker/swing_sequence.h"

namespace swing_capture::hil {
namespace {

constexpr auto kIoTimeout = std::chrono::milliseconds(500);
constexpr std::uint64_t kMaximumStartLatenessMicroseconds = 5'000;
constexpr std::uint64_t kMaximumLedDurationOvershootMicroseconds = 100;
constexpr std::uint64_t kMaximumToneDurationOvershootMicroseconds = 250;
constexpr std::uint64_t kMaximumOutputShutdownOvershootMicroseconds = 250;

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

bool RequiredBooleanField(const FeatherResponse &response, std::string_view name) {
  const std::uint64_t value = RequiredUnsignedField(response, name);
  if (value > 1U) {
    throw std::runtime_error("Feather HIL boolean field is not zero or one: " + std::string(name));
  }
  return value == 1U;
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

std::vector<std::uint32_t> ParseUnsignedList(std::string_view value) {
  std::vector<std::uint32_t> values;
  std::size_t begin = 0;
  while (begin < value.size()) {
    const std::size_t end = value.find(',', begin);
    const std::string_view token = value.substr(begin, end - begin);
    if (token.empty()) {
      throw std::runtime_error("invalid Feather HIL unsigned list");
    }
    FeatherResponse item;
    item.fields.emplace("value", token);
    values.push_back(RequiredUnsigned32Field(item, "value"));
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  if (values.empty() || value.ends_with(',') ||
      std::ranges::adjacent_find(values, std::equal_to<>()) != values.end()) {
    throw std::runtime_error("invalid Feather HIL unsigned list");
  }
  return values;
}

void ValidateDeviceInfo(const FeatherDeviceInfo &info) {
  const std::set<std::string, std::less<>> expected_capabilities = {"calibrate", "led", "query",
                                                                    "swing", "tone"};
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
      info.tone_maximum_level_permille == kFeatherHilToneMaximumLevelPermille &&
      info.swing_start_lead_microseconds == SWING_HIL_SWING_START_LEAD_US &&
      info.swing_step_microseconds == SWING_HIL_SWING_STEP_US &&
      info.swing_pre_steps == SWING_HIL_SWING_PRE_STEPS &&
      info.swing_white_microseconds == SWING_HIL_SWING_IMPACT_WHITE_US &&
      info.swing_post_steps == SWING_HIL_SWING_POST_STEPS &&
      info.swing_tone_duration_microseconds == SWING_HIL_SWING_TONE_DURATION_US &&
      info.swing_tone_frequency_hz == SWING_HIL_SWING_TONE_FREQUENCY_HZ &&
      info.swing_tone_level_permille == SWING_HIL_SWING_TONE_LEVEL_PERMILLE &&
      info.swing_maximum_lateness_microseconds == SWING_HIL_SWING_MAX_LATENESS_US &&
      info.swing_maximum_impact_delta_microseconds == SWING_HIL_SWING_MAX_IMPACT_COMMAND_DELTA_US &&
      info.swing_color_reference_brightness == SWING_HIL_SWING_COLOR_REFERENCE_BRIGHTNESS &&
      info.calibration_step_microseconds == SWING_HIL_CALIBRATION_STEP_US &&
      info.calibration_candidates.size() == SWING_HIL_CALIBRATION_CANDIDATE_COUNT &&
      info.fixture_neopixel_gpio == kFeatherHilFixtureNeopixelGpio &&
      info.fixture_neopixel_color_order == kFeatherHilFixtureNeopixelColorOrder &&
      info.shared_power_gpio == kFeatherHilSharedPowerGpio &&
      info.prepare_timeout_microseconds == kFeatherHilPrepareTimeoutMicroseconds;
  bool candidates_match = compatible;
  for (std::size_t index = 0; candidates_match && index < info.calibration_candidates.size();
       ++index) {
    candidates_match = info.calibration_candidates[index] == swing_hil_calibration_candidate(index);
  }
  if (!candidates_match) {
    throw std::runtime_error(
        "Feather HIL firmware, protocol, capabilities, limits, or fixture topology mismatch");
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
  info.swing_start_lead_microseconds = RequiredUnsigned32Field(response, "swing_start_lead_us");
  info.swing_step_microseconds = RequiredUnsigned32Field(response, "swing_step_us");
  info.swing_pre_steps = RequiredUnsigned32Field(response, "swing_pre_steps");
  info.swing_white_microseconds = RequiredUnsigned32Field(response, "swing_white_us");
  info.swing_post_steps = RequiredUnsigned32Field(response, "swing_post_steps");
  info.swing_tone_duration_microseconds =
      RequiredUnsigned32Field(response, "swing_tone_duration_us");
  info.swing_tone_frequency_hz = RequiredUnsigned32Field(response, "swing_tone_frequency_hz");
  info.swing_tone_level_permille = RequiredUnsigned32Field(response, "swing_tone_level_permille");
  info.swing_maximum_lateness_microseconds =
      RequiredUnsigned32Field(response, "swing_lateness_max_us");
  info.swing_maximum_impact_delta_microseconds =
      RequiredUnsigned32Field(response, "swing_impact_delta_max_us");
  info.swing_color_reference_brightness =
      RequiredUnsigned32Field(response, "swing_color_reference_brightness");
  info.calibration_step_microseconds = RequiredUnsigned32Field(response, "calibration_step_us");
  info.calibration_candidates =
      ParseUnsignedList(RequiredField(response, "calibration_candidates"));
  if (RequiredUnsigned32Field(response, "calibration_count") !=
      info.calibration_candidates.size()) {
    throw std::runtime_error("Feather calibration candidate count mismatch");
  }
  info.fixture_neopixel_gpio = RequiredUnsigned32Field(response, "fixture_neopixel_gpio");
  info.fixture_neopixel_color_order = RequiredField(response, "fixture_neopixel_color_order");
  info.shared_power_gpio = RequiredUnsigned32Field(response, "shared_power_gpio");
  info.prepare_timeout_microseconds = RequiredUnsignedField(response, "prepare_timeout_us");
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

void PopulateCalibrationAcknowledgement(const FeatherResponse &response,
                                        const FeatherDeviceInfo &info,
                                        FeatherCalibrationReceipt *receipt) {
  RequireResponse(response, FeatherResponseKind::kAcknowledgement, "CALIBRATE");
  receipt->acknowledgement = response;
  receipt->accepted_device_microseconds = RequiredUnsignedField(response, "accepted_us");
  receipt->start_scheduled_device_microseconds =
      RequiredUnsignedField(response, "start_scheduled_us");
  receipt->start_lead_microseconds = RequiredUnsigned32Field(response, "start_lead_us");
  receipt->step_microseconds = RequiredUnsigned32Field(response, "step_us");
  const std::uint32_t step_count = RequiredUnsigned32Field(response, "step_count");
  receipt->requested_duration_microseconds = RequiredUnsigned32Field(response, "duration_us");
  receipt->candidates = ParseUnsignedList(RequiredField(response, "candidates"));
  receipt->fixture_neopixel_gpio = RequiredUnsigned32Field(response, "fixture_neopixel_gpio");
  receipt->shared_power_gpio = RequiredUnsigned32Field(response, "shared_power_gpio");
  receipt->prepare_timeout_microseconds = RequiredUnsignedField(response, "prepare_timeout_us");
  if (receipt->start_lead_microseconds != info.swing_start_lead_microseconds ||
      receipt->step_microseconds != info.calibration_step_microseconds ||
      step_count != info.calibration_candidates.size() ||
      receipt->candidates != info.calibration_candidates ||
      receipt->requested_duration_microseconds !=
          receipt->step_microseconds * receipt->candidates.size() ||
      receipt->start_scheduled_device_microseconds !=
          receipt->accepted_device_microseconds + receipt->start_lead_microseconds ||
      receipt->fixture_neopixel_gpio != info.fixture_neopixel_gpio ||
      receipt->shared_power_gpio != info.shared_power_gpio ||
      receipt->prepare_timeout_microseconds != info.prepare_timeout_microseconds) {
    throw std::runtime_error("inconsistent Feather calibration acknowledgement");
  }
  receipt->steps.reserve(receipt->candidates.size());
}

void AppendCalibrationStep(const FeatherResponse &response, const FeatherDeviceInfo &info,
                           std::chrono::steady_clock::time_point host_received,
                           FeatherCalibrationReceipt *receipt) {
  RequireResponse(response, FeatherResponseKind::kEvent, "CALIBRATE", "STEP");
  FeatherCalibrationStep step = {
      .index = RequiredUnsigned32Field(response, "index"),
      .brightness = RequiredUnsigned32Field(response, "brightness"),
      .scheduled_device_microseconds = RequiredUnsignedField(response, "scheduled_us"),
      .device_microseconds = RequiredUnsignedField(response, "device_us"),
      .lateness_microseconds = RequiredUnsignedField(response, "lateness_us"),
      .host_received = host_received,
      .response = response,
  };
  const std::size_t expected_index = receipt->steps.size();
  const std::uint64_t expected_schedule =
      receipt->start_scheduled_device_microseconds + expected_index * receipt->step_microseconds;
  if (step.index != expected_index || expected_index >= receipt->candidates.size() ||
      step.brightness != receipt->candidates[expected_index] ||
      step.scheduled_device_microseconds != expected_schedule ||
      step.device_microseconds < step.scheduled_device_microseconds ||
      step.lateness_microseconds != step.device_microseconds - step.scheduled_device_microseconds ||
      step.lateness_microseconds > info.swing_maximum_lateness_microseconds) {
    throw std::runtime_error("inconsistent Feather calibration step");
  }
  receipt->steps.push_back(std::move(step));
}

void PopulateCalibrationDone(const FeatherResponse &response, const FeatherDeviceInfo &info,
                             FeatherCalibrationReceipt *receipt) {
  RequireResponse(response, FeatherResponseKind::kEvent, "CALIBRATE", "DONE");
  receipt->done = response;
  receipt->start_device_microseconds = RequiredUnsignedField(response, "start_us");
  receipt->end_device_microseconds = RequiredUnsignedField(response, "end_us");
  receipt->elapsed_device_microseconds = RequiredUnsignedField(response, "elapsed_us");
  receipt->maximum_step_lateness_microseconds =
      RequiredUnsignedField(response, "max_step_lateness_us");
  receipt->power_on_device_microseconds = RequiredUnsignedField(response, "power_on_us");
  receipt->prepared_until_device_microseconds =
      RequiredUnsignedField(response, "prepared_until_us");
  receipt->pixel_off = RequiredBooleanField(response, "pixel_off");
  receipt->i2s_inactive = RequiredBooleanField(response, "i2s_inactive");
  receipt->rail_powered = RequiredBooleanField(response, "rail_powered");
  receipt->prepared = RequiredBooleanField(response, "prepared");
  const std::uint32_t fixture_neopixel_gpio =
      RequiredUnsigned32Field(response, "fixture_neopixel_gpio");
  const std::uint32_t shared_power_gpio = RequiredUnsigned32Field(response, "shared_power_gpio");
  const std::uint32_t repeated_duration = RequiredUnsigned32Field(response, "requested_us");
  std::uint64_t measured_maximum_lateness = 0;
  for (const FeatherCalibrationStep &step : receipt->steps) {
    measured_maximum_lateness = std::max(measured_maximum_lateness, step.lateness_microseconds);
  }
  const bool preparation_deadline_valid =
      receipt->power_on_device_microseconds <=
          std::numeric_limits<std::uint64_t>::max() - receipt->prepare_timeout_microseconds &&
      receipt->prepared_until_device_microseconds ==
          receipt->power_on_device_microseconds + receipt->prepare_timeout_microseconds &&
      receipt->power_on_device_microseconds <= receipt->start_device_microseconds &&
      receipt->prepared_until_device_microseconds > receipt->end_device_microseconds;
  if (receipt->steps.size() != receipt->candidates.size() || receipt->steps.empty() ||
      receipt->start_device_microseconds != receipt->steps.front().device_microseconds ||
      receipt->end_device_microseconds < receipt->start_device_microseconds ||
      receipt->elapsed_device_microseconds !=
          receipt->end_device_microseconds - receipt->start_device_microseconds ||
      repeated_duration != receipt->requested_duration_microseconds ||
      receipt->elapsed_device_microseconds < receipt->requested_duration_microseconds ||
      receipt->elapsed_device_microseconds - receipt->requested_duration_microseconds >
          kMaximumOutputShutdownOvershootMicroseconds ||
      receipt->maximum_step_lateness_microseconds != measured_maximum_lateness ||
      measured_maximum_lateness > info.swing_maximum_lateness_microseconds ||
      fixture_neopixel_gpio != receipt->fixture_neopixel_gpio ||
      shared_power_gpio != receipt->shared_power_gpio || !receipt->pixel_off ||
      !receipt->i2s_inactive || !receipt->rail_powered || !receipt->prepared ||
      !preparation_deadline_valid) {
    throw std::runtime_error("inconsistent Feather calibration completion");
  }
}

void PopulateSwingAcknowledgement(const FeatherResponse &response, const FeatherDeviceInfo &info,
                                  std::uint32_t brightness, FeatherSwingReceipt *receipt) {
  RequireResponse(response, FeatherResponseKind::kAcknowledgement, "SWING");
  receipt->acknowledgement = response;
  receipt->accepted_device_microseconds = RequiredUnsignedField(response, "accepted_us");
  receipt->sequence_start_scheduled_device_microseconds =
      RequiredUnsignedField(response, "sequence_start_scheduled_us");
  receipt->impact_scheduled_device_microseconds =
      RequiredUnsignedField(response, "impact_scheduled_us");
  receipt->post_scheduled_device_microseconds =
      RequiredUnsignedField(response, "post_scheduled_us");
  receipt->end_scheduled_device_microseconds = RequiredUnsignedField(response, "end_scheduled_us");
  receipt->tone_sample_rate_hz = RequiredUnsigned32Field(response, "tone_sample_rate_hz");
  receipt->tone_sample_count = RequiredUnsigned32Field(response, "tone_sample_count");
  receipt->brightness = RequiredUnsigned32Field(response, "brightness");
  receipt->fixture_neopixel_gpio = RequiredUnsigned32Field(response, "fixture_neopixel_gpio");
  receipt->shared_power_gpio = RequiredUnsigned32Field(response, "shared_power_gpio");
  receipt->prepared_at_acknowledgement = RequiredBooleanField(response, "prepared");
  receipt->rail_powered_at_acknowledgement = RequiredBooleanField(response, "rail_powered");
  const std::uint64_t expected_tone_samples =
      static_cast<std::uint64_t>(SWING_HIL_SWING_TONE_DURATION_US) * kFeatherHilToneSampleRateHz /
      1'000'000U;
  const bool constants_match =
      RequiredUnsigned32Field(response, "start_lead_us") == info.swing_start_lead_microseconds &&
      RequiredUnsigned32Field(response, "step_us") == info.swing_step_microseconds &&
      RequiredUnsigned32Field(response, "pre_steps") == info.swing_pre_steps &&
      RequiredUnsigned32Field(response, "white_us") == info.swing_white_microseconds &&
      RequiredUnsigned32Field(response, "post_steps") == info.swing_post_steps &&
      RequiredUnsigned32Field(response, "tone_duration_us") ==
          info.swing_tone_duration_microseconds &&
      RequiredUnsigned32Field(response, "tone_frequency_hz") == info.swing_tone_frequency_hz &&
      RequiredUnsigned32Field(response, "tone_level_permille") == info.swing_tone_level_permille;
  if (!constants_match || receipt->brightness != brightness ||
      receipt->fixture_neopixel_gpio != info.fixture_neopixel_gpio ||
      receipt->shared_power_gpio != info.shared_power_gpio ||
      !receipt->prepared_at_acknowledgement || !receipt->rail_powered_at_acknowledgement ||
      receipt->tone_sample_rate_hz != kFeatherHilToneSampleRateHz ||
      receipt->tone_sample_count != expected_tone_samples ||
      receipt->sequence_start_scheduled_device_microseconds !=
          receipt->accepted_device_microseconds + info.swing_start_lead_microseconds ||
      receipt->impact_scheduled_device_microseconds !=
          receipt->sequence_start_scheduled_device_microseconds +
              static_cast<std::uint64_t>(info.swing_step_microseconds) * info.swing_pre_steps ||
      receipt->post_scheduled_device_microseconds !=
          receipt->impact_scheduled_device_microseconds + info.swing_white_microseconds ||
      receipt->end_scheduled_device_microseconds !=
          receipt->post_scheduled_device_microseconds +
              static_cast<std::uint64_t>(info.swing_step_microseconds) * info.swing_post_steps) {
    throw std::runtime_error("inconsistent Feather swing acknowledgement");
  }
}

FeatherSwingPhase DecodeSwingPhase(const FeatherResponse &response, const FeatherDeviceInfo &info,
                                   std::string_view expected_phase, std::uint64_t expected_schedule,
                                   std::chrono::steady_clock::time_point host_received) {
  RequireResponse(response, FeatherResponseKind::kEvent, "SWING", "PHASE");
  FeatherSwingPhase phase = {
      .phase = RequiredField(response, "phase"),
      .scheduled_device_microseconds = RequiredUnsignedField(response, "scheduled_us"),
      .device_microseconds = RequiredUnsignedField(response, "device_us"),
      .lateness_microseconds = RequiredUnsignedField(response, "lateness_us"),
      .maximum_step_lateness_microseconds = RequiredUnsignedField(response, "max_step_lateness_us"),
      .step_microseconds = RequiredUnsigned32Field(response, "step_us"),
      .step_count = RequiredUnsigned32Field(response, "step_count"),
      .host_received = host_received,
      .response = response,
  };
  const std::uint32_t expected_count =
      expected_phase == "pre" ? info.swing_pre_steps : info.swing_post_steps;
  if (phase.phase != expected_phase || phase.scheduled_device_microseconds != expected_schedule ||
      phase.device_microseconds < phase.scheduled_device_microseconds ||
      phase.lateness_microseconds !=
          phase.device_microseconds - phase.scheduled_device_microseconds ||
      phase.lateness_microseconds > info.swing_maximum_lateness_microseconds ||
      phase.maximum_step_lateness_microseconds > info.swing_maximum_lateness_microseconds ||
      phase.step_microseconds != info.swing_step_microseconds ||
      phase.step_count != expected_count) {
    throw std::runtime_error("inconsistent Feather swing phase: " + std::string(expected_phase));
  }
  return phase;
}

FeatherSwingImpact DecodeSwingImpact(const FeatherResponse &response, const FeatherDeviceInfo &info,
                                     const FeatherSwingReceipt &receipt,
                                     std::chrono::steady_clock::time_point host_received) {
  RequireResponse(response, FeatherResponseKind::kEvent, "SWING", "IMPACT");
  FeatherSwingImpact impact = {
      .scheduled_device_microseconds = RequiredUnsignedField(response, "scheduled_us"),
      .device_microseconds = RequiredUnsignedField(response, "device_us"),
      .lateness_microseconds = RequiredUnsignedField(response, "lateness_us"),
      .white_command_device_microseconds = RequiredUnsignedField(response, "white_command_us"),
      .tone_command_device_microseconds = RequiredUnsignedField(response, "tone_command_us"),
      .command_delta_microseconds = RequiredUnsignedField(response, "command_delta_us"),
      .white_end_device_microseconds = RequiredUnsignedField(response, "white_end_us"),
      .tone_end_device_microseconds = RequiredUnsignedField(response, "tone_end_us"),
      .brightness = RequiredUnsigned32Field(response, "brightness"),
      .host_received = host_received,
      .response = response,
  };
  const std::uint64_t measured_delta =
      impact.white_command_device_microseconds > impact.tone_command_device_microseconds
          ? impact.white_command_device_microseconds - impact.tone_command_device_microseconds
          : impact.tone_command_device_microseconds - impact.white_command_device_microseconds;
  if (impact.tone_end_device_microseconds < impact.tone_command_device_microseconds ||
      impact.white_end_device_microseconds < impact.white_command_device_microseconds) {
    throw std::runtime_error("inconsistent Feather swing impact output times");
  }
  const std::uint64_t tone_elapsed =
      impact.tone_end_device_microseconds - impact.tone_command_device_microseconds;
  const std::uint64_t white_elapsed =
      impact.white_end_device_microseconds - impact.white_command_device_microseconds;
  if (impact.scheduled_device_microseconds != receipt.impact_scheduled_device_microseconds ||
      impact.device_microseconds != impact.white_command_device_microseconds ||
      impact.device_microseconds < impact.scheduled_device_microseconds ||
      impact.lateness_microseconds !=
          impact.device_microseconds - impact.scheduled_device_microseconds ||
      impact.lateness_microseconds > info.swing_maximum_lateness_microseconds ||
      impact.command_delta_microseconds != measured_delta ||
      measured_delta > info.swing_maximum_impact_delta_microseconds ||
      impact.brightness != receipt.brightness ||
      RequiredUnsigned32Field(response, "white_us") != info.swing_white_microseconds ||
      RequiredUnsigned32Field(response, "tone_duration_us") !=
          info.swing_tone_duration_microseconds ||
      RequiredUnsigned32Field(response, "tone_sample_count") != receipt.tone_sample_count ||
      white_elapsed < info.swing_white_microseconds ||
      white_elapsed - info.swing_white_microseconds > kMaximumOutputShutdownOvershootMicroseconds ||
      tone_elapsed < info.swing_tone_duration_microseconds ||
      tone_elapsed - info.swing_tone_duration_microseconds >
          kMaximumToneDurationOvershootMicroseconds) {
    throw std::runtime_error("inconsistent Feather swing impact");
  }
  return impact;
}

void PopulateSwingDone(const FeatherResponse &response, const FeatherDeviceInfo &info,
                       FeatherSwingReceipt *receipt) {
  RequireResponse(response, FeatherResponseKind::kEvent, "SWING", "DONE");
  receipt->done = response;
  receipt->outputs_inactive_at_completion = RequiredBooleanField(response, "outputs_inactive");
  receipt->prepared_at_completion = RequiredBooleanField(response, "prepared");
  receipt->pixel_off_at_completion = RequiredBooleanField(response, "pixel_off");
  receipt->i2s_inactive_at_completion = RequiredBooleanField(response, "i2s_inactive");
  receipt->rail_powered_at_completion = RequiredBooleanField(response, "rail_powered");
  const std::uint32_t fixture_neopixel_gpio =
      RequiredUnsigned32Field(response, "fixture_neopixel_gpio");
  const std::uint32_t shared_power_gpio = RequiredUnsigned32Field(response, "shared_power_gpio");
  const std::uint64_t repeated_start = RequiredUnsignedField(response, "sequence_start_us");
  const std::uint64_t repeated_impact = RequiredUnsignedField(response, "impact_us");
  const std::uint64_t repeated_post_start = RequiredUnsignedField(response, "post_start_us");
  receipt->end_device_microseconds = RequiredUnsignedField(response, "end_us");
  receipt->elapsed_device_microseconds = RequiredUnsignedField(response, "elapsed_us");
  if (receipt->end_device_microseconds < receipt->pre.device_microseconds ||
      receipt->end_device_microseconds < receipt->post.device_microseconds) {
    throw std::runtime_error("inconsistent Feather swing completion times");
  }
  const std::uint64_t post_elapsed =
      receipt->end_device_microseconds - receipt->post.device_microseconds;
  if (repeated_start != receipt->pre.device_microseconds ||
      repeated_impact != receipt->impact.device_microseconds ||
      repeated_post_start != receipt->post.device_microseconds ||
      RequiredUnsigned32Field(response, "pre_us") !=
          info.swing_step_microseconds * info.swing_pre_steps ||
      RequiredUnsigned32Field(response, "post_us") !=
          info.swing_step_microseconds * info.swing_post_steps ||
      receipt->elapsed_device_microseconds !=
          receipt->end_device_microseconds - receipt->pre.device_microseconds ||
      fixture_neopixel_gpio != receipt->fixture_neopixel_gpio ||
      shared_power_gpio != receipt->shared_power_gpio || !receipt->outputs_inactive_at_completion ||
      receipt->prepared_at_completion || !receipt->pixel_off_at_completion ||
      !receipt->i2s_inactive_at_completion || receipt->rail_powered_at_completion ||
      post_elapsed <
          static_cast<std::uint64_t>(info.swing_step_microseconds) * info.swing_post_steps ||
      post_elapsed -
              static_cast<std::uint64_t>(info.swing_step_microseconds) * info.swing_post_steps >
          kMaximumOutputShutdownOvershootMicroseconds) {
    throw std::runtime_error("inconsistent Feather swing completion");
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
    case FeatherHilTransactionStage::kCalibrationStep:
      return "calibration_step";
    case FeatherHilTransactionStage::kPrePhase:
      return "pre_phase";
    case FeatherHilTransactionStage::kImpact:
      return "impact";
    case FeatherHilTransactionStage::kPostPhase:
      return "post_phase";
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
      .calibration_receipt = std::nullopt,
      .swing_receipt = std::nullopt,
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

FeatherCalibrationReceipt FeatherHilController::CalibrateSwingBrightness() {
  static_cast<void>(RequireNegotiated());
  return RunCalibration(NextRequestId());
}

FeatherSwingReceipt FeatherHilController::RunSyntheticSwing(std::uint32_t brightness) {
  const FeatherDeviceInfo &info = RequireNegotiated();
  if (std::ranges::find(info.calibration_candidates, brightness) ==
      info.calibration_candidates.end()) {
    throw std::invalid_argument("swing brightness is not a negotiated calibration candidate");
  }
  return RunSwing(NextRequestId(), brightness);
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
      .calibration_receipt = std::nullopt,
      .swing_receipt = std::nullopt,
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

FeatherCalibrationReceipt FeatherHilController::RunCalibration(std::uint32_t request_id) {
  const FeatherDeviceInfo &info = RequireNegotiated();
  FeatherCalibrationReceipt receipt;
  receipt.request_id = request_id;
  FeatherHilFailureEvidence evidence = {
      .request_id = request_id,
      .subject = "CALIBRATE",
      .stage = FeatherHilTransactionStage::kStimulusWrite,
      .command_wire = BuildFeatherCalibrationCommand(request_id),
      .device_info = info,
      .stimulus_receipt = std::nullopt,
      .calibration_receipt = receipt,
      .swing_receipt = std::nullopt,
      .offending_response = std::nullopt,
  };
  receipt.host_command_write_started = std::chrono::steady_clock::now();
  try {
    serial_->Write(evidence.command_wire, kIoTimeout);
  } catch (const std::exception &error) {
    ThrowTransactionFailure(error, std::move(evidence));
  }
  receipt.host_command_sent = std::chrono::steady_clock::now();

  evidence.stage = FeatherHilTransactionStage::kAcknowledgement;
  try {
    const FeatherResponse acknowledgement = ReadFor(request_id, kIoTimeout, false);
    receipt.host_acknowledgement_received = std::chrono::steady_clock::now();
    evidence.offending_response = acknowledgement;
    RequireResponseRequest(acknowledgement, request_id);
    PopulateCalibrationAcknowledgement(acknowledgement, info, &receipt);
  } catch (const std::exception &error) {
    evidence.calibration_receipt = receipt;
    ThrowTransactionFailure(error, std::move(evidence));
  }

  evidence.stage = FeatherHilTransactionStage::kCalibrationStep;
  for (std::size_t index = 0; index < info.calibration_candidates.size(); ++index) {
    try {
      const auto timeout =
          index == 0U ? CompletionTimeout(
                            info.swing_start_lead_microseconds,
                            info.calibration_step_microseconds *
                                static_cast<std::uint32_t>(info.calibration_candidates.size()))
                      : kIoTimeout;
      const FeatherResponse step = ReadFor(request_id, timeout, false);
      const auto received = std::chrono::steady_clock::now();
      evidence.offending_response = step;
      RequireResponseRequest(step, request_id);
      AppendCalibrationStep(step, info, received, &receipt);
    } catch (const std::exception &error) {
      evidence.calibration_receipt = receipt;
      ThrowTransactionFailure(error, std::move(evidence));
    }
  }

  evidence.stage = FeatherHilTransactionStage::kDone;
  try {
    const FeatherResponse done = ReadFor(request_id, kIoTimeout, false);
    receipt.host_done_received = std::chrono::steady_clock::now();
    evidence.offending_response = done;
    RequireResponseRequest(done, request_id);
    PopulateCalibrationDone(done, info, &receipt);
    return receipt;
  } catch (const std::exception &error) {
    evidence.calibration_receipt = receipt;
    ThrowTransactionFailure(error, std::move(evidence));
  }
}

FeatherSwingReceipt FeatherHilController::RunSwing(std::uint32_t request_id,
                                                   std::uint32_t brightness) {
  const FeatherDeviceInfo &info = RequireNegotiated();
  FeatherSwingReceipt receipt;
  receipt.request_id = request_id;
  FeatherHilFailureEvidence evidence = {
      .request_id = request_id,
      .subject = "SWING",
      .stage = FeatherHilTransactionStage::kStimulusWrite,
      .command_wire = BuildFeatherSwingCommand(request_id, brightness),
      .device_info = info,
      .stimulus_receipt = std::nullopt,
      .calibration_receipt = std::nullopt,
      .swing_receipt = receipt,
      .offending_response = std::nullopt,
  };
  receipt.host_command_write_started = std::chrono::steady_clock::now();
  try {
    serial_->Write(evidence.command_wire, kIoTimeout);
  } catch (const std::exception &error) {
    ThrowTransactionFailure(error, std::move(evidence));
  }
  receipt.host_command_sent = std::chrono::steady_clock::now();

  evidence.stage = FeatherHilTransactionStage::kAcknowledgement;
  try {
    const FeatherResponse acknowledgement = ReadFor(request_id, kIoTimeout, false);
    receipt.host_acknowledgement_received = std::chrono::steady_clock::now();
    evidence.offending_response = acknowledgement;
    RequireResponseRequest(acknowledgement, request_id);
    PopulateSwingAcknowledgement(acknowledgement, info, brightness, &receipt);
  } catch (const std::exception &error) {
    evidence.swing_receipt = receipt;
    ThrowTransactionFailure(error, std::move(evidence));
  }

  const std::uint32_t total_duration = info.swing_step_microseconds * info.swing_pre_steps +
                                       info.swing_white_microseconds +
                                       info.swing_step_microseconds * info.swing_post_steps;
  evidence.stage = FeatherHilTransactionStage::kPrePhase;
  try {
    const FeatherResponse pre = ReadFor(
        request_id, CompletionTimeout(info.swing_start_lead_microseconds, total_duration), false);
    const auto received = std::chrono::steady_clock::now();
    evidence.offending_response = pre;
    RequireResponseRequest(pre, request_id);
    receipt.pre = DecodeSwingPhase(pre, info, "pre",
                                   receipt.sequence_start_scheduled_device_microseconds, received);
  } catch (const std::exception &error) {
    evidence.swing_receipt = receipt;
    ThrowTransactionFailure(error, std::move(evidence));
  }

  evidence.stage = FeatherHilTransactionStage::kImpact;
  try {
    const FeatherResponse impact = ReadFor(request_id, kIoTimeout, false);
    const auto received = std::chrono::steady_clock::now();
    evidence.offending_response = impact;
    RequireResponseRequest(impact, request_id);
    receipt.impact = DecodeSwingImpact(impact, info, receipt, received);
  } catch (const std::exception &error) {
    evidence.swing_receipt = receipt;
    ThrowTransactionFailure(error, std::move(evidence));
  }

  evidence.stage = FeatherHilTransactionStage::kPostPhase;
  try {
    const FeatherResponse post = ReadFor(request_id, kIoTimeout, false);
    const auto received = std::chrono::steady_clock::now();
    evidence.offending_response = post;
    RequireResponseRequest(post, request_id);
    receipt.post =
        DecodeSwingPhase(post, info, "post", receipt.post_scheduled_device_microseconds, received);
  } catch (const std::exception &error) {
    evidence.swing_receipt = receipt;
    ThrowTransactionFailure(error, std::move(evidence));
  }

  evidence.stage = FeatherHilTransactionStage::kDone;
  try {
    const FeatherResponse done = ReadFor(request_id, kIoTimeout, false);
    receipt.host_done_received = std::chrono::steady_clock::now();
    evidence.offending_response = done;
    RequireResponseRequest(done, request_id);
    PopulateSwingDone(done, info, &receipt);
    return receipt;
  } catch (const std::exception &error) {
    evidence.swing_receipt = receipt;
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
