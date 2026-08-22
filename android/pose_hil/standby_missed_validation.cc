#include "android/pose_hil/standby_missed_validation.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace swing_capture::android::pose_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr std::size_t kWavHeaderBytes = 44;
constexpr std::size_t kSampleRateHz = 48'000;
constexpr std::size_t kFullPreRollFrames = 10 * kSampleRateHz;
constexpr std::size_t kPostRollFrames = 2 * kSampleRateHz;
constexpr std::size_t kMinimumStartupPreRollFrames = kSampleRateHz / 2;
constexpr std::size_t kMinimumPreviewFrames = 3;
constexpr std::size_t kMaximumZipBytes = 128ULL * 1024ULL * 1024ULL;

std::int64_t DecimalString(const Json &value, std::string_view field) {
  if (!value.contains(field) || !value.at(field).is_string()) {
    throw std::invalid_argument(std::string(field) + " must be a canonical decimal string");
  }
  const auto &text = value.at(field).get_ref<const std::string &>();
  std::int64_t parsed = 0;
  // std::from_chars exposes a bounded pointer-pair interface.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char *const end_pointer = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), end_pointer, parsed);
  if (error != std::errc() || end != end_pointer || std::to_string(parsed) != text) {
    throw std::invalid_argument(std::string(field) + " is not a canonical decimal integer");
  }
  return parsed;
}

std::size_t NonnegativeSize(const Json &value, std::string_view field) {
  if (!value.contains(field)) {
    throw std::invalid_argument(std::string(field) + " is missing");
  }
  const Json &encoded = value.at(field);
  if (encoded.is_number_unsigned()) {
    const std::uint64_t parsed = encoded.get<std::uint64_t>();
    if (parsed > std::numeric_limits<std::size_t>::max()) {
      throw std::invalid_argument(std::string(field) + " exceeds size_t");
    }
    return static_cast<std::size_t>(parsed);
  }
  if (!encoded.is_number_integer()) {
    throw std::invalid_argument(std::string(field) + " must be a nonnegative integer");
  }
  const std::int64_t parsed = encoded.get<std::int64_t>();
  if (parsed < 0 || !std::in_range<std::size_t>(parsed)) {
    throw std::invalid_argument(std::string(field) + " must be a nonnegative integer");
  }
  return static_cast<std::size_t>(parsed);
}

void ExactFields(const Json &value, std::initializer_list<std::string_view> fields,
                 std::string_view label) {
  if (!value.is_object() || value.size() != fields.size() ||
      !std::ranges::all_of(fields,
                           [&value](std::string_view field) { return value.contains(field); })) {
    throw std::invalid_argument(std::string(label) + " fields do not match schema v1");
  }
}

bool SafeIdentifier(std::string_view value) {
  return !value.empty() && value.size() <= 128U && std::ranges::all_of(value, [](char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '_' ||
           character == '.';
  });
}

std::uint16_t Little16(std::string_view bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 2U) {
    throw std::invalid_argument("binary artifact is truncated");
  }
  return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes.at(offset))) |
         static_cast<std::uint16_t>(static_cast<unsigned char>(bytes.at(offset + 1U))) << 8U;
}

std::uint32_t Little32(std::string_view bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 4U) {
    throw std::invalid_argument("binary artifact is truncated");
  }
  std::uint32_t result = 0;
  for (std::size_t index = 0; index < 4U; ++index) {
    result |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(offset + index)))
              << static_cast<unsigned int>(index * 8U);
  }
  return result;
}

std::uint32_t Crc32(std::string_view bytes) {
  std::uint32_t crc = 0xFFFF'FFFFU;
  for (const unsigned char byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB8'8320U & mask);
    }
  }
  return ~crc;
}

