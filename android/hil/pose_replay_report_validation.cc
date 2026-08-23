#include "android/hil/pose_replay_report_validation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace swing_capture::android::hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using ArmDecision = std::tuple<std::uint64_t, std::string, std::string, std::string>;

[[noreturn]] void Invalid(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

const Json &ObjectField(const Json &object, std::string_view name) {
  const auto iterator = object.find(name);
  if (iterator == object.end() || !iterator->is_object()) {
    Invalid(std::string(name) + " must be an object");
  }
  return *iterator;
}

const Json &ArrayField(const Json &object, std::string_view name) {
  const auto iterator = object.find(name);
  if (iterator == object.end() || !iterator->is_array()) {
    Invalid(std::string(name) + " must be an array");
  }
  return *iterator;
}

std::string StringField(const Json &object, std::string_view name) {
  const auto iterator = object.find(name);
  if (iterator == object.end() || !iterator->is_string()) {
    Invalid(std::string(name) + " must be a string");
  }
  return iterator->get<std::string>();
}

bool BoolField(const Json &object, std::string_view name) {
  const auto iterator = object.find(name);
  if (iterator == object.end() || !iterator->is_boolean()) {
    Invalid(std::string(name) + " must be a boolean");
  }
  return iterator->get<bool>();
}

std::uint64_t UnsignedField(const Json &object, std::string_view name) {
  const auto iterator = object.find(name);
  if (iterator == object.end() ||
      !(iterator->is_number_unsigned() || iterator->is_number_integer())) {
    Invalid(std::string(name) + " must be a nonnegative integer");
  }
  if (iterator->is_number_integer() && iterator->get<std::int64_t>() < 0) {
    Invalid(std::string(name) + " cannot be negative");
  }
  return iterator->get<std::uint64_t>();
}

std::optional<std::uint64_t> OptionalUnsignedField(const Json &object, std::string_view name) {
  const auto iterator = object.find(name);
  if (iterator == object.end()) {
    Invalid(std::string(name) + " is required");
  }
  if (iterator->is_null()) {
    return std::nullopt;
  }
  return UnsignedField(object, name);
}

double UnitField(const Json &object, std::string_view name) {
  const auto iterator = object.find(name);
  if (iterator == object.end() || !iterator->is_number()) {
    Invalid(std::string(name) + " must be numeric");
  }
  const double value = iterator->get<double>();
  if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
    Invalid(std::string(name) + " must be finite and in [0,1]");
  }
  return value;
}

void RequireOneOf(std::string_view value, std::initializer_list<std::string_view> allowed,
                  std::string_view name) {
  if (std::ranges::find(allowed, value) == allowed.end()) {
    Invalid(std::string(name) + " has an unsupported value");
  }
}

std::size_t ToSize(std::uint64_t value, std::string_view name) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    Invalid(std::string(name) + " exceeds host size bounds");
  }
  return static_cast<std::size_t>(value);
}

void ValidateSourceId(std::string_view value) {
  if (value.empty() || value.size() > 128U) {
    Invalid("source_id is outside schema bounds");
  }
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20U || byte > 0x7eU || character == '/' || character == '\\') {
      Invalid("source_id is not a safe file name");
    }
  }
}

void ValidateMetrics(const Json &metrics, std::size_t trace_size, std::uint64_t traced_duration_ns,
                     std::uint64_t maximum_duration_ns) {
  const std::uint64_t offered = UnsignedField(metrics, "offered");
  const std::uint64_t rejected = UnsignedField(metrics, "cadence_rejected");
  const std::uint64_t scheduled = UnsignedField(metrics, "scheduled");
  const std::uint64_t dropped = UnsignedField(metrics, "dropped_backpressure");
  const std::uint64_t succeeded = UnsignedField(metrics, "succeeded");
  const std::uint64_t failed = UnsignedField(metrics, "failed");
  const std::uint64_t total = UnsignedField(metrics, "total_inference_ns");
  const std::uint64_t maximum = UnsignedField(metrics, "max_inference_ns");
  if (rejected != 0U || dropped != 0U || scheduled != offered ||
      succeeded != static_cast<std::uint64_t>(trace_size) || succeeded + failed != offered ||
      total < traced_duration_ns || maximum < maximum_duration_ns) {
    Invalid("inference_metrics contradict synchronous replay trace");
  }
}

