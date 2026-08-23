# Paired field-evidence contract

`//tools/field_evidence:field_evidence` turns a raw, coordinated normal-rate
recording into a hash-pinned annotation skeleton and audits whether reviewed
evidence is actually sufficient for pose or audio selection. It does not infer
ground truth from detector candidates, filenames, or the existence of media.

Before the first holdout recording, emit the acquisition checklist from the
development phone assignment:

```bash
bazel run //tools/field_evidence:field_evidence -- plan \
  --development-down-the-line-device=pixel5a \
  --development-face-on-device=pixel6 \
  --output=/absolute/path/to/corpus/next_collection_plan.json
```

The plan requires the reverse phone/view assignment and enumerates every pose
lifecycle, audio hard negative, quiet impact, and diversity stratum still
needed. Readiness policy `field_readiness_v2` requires at least two reviewed
occurrences of every required category in at least two distinct, fully reviewed
phone/view-swapped sessions; quiet real impacts have the same two-occurrence,
two-session minimum. Its ordered `scenario_sequence` assigns stable `F01`-style slates and concrete operator
instructions, while `operator_protocol` requires continuous dual-phone recording, neutral gaps,
retention of failed/unexpected takes, and complete later review. This makes omissions visible before
leaving the hitting area without treating a detector candidate or scenario name as ground truth. It
contains no inferred labels. Each scenario reports both the additional reviewed
occurrences and additional distinct sessions still required, so repeating a
hard negative twice in one recording cannot manufacture cross-session coverage.

Create the skeleton immediately after collecting a pair:

```bash
bazel run //tools/field_evidence:field_evidence -- create \
  --corpus-id=field-holdout-2026 \
  --development-recording-id=field-95482d93-f024-400e-9532-5ba7082de05e \
  --development-down-the-line-device=pixel5a \
  --development-face-on-device=pixel6 \
  --down-the-line-device=pixel6 \
  --face-on-device=pixel5a \
  --golfer-label=golfer-a \
  --environment-label=studio-a \
  --lighting-label=artificial-a \
  --framing-label=portrait-tripod-a \
  --media-root=/absolute/path/to/corpus \
  --down-the-line-manifest=/absolute/path/to/corpus/session/dtl_manifest.json \
  --down-the-line-video=/absolute/path/to/corpus/session/dtl_video.mp4 \
  --down-the-line-audio=/absolute/path/to/corpus/session/dtl_audio.wav \
  --face-on-manifest=/absolute/path/to/corpus/session/atl_manifest.json \
  --face-on-video=/absolute/path/to/corpus/session/atl_video.mp4 \
  --face-on-audio=/absolute/path/to/corpus/session/atl_audio.wav \
  --output=/absolute/path/to/corpus/evidence_manifest.json
```

The command verifies the two source manifests share one recording ID, checks
their roles, node identities, duration and media byte counts, and pins all six
files by SHA-256. Its output deliberately has `review.state=needs_review`, no
reviewed intervals, and no episodes. A recording is not labeled merely because
an adaptive detector found an impulse.

For later sessions, pass `--append-to` with the prior corpus manifest. The tool
requires the corpus ID, development recording IDs, and development device
assignment to match exactly, rejects duplicate recording IDs or reused assets,
and re-verifies every declared asset beneath the same media root before writing
the combined document.

## Annotation contract

Review each local video/audio timeline from start to finish. `reviewed_intervals`
must tile `[0, duration_ms)` with no gaps before `review.state` becomes
`complete`; record a reviewer and a timezone-aware RFC 3339 review time (stored
canonically in UTC). Every semantic episode has
the same ID and category in both views but separate stream-local times. Episodes must be
chronological and nonoverlapping independently on each phone timeline, so one interval cannot
satisfy multiple categories. This avoids silently treating phone start-time skew as ground truth.

