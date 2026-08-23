# On-device pose inference foundation

This package is intentionally independent of the capture service. It provides:

- a pull-based `PoseFrameSource` and a MediaCodec app-private clip replay source;
- a deterministic five-Hz, latest-frame-wins inference pipeline;
- bounded metrics and NDJSON traces integrated into standby diagnostic archives;
- MediaPipe Pose Landmarker Lite with explicit CPU/GPU policy; and
- a synchronous Camera2 input boundary that converts YUV into one reusable direct RGB buffer.

`PoseReplayHilRunner` runs an app-private MP4 through the same MediaPipe landmarker, landmark
feature extractor, and trigger controller used by production. `runAsync` accepts the app's executor;
`runAndPersistAsync` accepts a small writer callback, so callers can pass
`json -> ReportStore.writeLatest(context, json)` without introducing an app-package dependency.
The schema-v1 `PoseReplayReport` is capped at 1,200 trace frames and validated by the public
`PoseReplayReportValidator` contract before serialization.

MediaPipe Tasks Vision 0.10.35 recognizes YUV metadata in `MediaImageBuilder`, but its Android
packet creator accepts only `PixelFormat.RGBA_8888` media images. Camera2 exposes this standby
stream as `YUV_420_888`, so passing the camera image directly fails before inference. Production
instead converts each accepted five-Hz frame in one pass into a fixed-size reusable direct RGB
buffer with MediaPipe's required four-byte row alignment and presents it through
`ByteBufferImageBuilder`; this saves one channel relative to RGBA and the GPU delegate remains
enabled on the Pixel 6. The pinned JNI validates the exact direct-buffer capacity and copies the
pixels synchronously, so the same buffer is safe to reuse after `detectForVideo` returns. The caller
retains ownership of the Camera2 `Image`; the transient `MPImage` does not own or close it. Replay
frames are owned by the source and their pixel arrays are read-only to consumers.

Pixel 5a and Pixel 6 use this same arm64 model, stride-aware YUV-to-RGB direct-buffer path, five-Hz
scheduler, and trigger controller. There is deliberately no device-model compatibility branch. The
default `GPU_PREFERRED` policy probes the GPU independently on each phone and records the delegate
that was actually selected; a Pixel 5a fallback therefore cannot force the Pixel 6 onto the CPU.
Operators can require the GPU when qualification evidence shows that silent fallback is
unacceptable. MediaPipe 0.10.35 also exposes its experimental NPU delegate, so `NPU_PREFERRED` and
`NPU_REQUIRED` are available for per-device qualification. NPU-preferred falls back to GPU and then
CPU on that phone only. The Lite float16 model and 640x360 camera input remain the lowest-cost
production configuration; delegate comparison therefore does not silently change model accuracy or
the Pixel 6 input path.

## Pinned upstream components

| Component | Exact version | License | Source |
| --- | --- | --- | --- |
| MediaPipe Tasks Vision/Core for Android | 0.10.35 | Apache-2.0 | Google Maven |
| Pose Landmarker Lite float16 model bundle | 1 | Apache-2.0 | Google MediaPipe model storage |
| Pose Landmarker Full float16 model bundle | 1 | Apache-2.0 | Google MediaPipe model storage |
| Pose Landmarker Heavy float16 model bundle | 1 | Apache-2.0 | Google MediaPipe model storage |
| rules_jvm_external | 7.1 | Apache-2.0 | Bazel Central Registry |
| Android NDK toolchain | r25c (API 34) | Android SDK License | Hermetic Android toolchains |

`//third_party/mediapipe:artifacts.lock.json` records every transitive Maven coordinate, exact
version, URL, and SHA-256 checksum. The only direct Maven coordinate is
`com.google.mediapipe:tasks-vision:0.10.35`; all other entries are its published transitive graph.
Each model bundle has a separate SHA-256-integrity-pinned `http_file` declaration in
`MODULE.bazel`.

The manual `//android/pose_inference:pose_inference_packaging_fixture` APK exists only to verify
that the AAR's resources/native libraries and all three closed pose-model asset names survive
Bazel packaging. The ordinary `//android/app:swing_capture` APK contains only Lite. Full and Heavy
are isolated in `//android/app:swing_capture_pose_experiment`, which is installed only by the
manual pose-standby experiment target; model comparison therefore adds no production APK size,
install-time, storage, or runtime cost.
The repository's default build configuration selects
`//android/pose_inference:android_arm64`; without an explicit Android platform, rules_android would
select the host CPU's AAR native library.
