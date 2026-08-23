#include "android/dual_hil/hil_artifact_validation.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using swing_capture::android::dual_hil::ValidateHilReportArtifacts;

void Check(bool condition) {
  if (!condition) {
    throw std::runtime_error("HIL artifact validation assertion failed");
  }
}

void WriteFile(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << "evidence\n";
  Check(output.good());
}

void ExpectFailure(const Json &report, const std::filesystem::path &root,
                   std::string_view expected_diagnostic,
                   std::optional<std::filesystem::path> self_report = std::nullopt) {
  try {
    ValidateHilReportArtifacts(report.dump(), root, std::move(self_report));
  } catch (const std::exception &failure) {
    if (std::string_view(failure.what()).find(expected_diagnostic) == std::string_view::npos) {
      throw std::runtime_error("expected diagnostic '" + std::string(expected_diagnostic) +
                               "', got '" + failure.what() + "'");
    }
    return;
  }
  throw std::runtime_error("invalid HIL artifact report was accepted");
}

std::filesystem::path TestRoot() {
  const char *test_tmpdir = std::getenv("TEST_TMPDIR");
  if (test_tmpdir == nullptr || std::string_view(test_tmpdir).empty()) {
    throw std::runtime_error("TEST_TMPDIR is required");
  }
  const std::filesystem::path root = std::filesystem::path(test_tmpdir) / "artifact-root";
  std::filesystem::create_directories(root);
  return root;
}

void NestedMapsArraysAndNullPass(const std::filesystem::path &root) {
  WriteFile(root / "top.json");
  WriteFile(root / "phone" / "frame.png");
  const Json report = {
      {"artifacts",
       {{"top", "top.json"},
        {"phones",
         Json::array({Json{{"frame", "phone/frame.png"}, {"optional", nullptr}}, nullptr})}}},
  };
  ValidateHilReportArtifacts(report.dump(), root);
}

void UnsafePathsFail(const std::filesystem::path &root) {
  ExpectFailure(Json{{"artifacts", {{"escape", "../outside.json"}}}}, root,
                "must not contain empty, current, or parent components");
  ExpectFailure(Json{{"artifacts", {{"escape", "nested/../../outside.json"}}}}, root,
                "canonical, nonempty relative path");
  ExpectFailure(Json{{"artifacts", {{"absolute", "/tmp/outside.json"}}}}, root,
                "canonical, nonempty relative path");
  ExpectFailure(Json{{"artifacts", {{"directory_syntax", "phone/"}}}}, root,
                "canonical, nonempty relative path");
  ExpectFailure(Json{{"artifacts", {{"windows_separator", "phone\\frame.png"}}}}, root,
                "canonical, nonempty relative path");
}

void DeadAndNonFileLinksFail(const std::filesystem::path &root) {
  std::filesystem::create_directories(root / "directory");
  ExpectFailure(Json{{"artifacts", {{"missing", "missing.json"}}}}, root, "does not exist");
  ExpectFailure(Json{{"artifacts", {{"directory", "directory"}}}}, root, "not a regular file");
}

void ParentSymlinksCannotEscapeTheRoot(const std::filesystem::path &root) {
  const std::filesystem::path outside = root.parent_path() / "outside-artifact-root";
  std::filesystem::create_directories(outside);
  WriteFile(outside / "external.json");
  std::filesystem::create_directory_symlink(outside, root / "external");
  ExpectFailure(Json{{"artifacts", {{"escaped", "external/external.json"}}}}, root,
                "outside the undeclared-output root");
  ExpectFailure(Json{{"artifacts", {{"report", "external/report.json"}}}}, root,
                "self-report parent is outside", "external/report.json");
}

void OnlyTheExactMissingSelfReportIsAllowed(const std::filesystem::path &root) {
  const Json self_only = {{"artifacts", {{"report", "report.json"}, {"optional", nullptr}}}};
  ValidateHilReportArtifacts(self_only.dump(), root, "report.json");
  ExpectFailure(self_only, root, "does not exist", "different-report.json");

  const Json self_and_missing = {
      {"artifacts", {{"report", "report.json"}, {"other", "other.json"}}}};
  ExpectFailure(self_and_missing, root, "does not exist", "report.json");

  std::filesystem::create_directories(root / "directory-report.json");
  const Json self_directory = {{"artifacts", {{"report", "directory-report.json"}}}};
  ExpectFailure(self_directory, root, "not a regular file", "directory-report.json");
}

