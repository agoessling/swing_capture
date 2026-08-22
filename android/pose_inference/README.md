# On-device pose inference foundation

This package is intentionally independent of the capture service. It provides:

- a pull-based `PoseFrameSource` and a MediaCodec app-private clip replay source;
- a deterministic five-Hz, latest-frame-wins inference pipeline;
- bounded metrics and NDJSON traces for later diagnostic-archive integration;
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
scheduler, and trigger controller. There is deliberately no device-model compatibility branch. The default
`GPU_PREFERRED` policy probes the GPU independently on each phone and records the delegate that was
actually selected; a Pixel 5a fallback therefore cannot force the Pixel 6 onto the CPU. Operators
can require the GPU when qualification evidence shows that silent fallback is unacceptable.

## Pinned upstream components

| Component | Exact version | License | Source |
| --- | --- | --- | --- |
| MediaPipe Tasks Vision/Core for Android | 0.10.35 | Apache-2.0 | Google Maven |
| Pose Landmarker Lite float16 model bundle | 1 | Apache-2.0 | Google MediaPipe model storage |
| rules_jvm_external | 7.1 | Apache-2.0 | Bazel Central Registry |
| Android NDK toolchain | r25c (API 34) | Android SDK License | Hermetic Android toolchains |

`//third_party/mediapipe:artifacts.lock.json` records every transitive Maven coordinate, exact
version, URL, and SHA-256 checksum. The only direct Maven coordinate is
`com.google.mediapipe:tasks-vision:0.10.35`; all other entries are its published transitive graph.
The model has a separate SHA-256-integrity-pinned `http_file` declaration in `MODULE.bazel`.

The manual `//android/pose_inference:pose_inference_packaging_fixture` APK exists only to verify
that the AAR's resources/native libraries and `pose_landmarker_lite.task` survive Bazel packaging.
The repository's default build configuration selects
`//android/pose_inference:android_arm64`; without an explicit Android platform, rules_android would
select the host CPU's AAR native library.
