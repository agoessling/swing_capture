#include "android/pose_hil/standby_missed_validation.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pose_hil = swing_capture::android::pose_hil;

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void Append16(std::string &output, std::uint16_t value) {
  output.push_back(static_cast<char>(value & 0xFFU));
  output.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void Append32(std::string &output, std::uint32_t value) {
  for (unsigned int shift = 0; shift < 32U; shift += 8U) {
    output.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

void Set32(std::string &output, std::size_t offset, std::uint32_t value) {
  for (unsigned int shift = 0; shift < 32U; shift += 8U) {
    output.at(offset + shift / 8U) = static_cast<char>((value >> shift) & 0xFFU);
  }
}

std::uint32_t Crc32(std::string_view bytes) {
  std::uint32_t crc = 0xFFFF'FFFFU;
  for (unsigned char byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB8'8320U & mask);
    }
  }
  return ~crc;
}

std::string BuildWav(std::size_t sample_count) {
  std::string wav(44U + sample_count * 2U, '\0');
  wav.replace(0, 4, "RIFF");
  Set32(wav, 4, static_cast<std::uint32_t>(wav.size() - 8U));
  wav.replace(8, 4, "WAVE");
  wav.replace(12, 4, "fmt ");
  Set32(wav, 16, 16);
  wav.at(20) = 1;
  wav.at(22) = 1;
  Set32(wav, 24, 48'000);
  Set32(wav, 28, 96'000);
  wav.at(32) = 2;
  wav.at(34) = 16;
  wav.replace(36, 4, "data");
  Set32(wav, 40, static_cast<std::uint32_t>(sample_count * 2U));
  return wav;
}

struct StoredSource {
  std::string name;
  std::string contents;
  std::uint32_t crc = 0;
  std::uint32_t local_offset = 0;
};

std::string BuildStoredZip(std::vector<StoredSource> sources) {
  std::string archive;
  for (StoredSource &source : sources) {
    source.crc = Crc32(source.contents);
    source.local_offset = static_cast<std::uint32_t>(archive.size());
    Append32(archive, 0x0403'4B50U);
    Append16(archive, 10);
    Append16(archive, 0);
    Append16(archive, 0);
    Append16(archive, 0);
    Append16(archive, 0);
    Append32(archive, source.crc);
    Append32(archive, static_cast<std::uint32_t>(source.contents.size()));
    Append32(archive, static_cast<std::uint32_t>(source.contents.size()));
    Append16(archive, static_cast<std::uint16_t>(source.name.size()));
    Append16(archive, 0);
    archive += source.name;
    archive += source.contents;
  }
  const std::uint32_t central_offset = static_cast<std::uint32_t>(archive.size());
  for (const StoredSource &source : sources) {
    Append32(archive, 0x0201'4B50U);
    Append16(archive, 20);
    Append16(archive, 10);
    Append16(archive, 0);
    Append16(archive, 0);
    Append16(archive, 0);
    Append16(archive, 0);
    Append32(archive, source.crc);
    Append32(archive, static_cast<std::uint32_t>(source.contents.size()));
    Append32(archive, static_cast<std::uint32_t>(source.contents.size()));
    Append16(archive, static_cast<std::uint16_t>(source.name.size()));
    Append16(archive, 0);
    Append16(archive, 0);
    Append16(archive, 0);
    Append16(archive, 0);
    Append32(archive, 0);
    Append32(archive, source.local_offset);
    archive += source.name;
  }
  const std::uint32_t central_size = static_cast<std::uint32_t>(archive.size()) - central_offset;
  Append32(archive, 0x0605'4B50U);
  Append16(archive, 0);
  Append16(archive, 0);
  Append16(archive, static_cast<std::uint16_t>(sources.size()));
  Append16(archive, static_cast<std::uint16_t>(sources.size()));
  Append32(archive, central_size);
  Append32(archive, central_offset);
  Append16(archive, 0);
  return archive;
}

struct Fixture {
  std::string session_id = "android-1-abcdef12";
  std::string manifest;
  std::string wav;
  std::string incident;
  std::string preview;
  std::string trace;
  std::string zip;
};

std::string ExportManifest(const Fixture &fixture) {
  const std::string prefix = fixture.session_id + "/";
  const std::map<std::string, std::size_t, std::less<>> files = {
      {prefix + "manifest.json", fixture.manifest.size()},
      {prefix + "diagnostic_audio.wav", fixture.wav.size()},
      {prefix + "diagnostic_incident.json", fixture.incident.size()},
      {prefix + "pose_diagnostics/preview_frames.mjpeg", fixture.preview.size()},
      {prefix + "pose_diagnostics/pose_trace.ndjson", fixture.trace.size()},
  };
  Json encoded_files = Json::array();
  for (const auto &[path, bytes] : files) {
    encoded_files.push_back({{"path", path}, {"bytes", bytes}, {"sha256", std::string(64, '0')}});
  }
  return Json({{"schema_version", 1},
               {"session_id", fixture.session_id},
               {"created_at_utc", "2026-08-17T00:00:00Z"},
               {"files", std::move(encoded_files)}})
             .dump() +
         "\n";
}

void RefreshZip(Fixture &fixture, bool include_trace = true) {
  const std::string prefix = fixture.session_id + "/";
  std::vector<StoredSource> sources = {
      {"diagnostic_export.json", ExportManifest(fixture)},
      {prefix + "manifest.json", fixture.manifest},
      {prefix + "diagnostic_audio.wav", fixture.wav},
      {prefix + "diagnostic_incident.json", fixture.incident},
      {prefix + "pose_diagnostics/preview_frames.mjpeg", fixture.preview},
  };
  if (include_trace) {
    sources.push_back({prefix + "pose_diagnostics/pose_trace.ndjson", fixture.trace});
  }
  fixture.zip = BuildStoredZip(std::move(sources));
}

Fixture ValidFixture() {
  Fixture fixture;
  std::size_t offset = 0;
  for (std::size_t index = 0; index < 5; ++index) {
    const std::int64_t timestamp = 1'000'000'000 + static_cast<std::int64_t>(index) * 200'000'000;
    const bool has_frame = index == 0U || index == 2U || index == 4U;
    Json row = {{"schema_version", 1},
                {"sequence_index", index},
                {"timestamp_boottime_ns", std::to_string(timestamp)},
                {"frame_available", has_frame},
                {"model_id", "pose_landmarker_lite"},
                {"inference_duration_ns", "1000000"},
                {"person_confidence", 0.9},
                {"address_confidence", 0.8},
                {"motion_magnitude", 0.1},
                {"hitting_region_occupied", true},
                {"controller_state", "monitoring"},
                {"decision_reason", "stable"}};
    if (has_frame) {
      const std::string jpeg = "\xff\xd8" + std::string(index + 1U, 'x') + "\xff\xd9";
      row["frame_content_type"] = "image/jpeg";
      row["frame_byte_offset"] = std::to_string(offset);
      row["frame_byte_length"] = jpeg.size();
      fixture.preview += jpeg;
      offset += jpeg.size();
    } else {
      row["frame_content_type"] = nullptr;
      row["frame_byte_offset"] = nullptr;
      row["frame_byte_length"] = 0;
    }
    fixture.trace += row.dump() + "\n";
  }
  const Json preview = {
      {"frames_path", "pose_diagnostics/preview_frames.mjpeg"},
      {"frames_content_type", "image/jpeg"},
      {"frames_bytes", std::to_string(fixture.preview.size())},
      {"trace_path", "pose_diagnostics/pose_trace.ndjson"},
      {"trace_content_type", "application/x-ndjson"},
      {"trace_bytes", fixture.trace.size()},
      {"first_timestamp_boottime_ns", "1000000000"},
      {"end_timestamp_boottime_ns_exclusive", "1800000001"},
      {"observation_count", 5},
      {"jpeg_frame_count", 3},
      {"frame_count", 3},
  };
  fixture.wav = BuildWav(144'000);
  fixture.incident = Json({{"schema_version", 1},
                           {"incident_id", fixture.session_id},
                           {"classification", "user_reported"},
                           {"created_at_epoch_ms", "1770000000000"},
                           {"source_node_id", "node-1"},
                           {"user_feedback", {{"classification", "missed_shot"}, {"note", ""}}},
                           {"timing_marks", Json::array()}})
                         .dump();
  fixture.manifest =
      Json({{"schema_version", 1},
            {"session_kind", "standby_diagnostic"},
            {"session_id", fixture.session_id},
            {"created_at_epoch_ms", "1770000000000"},
            {"source_node_id", "node-1"},
            {"event",
             {{"sequence", "1"},
              {"kind", "operator_tag"},
              {"marker_audio_frame_position", "48000"},
              {"audio_clock_status", "audio_clock_unvalidated"},
              {"marker_boottime_ns", nullptr},
              {"marker_uncertainty_ns", nullptr},
              {"operator_received_boottime_ns", "1000000000"},
              {"detector", nullptr}}},
            {"evidence",
             {{"audio",
               {{"path", "diagnostic_audio.wav"},
                {"content_type", "audio/wav"},
                {"bytes", std::to_string(fixture.wav.size())},
                {"sample_rate_hz", 48'000},
                {"first_frame_position", "0"},
                {"end_frame_position", "144000"},
                {"marker_frame_position", "48000"},
                {"sample_count", 144'000},
                {"marker_sample_index", 48'000}}},
              {"preview_status", "available"},
              {"preview", std::move(preview)}}},
            {"incident",
             {{"path", "diagnostic_incident.json"}, {"bytes", fixture.incident.size()}}}})
          .dump();
  RefreshZip(fixture);
  return fixture;
}

pose_hil::StandbyMissedEvidence Inspect(const Fixture &fixture) {
  return pose_hil::ValidateStandbyMissedEvidence({
      .manifest = fixture.manifest,
      .diagnostic_audio_wav = fixture.wav,
      .diagnostic_incident = fixture.incident,
      .preview_mjpeg = fixture.preview,
      .pose_trace_ndjson = fixture.trace,
      .diagnostics_zip = fixture.zip,
      .expected_session_id = fixture.session_id,
  });
}

Fixture AutomaticImpactFixture() {
  Fixture fixture = ValidFixture();
  Json manifest = Json::parse(fixture.manifest);
  manifest["event"]["kind"] = "detected_impact";
  manifest["event"]["operator_received_boottime_ns"] = nullptr;
  manifest["event"]["detector"] = {
      {"strike_frame_position", "48000"},
      {"confirmation_frame_position", "48010"},
      {"peak_amplitude", "0.9"},
      {"noise_floor", "0.1"},
      {"threshold", "0.2"},
      {"strike_boottime_ns", nullptr},
      {"strike_uncertainty_ns", nullptr},
      {"confirmation_boottime_ns", nullptr},
      {"confirmation_uncertainty_ns", nullptr},
  };
  Json incident = Json::parse(fixture.incident);
  incident["classification"] = "impact_while_not_armed";
  incident["user_feedback"]["classification"] = "unreviewed";
  incident["timing_marks"] = Json::array({{
      {"kind", "audio_impact_transient"},
      {"stream_id", "standby_audio"},
      {"offset_us", "0"},
  }});
  fixture.incident = incident.dump();
  manifest["incident"]["bytes"] = fixture.incident.size();
  fixture.manifest = manifest.dump();
  RefreshZip(fixture);
  return fixture;
}

pose_hil::StandbyMissedEvidence InspectAutomatic(const Fixture &fixture) {
  return pose_hil::ValidateStandbyMissedEvidence({
      .manifest = fixture.manifest,
      .diagnostic_audio_wav = fixture.wav,
      .diagnostic_incident = fixture.incident,
      .preview_mjpeg = fixture.preview,
      .pose_trace_ndjson = fixture.trace,
      .diagnostics_zip = fixture.zip,
      .expected_session_id = fixture.session_id,
      .expectation = pose_hil::StandbyDiagnosticExpectation::kAutomaticImpact,
  });
}

}  // namespace

int main() {
  Fixture valid = ValidFixture();
  const pose_hil::StandbyMissedEvidence evidence = Inspect(valid);
  Check(evidence.valid, evidence.diagnostic.c_str());
  Check(evidence.sample_count == 144'000, "sample count preserved");
  Check(evidence.pre_roll_frames == 48'000, "startup pre-roll preserved");
  Check(evidence.post_roll_frames == 96'000, "post-roll preserved");
  Check(evidence.startup_short_pre_roll, "startup-short pre-roll classified");
  Check(evidence.preview_frame_count == 3, "preview count preserved");
  Check(evidence.zip_entry_count == 6, "ZIP entry count preserved");

  Fixture automatic_impact = AutomaticImpactFixture();
  Check(InspectAutomatic(automatic_impact).valid, "automatic standby impact accepted");
  Check(!Inspect(automatic_impact).valid, "automatic impact rejected as operator tag");

  Fixture automatic_missing_detector = AutomaticImpactFixture();
  Json automatic_missing_manifest = Json::parse(automatic_missing_detector.manifest);
  automatic_missing_manifest["event"]["detector"] = nullptr;
  automatic_missing_detector.manifest = automatic_missing_manifest.dump();
  RefreshZip(automatic_missing_detector);
  Check(!InspectAutomatic(automatic_missing_detector).valid,
        "automatic impact without detector evidence rejected");

  Fixture wrong_kind = ValidFixture();
  Json wrong_kind_manifest = Json::parse(wrong_kind.manifest);
  wrong_kind_manifest["session_kind"] = "capture";
  wrong_kind.manifest = wrong_kind_manifest.dump();
  RefreshZip(wrong_kind);
  Check(!Inspect(wrong_kind).valid, "wrong session kind rejected");

  Fixture wrong_feedback = ValidFixture();
  Json wrong_feedback_incident = Json::parse(wrong_feedback.incident);
  wrong_feedback_incident["user_feedback"]["classification"] = "unreviewed";
  wrong_feedback.incident = wrong_feedback_incident.dump();
  Json wrong_feedback_manifest = Json::parse(wrong_feedback.manifest);
  wrong_feedback_manifest["incident"]["bytes"] = wrong_feedback.incident.size();
  wrong_feedback.manifest = wrong_feedback_manifest.dump();
  RefreshZip(wrong_feedback);
  Check(!Inspect(wrong_feedback).valid, "wrong incident semantics rejected");

  Fixture broken_wav = ValidFixture();
  broken_wav.wav.at(0) = 'X';
  RefreshZip(broken_wav);
  Check(!Inspect(broken_wav).valid, "noncanonical WAV rejected");

  Fixture broken_post_roll = ValidFixture();
  Json broken_post_manifest = Json::parse(broken_post_roll.manifest);
  broken_post_manifest["evidence"]["audio"]["end_frame_position"] = "143999";
  broken_post_roll.manifest = broken_post_manifest.dump();
  RefreshZip(broken_post_roll);
  Check(!Inspect(broken_post_roll).valid, "incorrect post-roll rejected");

  Fixture broken_trace = ValidFixture();
  const std::size_t first_newline = broken_trace.trace.find('\n');
  Json second_row = Json::parse(broken_trace.trace.substr(
      first_newline + 1U, broken_trace.trace.find('\n', first_newline + 1U) - first_newline - 1U));
  second_row["frame_byte_offset"] = "999";
  const std::size_t second_newline = broken_trace.trace.find('\n', first_newline + 1U);
  broken_trace.trace.replace(first_newline + 1U, second_newline - first_newline - 1U,
                             second_row.dump());
  Json broken_trace_manifest = Json::parse(broken_trace.manifest);
  broken_trace_manifest["evidence"]["preview"]["trace_bytes"] = broken_trace.trace.size();
  broken_trace.manifest = broken_trace_manifest.dump();
  RefreshZip(broken_trace);
  Check(!Inspect(broken_trace).valid, "broken preview linkage rejected");

  Fixture missing_zip_entry = ValidFixture();
  RefreshZip(missing_zip_entry, false);
  Check(!Inspect(missing_zip_entry).valid, "incomplete diagnostics ZIP rejected");

  std::cout << "standby missed-shot validation tests passed\n";
  return 0;
}
