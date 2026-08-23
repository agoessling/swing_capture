# Two-phone audio consensus experiment

This package evaluates online two-phone impact coordination against the frozen
field benchmark in `capture/offline/experiments/BENCHMARK.md`. It keeps every
production detector candidate, maps both WAVs and ATL video onto the measured
ATL-WAV axis, and reports candidate generation, armed terminal behavior, and
the complete pose/capture lifecycle separately.

The lifecycle replay ports the production pose-controller thresholds and event
ordering: pose is blind while Camera2 is in high-speed mode, an accepted event
completes after one second, camera restart takes 800 ms, reset/cooldown gates
rearm, an untriggered attempt expires after 15 seconds, and a retained clip has
a three-second encoded history. Detector state is reset at each dynamic arm.

Policies are deliberately online and bounded:

- `leader_immediate` accepts the leader's first post-readiness event;
- `hard_consensus` skips an event unless a peer candidate arrives within the
  declared wait and 80 ms pairing tolerance; and
- `soft_bounded` lets a corroborated event supersede the first event until the
  deadline, then accepts the original event.

The shadow comparison reproduces the current latest-candidate-within-250-ms
rule and compares it with a short candidate ring. The ring waits a bounded
interval after request arrival and chooses the candidate nearest the conveyed
leader strike; normalized peak/threshold strength breaks an exact timing tie.
That rule requires an estimated cross-phone monotonic-clock offset and
uncertainty. Request arrival by itself contains unknown network delay and is
not a safe proxy for strike time.

Build and run:

```bash
bazel test //capture/offline/experiments/consensus:consensus_evaluator_test
bazel build //capture/offline/experiments/consensus:consensus_evaluator

bazel-bin/capture/offline/experiments/consensus/consensus_evaluator \
  "$DATA/down_the_line_pixel5a_audio.wav" \
  "$DATA/face_on_pixel6_audio.wav" \
  "$DATA/pose_replay/target_swing_index.json" \
  "$DATA/pose_replay/full_frame_roi/face_on/session_replay_roi_free.json" \
  "$DATA/production_impact_candidates.json" \
  "$DATA/experiments/envelope/face_on.json" \
  "$DATA/pose_replay/full_frame_roi/face_on/observations.csv" \
  "$DATA/experiments/consensus/report.json"
```

`robust_hp120_x12` is also replayed as a conservative envelope comparison. The
experiment intentionally does not consume the development-tuned `x20_hf40`
configuration.