void RequireOptionalEqual(const std::optional<std::uint64_t> &actual,
                          const std::optional<std::uint64_t> &expected, std::string_view name) {
  if (actual != expected) {
    Invalid(std::string(name) + " contradicts trace");
  }
}

}  // namespace

// Schema validation is intentionally one top-level transaction so every failure
// is converted to a diagnostic rather than escaping across the HIL boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
PoseReplayReportInspection InspectPoseReplayReport(std::string_view report) noexcept {
  PoseReplayReportInspection inspection;
  try {
    if (report.size() > kPoseReplayMaximumReportBytes) {
      Invalid("report exceeds byte bound");
    }
    const Json root = Json::parse(report.begin(), report.end());
    if (!root.is_object() || root.size() != 22U ||
        UnsignedField(root, "schema_version") !=
            static_cast<std::uint64_t>(kPoseReplayReportSchemaVersion) ||
        StringField(root, "report_type") != kPoseReplayReportType) {
      Invalid("unexpected pose replay report envelope");
    }

    inspection.passed = BoolField(root, "passed");
    inspection.outcome = StringField(root, "outcome");
    inspection.failure_code = StringField(root, "failure_code");
    inspection.role = StringField(root, "role");
    inspection.projection = StringField(root, "projection");
    ValidateSourceId(StringField(root, "source_id"));
    const bool role_matches =
        (inspection.role == "down_the_line" && inspection.projection == "dtl") ||
        (inspection.role == "face_on" && inspection.projection == "atl");
    if (!role_matches) {
      Invalid("role and projection disagree");
    }
    RequireOneOf(StringField(root, "delegate_policy"),
                 {"cpu_only", "gpu_preferred", "gpu_required", "npu_preferred", "npu_required"},
                 "delegate_policy");
    const auto delegate = root.find("actual_delegate");
    if (delegate == root.end()) {
      Invalid("actual_delegate is required");
    }
    if (!delegate->is_null()) {
      if (!delegate->is_string()) {
        Invalid("actual_delegate must be null or a string");
      }
      inspection.actual_delegate = delegate->get<std::string>();
      RequireOneOf(*inspection.actual_delegate, {"cpu", "gpu", "npu"}, "actual_delegate");
    }
    const std::string expectation = StringField(root, "expectation");
    RequireOneOf(expectation, {"observe_only", "require_arm", "require_no_arm"}, "expectation");

    const std::size_t maximum_frames =
        ToSize(UnsignedField(root, "maximum_frames"), "maximum_frames");
    if (maximum_frames == 0U || maximum_frames > kPoseReplayMaximumTraceFrames) {
      Invalid("maximum_frames is outside schema bounds");
    }
    const Json &region = ObjectField(root, "hitting_region");
    const double left = UnitField(region, "left");
    const double top = UnitField(region, "top");
    const double right = UnitField(region, "right");
    const double bottom = UnitField(region, "bottom");
    if (right <= left || bottom <= top) {
      Invalid("hitting_region is empty");
    }

    const Json &trace = ArrayField(root, "trace");
    inspection.frame_count = ToSize(UnsignedField(root, "frame_count"), "frame_count");
    if (inspection.frame_count != trace.size() || trace.size() > maximum_frames) {
      Invalid("frame_count contradicts bounded trace");
    }
    std::optional<std::uint64_t> first_timestamp;
    std::optional<std::uint64_t> last_timestamp;
    std::optional<std::uint64_t> first_arm;
    std::string last_state;
    std::uint64_t traced_duration_ns = 0;
    std::uint64_t maximum_duration_ns = 0;
    std::vector<ArmDecision> expected_arm_decisions;
    for (std::size_t index = 0; index < trace.size(); ++index) {
      const Json &frame = trace.at(index);
      if (!frame.is_object() || UnsignedField(frame, "sequence_index") != index) {
        Invalid("trace sequence is not contiguous");
      }
      const std::uint64_t timestamp = UnsignedField(frame, "source_timestamp_ns");
      if (last_timestamp.has_value() && timestamp <= *last_timestamp) {
        Invalid("trace timestamps do not increase strictly");
      }
      if (!first_timestamp.has_value()) {
        first_timestamp = timestamp;
      }
      last_timestamp = timestamp;
      static_cast<void>(UnsignedField(frame, "inference_started_ns"));
      const std::uint64_t duration = UnsignedField(frame, "inference_duration_ns");
      if (duration > std::numeric_limits<std::uint64_t>::max() - traced_duration_ns) {
        Invalid("inference duration sum overflow");
      }
      traced_duration_ns += duration;
      maximum_duration_ns = std::max(maximum_duration_ns, duration);
      if (UnsignedField(frame, "pose_count") > 1U) {
        Invalid("pose_count exceeds Lite model bound");
      }
      const Json &observation = ObjectField(frame, "observation");
      static_cast<void>(UnitField(observation, "person_confidence"));
      static_cast<void>(UnitField(observation, "address_confidence"));
      static_cast<void>(UnitField(observation, "motion_magnitude"));
      static_cast<void>(BoolField(observation, "inside_hitting_region"));
      const Json &decision = ObjectField(frame, "decision");
      last_state = StringField(decision, "state");
      RequireOneOf(last_state, {"watching", "qualifying", "arm_requested", "waiting_for_clear"},
                   "decision.state");
      const std::string command = StringField(decision, "command");
      RequireOneOf(command, {"none", "start_high_speed", "stop_high_speed"}, "decision.command");
      const std::string transition = StringField(decision, "transition_reason");
      RequireOneOf(transition,
                   {"waiting_for_address", "qualification_started", "qualification_continuing",
                    "qualification_dropout_tolerated", "qualification_dropped",
                    "observation_gap_restarted", "observation_gap_cleared", "address_stable_armed",
                    "active_window_extended", "active_clearing", "active_clear_stopped",
                    "active_evidence_expired", "thermal_hard_cap_reached", "capture_ended",
                    "waiting_for_clear", "clear_complete_rearmed"},
                   "decision.transition_reason");
      const auto qualification = OptionalUnsignedField(decision, "qualification_started_ns");
      const auto decision_arm = OptionalUnsignedField(decision, "arm_requested_ns");
      const auto active_until = OptionalUnsignedField(decision, "active_until_ns");
      const auto thermal_stop = OptionalUnsignedField(decision, "thermal_hard_stop_ns");
      if ((qualification && *qualification > timestamp) ||
          (decision_arm && *decision_arm > timestamp)) {
        Invalid("decision lifecycle timestamp is in the future");
      }
      if (last_state == "watching") {
        if (qualification || decision_arm || active_until || thermal_stop) {
          Invalid("watching decision retains stale lifecycle state");
        }
      } else if (last_state == "qualifying") {
        if (!qualification || decision_arm || active_until || thermal_stop) {
          Invalid("qualifying decision has inconsistent lifecycle state");
        }
      } else if (last_state == "arm_requested") {
        if (qualification || !decision_arm || !active_until || !thermal_stop ||
            *active_until < timestamp || *active_until > *thermal_stop) {
          Invalid("armed decision has an invalid bounded active window");
        }
      } else if (!decision_arm || qualification || active_until || thermal_stop) {
        Invalid("waiting decision has inconsistent lifecycle state");
      }
      if (command == "start_high_speed" &&
          (last_state != "arm_requested" || transition != "address_stable_armed" || !decision_arm ||
           *decision_arm != timestamp)) {
        Invalid("start command contradicts controller decision");
      }
      if (command == "stop_high_speed") {
        const bool stop_reason = transition == "active_clear_stopped" ||
                                 transition == "active_evidence_expired" ||
                                 transition == "thermal_hard_cap_reached";
        if (last_state != "waiting_for_clear" || !stop_reason) {
          Invalid("stop command contradicts controller decision");
        }
      }
      if (command != "none") {
        expected_arm_decisions.emplace_back(timestamp, command, last_state, transition);
      }
      if (command == "start_high_speed") {
        ++inspection.arm_request_count;
        if (!first_arm.has_value()) {
          first_arm = timestamp;
        }
        if (!decision_arm.has_value() || *decision_arm != timestamp) {
          Invalid("start decision lacks matching arm_requested_ns");
        }
      }
    }

    inspection.first_source_timestamp_ns = OptionalUnsignedField(root, "first_source_timestamp_ns");
    inspection.last_source_timestamp_ns = OptionalUnsignedField(root, "last_source_timestamp_ns");
    inspection.first_arm_request_ns = OptionalUnsignedField(root, "first_arm_request_ns");
    RequireOptionalEqual(inspection.first_source_timestamp_ns, first_timestamp,
                         "first_source_timestamp_ns");
    RequireOptionalEqual(inspection.last_source_timestamp_ns, last_timestamp,
                         "last_source_timestamp_ns");
    RequireOptionalEqual(inspection.first_arm_request_ns, first_arm, "first_arm_request_ns");
    if (ToSize(UnsignedField(root, "arm_request_count"), "arm_request_count") !=
        inspection.arm_request_count) {
      Invalid("arm_request_count contradicts trace");
    }
    const std::string final_state = StringField(root, "final_state");
    if (!last_state.empty() && final_state != last_state) {
      Invalid("final_state contradicts trace");
    }

    const Json &arm_decisions = ArrayField(root, "arm_decisions");
    if (arm_decisions.size() != expected_arm_decisions.size()) {
      Invalid("arm_decisions count contradicts trace");
    }
    for (std::size_t index = 0; index < arm_decisions.size(); ++index) {
      const Json &decision = arm_decisions.at(index);
      const ArmDecision actual{UnsignedField(decision, "source_timestamp_ns"),
                               StringField(decision, "command"), StringField(decision, "state"),
                               StringField(decision, "transition_reason")};
      if (actual != expected_arm_decisions.at(index)) {
        Invalid("arm_decisions content contradicts trace");
      }
    }
    ValidateMetrics(ObjectField(root, "inference_metrics"), trace.size(), traced_duration_ns,
                    maximum_duration_ns);

    RequireOneOf(inspection.failure_code,
                 {"none", "runtime_failure", "frame_limit_exceeded", "expected_arm_not_observed",
                  "unexpected_arm_request"},
                 "failure_code");
    const bool runtime_failure = inspection.failure_code == "runtime_failure" ||
                                 inspection.failure_code == "frame_limit_exceeded";
    if (runtime_failure != (inspection.outcome == "runtime_failure")) {
      Invalid("runtime outcome and failure_code disagree");
    }
    if (!runtime_failure) {
      const std::string expected_outcome =
          inspection.arm_request_count == 0U ? "no_arm_request" : "arm_requested";
      if (inspection.outcome != expected_outcome || trace.empty() || !inspection.actual_delegate ||
          UnsignedField(ObjectField(root, "inference_metrics"), "failed") != 0U) {
        Invalid("completed replay outcome lacks matching evidence");
      }
      std::string expected_failure = "none";
      if (expectation == "require_arm" && inspection.arm_request_count == 0U) {
        expected_failure = "expected_arm_not_observed";
      } else if (expectation == "require_no_arm" && inspection.arm_request_count != 0U) {
        expected_failure = "unexpected_arm_request";
      }
      if (inspection.failure_code != expected_failure) {
        Invalid("expectation result contradicts arm trace");
      }
    }
    if (inspection.passed != (inspection.failure_code == "none")) {
      Invalid("passed disagrees with failure_code");
    }
    inspection.valid = true;
  } catch (const std::exception &exception) {
    inspection.diagnostic = exception.what();
  } catch (...) {
    inspection.diagnostic = "unknown pose replay report validation failure";
  }
  return inspection;
}

}  // namespace swing_capture::android::hil