void ValidateWav(std::string_view wav, std::size_t sample_count) {
  if (sample_count > (std::numeric_limits<std::uint32_t>::max() - kWavHeaderBytes) / 2U ||
      wav.size() != kWavHeaderBytes + sample_count * 2U || !wav.starts_with("RIFF") ||
      wav.substr(8, 4) != "WAVE" || wav.substr(12, 4) != "fmt " ||
      Little32(wav, 4) != wav.size() - 8U || Little32(wav, 16) != 16U || Little16(wav, 20) != 1U ||
      Little16(wav, 22) != 1U || Little32(wav, 24) != kSampleRateHz ||
      Little32(wav, 28) != kSampleRateHz * 2U || Little16(wav, 32) != 2U ||
      Little16(wav, 34) != 16U || wav.substr(36, 4) != "data" ||
      Little32(wav, 40) != sample_count * 2U) {
    throw std::invalid_argument("diagnostic audio is not canonical 48 kHz mono PCM16LE WAV");
  }
}

void ValidateJpeg(std::string_view bytes, std::size_t offset, std::size_t length) {
  if (length < 4U || offset > bytes.size() || length > bytes.size() - offset ||
      static_cast<unsigned char>(bytes.at(offset)) != 0xFFU ||
      static_cast<unsigned char>(bytes.at(offset + 1U)) != 0xD8U ||
      static_cast<unsigned char>(bytes.at(offset + length - 2U)) != 0xFFU ||
      static_cast<unsigned char>(bytes.at(offset + length - 1U)) != 0xD9U) {
    throw std::invalid_argument("pose preview trace does not index complete JPEG images");
  }
}

std::size_t ValidatePreview(const Json &preview, std::string_view mjpeg, std::string_view trace) {
  ExactFields(
      preview,
      {"frames_path", "frames_content_type", "frames_bytes", "trace_path", "trace_content_type",
       "trace_bytes", "first_timestamp_boottime_ns", "end_timestamp_boottime_ns_exclusive",
       "observation_count", "jpeg_frame_count", "frame_count"},
      "standby pose preview");
  const std::int64_t frames_bytes = DecimalString(preview, "frames_bytes");
  const std::int64_t first_manifest_timestamp =
      DecimalString(preview, "first_timestamp_boottime_ns");
  const std::int64_t end_manifest_timestamp =
      DecimalString(preview, "end_timestamp_boottime_ns_exclusive");
  const std::size_t trace_bytes = NonnegativeSize(preview, "trace_bytes");
  const std::size_t frame_count = NonnegativeSize(preview, "frame_count");
  const std::size_t jpeg_frame_count = NonnegativeSize(preview, "jpeg_frame_count");
  const std::size_t observation_count = NonnegativeSize(preview, "observation_count");
  if (preview.at("frames_path") != "pose_diagnostics/preview_frames.mjpeg" ||
      preview.at("frames_content_type") != "image/jpeg" ||
      preview.at("trace_path") != "pose_diagnostics/pose_trace.ndjson" ||
      preview.at("trace_content_type") != "application/x-ndjson" || frames_bytes < 0 ||
      static_cast<std::size_t>(frames_bytes) != mjpeg.size() || trace_bytes != trace.size() ||
      frame_count != jpeg_frame_count || frame_count < kMinimumPreviewFrames ||
      observation_count < frame_count || first_manifest_timestamp < 0 ||
      end_manifest_timestamp <= first_manifest_timestamp) {
    throw std::invalid_argument("standby pose preview manifest is invalid");
  }

  std::size_t expected_offset = 0;
  std::size_t trace_start = 0;
  std::int64_t first_timestamp = -1;
  std::int64_t previous_timestamp = -1;
  std::size_t available_frames = 0;
  for (std::size_t index = 0; index < observation_count; ++index) {
    const std::size_t trace_end = trace.find('\n', trace_start);
    if (trace_end == std::string_view::npos || trace_end == trace_start) {
      throw std::invalid_argument("pose trace does not contain every five-Hz observation row");
    }
    const Json row = Json::parse(trace.substr(trace_start, trace_end - trace_start));
    ExactFields(
        row,
        {"schema_version", "sequence_index", "timestamp_boottime_ns", "frame_available",
         "frame_content_type", "frame_byte_offset", "frame_byte_length", "model_id",
         "inference_duration_ns", "person_confidence", "address_confidence", "motion_magnitude",
         "hitting_region_occupied", "controller_state", "decision_reason"},
        "pose trace row");
    const std::int64_t timestamp = DecimalString(row, "timestamp_boottime_ns");
    const std::size_t length = NonnegativeSize(row, "frame_byte_length");
    const std::int64_t inference_duration = DecimalString(row, "inference_duration_ns");
    if (row.at("schema_version") != 1 || NonnegativeSize(row, "sequence_index") != index ||
        timestamp <= previous_timestamp || !row.at("frame_available").is_boolean() ||
        inference_duration < 0 || !row.at("model_id").is_string() ||
        row.at("model_id").get<std::string>().empty() || !row.at("person_confidence").is_number() ||
        !std::isfinite(row.at("person_confidence").get<double>()) ||
        !row.at("address_confidence").is_number() ||
        !std::isfinite(row.at("address_confidence").get<double>()) ||
        !row.at("motion_magnitude").is_number() ||
        !std::isfinite(row.at("motion_magnitude").get<double>()) ||
        !row.at("hitting_region_occupied").is_boolean() ||
        !row.at("controller_state").is_string() || !row.at("decision_reason").is_string()) {
      throw std::invalid_argument("pose trace row is invalid or noncontiguous");
    }
    if (index == 0) {
      first_timestamp = timestamp;
    }
    if (row.at("frame_available").get<bool>()) {
      const std::int64_t offset = DecimalString(row, "frame_byte_offset");
      if (offset < 0 || !std::cmp_equal(offset, expected_offset) ||
          row.at("frame_content_type") != "image/jpeg") {
        throw std::invalid_argument("available pose JPEG metadata is inconsistent");
      }
      ValidateJpeg(mjpeg, expected_offset, length);
      expected_offset += length;
      ++available_frames;
    } else if (!row.at("frame_content_type").is_null() || !row.at("frame_byte_offset").is_null() ||
               length != 0U) {
      throw std::invalid_argument("trace-only pose row claims JPEG metadata");
    }
    previous_timestamp = timestamp;
    trace_start = trace_end + 1U;
  }
  if (available_frames != frame_count || expected_offset != mjpeg.size() ||
      trace_start != trace.size() || first_timestamp != first_manifest_timestamp ||
      previous_timestamp < 0 || previous_timestamp >= end_manifest_timestamp) {
    throw std::invalid_argument("pose preview files disagree with manifest timing or byte bounds");
  }
  return frame_count;
}

