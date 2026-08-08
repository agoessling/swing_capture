#include "capture/audio/arecord_pcm_source.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "capture/trigger/impact_detector.h"

namespace {

using swing_capture::ArecordPcmConfig;
using swing_capture::ArecordPcmSource;
using swing_capture::ImpactDetector;
using swing_capture::ImpactEvent;
using swing_capture::PcmReadStatus;

ArecordPcmConfig FakeConfig(const std::string &executable) {
  return {
      .capture_executable = executable,
      .device = "fake:frames=150",
      .sample_rate_hz = 8000,
      .channel_count = 2,
      .selected_channel = 1,
      .frames_per_block = 64,
      .read_timeout = std::chrono::seconds(1),
  };
}

void ReadsBoundedSelectedChannelAndTimestamps(const std::string &executable) {
  ArecordPcmSource source(FakeConfig(executable));
  std::string error;
  assert(source.Start(&error));
  assert(error.empty());
  assert(source.running());

  ImpactDetector detector;
  std::array<ImpactEvent, 4> events;
  std::vector<std::int16_t> all_samples;
  std::vector<std::chrono::steady_clock::time_point> starts;
  std::size_t impacts = 0;
  std::size_t blocks = 0;

  while (true) {
    auto result = source.ReadBlock();
    if (result.status == PcmReadStatus::kEndOfStream) {
      break;
    }
    assert(result.status == PcmReadStatus::kData);
    assert(!result.block.samples.empty());
    assert(result.block.samples.size() <= source.config().frames_per_block);
    assert(result.block.first_frame_index == all_samples.size());
    starts.push_back(result.block.estimated_start_time);
    const auto impact_result =
        detector.ProcessBlock(result.block.samples, result.block.estimated_start_time,
                              source.config().sample_rate_hz, events);
    impacts += impact_result.events_detected;
    all_samples.insert(all_samples.end(), result.block.samples.begin(), result.block.samples.end());
    ++blocks;
  }

  assert(blocks == 3);
  assert(all_samples.size() == 150);
  assert(source.frames_read() == 150);
  assert(all_samples[0] == -200);
  assert(all_samples[49] == -200);
  assert(all_samples[50] == -30000);
  assert(all_samples[51] == -200);
  assert(impacts == 1);
  assert(!source.running());

  const auto expected_block_duration =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(64.0 / 8000.0));
  assert(starts[1] - starts[0] == expected_block_duration);
  assert(starts[2] - starts[1] == expected_block_duration);
}

void ReportsCaptureProcessFailure(const std::string &executable) {
  ArecordPcmConfig config = FakeConfig(executable);
  config.device = "fake:fail";
  ArecordPcmSource source(config);
  std::string error;
  assert(source.Start(&error));

  const auto result = source.ReadBlock();

  assert(result.status == PcmReadStatus::kError);
  assert(result.message.find("status 23") != std::string::npos);
  assert(!source.running());
}

void ReportsMissingExecutable() {
  ArecordPcmConfig config;
  config.capture_executable = "/definitely/not/a/real/swing_capture_arecord";
  ArecordPcmSource source(config);
  std::string error;

  assert(!source.Start(&error));
  assert(!error.empty());
  assert(!source.running());
}

void ReportsPcmReadTimeout(const std::string &executable) {
  ArecordPcmConfig config = FakeConfig(executable);
  config.device = "fake:stall";
  config.read_timeout = std::chrono::milliseconds(20);
  ArecordPcmSource source(config);
  std::string error;
  assert(source.Start(&error));
  const auto start = std::chrono::steady_clock::now();

  const auto result = source.ReadBlock();

  const auto elapsed = std::chrono::steady_clock::now() - start;
  assert(result.status == PcmReadStatus::kError);
  assert(result.message.find("timed out") != std::string::npos);
  assert(elapsed < std::chrono::seconds(1));
  assert(!source.running());
}

void AppliesOneDeadlineAcrossPartialReads(const std::string &executable) {
  ArecordPcmConfig config = FakeConfig(executable);
  config.device = "fake:dribble";
  config.frames_per_block = 30;
  config.read_timeout = std::chrono::milliseconds(100);
  ArecordPcmSource source(config);
  std::string error;
  assert(source.Start(&error));
  const auto start = std::chrono::steady_clock::now();

  const auto result = source.ReadBlock();

  const auto elapsed = std::chrono::steady_clock::now() - start;
  assert(result.status == PcmReadStatus::kError);
  assert(result.message.find("timed out") != std::string::npos);
  assert(elapsed < std::chrono::seconds(1));
  assert(!source.running());
}

void StopTerminatesAndReaps(const std::string &executable) {
  ArecordPcmConfig config = FakeConfig(executable);
  config.device = "fake:continuous";
  ArecordPcmSource source(config);
  std::string error;
  assert(source.Start(&error));
  assert(source.ReadBlock().status == PcmReadStatus::kData);

  source.Stop();

  assert(!source.running());
  assert(source.ReadBlock().status == PcmReadStatus::kError);
}

void RejectsInvalidChannelSelection(const std::string &executable) {
  ArecordPcmConfig config = FakeConfig(executable);
  config.selected_channel = config.channel_count;
  bool threw = false;
  try {
    ArecordPcmSource source(config);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw);
}

}  // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string executable = argv[1];
  ReadsBoundedSelectedChannelAndTimestamps(executable);
  ReportsCaptureProcessFailure(executable);
  ReportsMissingExecutable();
  ReportsPcmReadTimeout(executable);
  AppliesOneDeadlineAcrossPartialReads(executable);
  StopTerminatesAndReaps(executable);
  RejectsInvalidChannelSelection(executable);
  return 0;
}
