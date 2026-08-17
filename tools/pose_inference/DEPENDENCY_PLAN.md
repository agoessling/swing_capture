# Host pose-inference dependencies

The host prototype is hermetic through Bazel:

- `MODULE.bazel` resolves the hash-locked `mediapipe==0.10.35` wheel graph with
  the repository's Python 3.11 toolchain;
- the official MediaPipe Pose Landmarker Lite float16 revision-1 model is an
  `http_file` with a pinned integrity value; and
- `//tools/pose_inference:pose_landmarker` resolves that model from Bazel
  runfiles by default. It does not require system `pip` or a user-supplied model.

The model is 5,777,746 bytes with SHA-256
`59929e1d1ee95287735ddd833b19cf4ac46d29bc7afddbbf6753c459690d574a`.
The selected Linux MediaPipe wheel has SHA-256
`db9a579df48cffe9570cd3e93f6a5d2dd089a1103b846c60c5b5de8a21c38db0`.

Update and verify the lock only through Bazel:

```bash
bazel run //tools/pose_inference:requirements.update
bazel test //tools/pose_inference:requirements.test
```

The runtime samples exactly every 200 ms in MediaPipe `VIDEO` mode and emits
versioned NDJSON with both normalized and world landmarks. OpenCV seeking is
isolated behind `VideoReader` and is sufficient for the constant-frame-rate
prototype clips. Variable-frame-rate evaluation should replace that adapter
with a timestamp-preserving decoder without changing the downstream contracts.
