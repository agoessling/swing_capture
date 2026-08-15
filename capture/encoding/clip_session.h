#ifndef SWING_CAPTURE_CAPTURE_ENCODING_CLIP_SESSION_H_
#define SWING_CAPTURE_CAPTURE_ENCODING_CLIP_SESSION_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "capture/core/camera_source.h"

namespace swing_capture::encoding {

struct ClipFrameInput {
  FrameView frame;

  // Source frame time after mapping this camera's device clock to the host
  // monotonic clock, expressed relative to the audio impact. This remains
  // independent for each free-running camera.
  std::chrono::nanoseconds time_from_impact{};
};

struct CameraClipInput {
  std::string_view role;
  std::string_view camera_serial;
  std::string_view pixel_format;
  std::span<const ClipFrameInput> frames;
};

struct ClipTriggerMetadata {
  std::string_view source;
  std::int64_t host_monotonic_time_ns = 0;
  std::optional<std::int64_t> confirmation_host_monotonic_time_ns = std::nullopt;
  std::optional<std::uint32_t> sample_rate_hz = std::nullopt;
  std::optional<double> peak_amplitude = std::nullopt;
  std::optional<double> noise_floor = std::nullopt;
  std::optional<double> threshold = std::nullopt;
};

struct SyntheticSwingHilViewEvidence {
  std::string_view role;
  std::size_t optical_white_impact_frame_index = 0;
  // The signed correction added to this camera's mapped host timestamps and
  // the conservative residual interval learned from the brightness sweep.
  std::int64_t mapped_time_correction_us = 0;
  std::uint64_t schedule_uncertainty_us = 0;
  // Positive means the audio trigger estimate is later than the optically
  // observed white-impact frame. It remains a diagnostic until ALSA latency
  // is calibrated.
  std::int64_t audio_trigger_estimate_offset_us = 0;
  bool optical_white_passed = false;
  std::size_t stable_frame_count = 0;
  std::size_t matching_frame_count = 0;
  double matching_fraction = 0.0;
  double mean_signal_delta = 0.0;
  double mean_expected_color_distance = 0.0;
  double maximum_saturated_fraction = 0.0;
  double maximum_bloom_fraction = 0.0;
  double exposure_us = 0.0;
  double gain_db = 0.0;
};

struct SyntheticSwingHilEvidence {
  std::uint32_t selected_brightness = 0;
  std::uint32_t step_duration_us = 0;
  std::uint32_t pre_impact_step_count = 0;
  std::uint32_t white_impact_duration_us = 0;
  std::uint32_t post_impact_step_count = 0;
  std::uint32_t tone_duration_us = 0;
  std::uint32_t tone_frequency_hz = 0;
  std::array<SyntheticSwingHilViewEvidence, 2> views;
};

// Capture-side durations measured before publication begins. Signed durations
// are used deliberately: the provisional audio acceptance timeline can lead
// its later host confirmation, and freeze schedule lateness is negative when
// the freeze begins early.
struct ClipCapturePipelineProfile {
  std::chrono::steady_clock::duration trigger_estimate_to_confirmation{};
  std::chrono::steady_clock::duration confirmation_to_acceptance{};
  std::chrono::steady_clock::duration acceptance_to_freeze_start{};
  std::chrono::steady_clock::duration freeze_schedule_lateness{};
  std::chrono::steady_clock::duration freeze_and_rotate{};
  std::chrono::steady_clock::duration audio_stop{};
  // Measured after capture completion and before ClipSessionPublisher begins;
  // this includes synthetic-HIL optical analysis when present.
  std::chrono::steady_clock::duration prepublication_analysis{};
  // Measured by ClipSessionPublisher before entering WriteClipSession, but
  // serialized with the publication/session work rather than capture stages.
  std::chrono::steady_clock::duration publisher_planning{};
  std::chrono::steady_clock::duration impact_preview_render{};
  std::chrono::steady_clock::duration impact_preview_ready_after_confirmation{};
};

struct ClipViewPipelineProfile {
  std::string role;
  std::size_t frame_count = 0;
  double timeline_ms = 0.0;
  double bayer_fit_demosaic_ms = 0.0;
  double rgb_to_yuv420_ms = 0.0;
  double codec_encode_ms = 0.0;
  double webm_mux_ms = 0.0;
  double finalize_ms = 0.0;
  double output_verification_ms = 0.0;
  double total_ms = 0.0;
};

struct ClipSessionPipelineProfile {
  double prepublication_analysis_ms = 0.0;
  double publisher_planning_ms = 0.0;
  double impact_preview_render_ms = 0.0;
  double impact_preview_ready_after_confirmation_ms = 0.0;
  double validation_and_timeline_ms = 0.0;
  double output_setup_ms = 0.0;
  double media_encoding_wall_ms = 0.0;
  double frame_metadata_ms = 0.0;
  double profile_snapshot_after_confirmation_ms = 0.0;
  std::int64_t profile_snapshot_host_monotonic_ns = 0;
};

struct ClipPipelineProfile {
  static constexpr std::uint32_t kSchemaVersion = 3;

  ClipCapturePipelineProfile capture;
  ClipSessionPipelineProfile session;
  std::array<ClipViewPipelineProfile, 2> views;
};

struct DualViewClipInput {
  std::string_view session_id;
  std::string_view created_at_utc;
  ClipTriggerMetadata trigger;
  std::array<CameraClipInput, 2> views;

