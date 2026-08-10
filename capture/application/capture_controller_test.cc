#include "capture/application/capture_controller.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/application/camera_clip_buffer.h"
#include "capture/audio/audio_capture_session.h"
#include "capture/core/camera_source.h"

namespace {

using namespace std::chrono_literals;
using swing_capture::AudioCaptureSource;
using swing_capture::FrameMetadata;
using swing_capture::FrameView;
using swing_capture::MonoPcmBlock;
using swing_capture::PcmReadResult;
using swing_capture::PcmReadStatus;
using swing_capture::application::CameraCaptureEndpoint;
using swing_capture::application::CameraClipBuffer;
using swing_capture::application::CaptureApplicationState;
using swing_capture::application::CaptureController;
using swing_capture::application::CaptureControllerConfig;
using swing_capture::application::CapturedSession;
using swing_capture::application::CaptureTriggerSource;
using swing_capture::application::PublishedSession;
using swing_capture::application::SessionIdentity;

class TriggerableSource final : public AudioCaptureSource {
 public:
  TriggerableSource(std::shared_ptr<std::atomic<bool>> fire,
                    std::chrono::milliseconds startup_delay)
      : fire_(std::move(fire)), startup_delay_(startup_delay) {}

  bool Start(std::string *error) override {
    std::this_thread::sleep_for(startup_delay_);
    error->clear();
    return true;
  }

  PcmReadResult ReadBlock() override {
    std::this_thread::sleep_for(1ms);
    std::vector<std::int16_t> samples(64, 60);
    if (fire_->exchange(false)) {
      samples[8] = 12000;
      samples[9] = 18000;
      samples[10] = 9000;
    }
    MonoPcmBlock block = {
        .samples = std::move(samples),
        .estimated_start_time = origin_ + std::chrono::milliseconds(block_index_ * 2U),
        .first_frame_index = block_index_ * 64U,
    };
    ++block_index_;
    return {.status = PcmReadStatus::kData, .block = std::move(block), .message = {}};
  }

  void Stop() noexcept override {}