The closed episode vocabulary includes the complete-lifecycle pose cases
(`real_swing`, `aborted_address`, `address_no_swing`, `clear_and_rearm`,
`practice_swing`, `empty_scene`, `walk_through`, `post_shot_finish`, and
`repeated_setup`) and the
audio hard negatives (`practice_swing`, `mat_strike`, `waggle`, `speech`,
`footsteps`, `club_drop`, and `aborted_address`). `pose_expectation` and
`audio_expectation` state what the product should do; a category alone is not
treated as a pass/fail label. The parser rejects category/expectation
contradictions: real swings, aborted addresses, address-without-swing, and
clear-and-rearm episodes must arm; empty scenes, walk-throughs, practice swings,
post-shot finishes, and repeated setups must not arm; real swings must trigger
while every declared audio hard negative must not trigger. A real swing requires
safe/preferred/takeaway and impact-reference times on both views. A
clear-and-rearm episode records the first safe/preferred arm window, an observed
clear, a second safe/preferred arm window, and the final takeaway in that order.
Quiet real impacts are identified explicitly through
`audio_character=quiet` rather than by retrospectively selecting the weakest
detector hit.

Each session also records opaque golfer, environment, lighting, and framing
labels. The pose gate requires at least two reviewed golfer, lighting, and
framing strata; these labels need not contain names or other personal data.

One reviewed real-swing row has this shape (times below are illustrative, not
field labels):

```json
{
  "id": "S001",
  "category": "real_swing",
  "pose_expectation": "must_arm",
  "audio_expectation": "must_trigger",
  "audio_character": "quiet",
  "views": {
    "down_the_line": {
      "start_ms": 10000,
      "end_ms": 20000,
      "reference_ms": 19000,
      "safe_arm_start_ms": 12000,
      "preferred_arm_ms": 14000,
      "takeaway_ms": 18000,
      "clear_ms": null,
      "rearm_safe_start_ms": null,
      "rearm_preferred_ms": null
    },
    "face_on": {
      "start_ms": 10080,
      "end_ms": 20080,
      "reference_ms": 19080,
      "safe_arm_start_ms": 12080,
      "preferred_arm_ms": 14080,
      "takeaway_ms": 18080,
      "clear_ms": null,
      "rearm_safe_start_ms": null,
      "rearm_preferred_ms": null
    }
  },
  "notes": "Illustrative schema row only."
}
```

Use `diagnostic` when a reviewer cannot defend a required outcome. Diagnostic
episodes remain visible in the inventory but do not satisfy a release-gate
category.

## Readiness audit

```bash
bazel run //tools/field_evidence:field_evidence -- validate \
  --manifest=/absolute/path/to/corpus/evidence_manifest.json \
  --media-root=/absolute/path/to/corpus \
  --output=/absolute/path/to/corpus/readiness_report.json
```

The report keeps pose and audio gates separate and lists every missing category,
partial timeline, unpinned asset, or missing phone/view swap. Gate booleans stay
false unless the declared hashes have actually been checked with `--media-root`.
Its `next_collection_plan` converts those gaps into a deterministic acquisition
checklist: the required phone/view reversal, remaining pose and audio-negative
episodes, whether a quiet real impact is still needed, how many new golfer,
lighting, and framing labels remain, and the requirements for complete timeline
review and hash-pinned media. Generate the initial plan before the first
recording, then regenerate this readiness report before each subsequent session
so omissions are discovered while they can still be collected.
A qualifying session must be outside the enumerated development recording IDs,
fully reviewed in both views, and include a complete Pixel/view swap relative
to the development assignment. Required pose and audio categories, the quiet
real impact, and pose diversity are counted only inside those fully reviewed
swapped sessions; an empty swapped session cannot qualify evidence gathered
under the development assignment. The versioned `field_readiness_v2` policy
requires at least two such swapped sessions, at least two qualifying episodes
of each required pose and audio-negative category distributed across at least
two of those sessions, and at least two explicitly quiet real impacts
distributed across at least two sessions. A singleton category therefore
remains an explicit readiness gap even if every category name has appeared
once. Pose qualification additionally requires at
least two distinct golfer, lighting, and framing labels among the swapped
sessions. This prevents the first
247-second session from being relabeled as its own holdout and prevents one
carefully framed follow-up from being described as varied-field evidence.

This contract makes the next collection immediately usable and auditable. It
does not supply the human labels, create a second physical session, establish
absolute acoustic latency, or prove model/detector accuracy.