  // Clock-correlation diagnostic only, not exposure synchronization proof.
  std::optional<std::chrono::nanoseconds> mapped_nearest_frame_skew;
  std::optional<SyntheticSwingHilEvidence> hil_evidence;
  // Supplying capture timing enables the optional root pipeline_profile. A
  // confirmed trigger timestamp is required so publication can snapshot its
  // elapsed time on the same host steady-clock timeline.
  std::optional<ClipCapturePipelineProfile> capture_pipeline_profile;
};

struct ClipMediaEncoderCapabilities {
  std::string encoder_id;
  std::string file_extension;
  std::string mime_type;
  std::string codec;
  bool hardware_accelerated = false;
  bool deterministic = false;
  bool all_frames_keyframes = false;
};

struct EncodedClipMedia {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t frame_count = 0;
  std::size_t keyframe_count = 0;
  std::uintmax_t encoded_bytes = 0;
  ClipViewPipelineProfile pipeline_profile;
};

struct WebmInspection {
  std::string codec;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t frame_count = 0;
  std::size_t keyframe_count = 0;
  std::chrono::nanoseconds first_frame_time{};
  std::chrono::nanoseconds last_frame_time{};
};

// Fully parses one published WebM using libwebm and counts blocks/keyframes.
// This is shared by the software encoder's post-write verification and short
// HIL artifact checks; it throws on malformed, multi-track, or unsupported
// media. The current application contract accepts VP8 and VP9 video-only
// WebM assets.
[[nodiscard]] WebmInspection InspectWebm(const std::filesystem::path &path);

// Compatibility spelling retained for existing fixture-oriented callers. It
// additionally requires that the parsed WebM codec is VP8.
[[nodiscard]] WebmInspection InspectVp8Webm(const std::filesystem::path &path);

// Codec/container capability boundary. Implementations receive immutable
// selected frames and write one browser media asset. Encode must be safe to
// call concurrently for the two camera views, and must never retain a frame
// payload after it returns.
class ClipMediaEncoder {
 public:
  ClipMediaEncoder() = default;
  virtual ~ClipMediaEncoder() = default;

  ClipMediaEncoder(const ClipMediaEncoder &) = delete;
  ClipMediaEncoder &operator=(const ClipMediaEncoder &) = delete;
  ClipMediaEncoder(ClipMediaEncoder &&) = delete;
  ClipMediaEncoder &operator=(ClipMediaEncoder &&) = delete;

  [[nodiscard]] virtual ClipMediaEncoderCapabilities capabilities() const = 0;
  [[nodiscard]] virtual EncodedClipMedia Encode(const CameraClipInput &input,
                                                const std::filesystem::path &output_path) const = 0;
};

struct SoftwareVp8WebmOptions {
  // Encoding is deliberately bounded independently of the raw capture size so
  // a dual-view 1.5 second clip can be published within the short HIL loop.
  std::uint32_t maximum_width = 640;
  std::uint32_t maximum_height = 480;
  std::uint32_t target_bitrate_kbps = 35'000;
  unsigned int minimum_quantizer = 4;
  unsigned int maximum_quantizer = 30;
  // BEST_QUALITY avoids libvpx's wall-time-dependent speed adaptation. The
  // software path is for deterministic fixtures, so throughput is secondary.
  int cpu_used = 0;
};

// Hermetic CPU BayerRG8 -> RGB -> fitted I420 -> all-intra VP8/WebM encoder.
// libvpx is intentionally single-threaded here so identical input produces
// identical media bytes. Production uses the VA-API backend below; this path
// remains the deterministic fixture and injected-test implementation.
[[nodiscard]] std::unique_ptr<ClipMediaEncoder> MakeSoftwareVp8WebmEncoder(
    SoftwareVp8WebmOptions options = {});

struct VaapiVp9WebmOptions {
  std::filesystem::path render_node = "/dev/dri/renderD128";
  std::uint32_t maximum_width = 640;
  std::uint32_t maximum_height = 480;
  // Frame preparation is CPU-bound and independent for all-intra output.
  // Workers prepare a bounded look-ahead queue while VA-API encodes the
  // preceding frame, keeping both the CPU and GPU active without retaining a
  // second full clip in converted form.
  std::size_t preprocessing_threads = 4;
  // Independent all-intra frames can be queued on separate VA surfaces. This
  // avoids a per-frame GPU round trip while keeping output completion ordered.
  std::size_t encoding_queue_depth = 4;
  // VA-API VP9 constant-quality index. Intel iHD accepts [0, 255]; 24 gives
  // retained-clip size and visual quality close to the former software VP8
  // path on this station while remaining substantially faster.
  std::uint32_t quality_index = 24;
};

// Direct BayerRG8 -> RGB -> NV12 -> Intel VA-API VP9/WebM encoder. It uses an
// all-keyframe stream so exact browser frame stepping remains independent of
// predictive decode state. Construction is side-effect free; device/runtime
// availability is checked when Encode begins.
[[nodiscard]] std::unique_ptr<ClipMediaEncoder> MakeVaapiVp9WebmEncoder(
    VaapiVp9WebmOptions options = {});

struct ClipSessionWriteResult {
  std::filesystem::path session_directory;
  std::filesystem::path manifest_path;
  std::array<std::filesystem::path, 2> media_paths;
  std::optional<ClipPipelineProfile> pipeline_profile;
};

// Writes both media files and manifest into a private sibling directory, then
// atomically renames that directory to <output_parent>/<session_id>. The final
// path is never visible partially populated and an existing session is never
// overwritten. This synchronous operation is intended to run on an
// application-owned asynchronous encoding worker while frozen frame handles
// keep the input payloads alive.
[[nodiscard]] ClipSessionWriteResult WriteClipSession(const std::filesystem::path &output_parent,
                                                      const DualViewClipInput &input,
                                                      const ClipMediaEncoder &encoder);

}  // namespace swing_capture::encoding

#endif  // SWING_CAPTURE_CAPTURE_ENCODING_CLIP_SESSION_H_