struct ZipEntry {
  std::string name;
  std::string_view contents;
  std::uint32_t crc32 = 0;
  std::size_t local_offset = 0;
};

struct ZipBounds {
  std::size_t entry_count = 0;
  std::size_t central_offset = 0;
  std::size_t central_end = 0;
};

bool SafeArchivePath(std::string_view path) {
  if (path.empty() || path.starts_with('/') || path.ends_with('/') || path.contains("//")) {
    return false;
  }
  std::size_t start = 0;
  while (start < path.size()) {
    const std::size_t end = path.find('/', start);
    const std::string_view segment =
        path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (!SafeIdentifier(segment) || segment == "." || segment == ".." ||
        segment.ends_with(".tmp")) {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1U;
  }
  return true;
}

ZipBounds ParseZipBounds(std::string_view archive) {
  if (archive.size() < 22U || archive.size() > kMaximumZipBytes) {
    throw std::invalid_argument("diagnostics ZIP size is outside its bound");
  }
  const std::size_t eocd = archive.size() - 22U;
  if (Little32(archive, eocd) != 0x0605'4B50U || Little16(archive, eocd + 4U) != 0U ||
      Little16(archive, eocd + 6U) != 0U || Little16(archive, eocd + 20U) != 0U) {
    throw std::invalid_argument("diagnostics ZIP has no canonical single-disk end record");
  }
  const std::size_t entry_count = Little16(archive, eocd + 8U);
  if (entry_count != Little16(archive, eocd + 10U) || entry_count != 6U) {
    throw std::invalid_argument("diagnostics ZIP must contain exactly six entries");
  }
  const std::size_t central_size = Little32(archive, eocd + 12U);
  const std::size_t central_offset = Little32(archive, eocd + 16U);
  if (central_offset > eocd || central_size != eocd - central_offset) {
    throw std::invalid_argument("diagnostics ZIP central directory bounds are invalid");
  }
  return {.entry_count = entry_count, .central_offset = central_offset, .central_end = eocd};
}

ZipEntry ParseZipEntry(std::string_view archive, const ZipBounds &bounds, std::size_t &cursor) {
  if (Little32(archive, cursor) != 0x0201'4B50U || cursor > bounds.central_end ||
      bounds.central_end - cursor < 46U) {
    throw std::invalid_argument("diagnostics ZIP central entry is truncated");
  }
  const std::uint16_t flags = Little16(archive, cursor + 8U);
  const std::uint16_t method = Little16(archive, cursor + 10U);
  const std::uint32_t crc = Little32(archive, cursor + 16U);
  const std::size_t compressed = Little32(archive, cursor + 20U);
  const std::size_t uncompressed = Little32(archive, cursor + 24U);
  const std::size_t name_length = Little16(archive, cursor + 28U);
  const std::size_t extra_length = Little16(archive, cursor + 30U);
  const std::size_t comment_length = Little16(archive, cursor + 32U);
  const std::size_t local_offset = Little32(archive, cursor + 42U);
  const std::size_t central_entry_size = 46U + name_length + extra_length + comment_length;
  if ((flags & ~0x0800U) != 0U || method != 0U || compressed != uncompressed ||
      central_entry_size > bounds.central_end - cursor) {
    throw std::invalid_argument("diagnostics ZIP entry is not bounded stored data");
  }
  const std::string name(archive.substr(cursor + 46U, name_length));
  if (!SafeArchivePath(name) || local_offset > bounds.central_offset ||
      bounds.central_offset - local_offset < 30U ||
      Little32(archive, local_offset) != 0x0403'4B50U) {
    throw std::invalid_argument("diagnostics ZIP entry path or local header is invalid");
  }
  const std::uint16_t local_flags = Little16(archive, local_offset + 6U);
  const std::uint16_t local_method = Little16(archive, local_offset + 8U);
  const std::uint32_t local_crc = Little32(archive, local_offset + 14U);
  const std::size_t local_compressed = Little32(archive, local_offset + 18U);
  const std::size_t local_uncompressed = Little32(archive, local_offset + 22U);
  const std::size_t local_name_length = Little16(archive, local_offset + 26U);
  const std::size_t local_extra_length = Little16(archive, local_offset + 28U);
  const std::size_t contents_offset = local_offset + 30U + local_name_length + local_extra_length;
  if (local_flags != flags || local_method != method || local_crc != crc ||
      local_compressed != compressed || local_uncompressed != uncompressed ||
      contents_offset > bounds.central_offset ||
      compressed > bounds.central_offset - contents_offset ||
      archive.substr(local_offset + 30U, local_name_length) != name) {
    throw std::invalid_argument("diagnostics ZIP local and central entries disagree");
  }
  const std::string_view contents = archive.substr(contents_offset, compressed);
  if (Crc32(contents) != crc) {
    throw std::invalid_argument("diagnostics ZIP entry CRC is invalid");
  }
  cursor += central_entry_size;
  return {
      .name = name,
      .contents = contents,
      .crc32 = crc,
      .local_offset = local_offset,
  };
}

void ValidateContiguousZipEntries(std::string_view archive,
                                  const std::map<std::string, ZipEntry, std::less<>> &entries,
                                  std::size_t central_offset) {
  std::vector<const ZipEntry *> ordered;
  ordered.reserve(entries.size());
  for (const auto &[name, entry] : entries) {
    static_cast<void>(name);
    ordered.push_back(&entry);
  }
  std::ranges::sort(ordered, {}, &ZipEntry::local_offset);
  std::size_t expected_offset = 0;
  for (const ZipEntry *entry : ordered) {
    if (entry->local_offset != expected_offset) {
      throw std::invalid_argument("diagnostics ZIP local entries are not contiguous");
    }
    const std::size_t local_name_length = Little16(archive, entry->local_offset + 26U);
    const std::size_t local_extra_length = Little16(archive, entry->local_offset + 28U);
    expected_offset =
        entry->local_offset + 30U + local_name_length + local_extra_length + entry->contents.size();
  }
  if (expected_offset != central_offset) {
    throw std::invalid_argument("diagnostics ZIP local data does not meet its central directory");
  }
}

std::map<std::string, ZipEntry, std::less<>> ParseZip(std::string_view archive) {
  const ZipBounds bounds = ParseZipBounds(archive);

  std::map<std::string, ZipEntry, std::less<>> entries;
  std::size_t cursor = bounds.central_offset;
  for (std::size_t index = 0; index < bounds.entry_count; ++index) {
    ZipEntry entry = ParseZipEntry(archive, bounds, cursor);
    if (!entries.emplace(entry.name, std::move(entry)).second) {
      throw std::invalid_argument("diagnostics ZIP entry names are not unique");
    }
  }
  if (cursor != bounds.central_end) {
    throw std::invalid_argument("diagnostics ZIP contains trailing central-directory data");
  }
  ValidateContiguousZipEntries(archive, entries, bounds.central_offset);
  return entries;
}

bool LowercaseSha256(std::string_view value) {
  return value.size() == 64U && std::ranges::all_of(value, [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

void ValidateZip(const StandbyMissedInspection &inspection) {
  const auto entries = ParseZip(inspection.diagnostics_zip);
  const std::string session_prefix = std::string(inspection.expected_session_id) + "/";
  const std::map<std::string, std::string_view, std::less<>> expected = {
      {session_prefix + "manifest.json", inspection.manifest},
      {session_prefix + "diagnostic_audio.wav", inspection.diagnostic_audio_wav},
      {session_prefix + "diagnostic_incident.json", inspection.diagnostic_incident},
      {session_prefix + "pose_diagnostics/preview_frames.mjpeg", inspection.preview_mjpeg},
      {session_prefix + "pose_diagnostics/pose_trace.ndjson", inspection.pose_trace_ndjson},
  };
  if (!entries.contains("diagnostic_export.json")) {
    throw std::invalid_argument("diagnostics ZIP lacks its export manifest");
  }
  for (const auto &[path, contents] : expected) {
    const auto found = entries.find(path);
    if (found == entries.end() || found->second.contents != contents) {
      throw std::invalid_argument("diagnostics ZIP does not preserve exact session artifacts");
    }
  }

  const Json export_manifest = Json::parse(entries.at("diagnostic_export.json").contents);
  ExactFields(export_manifest, {"schema_version", "session_id", "created_at_utc", "files"},
              "diagnostic export manifest");
  if (export_manifest.at("schema_version") != 1 ||
      export_manifest.at("session_id") != inspection.expected_session_id ||
      !export_manifest.at("created_at_utc").is_string() ||
      export_manifest.at("created_at_utc").get<std::string>().empty() ||
      !export_manifest.at("files").is_array() ||
      export_manifest.at("files").size() != expected.size()) {
    throw std::invalid_argument("diagnostic export manifest identity is invalid");
  }
  std::map<std::string, std::size_t, std::less<>> described;
  for (const Json &file : export_manifest.at("files")) {
    ExactFields(file, {"path", "bytes", "sha256"}, "diagnostic export file");
    if (!file.at("path").is_string() || !file.at("sha256").is_string()) {
      throw std::invalid_argument("diagnostic export file metadata types are invalid");
    }
    const std::string path = file.at("path").get<std::string>();
    const std::size_t bytes = NonnegativeSize(file, "bytes");
    if (!expected.contains(path) || expected.at(path).size() != bytes ||
        !LowercaseSha256(file.at("sha256").get<std::string>()) ||
        !described.emplace(path, bytes).second) {
      throw std::invalid_argument("diagnostic export file metadata is inconsistent");
    }
  }
  if (described.size() != expected.size()) {
    throw std::invalid_argument("diagnostic export manifest does not describe every artifact");
  }
}

double UnitDecimalString(const Json &value, std::string_view field) {
  if (!value.contains(field) || !value.at(field).is_string()) {
    throw std::invalid_argument(std::string(field) + " must be a decimal string");
  }
  const auto &text = value.at(field).get_ref<const std::string &>();
  double parsed = 0.0;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char *const end_pointer = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), end_pointer, parsed);
  if (error != std::errc() || end != end_pointer || !std::isfinite(parsed) || parsed < 0.0 ||
      parsed > 1.0) {
    throw std::invalid_argument(std::string(field) + " is outside [0, 1]");
  }
  return parsed;
}

void ValidateOptionalClockEstimate(const Json &value, std::string_view time_field,
                                   std::string_view uncertainty_field) {
  if (value.at(time_field).is_null() && value.at(uncertainty_field).is_null()) {
    return;
  }
  if (DecimalString(value, time_field) <= 0 || DecimalString(value, uncertainty_field) < 0) {
    throw std::invalid_argument("diagnostic detector clock estimate is invalid");
  }
}

void ValidateExpectedEvent(const Json &event, std::int64_t marker,
                           StandbyDiagnosticExpectation expectation) {
  if (expectation == StandbyDiagnosticExpectation::kOperatorMissedShot) {
    if (event.at("kind") != "operator_tag" || !event.at("detector").is_null() ||
        DecimalString(event, "operator_received_boottime_ns") <= 0) {
      throw std::invalid_argument("standby event is not an operator missed-shot tag");
    }
    return;
  }
  if (event.at("kind") != "detected_impact" ||
      !event.at("operator_received_boottime_ns").is_null() || !event.at("detector").is_object()) {
    throw std::invalid_argument("standby event is not an automatic detected impact");
  }
  const Json &detector = event.at("detector");
  ExactFields(detector,
              {"strike_frame_position", "confirmation_frame_position", "peak_amplitude",
               "noise_floor", "threshold", "strike_boottime_ns", "strike_uncertainty_ns",
               "confirmation_boottime_ns", "confirmation_uncertainty_ns"},
              "standby detector evidence");
  const std::int64_t strike = DecimalString(detector, "strike_frame_position");
  const std::int64_t confirmation = DecimalString(detector, "confirmation_frame_position");
  static_cast<void>(UnitDecimalString(detector, "peak_amplitude"));
  static_cast<void>(UnitDecimalString(detector, "noise_floor"));
  static_cast<void>(UnitDecimalString(detector, "threshold"));
  ValidateOptionalClockEstimate(detector, "strike_boottime_ns", "strike_uncertainty_ns");
  ValidateOptionalClockEstimate(detector, "confirmation_boottime_ns",
                                "confirmation_uncertainty_ns");
  if (strike != marker || confirmation < strike) {
    throw std::invalid_argument("automatic detector frame positions are inconsistent");
  }
}

void ValidateEventAudioClock(const Json &event) {
  const std::string clock_status = event.at("audio_clock_status").get<std::string>();
  if (clock_status == "validated") {
    if (DecimalString(event, "marker_boottime_ns") <= 0 ||
        DecimalString(event, "marker_uncertainty_ns") < 0) {
      throw std::invalid_argument("validated standby audio clock fields are invalid");
    }
    return;
  }
  if (clock_status != "audio_clock_unvalidated" || !event.at("marker_boottime_ns").is_null() ||
      !event.at("marker_uncertainty_ns").is_null()) {
    throw std::invalid_argument("standby audio clock status is inconsistent");
  }
}

void ValidateExpectedIncident(const Json &incident, const Json &feedback,
                              StandbyDiagnosticExpectation expectation) {
  if (!feedback.at("note").is_string() ||
      !feedback.at("note").get_ref<const std::string &>().empty() ||
      !incident.at("timing_marks").is_array()) {
    throw std::invalid_argument("diagnostic incident feedback or timing marks are invalid");
  }
  if (expectation == StandbyDiagnosticExpectation::kOperatorMissedShot) {
    if (incident.at("classification") != "user_reported" ||
        feedback.at("classification") != "missed_shot" || !incident.at("timing_marks").empty()) {
      throw std::invalid_argument("diagnostic incident does not encode user missed-shot semantics");
    }
    return;
  }
  const Json &marks = incident.at("timing_marks");
  if (incident.at("classification") != "impact_while_not_armed" ||
      feedback.at("classification") != "unreviewed" || marks.size() != 1U) {
    throw std::invalid_argument(
        "diagnostic incident does not encode automatic standby-impact semantics");
  }
  const Json &mark = marks.at(0);
  ExactFields(mark, {"kind", "stream_id", "offset_us"}, "standby impact timing mark");
  if (mark.at("kind") != "audio_impact_transient" || mark.at("stream_id") != "standby_audio" ||
      DecimalString(mark, "offset_us") != 0) {
    throw std::invalid_argument("standby impact timing mark is invalid");
  }
}

}  // namespace

StandbyMissedEvidence ValidateStandbyMissedEvidence(
    const StandbyMissedInspection &inspection) noexcept {
  StandbyMissedEvidence evidence;
  try {
    if (!SafeIdentifier(inspection.expected_session_id)) {
      throw std::invalid_argument("expected standby diagnostic session ID is unsafe");
    }
    const Json manifest = Json::parse(inspection.manifest);
    ExactFields(manifest,
                {"schema_version", "session_kind", "session_id", "created_at_epoch_ms",
                 "source_node_id", "event", "evidence", "incident"},
                "standby diagnostic manifest");
    const std::int64_t created_at = DecimalString(manifest, "created_at_epoch_ms");
    if (manifest.at("schema_version") != 1 || manifest.at("session_kind") != "standby_diagnostic" ||
        manifest.at("session_id") != inspection.expected_session_id || created_at <= 0 ||
        !manifest.at("source_node_id").is_string() ||
        !SafeIdentifier(manifest.at("source_node_id").get<std::string>())) {
      throw std::invalid_argument("standby diagnostic manifest identity is invalid");
    }

    const Json &event = manifest.at("event");
    ExactFields(event,
                {"sequence", "kind", "marker_audio_frame_position", "audio_clock_status",
                 "marker_boottime_ns", "marker_uncertainty_ns", "operator_received_boottime_ns",
                 "detector"},
                "standby diagnostic event");
    const std::int64_t sequence = DecimalString(event, "sequence");
    const std::int64_t marker = DecimalString(event, "marker_audio_frame_position");
    if (sequence <= 0 || marker < 0) {
      throw std::invalid_argument("standby diagnostic event identity is invalid");
    }
    ValidateExpectedEvent(event, marker, inspection.expectation);
    ValidateEventAudioClock(event);

    const Json &manifest_evidence = manifest.at("evidence");
    ExactFields(manifest_evidence, {"audio", "preview_status", "preview"},
                "standby diagnostic evidence");
    const Json &audio = manifest_evidence.at("audio");
    ExactFields(
        audio,
        {"path", "content_type", "bytes", "sample_rate_hz", "first_frame_position",
         "end_frame_position", "marker_frame_position", "sample_count", "marker_sample_index"},
        "standby diagnostic audio");
    const std::int64_t first = DecimalString(audio, "first_frame_position");
    const std::int64_t end = DecimalString(audio, "end_frame_position");
    const std::int64_t audio_marker = DecimalString(audio, "marker_frame_position");
    const std::int64_t encoded_bytes = DecimalString(audio, "bytes");
    evidence.sample_count = NonnegativeSize(audio, "sample_count");
    const std::size_t marker_sample_index = NonnegativeSize(audio, "marker_sample_index");
    evidence.wav_bytes = inspection.diagnostic_audio_wav.size();
    if (audio.at("path") != "diagnostic_audio.wav" || audio.at("content_type") != "audio/wav" ||
        audio.at("sample_rate_hz") != kSampleRateHz || first < 0 || end <= first ||
        audio_marker != marker || marker < first || marker >= end || encoded_bytes <= 0 ||
        !std::cmp_equal(encoded_bytes, evidence.wav_bytes) ||
        !std::cmp_equal(end - first, evidence.sample_count) ||
        !std::cmp_equal(marker - first, marker_sample_index)) {
      throw std::invalid_argument("standby diagnostic audio metadata is inconsistent");
    }
    evidence.pre_roll_frames = marker_sample_index;
    evidence.post_roll_frames = static_cast<std::size_t>(end - marker);
    if (evidence.post_roll_frames != kPostRollFrames) {
      throw std::invalid_argument(
          "standby diagnostic WAV does not contain exact two-second post-roll");
    }
    if (evidence.pre_roll_frames == kFullPreRollFrames) {
      evidence.startup_short_pre_roll = false;
    } else if (std::cmp_less(marker, kFullPreRollFrames) && first == 0 &&
               std::cmp_equal(evidence.pre_roll_frames, marker) &&
               evidence.pre_roll_frames >= kMinimumStartupPreRollFrames) {
      evidence.startup_short_pre_roll = true;
    } else {
      throw std::invalid_argument(
          "standby diagnostic WAV has neither full nor explicit startup-short pre-roll");
    }
    ValidateWav(inspection.diagnostic_audio_wav, evidence.sample_count);

    if (manifest_evidence.at("preview_status") != "available" ||
        !manifest_evidence.at("preview").is_object()) {
      throw std::invalid_argument("standby diagnostic pose preview is unavailable");
    }
    evidence.preview_frame_count = ValidatePreview(
        manifest_evidence.at("preview"), inspection.preview_mjpeg, inspection.pose_trace_ndjson);

    const Json &incident_metadata = manifest.at("incident");
    ExactFields(incident_metadata, {"path", "bytes"}, "standby incident metadata");
    if (incident_metadata.at("path") != "diagnostic_incident.json" ||
        NonnegativeSize(incident_metadata, "bytes") != inspection.diagnostic_incident.size()) {
      throw std::invalid_argument("standby incident byte metadata is inconsistent");
    }
    const Json incident = Json::parse(inspection.diagnostic_incident);
    ExactFields(incident,
                {"schema_version", "incident_id", "classification", "created_at_epoch_ms",
                 "source_node_id", "user_feedback", "timing_marks"},
                "diagnostic incident");
    const Json &feedback = incident.at("user_feedback");
    ExactFields(feedback, {"classification", "note"}, "diagnostic feedback");
    if (incident.at("schema_version") != 1 ||
        incident.at("incident_id") != inspection.expected_session_id ||
        DecimalString(incident, "created_at_epoch_ms") != created_at ||
        incident.at("source_node_id") != manifest.at("source_node_id") ||
        !feedback.at("classification").is_string()) {
      throw std::invalid_argument("diagnostic incident identity is inconsistent");
    }
    ValidateExpectedIncident(incident, feedback, inspection.expectation);

    ValidateZip(inspection);
    evidence.zip_bytes = inspection.diagnostics_zip.size();
    evidence.zip_entry_count = 6;
    evidence.valid = true;
    evidence.diagnostic =
        inspection.expectation == StandbyDiagnosticExpectation::kOperatorMissedShot
            ? "standby missed-shot audio, pose preview, incident, and ZIP are valid"
            : "standby automatic-impact audio, pose preview, incident, and ZIP are valid";
  } catch (const std::exception &failure) {
    evidence.diagnostic = failure.what();
  }
  return evidence;
}

}  // namespace swing_capture::android::pose_hil
