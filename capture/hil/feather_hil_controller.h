#ifndef SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_CONTROLLER_H_
#define SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_CONTROLLER_H_

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "capture/hil/feather_hil_serial.h"

namespace swing_capture::hil {

struct FeatherDeviceInfo {
  std::map<std::string, std::string, std::less<>> fields;
  std::string firmware;
  std::uint32_t protocol_version = 0;
  std::set<std::string, std::less<>> capabilities;
  std::uint32_t maximum_lead_microseconds = 0;
  std::uint32_t led_minimum_duration_microseconds = 0;
  std::uint32_t led_maximum_duration_microseconds = 0;
  std::uint32_t tone_minimum_lead_microseconds = 0;
  std::uint32_t tone_minimum_duration_microseconds = 0;
  std::uint32_t tone_maximum_duration_microseconds = 0;
  std::uint32_t tone_minimum_frequency_hz = 0;
  std::uint32_t tone_maximum_frequency_hz = 0;
  std::uint32_t tone_minimum_level_permille = 0;
  std::uint32_t tone_maximum_level_permille = 0;
  std::uint64_t device_microseconds = 0;
  std::chrono::steady_clock::time_point host_query_write_started;
  std::chrono::steady_clock::time_point host_query_sent;
  std::chrono::steady_clock::time_point host_response_received;
  FeatherResponse response;
};

struct FeatherStimulusReceipt {
  std::uint32_t request_id = 0;
  std::string subject;
  std::uint64_t accepted_device_microseconds = 0;
  std::uint64_t scheduled_device_microseconds = 0;
  std::uint64_t start_device_microseconds = 0;
  std::uint64_t end_device_microseconds = 0;
  std::uint64_t elapsed_device_microseconds = 0;
  std::uint64_t start_lateness_microseconds = 0;
  std::uint64_t quantized_duration_microseconds = 0;
  std::uint32_t requested_lead_microseconds = 0;
  std::uint32_t requested_duration_microseconds = 0;
  std::uint32_t frequency_hz = 0;
  std::uint32_t level_permille = 0;
  std::uint32_t sample_rate_hz = 0;
  std::uint32_t sample_count = 0;
  std::chrono::steady_clock::time_point host_command_write_started;
  std::chrono::steady_clock::time_point host_command_sent;
  std::chrono::steady_clock::time_point host_acknowledgement_received;
  std::chrono::steady_clock::time_point host_start_received;
  std::chrono::steady_clock::time_point host_done_received;
  FeatherResponse acknowledgement;
  FeatherResponse started;
  FeatherResponse done;
};

enum class FeatherHilTransactionStage {
  kQueryWrite,
  kQueryResponse,
  kStimulusWrite,
  kAcknowledgement,
  kStart,
  kDone,
};

[[nodiscard]] std::string_view FeatherHilTransactionStageName(
    FeatherHilTransactionStage stage) noexcept;

// Structured evidence retained when a transaction cannot return its normal
// success object. A parsed response is recorded before semantic validation, so
// malformed echoes and out-of-tolerance timing remain available to reports.
struct FeatherHilFailureEvidence {
  std::uint32_t request_id = 0;
  std::string subject;
  FeatherHilTransactionStage stage = FeatherHilTransactionStage::kQueryWrite;
  std::string command_wire;
  std::optional<FeatherDeviceInfo> device_info;
  std::optional<FeatherStimulusReceipt> stimulus_receipt;
  std::optional<FeatherResponse> offending_response;
};

class FeatherHilTransactionError final : public std::runtime_error {
 public:
  FeatherHilTransactionError(const std::string &message, FeatherHilFailureEvidence evidence);

  [[nodiscard]] const FeatherHilFailureEvidence &evidence() const noexcept;

 private:
  FeatherHilFailureEvidence evidence_;
};

class FeatherHilController final {
 public:
  explicit FeatherHilController(FeatherHilSerial &serial);
  // Deterministic request-ID seed for tests. Production callers should use the
  // single-argument constructor's process/time-derived nontrivial seed.
  FeatherHilController(FeatherHilSerial &serial, std::uint32_t initial_request_id);

  [[nodiscard]] FeatherDeviceInfo QueryInfo();
  [[nodiscard]] FeatherStimulusReceipt PulseLed(std::chrono::microseconds lead,
                                                std::chrono::microseconds duration);
  [[nodiscard]] FeatherStimulusReceipt PlayTone(std::chrono::microseconds lead,
                                                std::chrono::microseconds duration,
                                                std::uint32_t frequency_hz,
                                                std::uint32_t level_permille);

 private:
  struct StimulusCommand {
    std::string subject;
    std::string wire;
    std::uint32_t lead_microseconds = 0;
    std::uint32_t duration_microseconds = 0;
    std::uint32_t frequency_hz = 0;
    std::uint32_t level_permille = 0;
  };

  [[nodiscard]] FeatherResponse ReadFor(std::uint32_t request_id, std::chrono::milliseconds timeout,
                                        bool permit_initial_boot);
  [[nodiscard]] FeatherStimulusReceipt RunStimulus(std::uint32_t request_id,
                                                   const StimulusCommand &command);
  [[nodiscard]] std::uint32_t NextRequestId();
  [[nodiscard]] const FeatherDeviceInfo &RequireNegotiated() const;

  FeatherHilSerial *serial_;
  std::uint32_t next_request_id_;
  std::optional<FeatherDeviceInfo> device_info_;
};

}  // namespace swing_capture::hil

#endif  // SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_CONTROLLER_H_