void InvalidArtifactTreesFail(const std::filesystem::path &root) {
  ExpectFailure(Json{{"artifacts", {{"not_a_path", true}}}}, root, "must be strings or null");
  ExpectFailure(Json{{"passed", true}}, root, "artifacts object");
}

void QualificationArtifactTreeIsFileOnlyAndRootRelative(const std::filesystem::path &root) {
  constexpr std::string_view kCycleRoot = "cycles/cycle-0/";
  const Json cycle_artifacts = {
      {"cycle", std::string(kCycleRoot) + "cycle.json"},
      {"coordination", std::string(kCycleRoot) + "coordination.json"},
      {"face_on",
       {{"clock", std::string(kCycleRoot) + "face_on/clock.json"},
        {"report", std::string(kCycleRoot) + "face_on/report.json"},
        {"manifest", std::string(kCycleRoot) + "face_on/manifest.json"},
        {"media", std::string(kCycleRoot) + "face_on/face_on.mp4"},
        {"audio", std::string(kCycleRoot) + "face_on/audio_evidence.wav"},
        {"ffprobe", std::string(kCycleRoot) + "face_on/ffprobe.json"},
        {"diagnostic_frames",
         {{"pre", std::string(kCycleRoot) + "face_on/diagnostic-01.png"},
          {"marker", std::string(kCycleRoot) + "face_on/diagnostic-02.png"},
          {"post", std::string(kCycleRoot) + "face_on/diagnostic-03.png"}}}}},
      {"down_the_line",
       {{"clock", std::string(kCycleRoot) + "down_the_line/clock.json"},
        {"report", std::string(kCycleRoot) + "down_the_line/report.json"},
        {"manifest", std::string(kCycleRoot) + "down_the_line/manifest.json"},
        {"media", std::string(kCycleRoot) + "down_the_line/down_the_line.mp4"},
        {"audio", std::string(kCycleRoot) + "down_the_line/diagnostic_audio.wav"},
        {"ffprobe", std::string(kCycleRoot) + "down_the_line/ffprobe.json"},
        {"diagnostic_frames",
         {{"pre", std::string(kCycleRoot) + "down_the_line/diagnostic-01.png"},
          {"marker", std::string(kCycleRoot) + "down_the_line/diagnostic-02.png"},
          {"post", std::string(kCycleRoot) + "down_the_line/diagnostic-03.png"}}}}},
  };
  for (const std::string_view path : {
           "qualification-progress.json",
           "face_on/face_on.mp4",
           "down_the_line/down_the_line.mp4",
           "cycles/cycle-0/cycle.json",
           "cycles/cycle-0/coordination.json",
           "cycles/cycle-0/face_on/clock.json",
           "cycles/cycle-0/face_on/report.json",
           "cycles/cycle-0/face_on/manifest.json",
           "cycles/cycle-0/face_on/face_on.mp4",
           "cycles/cycle-0/face_on/audio_evidence.wav",
           "cycles/cycle-0/face_on/ffprobe.json",
           "cycles/cycle-0/face_on/diagnostic-01.png",
           "cycles/cycle-0/face_on/diagnostic-02.png",
           "cycles/cycle-0/face_on/diagnostic-03.png",
           "cycles/cycle-0/down_the_line/clock.json",
           "cycles/cycle-0/down_the_line/report.json",
           "cycles/cycle-0/down_the_line/manifest.json",
           "cycles/cycle-0/down_the_line/down_the_line.mp4",
           "cycles/cycle-0/down_the_line/diagnostic_audio.wav",
           "cycles/cycle-0/down_the_line/ffprobe.json",
           "cycles/cycle-0/down_the_line/diagnostic-01.png",
           "cycles/cycle-0/down_the_line/diagnostic-02.png",
           "cycles/cycle-0/down_the_line/diagnostic-03.png",
       }) {
    WriteFile(root / path);
  }
  const Json report = {
      {"artifacts",
       {{"report", "qualification-report.json"},
        {"progress", "qualification-progress.json"},
        {"cycles", Json::array({cycle_artifacts})},
        {"latest_face_on_media", "face_on/face_on.mp4"},
        {"latest_down_the_line_media", "down_the_line/down_the_line.mp4"}}},
  };
  ValidateHilReportArtifacts(report.dump(), root, "qualification-report.json");

  Json missing_nested_artifact = report;
  missing_nested_artifact["artifacts"]["cycles"][0]["face_on"]["audio"] =
      "cycles/cycle-0/face_on/missing.wav";
  ExpectFailure(missing_nested_artifact, root, "does not exist", "qualification-report.json");

  Json missing_cycle_media = report;
  missing_cycle_media["artifacts"]["cycles"][0]["down_the_line"]["media"] =
      "cycles/cycle-0/down_the_line/missing.mp4";
  ExpectFailure(missing_cycle_media, root, "does not exist", "qualification-report.json");
}

