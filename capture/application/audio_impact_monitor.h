#ifndef SWING_CAPTURE_CAPTURE_APPLICATION_AUDIO_IMPACT_MONITOR_H_
#define SWING_CAPTURE_CAPTURE_APPLICATION_AUDIO_IMPACT_MONITOR_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include "capture/audio/audio_capture_session.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture::application {

using ImpactHandler = std::function<void(const ImpactEvent &)>;

struct AudioImpactMonitorStatus {
  bool running = false;
  bool source_ready = false;
  std::uint64_t completed_blocks = 0;
  std::uint64_t completed_samples = 0;
  std::uint64_t detected_impacts = 0;
  float noise_floor = 0.0F;
  float detection_threshold = 0.0F;
  std::string error;
};

// Continuously reads bounded PCM blocks and runs the allocation-free impact
// detector. The source and callback are both invoked on the monitor thread.
// The callback must therefore enqueue work rather than perform capture freeze,
// encoding, disk I/O, or HTTP work itself.
class AudioImpactMonitor final {
 public:
  AudioImpactMonitor(AudioCaptureSourceFactory source_factory, std::uint32_t sample_rate_hz,
                     ImpactHandler impact_handler, ImpactDetectorConfig detector_config = {});
  ~AudioImpactMonitor();

  AudioImpactMonitor(const AudioImpactMonitor &) = delete;
  AudioImpactMonitor &operator=(const AudioImpactMonitor &) = delete;
  AudioImpactMonitor(AudioImpactMonitor &&) = delete;
  AudioImpactMonitor &operator=(AudioImpactMonitor &&) = delete;

  void Start();
  void Stop() noexcept;
  [[nodiscard]] AudioImpactMonitorStatus Status() const;

 private:
  struct SharedStatus;

  static void Run(const std::stop_token &stop_token, const std::shared_ptr<SharedStatus> &status,
                  const AudioCaptureSourceFactory &source_factory, std::uint32_t sample_rate_hz,
                  const ImpactHandler &impact_handler, ImpactDetectorConfig detector_config,
                  const std::shared_ptr<std::promise<void>> &startup) noexcept;
  static std::unique_ptr<AudioCaptureSource> StartSource(
      const std::shared_ptr<SharedStatus> &status, const AudioCaptureSourceFactory &source_factory,
      const std::shared_ptr<std::promise<void>> &startup);
  static void CaptureBlocks(const std::stop_token &stop_token,
                            const std::shared_ptr<SharedStatus> &status, AudioCaptureSource *source,
                            std::uint32_t sample_rate_hz, const ImpactHandler &impact_handler,
                            ImpactDetectorConfig detector_config);
  static void RecordFailure(const std::shared_ptr<SharedStatus> &status, std::string message);

  AudioCaptureSourceFactory source_factory_;
  std::uint32_t sample_rate_hz_;
  ImpactHandler impact_handler_;
  ImpactDetectorConfig detector_config_;
  std::shared_ptr<SharedStatus> status_;
  std::jthread worker_;
};

}  // namespace swing_capture::application

#endif  // SWING_CAPTURE_CAPTURE_APPLICATION_AUDIO_IMPACT_MONITOR_H_
