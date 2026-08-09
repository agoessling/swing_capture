#include "capture/audio/audio_capture_session.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/audio/arecord_pcm_source.h"

namespace {

using swing_capture::AudioCaptureSession;
using swing_capture::AudioCaptureSource;
using swing_capture::AudioCaptureSourceFactory;
using swing_capture::MonoPcmBlock;
using swing_capture::PcmReadResult;
using swing_capture::PcmReadStatus;

using namespace std::chrono_literals;

struct SourceTrace {
  std::mutex mutex;
  std::condition_variable changed;
  std::thread::id constructed_thread;
  std::thread::id started_thread;
  std::thread::id read_thread;
  std::thread::id stopped_thread;
  std::thread::id destroyed_thread;
  bool read_entered = false;
};

struct FakeSourcePlan {
  bool start_succeeds = true;
  bool stream_forever = false;
  std::chrono::milliseconds start_delay = 0ms;
  std::chrono::milliseconds read_delay = 0ms;
  std::vector<std::uint64_t> block_offsets;
  std::size_t error_after_blocks = static_cast<std::size_t>(-1);
};

class FakeCaptureSource final : public AudioCaptureSource {
 public:
  FakeCaptureSource(std::shared_ptr<SourceTrace> trace, FakeSourcePlan plan)
      : trace_(std::move(trace)), plan_(std::move(plan)) {
    std::lock_guard lock(trace_->mutex);
    trace_->constructed_thread = std::this_thread::get_id();
  }

  ~FakeCaptureSource() override {
    std::lock_guard lock(trace_->mutex);
    trace_->destroyed_thread = std::this_thread::get_id();
    trace_->changed.notify_all();
  }

  [[nodiscard]] bool Start(std::string *error) override {
    {
      std::lock_guard lock(trace_->mutex);
      trace_->started_thread = std::this_thread::get_id();
    }
    std::this_thread::sleep_for(plan_.start_delay);
    if (!plan_.start_succeeds) {
      *error = "synthetic start failure";
      return false;
    }
    error->clear();
    return true;
  }

  [[nodiscard]] PcmReadResult ReadBlock() override {
    {
      std::lock_guard lock(trace_->mutex);
      trace_->read_thread = std::this_thread::get_id();
      trace_->read_entered = true;
      trace_->changed.notify_all();
    }
    std::this_thread::sleep_for(plan_.read_delay);
    if (completed_blocks_ == plan_.error_after_blocks) {
      return {
          .status = PcmReadStatus::kError,
          .block = {},
          .message = "synthetic read failure",
      };
    }
    if (!plan_.stream_forever && completed_blocks_ >= plan_.block_offsets.size()) {
      return {
          .status = PcmReadStatus::kEndOfStream,
          .block = {},
          .message = {},
      };
    }

    const std::uint64_t offset = plan_.stream_forever ? completed_blocks_ * kSamplesPerBlock
                                                      : plan_.block_offsets[completed_blocks_];
    ++completed_blocks_;
    return {
        .status = PcmReadStatus::kData,
        .block =
            MonoPcmBlock{
                .samples = {100, -200, 300, -400},
                .estimated_start_time = {},
                .first_frame_index = offset,
            },
        .message = {},
    };
  }

  void Stop() noexcept override {
    std::lock_guard lock(trace_->mutex);
    trace_->stopped_thread = std::this_thread::get_id();
  }

 private:
  static constexpr std::uint64_t kSamplesPerBlock = 4;

