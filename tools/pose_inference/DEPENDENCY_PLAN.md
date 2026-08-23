# Host pose-inference dependencies

The host prototype is hermetic through Bazel:

- `MODULE.bazel` resolves the hash-locked `mediapipe==0.10.35` wheel graph with
  the repository's Python 3.11 toolchain;
- the official MediaPipe Pose Landmarker Lite, Full, and Heavy float16
  revision-1 models are `http_file` repositories with pinned integrity values;
  and
- `//tools/pose_inference:pose_landmarker` resolves Lite from Bazel runfiles by
  default. `//tools/pose_inference:evaluate_corpus` accepts the closed
  `--model-variant=lite|full|heavy` comparison option and resolves the selected
  official asset from its runfiles. Neither path requires system `pip` or an
  ad-hoc model path.

The Lite model is 5,777,746 bytes with SHA-256
`59929e1d1ee95287735ddd833b19cf4ac46d29bc7afddbbf6753c459690d574a`.
The selected Linux MediaPipe wheel has SHA-256
`db9a579df48cffe9570cd3e93f6a5d2dd089a1103b846c60c5b5de8a21c38db0`.

Update and verify the lock only through Bazel:

```bash
bazel run //tools/pose_inference:requirements.update
bazel test //tools/pose_inference:requirements.test
```

The runtime samples at exact 200 ms deadlines in MediaPipe `VIDEO` mode and
emits versioned NDJSON with both normalized and world landmarks. The OpenCV
adapter does not seek: it first scans decoded presentation timestamps to derive
the half-open duration, reopens the input, and then decodes forward to the first
frame at or after each deadline. `timestamp_ms` retains the requested deadline,
while `source_timestamp_ms` and the MediaPipe tracking call use the selected
frame's decoded presentation timestamp.

This path supports variable-frame-rate evaluation without adding another
native dependency. It rejects absent, negative, non-finite, non-increasing, or
sub-millisecond-colliding timestamps instead of silently synthesizing a
constant-rate timebase. A backward `VideoReader.read_at` call reopens and
decodes forward rather than relying on backend-specific keyframe seek landing.
Consequently, a codec/backend that does not expose usable
`CAP_PROP_POS_MSEC` values fails with an actionable error and must not be used
for evaluation evidence.
