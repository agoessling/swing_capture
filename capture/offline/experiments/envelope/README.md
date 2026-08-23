# Adaptive-envelope impact experiment

This package evaluates a cheap streaming transient detector against the frozen
field benchmark in `../BENCHMARK.md`. It is deliberately isolated from the
Android detector: none of these development-set thresholds are production
defaults.

The candidate path applies a one-pole high-pass, then compares a 1.5 ms RMS
window with the median RMS of 10 ms blocks from the preceding 240 ms. It also
measures the fast/prior rise ratio, crest factor, and normalized
first-difference energy as a high-frequency proxy. Qualifying frames are
clustered with a bounded 20 ms gap; the strongest frame supplies the retained
strike timestamp. Reports include the cluster's actual decision deadline, not
just the backdated strike time.

The CLI scans the complete WAV for candidate-generation metrics, then uses the
frozen pose-arm windows plus the 2.45 second trigger-readiness delay for terminal
scoring through the shared benchmark scorer:

```bash
bazel run //capture/offline/experiments/envelope:envelope_experiment -- \
  artifacts/field_recording_95482d93-f024-400e-9532-5ba7082de05e/face_on_pixel6_audio.wav \
  artifacts/field_recording_95482d93-f024-400e-9532-5ba7082de05e/face_on_production_audio_windows.json
```

`ExperimentConfigs()` contains every exact configuration in the sweep. The
full JSON retains every candidate and all terminal-window outcomes. The concise
field result lives beside the ignored recording artifacts under
`experiments/envelope/RESULT.md`.

This is an in-sample hypothesis-ranking experiment. The phone, camera view,
mount position, golfer, clubs, room, and acoustics are all confounded. A
phone-swap and negative-rich holdout recording are required before changing the
production detector.