 private:
  std::shared_ptr<std::atomic<bool>> fire_;
  std::chrono::milliseconds startup_delay_;
  std::chrono::steady_clock::time_point origin_ = std::chrono::steady_clock::now();
  std::uint64_t block_index_ = 0;
};

FrameView Frame(std::uint64_t frame_id, const std::array<std::byte, 4> &pixels) {
  return {
      .metadata =
          FrameMetadata{
              .frame_id = frame_id,
              .device_timestamp = frame_id * 1000U,
              .host_received_at = std::chrono::steady_clock::now(),
              .width = 2,
              .height = 2,
              .complete = true,
          },
      .payload = pixels,
  };
}

template <typename Predicate>
bool WaitFor(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

void TestAudioAndManualCaptureLifecycle() {
  CameraClipBuffer down(
      {.active_frame_capacity = 64, .reserve_frame_blocks = 64, .maximum_payload_bytes = 4});
  CameraClipBuffer face(
      {.active_frame_capacity = 64, .reserve_frame_blocks = 64, .maximum_payload_bytes = 4});
  auto fire = std::make_shared<std::atomic<bool>>(false);
  std::atomic<std::uint64_t> published_count = 0;
  std::atomic<std::uint64_t> identity_count = 0;
  CaptureTriggerSource published_source = CaptureTriggerSource::kManual;

  CaptureController controller(
      CaptureControllerConfig{.pre_roll = 8ms,
                              .post_roll = 5ms,
                              .frame_boundary_margin = 1ms,
                              .minimum_pre_roll_frames = 5},
      std::array{
          CameraCaptureEndpoint{.role = "down_the_line",
                                .serial = "DOWN",
                                .device_ticks_per_second = 1'000'000,
                                .buffer = &down},
          CameraCaptureEndpoint{.role = "face_on",
                                .serial = "FACE",
                                .device_ticks_per_second = 1'000'000,
                                .buffer = &face},
      },
      [fire] { return std::make_unique<TriggerableSource>(fire, 20ms); }, 32000,
      [&identity_count] {
        const std::uint64_t id = identity_count.fetch_add(1) + 1U;
        return SessionIdentity{.session_id = "session-" + std::to_string(id),
                               .created_at_utc = "2026-08-09T00:00:00Z"};
      },
      [&published_count, &published_source](CapturedSession session) {
        assert(session.cameras[0].role == "down_the_line");
        assert(session.cameras[1].role == "face_on");
        assert(session.cameras[0].frames.size() >= 5);
        assert(session.cameras[1].frames.size() >= 5);
        assert(session.pipeline_timing.freeze_started_at >=
               session.pipeline_timing.freeze_target_at);
        assert(session.pipeline_timing.freeze_completed_at >=
               session.pipeline_timing.freeze_started_at);
        assert(session.pipeline_timing.audio_stop_started_at ==
               session.pipeline_timing.freeze_completed_at);
        assert(session.pipeline_timing.audio_stop_completed_at >=
               session.pipeline_timing.audio_stop_started_at);
        published_source = session.trigger.source;
        published_count.fetch_add(1);
        return PublishedSession{.session_id = session.identity.session_id,
                                .created_at_utc = session.identity.created_at_utc,
                                .manifest_path = session.identity.session_id + "/manifest.json"};
      });

  std::atomic<bool> produce = true;
  std::jthread producer([&](const std::stop_token &stop_token) {
    const std::array pixels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    std::uint64_t frame_id = 1;
    while (!stop_token.stop_requested() && produce.load()) {
      down.ObserveFrame(Frame(frame_id, pixels));
      face.ObserveFrame(Frame(frame_id, pixels));
      ++frame_id;
      std::this_thread::sleep_for(1ms);
    }
  });

  controller.Arm();
  assert(controller.Status().state == CaptureApplicationState::kArming);
  assert(WaitFor([&] { return controller.Status().state == CaptureApplicationState::kArmed; }));
  fire->store(true);
  const bool audio_published = WaitFor([&] {
    return published_count.load() == 1 &&
           controller.Status().state == CaptureApplicationState::kReady;
  });
  if (!audio_published) {
    const auto failed = controller.Status();
    std::cerr << "state=" << static_cast<int>(failed.state)
              << " impacts=" << failed.audio.detected_impacts
              << " blocks=" << failed.audio.completed_blocks << " error=" << failed.error
              << " audio_error=" << failed.audio.error << '\n';
  }
  assert(audio_published);
  auto status = controller.Status();
  assert(status.state == CaptureApplicationState::kReady);
  assert(status.last_trigger.has_value());
  assert(status.last_trigger->source == CaptureTriggerSource::kAudio);
  assert(status.last_trigger->impact.peak_amplitude > 0.5F);
  assert(status.sessions.size() == 1);
  assert(published_source == CaptureTriggerSource::kAudio);
  assert(!status.armed);
  assert(!status.audio.running);

  controller.Arm();
  assert(WaitFor([&] { return controller.Status().state == CaptureApplicationState::kArmed; }));
  const SessionIdentity manual = controller.CaptureManually();
  assert(manual.session_id == "session-2");
  bool duplicate_rejected = false;
  try {
    static_cast<void>(controller.CaptureManually());
  } catch (const std::logic_error &) {
    duplicate_rejected = true;
  }
  assert(duplicate_rejected);
  assert(WaitFor([&] {
    return published_count.load() == 2 &&
           controller.Status().state == CaptureApplicationState::kReady;
  }));
  assert(published_source == CaptureTriggerSource::kManual);
  assert(controller.Status().sessions.size() == 2);

  controller.Disarm();
  assert(controller.Status().state == CaptureApplicationState::kSetup);
  produce.store(false);
  producer.request_stop();
  producer.join();
}

void TestPublisherFailureRecoversAndDisarmWaitsForPublication() {
  CameraClipBuffer down(
      {.active_frame_capacity = 64, .reserve_frame_blocks = 0, .maximum_payload_bytes = 4});
  CameraClipBuffer face(
      {.active_frame_capacity = 64, .reserve_frame_blocks = 0, .maximum_payload_bytes = 4});
  auto fire = std::make_shared<std::atomic<bool>>(false);
  std::atomic<std::uint64_t> identity_count = 0;
  std::atomic<std::uint64_t> publisher_calls = 0;
  std::atomic<bool> release_second_publication = false;

  CaptureController controller(
      CaptureControllerConfig{.pre_roll = 8ms,
                              .post_roll = 5ms,
                              .frame_boundary_margin = 1ms,
                              .minimum_pre_roll_frames = 5},
      std::array{
          CameraCaptureEndpoint{.role = "down_the_line",
                                .serial = "DOWN-RECOVERY",
                                .device_ticks_per_second = 1'000'000,
                                .buffer = &down},
          CameraCaptureEndpoint{.role = "face_on",
                                .serial = "FACE-RECOVERY",
                                .device_ticks_per_second = 1'000'000,
                                .buffer = &face},
      },
      [fire] { return std::make_unique<TriggerableSource>(fire, 1ms); }, 32000,
      [&identity_count] {
        const std::uint64_t id = identity_count.fetch_add(1) + 1U;
        return SessionIdentity{.session_id = "recovery-" + std::to_string(id),
                               .created_at_utc = "2026-08-09T00:00:00Z"};
      },
      [&publisher_calls, &release_second_publication](CapturedSession session) {
        const std::uint64_t call = publisher_calls.fetch_add(1) + 1U;
        if (call == 1U) {
          throw std::runtime_error("synthetic publisher failure");
        }
        while (!release_second_publication.load()) {
          std::this_thread::sleep_for(1ms);
        }
        return PublishedSession{.session_id = session.identity.session_id,
                                .created_at_utc = session.identity.created_at_utc,
                                .manifest_path = session.identity.session_id + "/manifest.json"};
      });

  std::jthread producer([&](const std::stop_token &stop_token) {
    const std::array pixels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    std::uint64_t frame_id = 1;
    while (!stop_token.stop_requested()) {
      down.ObserveFrame(Frame(frame_id, pixels));
      face.ObserveFrame(Frame(frame_id, pixels));
      ++frame_id;
      std::this_thread::sleep_for(1ms);
    }
  });

  controller.Arm();
  assert(WaitFor([&] { return controller.Status().state == CaptureApplicationState::kArmed; }));
  static_cast<void>(controller.CaptureManually());
  assert(WaitFor([&] { return controller.Status().state == CaptureApplicationState::kError; }));
  auto failed = controller.Status();
  assert(failed.error == "synthetic publisher failure");
  assert(!failed.audio.running);
  assert(!failed.cameras[0].armed);
  assert(!failed.cameras[1].armed);

  controller.Arm();
  assert(WaitFor([&] { return controller.Status().state == CaptureApplicationState::kArmed; }));
  static_cast<void>(controller.CaptureManually());
  assert(WaitFor([&] { return controller.Status().state == CaptureApplicationState::kEncoding; }));
  auto disarm = std::async(std::launch::async, [&controller] { controller.Disarm(); });
  assert(disarm.wait_for(10ms) == std::future_status::timeout);
  release_second_publication.store(true);
  assert(disarm.wait_for(500ms) == std::future_status::ready);
  disarm.get();
  const auto recovered = controller.Status();
  assert(recovered.state == CaptureApplicationState::kSetup);
  assert(recovered.sessions.size() == 1);
  assert(publisher_calls.load() == 2);

  producer.request_stop();
  producer.join();
}

}  // namespace

int main() {
  TestAudioAndManualCaptureLifecycle();
  TestPublisherFailureRecoversAndDisarmWaitsForPublication();
  return 0;
}
