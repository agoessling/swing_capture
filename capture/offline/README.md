# Field recording analyzer

This prototype indexes high-recall adaptive audio-impact candidates in a paired normal-rate field
recording. It estimates the difference between the two recording time axes from common impulses,
emits shot-centered windows for paired candidates, and retains every unmatched local candidate for
later tuning. False positives are intentionally preferred over silently missing a real strike.

```bash
bazel run //capture/offline:field_recording_analyzer -- \
  --down-the-line-manifest path/to/down_the_line_manifest.json \
  --down-the-line-wav path/to/down_the_line_audio.wav \
  --face-on-manifest path/to/face_on_manifest.json \
  --face-on-wav path/to/face_on_audio.wav \
  --output path/to/impact_index.json
```

When `--output` is set, a companion CSV with stable `P001`, `P002`, ... paired-candidate IDs is
written next to it. Use `--csv-output` to select a different CSV path. These high-recall impulses
are not declared swings; `S01`, `S02`, ... IDs are reserved for candidates confirmed by video/pose.

The default window is three seconds before through two seconds after each candidate.
`paired_candidates` holds cross-phone impulse matches; `unmatched_candidates` lists one-phone
events separately, and `streams.*.candidates` preserves the complete local detector output. These
are deliberately not called swings. The analyzer only uses audio today; pose/video confirmation
must select and assign `S01`, `S02`, ... to real swings.

Use `--production-detector` to replay the current Pixel 5a and Pixel 6 detector
policies instead of the deliberately sensitive indexing profile. For the more
important terminal behavior, `audio_trigger_window_analyzer` resets the
detector at every supplied arm time and scores the first candidate, matching a
real high-speed attempt:

```bash
bazel run //capture/offline:audio_trigger_window_analyzer -- \
  --wav path/to/audio.wav \
  --starts-ms 9208,38398,62401 \
  --targets-ms 19260,45020,67510 \
  --device pixel6 \
  --output path/to/window_report.json
```

Full-session pose replay is similarly available through
`//android/core/pose:pose_field_session_replay`. It models the periods when
Camera2 high-speed capture makes 5 Hz pose observations unavailable, separates
first-video-frame readiness from full audio/pre-roll readiness, and includes
post-impact completion plus camera restart time.

For a new field session, first use
`//tools/field_evidence:field_evidence` to hash-pin the paired manifests, video,
and audio into an explicitly unreviewed skeleton. Its readiness audit requires
complete per-view timeline review, stream-local lifecycle labels, the full
negative taxonomy, and a phone/view swap before either pose or audio evidence
can be called a holdout. See `tools/field_evidence/README.md`.