Across appended sessions, each human device label must remain bound one-to-one to the same durable
node ID. Merely moving a label to a replacement node cannot manufacture phone/view-swap evidence.
Source manifests' declared media lengths are checked against the actual files before hashing, and
manifest/report output paths cannot alias any input manifest or media asset. Immutable detector
locks, predictions, gate reports, and comparisons fail before expensive replay when their output
already exists and publish complete JSON with exclusive, fsynced, atomic creation.

## Frozen audio-detector evaluation

Do not sweep thresholds on a reviewed holdout. Before recording, freeze one
production baseline and each candidate in an `audio_detector_policy_lock` JSON
document. It names exactly the development recordings used for selection, one
detector implementation and configuration per phone, and fixed
evaluation/acceptance constants. Its top-level fields are:

```json
{
  "schema_version": 1,
  "report_type": "audio_detector_policy_lock",
  "policy_id": "descriptive-immutable-policy-id",
  "frozen_at_utc": "2026-08-22T00:00:00Z",
  "development_recording_ids": ["development-recording-id"],
  "detector": {
    "algorithm_id": "detector-algorithm-and-version",
    "implementation_target": "//path/to:detector_replay",
    "implementation_sha256": "lowercase-sha256-of-the-replay-implementation",
    "config_by_device": {"pixel5a": {}, "pixel6": {}}
  },
  "evaluation": {
    "leader_role": "face_on",
    "target_tolerance_ms": 100,
    "audio_ready_delay_ms": 2450,
    "video_ready_delay_ms": 800,
    "maximum_armed_ms": 15000,
    "post_terminal_ms": 1000,
    "rearm_delay_ms": 2000,
    "retained_history_ms": 3000,
    "minimum_target_recall_ppm": 1000000,
    "minimum_quiet_target_recall_ppm": 1000000,
    "maximum_negative_continuous_candidates": 0,
    "maximum_false_terminal_attempts": 0,
    "minimum_lifecycle_capture_ppm": 1000000,
    "maximum_high_speed_duty_ppm": 300000
  }
}
```

For the current Android production detector, generate that detector object from
the exact Java `ImpactDetector`, device policy, and host replay adapter before
the holdout is recorded:

```bash
bazel run //tools/field_evidence:production_audio_predictions -- describe \
  --device='pixel5a=Google|Pixel 5a' \
  --device='pixel6=Google|Pixel 6' \
  --output=/absolute/path/to/production_detector.json
```

Pass the complete output object to the non-overwriting lock creator instead of
copying fields by hand:

```bash
bazel run //tools/field_evidence:audio_policy_lock -- \
  --policy-id=production-android-audio-baseline-v1 \
  --frozen-at-utc=2026-08-23T11:10:13Z \
  --development-recording-id=field-95482d93-f024-400e-9532-5ba7082de05e \
  --development-down-the-line-device=pixel5a \
  --development-face-on-device=pixel6 \
  --detector=/absolute/path/to/production_detector.json \
  --output=/absolute/path/to/production_policy.json
```

The implementation digest deliberately changes when
the production detector, per-device policy, or replay adapter changes; a stale
lock is rejected instead of silently replaying different code. The creator
requires the detector's device set to match the development assignment exactly
and refuses to overwrite an existing lock.

The current pre-capture locks are checked in at
`policy_locks/production_android_audio_baseline_v1.json` and
`policy_locks/robust_envelope_hp120_x12_candidate_v1.json`. Both name only the
247-second development recording and were frozen at `2026-08-23T11:10:13Z`.
Their canonical lock SHA-256 values are respectively
`9ed6643d13e14a0a432480dc55149ee45b33225c6e14d77a5463fd096a3f440e` and
`df205fbc418291f40279d1b9a804b4f7b1513cde2be5dde764d0e4017b8a9053`.
Do not edit them. If a bound implementation changes before collection, create
new policy IDs and new locks before recording rather than updating these files.

The detector-specific replay named by `implementation_target` must emit one
`audio_detector_holdout_predictions` document. It embeds the exact detector
object and canonical policy-lock SHA-256. Every qualifying swapped session and
both roles are required. Each stream contains the complete continuous candidate
sequence and one reset-from-arm replay for every reviewed `must_arm` attempt:

