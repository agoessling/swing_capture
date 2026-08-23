#ifndef SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_CONTROLLER_H_
#define SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_CONTROLLER_H_

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
  std::uint32_t pcm_sample_rate_hz = 0;
  std::uint32_t pcm_maximum_samples = 0;
  std::uint32_t pcm_maximum_chunk_bytes = 0;
  std::uint32_t pcm_minimum_lead_microseconds = 0;
  std::uint32_t pcm_minimum_gain_permille = 0;
  std::uint32_t pcm_maximum_gain_permille = 0;
  std::uint32_t pcm_white_microseconds = 0;
  std::uint32_t swing_start_lead_microseconds = 0;
  std::uint32_t swing_step_microseconds = 0;
  std::uint32_t swing_pre_steps = 0;
  std::uint32_t swing_white_microseconds = 0;
  std::uint32_t swing_post_steps = 0;
  std::uint32_t swing_tone_duration_microseconds = 0;
  std::uint32_t swing_tone_frequency_hz = 0;
  std::uint32_t swing_tone_level_permille = 0;
  std::uint32_t swing_maximum_lateness_microseconds = 0;
  std::uint32_t swing_maximum_impact_delta_microseconds = 0;
  std::uint32_t swing_color_reference_brightness = 0;
  std::uint32_t calibration_step_microseconds = 0;
  std::vector<std::uint32_t> calibration_candidates;
  std::uint32_t fixture_neopixel_gpio = 0;
  std::string fixture_neopixel_color_order;
  std::uint32_t shared_power_gpio = 0;
  std::uint64_t prepare_timeout_microseconds = 0;
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

struct FeatherCalibrationStep {
  std::uint32_t index = 0;
  std::uint32_t brightness = 0;
  std::uint64_t scheduled_device_microseconds = 0;
  std::uint64_t device_microseconds = 0;
  std::uint64_t lateness_microseconds = 0;
  std::chrono::steady_clock::time_point host_received;
  FeatherResponse response;
};

struct FeatherCalibrationReceipt {
  std::uint32_t request_id = 0;
  std::uint64_t accepted_device_microseconds = 0;
  std::uint64_t start_scheduled_device_microseconds = 0;
  std::uint64_t start_device_microseconds = 0;
  std::uint64_t end_device_microseconds = 0;
  std::uint64_t elapsed_device_microseconds = 0;
  std::uint64_t maximum_step_lateness_microseconds = 0;
  std::uint32_t start_lead_microseconds = 0;
  std::uint32_t step_microseconds = 0;
  std::uint32_t requested_duration_microseconds = 0;
  std::uint32_t fixture_neopixel_gpio = 0;
  std::uint32_t shared_power_gpio = 0;
  std::uint64_t prepare_timeout_microseconds = 0;
  std::uint64_t power_on_device_microseconds = 0;
  std::uint64_t prepared_until_device_microseconds = 0;
  bool pixel_off = false;
  bool i2s_inactive = false;
  bool rail_powered = false;
  bool prepared = false;
  std::vector<std::uint32_t> candidates;
  std::vector<FeatherCalibrationStep> steps;
  std::chrono::steady_clock::time_point host_command_write_started;
  std::chrono::steady_clock::time_point host_command_sent;
  std::chrono::steady_clock::time_point host_acknowledgement_received;
  std::chrono::steady_clock::time_point host_done_received;
  FeatherResponse acknowledgement;
  FeatherResponse done;
};

struct FeatherSwingPhase {
  std::string phase;
  std::uint64_t scheduled_device_microseconds = 0;
  std::uint64_t device_microseconds = 0;
  std::uint64_t lateness_microseconds = 0;
  std::uint64_t maximum_step_lateness_microseconds = 0;
  std::uint32_t step_microseconds = 0;
  std::uint32_t step_count = 0;
  std::chrono::steady_clock::time_point host_received;
  FeatherResponse response;
};

struct FeatherSwingImpact {
  std::uint64_t scheduled_device_microseconds = 0;
  std::uint64_t device_microseconds = 0;
  std::uint64_t lateness_microseconds = 0;
  std::uint64_t white_command_device_microseconds = 0;
  std::uint64_t tone_command_device_microseconds = 0;
  std::uint64_t command_delta_microseconds = 0;
  std::uint64_t white_end_device_microseconds = 0;
  std::uint64_t tone_end_device_microseconds = 0;
  std::uint32_t brightness = 0;
  std::chrono::steady_clock::time_point host_received;
  FeatherResponse response;
};

struct FeatherSwingReceipt {
  std::uint32_t request_id = 0;
  std::uint32_t brightness = 0;
  std::uint64_t accepted_device_microseconds = 0;
  std::uint64_t sequence_start_scheduled_device_microseconds = 0;
  std::uint64_t impact_scheduled_device_microseconds = 0;
  std::uint64_t post_scheduled_device_microseconds = 0;
  std::uint64_t end_scheduled_device_microseconds = 0;
  std::uint64_t end_device_microseconds = 0;
  std::uint64_t elapsed_device_microseconds = 0;
  std::uint32_t tone_sample_rate_hz = 0;
  std::uint32_t tone_sample_count = 0;
  std::uint32_t fixture_neopixel_gpio = 0;
  std::uint32_t shared_power_gpio = 0;
  bool prepared_at_acknowledgement = false;
  bool rail_powered_at_acknowledgement = false;
  bool outputs_inactive_at_completion = false;
  bool prepared_at_completion = false;
  bool pixel_off_at_completion = false;
  bool i2s_inactive_at_completion = false;
  bool rail_powered_at_completion = false;
  FeatherSwingPhase pre;
  FeatherSwingImpact impact;
  FeatherSwingPhase post;
  std::chrono::steady_clock::time_point host_command_write_started;
  std::chrono::steady_clock::time_point host_command_sent;
  std::chrono::steady_clock::time_point host_acknowledgement_received;
  std::chrono::steady_clock::time_point host_done_received;
  FeatherResponse acknowledgement;
  FeatherResponse done;
};