  std::shared_ptr<SourceTrace> trace_;
  FakeSourcePlan plan_;
  std::uint64_t completed_blocks_ = 0;
};

AudioCaptureSourceFactory FakeFactory(std::shared_ptr<SourceTrace> trace, FakeSourcePlan plan) {
  return [trace = std::move(trace), plan = std::move(plan)] {
    return std::make_unique<FakeCaptureSource>(trace, plan);
  };
}

void ResultIsUnavailableBeforeJoinAndSourceStaysOnWorker() {
  const auto trace = std::make_shared<SourceTrace>();
  const std::thread::id control_thread = std::this_thread::get_id();
  FakeSourcePlan plan;
  plan.stream_forever = true;
  plan.read_delay = std::chrono::milliseconds(1);
  AudioCaptureSession session(FakeFactory(trace, std::move(plan)));
  session.Start();

  bool result_rejected = false;
  try {
    static_cast<void>(session.result());
  } catch (const std::logic_error &) {
    result_rejected = true;
  }
  assert(result_rejected);

  const auto ready = session.WaitUntilReady(std::chrono::steady_clock::now() + 200ms);
  assert(ready.condition_satisfied);
  const auto samples =
      session.WaitForCompletedSamples(12, std::chrono::steady_clock::now() + 200ms);
  assert(samples.condition_satisfied);
  assert(session.completed_sample_count() >= 12);

  const auto capture_through = std::chrono::steady_clock::now() + 5ms;
  const auto wall_time =
      session.WaitThroughWallTime(capture_through, std::chrono::steady_clock::now() + 100ms);
  assert(wall_time.condition_satisfied);
  assert(!wall_time.terminal);

  const auto stopped = session.RequestStopAndJoin(std::chrono::steady_clock::now() + 200ms);
  assert(stopped.condition_satisfied);
  assert(stopped.terminal);
  assert(stopped.joined);
  const auto &result = session.result();
  assert(!result.samples.empty());
  assert(result.samples.size() == result.diagnostics.completed_samples);
  assert(result.diagnostics.source_started);
  assert(result.diagnostics.stop_requested);
  assert(result.diagnostics.continuity_valid);
  assert(result.diagnostics.clean_shutdown);
  assert(result.diagnostics.error.empty());

  std::lock_guard lock(trace->mutex);
  assert(trace->constructed_thread != control_thread);
  assert(trace->constructed_thread == trace->started_thread);
  assert(trace->constructed_thread == trace->read_thread);
  assert(trace->constructed_thread == trace->stopped_thread);
  assert(trace->constructed_thread == trace->destroyed_thread);
}

void ContinuityFailurePreservesCompletedPcm() {
  const auto trace = std::make_shared<SourceTrace>();
  AudioCaptureSession session(FakeFactory(trace, {.block_offsets = {0, 99}}));
  session.Start();

  const auto terminal = session.WaitUntilTerminal(std::chrono::steady_clock::now() + 200ms);
  assert(terminal.condition_satisfied);
  assert(terminal.error.find("expected block offset 4, received 99") != std::string::npos);
  const auto joined = session.RequestStopAndJoin(std::chrono::steady_clock::now() + 200ms);
  assert(joined.joined);

  const auto &result = session.result();
  assert(result.samples.size() == 4);
  assert(result.diagnostics.completed_blocks == 1);
  assert(!result.diagnostics.continuity_valid);
  assert(!result.diagnostics.clean_shutdown);
}

void StartAndReadErrorsWakeWaitersAndPreserveEvidence() {
  {
    const auto trace = std::make_shared<SourceTrace>();
    FakeSourcePlan plan;
    plan.start_succeeds = false;
    AudioCaptureSession session(FakeFactory(trace, std::move(plan)));
    session.Start();
    const auto ready = session.WaitUntilReady(std::chrono::steady_clock::now() + 200ms);
    assert(!ready.condition_satisfied);
    assert(ready.terminal);
    assert(ready.error.find("synthetic start failure") != std::string::npos);
    assert(session.RequestStopAndJoin(std::chrono::steady_clock::now() + 200ms).joined);
    assert(session.result().samples.empty());
    assert(!session.result().diagnostics.source_started);
  }

  {
    const auto trace = std::make_shared<SourceTrace>();
    FakeSourcePlan plan;
    plan.stream_forever = true;
    plan.error_after_blocks = 1;
    AudioCaptureSession session(FakeFactory(trace, std::move(plan)));
    session.Start();
    const auto terminal = session.WaitUntilTerminal(std::chrono::steady_clock::now() + 200ms);
    assert(terminal.terminal);
    assert(terminal.completed_samples == 4);
    assert(terminal.error.find("synthetic read failure") != std::string::npos);
    assert(session.RequestStopAndJoin(std::chrono::steady_clock::now() + 200ms).joined);
    assert(session.result().samples.size() == 4);
    assert(!session.result().diagnostics.clean_shutdown);
  }
}

void DeadlineWaitsAndStopJoinAreBoundedAndRetryable() {
  const auto trace = std::make_shared<SourceTrace>();
  FakeSourcePlan plan;
  plan.stream_forever = true;
  plan.start_delay = 20ms;
  plan.read_delay = 80ms;
  AudioCaptureSession session(FakeFactory(trace, std::move(plan)));
  session.Start();

  const auto early_ready = session.WaitUntilReady(std::chrono::steady_clock::now() + 2ms);
  assert(!early_ready.condition_satisfied);
  assert(early_ready.deadline_expired);
  const auto ready = session.WaitUntilReady(std::chrono::steady_clock::now() + 100ms);
  assert(ready.condition_satisfied);

  {
    std::unique_lock lock(trace->mutex);
    assert(trace->changed.wait_for(lock, 100ms, [&trace] { return trace->read_entered; }));
  }
  const auto stop_start = std::chrono::steady_clock::now();
  const auto timed_out = session.RequestStopAndJoin(stop_start + 5ms);
  const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;
  assert(!timed_out.condition_satisfied);
  assert(timed_out.deadline_expired);
  assert(!timed_out.joined);
  assert(stop_elapsed < 40ms);
  assert(!session.CopyTerminalResult().has_value());

  const auto terminal = session.WaitUntilTerminal(std::chrono::steady_clock::now() + 200ms);
  assert(terminal.condition_satisfied);
  const auto terminal_copy = session.CopyTerminalResult();
  assert(terminal_copy.has_value());
  assert(!terminal_copy->samples.empty());
  assert(terminal_copy->diagnostics.clean_shutdown);

  const auto joined = session.RequestStopAndJoin(std::chrono::steady_clock::now() + 200ms);
  assert(joined.condition_satisfied);
  assert(joined.joined);
  assert(session.result().diagnostics.clean_shutdown);
}

}  // namespace

int main() {
  ResultIsUnavailableBeforeJoinAndSourceStaysOnWorker();
  ContinuityFailurePreservesCompletedPcm();
  StartAndReadErrorsWakeWaitersAndPreserveEvidence();
  DeadlineWaitsAndStopJoinAreBoundedAndRetryable();
  return 0;
}