```json
{
  "continuous_candidates": [{"strike_ms": 19000, "decision_ms": 19003}],
  "armed_replays": [{
    "attempt_id": "S001",
    "arm_ms": 14000,
    "evaluation_end_ms": 29000,
    "candidates": [{"strike_ms": 19000, "decision_ms": 19003}]
  }]
}
```

Use `EPISODE:initial` and `EPISODE:rearm` for the two reviewed attempts in a
`clear_and_rearm` episode. Candidate arrays are online decision-time ordered;
an armed replay must begin at the reviewed preferred-arm timestamp and cover
the entire locked maximum-armed window. Lifecycle scoring ends the initial attempt at the reviewed
clear timestamp, ignores later candidates for that attempt, and requires the rearm attempt to be
evaluated rather than silently skipped while the simulated camera is busy. Empty arrays are
explicit negative results, not missing evidence.

Generate the production prediction artifact without a second implementation or
hand-transcribed candidate list:

```bash
bazel run //tools/field_evidence:production_audio_predictions -- predict \
  --manifest=/absolute/path/to/corpus/evidence_manifest.json \
  --media-root=/absolute/path/to/corpus \
  --policy-lock=/absolute/path/to/frozen_production_policy.json \
  --output=/absolute/path/to/production_predictions.json
```

This validates all corpus hashes, requires the frozen production detector
digest to match the checked sources, obtains the exact Pixel-specific config
from `DeviceAudioDetectorPolicy`, and replays every qualifying WAV through the
same `ImpactDetector` used by Android.

Generate the candidate prediction artifact through the fixed-memory streaming
replay selected on the development set:

```bash
bazel run //tools/field_evidence:envelope_audio_predictions -- predict \
  --manifest=/absolute/path/to/corpus/evidence_manifest.json \
  --media-root=/absolute/path/to/corpus \
  --policy-lock=/absolute/path/to/checkout/tools/field_evidence/policy_locks/robust_envelope_hp120_x12_candidate_v1.json \
  --output=/absolute/path/to/envelope_predictions.json
```

The C++ replay uses a one-pole 120 Hz high-pass filter, bounded RMS/background
state, the frozen 12x robust-background threshold, and a bounded candidate
cluster. Tests require byte-equivalent candidate values to the selected offline
algorithm across arbitrary input chunks, fixed memory independent of recording
length, absolute reset-from-arm timestamps, and a complete synthetic
WAV-to-holdout-gate path.

Run the non-overwriting qualification gate only after review is complete:

```bash
bazel run //tools/field_evidence:audio_holdout_gate -- \
  --manifest=/absolute/path/to/corpus/evidence_manifest.json \
  --media-root=/absolute/path/to/corpus \
  --policy-lock=/absolute/path/to/frozen_policy.json \
  --predictions=/absolute/path/to/predictions.json \
  --output=/absolute/path/to/audio_holdout_report.json
```

The gate re-hashes all source manifests, video, and audio; verifies that the
lock predates both phones' source-manifest capture times; requires the lock's
selection IDs to equal the corpus development IDs; rejects a holdout ID in
selection; requires every qualifying swapped holdout and every ordinary or
clear/rearm armed replay; and compares the prediction detector object and lock
digest byte-for-byte at the JSON-value level. Only then does it score target
and quiet-impact recall, every non-target candidate across the fully reviewed
timeline (with hard-negative category attribution), first-terminal behavior,
captured swings, false attempts, required/evaluated/ignored attempt inventory, lifecycle attempt
coverage, and high-speed duty. A failing evaluation is
written for diagnosis and exits nonzero. Existing output is never overwritten.

Evaluate a candidate policy on the identical corpus through the same gate,
then compare the two evaluation reports:

```bash
bazel run //tools/field_evidence:audio_holdout_compare -- \
  --baseline=/absolute/path/to/production_holdout_report.json \
  --candidate=/absolute/path/to/candidate_holdout_report.json \
  --output=/absolute/path/to/audio_holdout_comparison.json
```

The comparator rejects different corpus/media evidence, evaluation policy,
holdout set, or leakage-control provenance. The production report must come
from the exact Android replay target. A candidate passes only if it also passes
the absolute gate, regresses no aggregate or negative-category metric, and
strictly improves at least one locked metric.