struct FeatherPcmUploadReceipt {
  std::uint32_t request_id = 0;
  std::uint32_t sample_rate_hz = 0;
  std::uint32_t sample_count = 0;
  std::uint32_t byte_count = 0;
  std::uint32_t source_crc32 = 0;
  std::uint32_t committed_crc32 = 0;
  std::vector<FeatherResponse> acknowledgements;
  std::chrono::steady_clock::time_point host_upload_started;
  std::chrono::steady_clock::time_point host_commit_received;
};

struct FeatherPcmPlaybackReceipt {
  std::uint32_t request_id = 0;
  std::uint64_t accepted_device_microseconds = 0;
  std::uint64_t scheduled_audio_device_microseconds = 0;
  std::uint64_t audio_command_device_microseconds = 0;
  std::uint64_t audio_lateness_microseconds = 0;
  std::uint64_t scheduled_marker_device_microseconds = 0;
  std::uint64_t marker_device_microseconds = 0;
  std::uint64_t marker_lateness_microseconds = 0;
  std::uint64_t marker_offset_microseconds = 0;
  std::uint64_t command_delta_microseconds = 0;
  std::uint64_t command_delta_error_microseconds = 0;
  std::uint64_t white_end_device_microseconds = 0;
  std::uint64_t audio_done_observed_device_microseconds = 0;
  std::uint64_t end_device_microseconds = 0;
  std::uint64_t elapsed_device_microseconds = 0;
  std::uint32_t requested_lead_microseconds = 0;
  std::uint32_t sample_rate_hz = 0;
  std::uint32_t sample_count = 0;
  std::uint32_t marker_sample = 0;
  std::uint32_t gain_permille = 0;
  std::uint32_t brightness = 0;
  std::uint32_t source_crc32 = 0;
  std::uint32_t expected_played_crc32 = 0;
  std::uint32_t played_crc32 = 0;
  std::uint32_t white_microseconds = 0;
  std::string prepare_source;
  bool rail_powered_at_acknowledgement = false;
  bool prepared_at_acknowledgement = false;
  bool outputs_inactive_at_completion = false;
  bool pixel_off_at_completion = false;
  bool i2s_inactive_at_completion = false;
  bool rail_powered_at_completion = false;
  bool prepared_at_completion = false;
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
  kCalibrationStep,
  kPrePhase,
  kImpact,
  kPostPhase,
  kDone,
  kPcmBegin,
  kPcmChunk,
  kPcmCommit,
  kPcmAbort,
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
  std::optional<FeatherCalibrationReceipt> calibration_receipt;
  std::optional<FeatherSwingReceipt> swing_receipt;
  std::optional<FeatherPcmUploadReceipt> pcm_upload_receipt;
  std::optional<FeatherPcmPlaybackReceipt> pcm_playback_receipt;
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
  [[nodiscard]] FeatherPcmUploadReceipt UploadPcm16(std::span<const std::int16_t> samples);
  [[nodiscard]] FeatherPcmPlaybackReceipt PlayUploadedPcm(std::chrono::microseconds lead,
                                                          std::uint32_t gain_permille,
                                                          std::uint32_t brightness,
                                                          std::uint32_t marker_sample);
  [[nodiscard]] FeatherCalibrationReceipt CalibrateSwingBrightness();
  [[nodiscard]] FeatherSwingReceipt RunSyntheticSwing(std::uint32_t brightness);

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
  [[nodiscard]] FeatherCalibrationReceipt RunCalibration(std::uint32_t request_id);
  [[nodiscard]] FeatherSwingReceipt RunSwing(std::uint32_t request_id, std::uint32_t brightness);
  [[nodiscard]] FeatherPcmUploadReceipt RunPcmUpload(std::uint32_t request_id,
                                                     std::span<const std::int16_t> samples);
  [[nodiscard]] FeatherPcmPlaybackReceipt RunPcmPlayback(std::uint32_t request_id,
                                                         std::uint32_t lead_microseconds,
                                                         std::uint32_t gain_permille,
                                                         std::uint32_t brightness,
                                                         std::uint32_t marker_sample);
  void BestEffortAbortPcm(std::uint32_t request_id) noexcept;
  [[nodiscard]] std::uint32_t NextRequestId();
  [[nodiscard]] const FeatherDeviceInfo &RequireNegotiated() const;

  FeatherHilSerial *serial_;
  std::uint32_t next_request_id_;
  std::optional<FeatherDeviceInfo> device_info_;
  std::optional<FeatherPcmUploadReceipt> pcm_upload_;
  std::vector<std::int16_t> pcm_samples_;
};

}  // namespace swing_capture::hil

#endif  // SWING_CAPTURE_CAPTURE_HIL_FEATHER_HIL_CONTROLLER_H_
