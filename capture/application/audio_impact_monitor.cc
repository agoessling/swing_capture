#include "capture/application/audio_impact_monitor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "capture/audio/arecord_pcm_source.h"
#include "capture/audio/audio_capture_session.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture::application {

struct AudioImpactMonitor::SharedStatus {
  mutable std::mutex mutex;
  AudioImpactMonitorStatus value;
};

namespace {

std::string ExceptionMessage() {
  try {
    throw;
  } catch (const std::exception &error) {
    return error.what();
  } catch (...) {
    return "non-standard audio impact monitor failure";
  }
}

}  // namespace

AudioImpactMonitor::AudioImpactMonitor(AudioCaptureSourceFactory source_factory,
                                       std::uint32_t sample_rate_hz, ImpactHandler impact_handler,
                                       ImpactDetectorConfig detector_config)
    : source_factory_(std::move(source_factory)),
      sample_rate_hz_(sample_rate_hz),
      impact_handler_(std::move(impact_handler)),
      detector_config_(detector_config),
      status_(std::make_shared<SharedStatus>()) {
  if (!source_factory_) {
    throw std::invalid_argument("audio impact monitor requires a source factory");
  }
  if (sample_rate_hz_ == 0) {
    throw std::invalid_argument("audio impact monitor sample rate must be positive");
  }
  if (!impact_handler_) {
    throw std::invalid_argument("audio impact monitor requires an impact handler");
  }
}

AudioImpactMonitor::~AudioImpactMonitor() { Stop(); }

void AudioImpactMonitor::Start() {
  if (worker_.joinable()) {
    throw std::logic_error("audio impact monitor is already running");
  }
  {
    const std::scoped_lock lock(status_->mutex);
    status_->value = {
        .running = true,
        .source_ready = false,
        .completed_blocks = 0,
        .completed_samples = 0,
        .detected_impacts = 0,
        .noise_floor = 0.0F,
        .detection_threshold = 0.0F,
        .error = {},
    };
  }
  auto startup = std::make_shared<std::promise<void>>();
  std::future<void> ready = startup->get_future();
  worker_ = std::jthread(&AudioImpactMonitor::Run, status_, source_factory_, sample_rate_hz_,
                         impact_handler_, detector_config_, startup);
  try {
    ready.get();
  } catch (...) {
    worker_.join();
    throw;
  }
}

void AudioImpactMonitor::RequestStop() noexcept {
  if (!worker_.joinable()) {
    return;
  }
  worker_.request_stop();
}

void AudioImpactMonitor::Stop() noexcept {
  RequestStop();
  if (!worker_.joinable()) {
    return;
  }
  worker_.join();
}

AudioImpactMonitorStatus AudioImpactMonitor::Status() const {
  const std::scoped_lock lock(status_->mutex);
  return status_->value;
}

void AudioImpactMonitor::Run(const std::stop_token &stop_token,
                             const std::shared_ptr<SharedStatus> &status,
                             const AudioCaptureSourceFactory &source_factory,
                             std::uint32_t sample_rate_hz, const ImpactHandler &impact_handler,
                             ImpactDetectorConfig detector_config,
                             const std::shared_ptr<std::promise<void>> &startup) noexcept {
  std::unique_ptr<AudioCaptureSource> source;
  try {
    source = StartSource(status, source_factory, startup);
    CaptureBlocks(stop_token, status, source.get(), sample_rate_hz, impact_handler,
                  detector_config);
    source->Stop();
    const std::scoped_lock lock(status->mutex);
    status->value.running = false;
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    if (source != nullptr) {
      source->Stop();
    }
    RecordFailure(status, ExceptionMessage());
    bool source_ready = false;
    {
      const std::scoped_lock lock(status->mutex);
      source_ready = status->value.source_ready;
    }
    if (!source_ready) {
      try {
        startup->set_exception(failure);
      } catch (const std::future_error &) {
        std::terminate();
      }
    }
  }
}

std::unique_ptr<AudioCaptureSource> AudioImpactMonitor::StartSource(
    const std::shared_ptr<SharedStatus> &status, const AudioCaptureSourceFactory &source_factory,
    const std::shared_ptr<std::promise<void>> &startup) {
  std::unique_ptr<AudioCaptureSource> source = source_factory();
  if (source == nullptr) {
    throw std::runtime_error("audio capture source factory returned null");
  }
  std::string start_error;
  if (!source->Start(&start_error)) {
    throw std::runtime_error(start_error.empty() ? "audio capture source failed to start"
                                                 : start_error);
  }
  {
    const std::scoped_lock lock(status->mutex);
    status->value.source_ready = true;
  }
  startup->set_value();
  return source;
}

void AudioImpactMonitor::CaptureBlocks(const std::stop_token &stop_token,
                                       const std::shared_ptr<SharedStatus> &status,
                                       AudioCaptureSource *source, std::uint32_t sample_rate_hz,
                                       const ImpactHandler &impact_handler,
                                       ImpactDetectorConfig detector_config) {
  ImpactDetector detector(detector_config);
  std::uint64_t expected_first_frame = 0;
  std::array<ImpactEvent, 8> impacts;
  while (!stop_token.stop_requested()) {
    PcmReadResult read = source->ReadBlock();
    if (read.status == PcmReadStatus::kEndOfStream) {
      throw std::runtime_error("audio capture source ended while the monitor was armed");
    }
    if (read.status == PcmReadStatus::kError) {
      throw std::runtime_error(read.message.empty() ? "audio capture source read failed"
                                                    : read.message);
    }
    if (read.block.samples.empty()) {
      throw std::runtime_error("audio capture source returned an empty block");
    }
    if (read.block.first_frame_index != expected_first_frame) {
      throw std::runtime_error("audio capture source returned discontinuous frame indices");
    }
    expected_first_frame += read.block.samples.size();
    const ImpactProcessResult result = detector.ProcessBlock(
        read.block.samples, read.block.estimated_start_time, sample_rate_hz, impacts);
    for (std::size_t index = 0; index < result.events_written; ++index) {
      impact_handler(impacts[index]);
    }
    const std::scoped_lock lock(status->mutex);
    ++status->value.completed_blocks;
    status->value.completed_samples = expected_first_frame;
    status->value.detected_impacts += result.events_detected;
    status->value.noise_floor = detector.noise_floor();
    status->value.detection_threshold = detector.detection_threshold();
  }
}

void AudioImpactMonitor::RecordFailure(const std::shared_ptr<SharedStatus> &status,
                                       std::string message) {
  const std::scoped_lock lock(status->mutex);
  status->value.error = std::move(message);
  status->value.running = false;
}

}  // namespace swing_capture::application