void PairedCaptureArtifactTreeIncludesRetainedMediaAndDiagnostics(
    const std::filesystem::path &root) {
  const auto capture_artifacts = [](std::string_view role, std::string_view audio_filename) {
    const std::string prefix = std::string(role) + "/";
    return Json{
        {"capture_report", prefix + "report.json"},
        {"manifest", prefix + "manifest.json"},
        {"media", prefix + std::string(role) + ".mp4"},
        {"audio", prefix + std::string(audio_filename)},
        {"ffprobe", prefix + "ffprobe.json"},
        {"diagnostic_frames",
         {{"pre", prefix + "diagnostic-01.png"},
          {"marker", prefix + "diagnostic-02.png"},
          {"post", prefix + "diagnostic-03.png"}}},
    };
  };
  for (const std::string_view path : {
           "face_on/report.json",
           "face_on/manifest.json",
           "face_on/face_on.mp4",
           "face_on/audio_evidence.wav",
           "face_on/ffprobe.json",
           "face_on/diagnostic-01.png",
           "face_on/diagnostic-02.png",
           "face_on/diagnostic-03.png",
           "down_the_line/report.json",
           "down_the_line/manifest.json",
           "down_the_line/down_the_line.mp4",
           "down_the_line/diagnostic_audio.wav",
           "down_the_line/ffprobe.json",
           "down_the_line/diagnostic-01.png",
           "down_the_line/diagnostic-02.png",
           "down_the_line/diagnostic-03.png",
       }) {
    WriteFile(root / path);
  }
  const Json report = {
      {"artifacts",
       {{"report", "paired-report.json"},
        {"face_on", {{"capture", capture_artifacts("face_on", "audio_evidence.wav")}}},
        {"down_the_line",
         {{"capture", capture_artifacts("down_the_line", "diagnostic_audio.wav")}}}}},
  };
  ValidateHilReportArtifacts(report.dump(), root, "paired-report.json");

  Json dead_diagnostic = report;
  dead_diagnostic["artifacts"]["down_the_line"]["capture"]["diagnostic_frames"]["post"] =
      "down_the_line/missing-diagnostic.png";
  ExpectFailure(dead_diagnostic, root, "does not exist", "paired-report.json");
}

}  // namespace

int main() {
  try {
    const std::filesystem::path root = TestRoot();
    NestedMapsArraysAndNullPass(root);
    UnsafePathsFail(root);
    DeadAndNonFileLinksFail(root);
    ParentSymlinksCannotEscapeTheRoot(root);
    OnlyTheExactMissingSelfReportIsAllowed(root);
    InvalidArtifactTreesFail(root);
    QualificationArtifactTreeIsFileOnlyAndRootRelative(root);
    PairedCaptureArtifactTreeIncludesRetainedMediaAndDiagnostics(root);
    return 0;
  } catch (const std::exception &failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "unknown HIL artifact validation test failure\n";
    return 1;
  }
}
