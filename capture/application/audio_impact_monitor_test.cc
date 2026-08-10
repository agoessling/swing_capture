#include "capture/application/audio_impact_monitor.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/audio/audio_capture_session.h"
#include "capture/trigger/impact_detector.h"

namespace {

using namespace std::chrono_literals;
using swing_capture::AudioCaptureSource;
using swing_capture::ImpactEvent;
using swing_capture::MonoPcmBlock;
using swing_capture::PcmReadResult;
using swing_capture::PcmReadStatus;
using swing_capture::application::AudioImpactMonitor;

class RepeatingSource final : public AudioCaptureSource {
 public:
  explicit RepeatingSource(std::shared_ptr<std::atomic<bool>> stopped)
      : stopped_(std::move(stopped)) {}

  bool Start(std::string *error) override {
    error->clear();
    return true;
  }

  PcmReadResult ReadBlock() override {
    std::this_thread::sleep_for(1ms);
    std::vector<std::int16_t> samples(64, 100);
    if (block_index_ == 2) {
      samples[12] = 12000;
      samples[13] = 18000;
      samples[14] = 10000;
    }
    MonoPcmBlock block = {
        .samples = std::move(samples),
        .estimated_start_time =
            std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(block_index_ * 2),
        .first_frame_index = block_index_ * 64U,
    };
    ++block_index_;
    return {.status = PcmReadStatus::kData, .block = std::move(block), .message = {}};
  }

  void Stop() noexcept override { stopped_->store(true); }

 private:
  std::shared_ptr<std::atomic<bool>> stopped_;
  std::uint64_t block_index_ = 0;
};

void TestDetectsImpactAndStopsSource() {
  auto stopped = std::make_shared<std::atomic<bool>>(false);
  std::atomic<std::uint64_t> detected = 0;
  ImpactEvent observed;
  AudioImpactMonitor monitor([stopped] { return std::make_unique<RepeatingSource>(stopped); },
                             32000,
                             [&detected, &observed](const ImpactEvent &impact) {
                               observed = impact;
                               detected.fetch_add(1);
                             });
  monitor.Start();
  const auto deadline = std::chrono::steady_clock::now() + 200ms;
  while (detected.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  monitor.Stop();

  assert(detected.load() == 1);
  assert(observed.peak_amplitude > 0.5F);
  assert(stopped->load());
  const auto status = monitor.Status();
  assert(!status.running);
  assert(status.source_ready);
  assert(status.detected_impacts == 1);
  assert(status.completed_samples >= 192);
  assert(status.error.empty());
}

}  // namespace

int main() {
  TestDetectsImpactAndStopsSource();
  return 0;
}
