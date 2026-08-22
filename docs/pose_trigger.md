# Low-rate pose trigger prototype

## Objective

The pose trigger is a thermal optimization, not the impact clock. One phone
observes a low-resolution standby stream at about 5 fps. When a golfer enters
the configured hitting region and coherently approaches address, the controller
requests that both phones start their existing 720p240 pipelines. The current
local microphone detector still determines impact time and freezes the retained
clip.

The first prototype is intentionally split at a narrow boundary:

```text
recorded video
  -> pose inference and feature adapter
  -> timestamped normalized observation CSV
  -> deterministic trigger controller
  -> replay result against hand-labeled timing
```

The controller, landmark feature extraction, and replay path are implemented in
pure Java under `//android/core/pose`. They do not depend on Camera2, Android, or
a particular ML runtime. `//tools/pose_inference` supplies the first host
adapter: a Bazel-pinned MediaPipe Pose Landmarker Lite runtime that samples a
recorded video at 5 fps and preserves all 33 normalized and world landmarks.
A strict fixed-width landmark CSV then feeds the same Java feature extractor we
intend to use on Android.

## Observation contract

The inference adapter emits exactly one row per sampled video frame:

```csv
timestamp_us,person_confidence,address_confidence,motion_magnitude,inside_hitting_region
0,0.91,0.20,0.04,true
200000,0.93,0.72,0.08,true
```

- `timestamp_us` is the source-video presentation timestamp, not wall time.
- `person_confidence` is the adapter's confidence that a usable golfer pose is
  present.
- `address_confidence` is a model- or geometry-derived score for an approach to
  address. It is deliberately separate from generic person detection.
- `motion_magnitude` is normalized landmark motion. Walking through the region
  should remain high; settling over the ball should decrease.
- `inside_hitting_region` is based on the station's configured golfer/ball
  region, not on detecting a golf ball that may be only a few pixels at 360p.

All confidence and motion values must be finite and in `[0, 1]`. Timestamps
must increase strictly. The parser rejects malformed rows rather than silently
dropping evidence.

For a recorded clip, the Bazel-owned stages are:

```bash
bazel run //tools/pose_inference:pose_landmarker -- \
  --input=/absolute/path/clip.mp4 \
  --output=/absolute/path/landmarks.ndjson

bazel run //tools/pose_inference:landmark_csv -- \
  --input=/absolute/path/landmarks.ndjson \
  --output=/absolute/path/landmarks.csv

bazel run //android/core/pose:pose_landmarks_to_observations -- \
  --landmarks=/absolute/path/landmarks.csv \
  --observations=/absolute/path/observations.csv \
  --hitting-region=0.2,0.2,0.8,1.0 \
  --projection=atl
```

The explicit hitting region will come from station setup. The current seed
replays use the full frame because every accepted clip contains one golfer and
were chosen to validate pose/timing before station calibration.

### Ball-relative gating

The first on-device prototype should not run a golf-ball detector continuously.
At a roughly 360p standby resolution a ball occupies very few pixels, may be
occluded by the club, and is easily confused with tees, range balls, and bright
background detail. That adds inference and thermal cost while making a missed
ball capable of suppressing an otherwise valid arm.

Instead, station setup should provide a persistent normalized hitting/ball
region, initially by a user tap or draggable preview overlay. Pose inference
then remains cheap and asks whether the ankle or hip support point is in the
station and whether the hands and body have address-like geometry near that
region. The configured phone role selects ATL- or DTL-specific policy; the
standby model does not need to infer the camera view. Actual ball detection is a
later, evidence-driven option if corpus replay shows that this calibrated-region
gate cannot control false arms. An off-ball practice swing should already fail
region occupancy, while an on-ball practice swing would not be disambiguated by
detecting the ball and instead needs temporal motion/rearm policy.

## Controller behavior

`PoseTriggerController.Config.defaultsForFiveFramesPerSecond()` currently uses:

| Setting | Default |
| --- | ---: |
| Minimum person confidence | 0.55 |
| Clear-region person confidence | 0.25 |
| Minimum address confidence | 0.45 |
| Maximum address motion | 0.45 |
| Continuous qualification | 400 ms |
| Maximum observation gap | 450 ms |
| Isolated dropout grace | 250 ms |
| Active-evidence lease | 15 s |
| Absolute thermal hard cap | 30 s |
| Required clear interval | 1 s |
| Rearm cooldown | 2 s |

At a strict 5 fps cadence, 400 ms means three qualifying observations. The
controller tolerates one isolated missed inference but resets on a larger gap.
It emits one `START_HIGH_SPEED` command. While pose observations continue, an
engaged golfer extends a 15-second active-evidence lease, including motion that
no longer looks like address. The lease cannot cross the absolute 30-second
thermal hard cap. A capture completion, expired lease, hard cap, or continuously
clear hitting region moves the controller to a waiting-for-clear state. Rearm
then requires both a fresh continuous one-second clear interval and the
two-second cooldown, so a finish pose or a golfer stepping back in during the
cooldown cannot immediately arm another swing.

These are prototype thresholds, not product constants. The address and motion
thresholds were first tuned against the reviewer-selected 7.0-second IN01 and
8–10-second IN02 arm windows; they still require the three-observation temporal
qualification. We should tune them only from reviewed replay evidence and
record every change in deterministic tests.

## Replay and labels

Run the Bazel-owned replay binary with a pose-observation CSV and hand labels:

```bash
bazel run //android/core/pose:pose_trigger_replay -- \
  --observations=/absolute/path/to/clip.csv \
  --safe-arm-start-ms=1200 \
  --preferred-arm-ms=1600 \
  --takeaway-ms=4800 \
  --startup-budget-ms=500 \
  --must-not-arm-ms=0:1200,6500:9000
```

Intervals are half-open. The output is a small canonical JSON object containing
the requested arm time, estimated high-speed-ready time, lead before takeaway,
forbidden-window checks, and the final pass/fail result. The startup budget must
eventually be the measured p99 interval from an arm command to a usable first
high-speed encoded frame on the slower supported node; it is not the existing
2.45-second full pre-roll readiness delay.

The initial hand-label contract for each reviewed clip is:

- `safe_arm_start_ms`: early arming becomes acceptable at or after this point;
- `preferred_arm_ms`: the reviewer-selected target for requesting high-speed
  capture, which may be later than the safe boundary;
- `takeaway_ms`: high-speed capture must be ready no later than this point;
- `must_not_arm`: hard-negative intervals such as empty scene, ordinary
  walking, or the post-shot finish; and
- review notes about framing, visibility at 640x360, practice swings, and
  whether the clip resembles the intended station.

The primary acceptance condition is:

```text
arm request + measured p99 high-speed startup <= takeaway
```

with no arm request before the safe window or inside a `must_not_arm` interval.

## Human-reviewed seed labels

The initial private evaluation established these labels. Times for trimmed
clips are relative to the trim; TI02 uses its original source time.

| Clip | Use | Safe arm | Preferred arm/address | Takeaway |
| --- | --- | ---: | ---: | ---: |
| IN01 | End-to-end positive | 7.0 s | 7.0 s arm | 17.6 s |
| IN02 | End-to-end harder positive | 8.0 s | 10.0 s arm | 16.2 s |
| TI02 | Address-transition positive only | — | 1:10 address | — |
| WC03 | Pose, swing, finish, and cooldown diagnostic only | — | Starts at address | — |
| C01 | DTL long-dwell positive | 11.0 s | 11.0 s arm | 20.1 s |
| C02 | Ambiguous-view geometry rejection | — | 10.0 s if evaluated | 18.2 s |
| D01 | DTL positive | 10.0 s | 10.0 s arm | 14.8 s |
| D02 | Front-view geometry rejection | — | 14.0 s if evaluated | 24.0 s |
| D03 | ATL positive with narrow-stance hard negative | 19.0 s | 19.0 s arm | 24.6 s |
| D04 | Shaky-camera DTL secondary positive | 13.0 s | 13.0 s arm | 18.8 s |
| E_D | Ambiguous-view geometry rejection | — | 14.0 s if evaluated | 20.8 s |
| E_E | ATL positive with practice-swing hard negative | 12.0 s | 12.0 s arm | 25.1 s |

WC03 is not an arm-timing positive because the golfer is already at address in
its first frame. Future primary positives must include a continuous visible
transition into address, not merely an address pose followed by a swing.
C02 is not a primary timing positive because its roughly 45-degree camera view
is neither a recognizable face-on/across-the-line view nor a DTL view. It can
remain a secondary test of view-geometry rejection. C01 is a DTL example; its
11.0-second human arm label deliberately precedes the final settled address and
tests whether the controller can start high-speed capture during a long setup.
D02 is likewise excluded from the primary timing corpus because it is filmed
from the front, opposite the intended DTL view. D03's narrow stance beginning
around 12.0 seconds is a useful hard negative: the desired ATL arm time is about
19.0 seconds. D04 is a secondary DTL positive whose camera shake should exercise
motion robustness rather than define the primary acceptance threshold.
E_D is another roughly 45-degree view and is excluded from the primary corpus.
E_E is a primary ATL positive; its complete practice swing around 1.8–5.2
seconds must not cause an arm intended for the final shot, whose human arm label
is about 12.0 seconds.

## Data and rollout plan

Downloaded evaluation video remains under ignored `artifacts/` storage and is
not a repository dependency. Check in only synthetic observation traces and,
after rights review, compact derived fixtures needed for hermetic tests. The
initial corpus review also established a content rule: use golf-instruction
material with realistic setup and camera angles, not generic stock footage.

The first end-to-end replay produced the following development-set results with
the original provisional 800 ms cold Pixel 6 budget:

| Clip | Human arm window | Controller arm | Estimated ready | Takeaway |
| --- | ---: | ---: | ---: | ---: |
| IN01 | 7.0 s | 7.400 s | 8.200 s | 17.600 s |
| IN02 | 8.0–10.0 s | 8.609 s | 9.409 s | 16.200 s |

The most recent seven-clip annotated run uses the configured ATL/DTL projection
policy and an 800 ms provisional Pixel 6 startup budget. It is preserved at
`artifacts/pose_trigger_corpus_evaluation_20260817T2308Z/report.json`. The arm
times below are controller decisions; changing the startup budget changes only
the estimated-ready time, not those decisions.

| Clip | View | Provisional safe arm | Controller arm | Strict label result |
| --- | --- | ---: | ---: | --- |
| IN01 | ATL | 7.0 s | 7.400 s | pass |
| IN02 | ATL | 8.0 s | 8.609 s | pass |
| C01 | DTL | 11.0 s | 11.000 s | pass |
| D01 | DTL | 10.0 s | 8.000 s | early |
| D03 | ATL | 19.0 s | 11.400 s | early |
| D04 | DTL | 13.0 s | 12.608 s | early |
| EE01 (E_E) | ATL | 12.0 s | 9.200 s | early |

The DTL policy now tolerates far-side joint occlusion while ATL retains
bilateral landmark requirements. That turns the former DTL no-arms into visible
decisions. The strict replay score is 3/7 because the current label contract
treats any arm before `safe_arm_start_ms` as a failure, including D04's 392 ms
offset. It should not be reported as model accuracy. After reviewing the
annotated videos, the human reviewer judged all seven displayed arm points
reasonable for starting high-speed capture. That visual acceptance also is not
a 7/7 accuracy claim: these are selected positive instructional clips, the
acceptable early boundary has not yet been relabeled, and the set does not
measure false arms during ordinary field use.

The retained strict boundaries remain useful as conservative regression
challenges until they are reconciled in a separate labeling pass. In
particular, they keep the narrow-stance, practice-swing, and early-setup periods
visible instead of silently redefining them after seeing controller output.
The current static pose, stillness, and region features still need field
evidence for aborted address, no-swing timeout, clear-and-rearm behavior,
walk-throughs, and repeated setup attempts. A temporal setup-phase feature or a
small sequence model remains an evidence-driven option if those cases produce
unacceptable false arms. A continuously running ball detector is still
unlikely to solve the practice/setup cases because the ball is already present.

## Warm Camera2 transition evidence

`//android/hil:android_warm_high_speed_transition_hil_test` opens one rear
CameraDevice, captures 640x360 YUV single shots at a fixed five-frame-per-second
schedule, preconfigures but does not run the AVC encoder, then replaces the
standby session with a constrained 1280x720p240 encoder session. The test rejects
a second camera open, standby outside 4-6 fps, transition deadlines, encoded
rate outside 235-245 fps, or any stable-window PTS gap above 6.25 ms.

Latest passing evidence:

| Device | Standby rate | First 240 fps camera frame | First encoded frame | Stable encoded rate | Max stable PTS gap |
| --- | ---: | ---: | ---: | ---: | ---: |
| Pixel 6 | 4.947 fps | 463.969 ms | 543.321 ms | 239.047 fps | 4.375 ms |
| Pixel 5a | 4.989 fps | 1,222.057 ms | 1,321.694 ms | 239.191 fps | 4.307 ms |

The corresponding reports are under
`artifacts/pose_trigger_warm_transition_20260816T160733PDT_pixel6_final`
and
`artifacts/pose_trigger_warm_transition_20260816T160752PDT_pixel5a_final`.
These final-tree runs passed the strengthened host validator and both remained
at Android thermal status `NONE`. One unchanged Pixel 5a attempt had a real
37.761 ms mid-run encoder PTS gap and is preserved under
`artifacts/pose_trigger_warm_transition_20260816T154318PDT_pixel5a_failed`;
the acceptance gate was not weakened. Pixel 6 behavior is not reduced for Pixel
5a compatibility.

These measurements explain the earlier apparent four-second startup: the cold
Pixel 6 reached its first usable encoded frame in 786 ms but the old service did
not report ready until its 2.45-second retained-history condition was full. The
cold Pixel 5a path took 4.119 s because camera open took 2.042 s and a 71.270 ms
encoder gap reset warm-up. Keeping CameraDevice open removes the cold-open cost;
it does not remove the requirement that pose arm early enough to accumulate the
desired pre-impact history.

Run the short gate sequentially:

```bash
bazel test //android/hil:android_warm_high_speed_transition_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_ROLE=down_the_line \
  --test_env=SWING_CAPTURE_ANDROID_PROFILE=720p240 \
  --test_output=streamed --nocache_test_results

bazel test //android/hil:android_warm_high_speed_transition_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=1A011JEG501717 \
  --test_env=SWING_CAPTURE_ANDROID_ROLE=face_on \
  --test_env=SWING_CAPTURE_ANDROID_PROFILE=720p240 \
  --test_output=streamed --nocache_test_results
```

## Current implementation and qualification

Most of the integration work formerly listed as the next implementation slice
now exists in the tree. `PoseStandbyEngine` runs the same pure-Java controller
from the foreground service, consumes a bounded latest-frame Camera2 YUV stream
at 5 Hz, runs the arm64 MediaPipe model with GPU-preferred/CPU-fallback delegate
selection on both supported Pixels, retains optional low-rate debug evidence,
and transfers its already-open camera into 720p240 capture. Setup selects the
phone role, leader/shadow mode, delegate policy, hitting region, and debug
evidence. A leader decision sends one authenticated pose-arm candidate to its
configured peer without blocking local capture, and peer outcome is retained
in status evidence. The controller implements an active-evidence lease,
absolute thermal cap, no-swing stop, and clear-plus-cooldown rearm policy.

The paired low-rate-to-high-speed physical gate passed on 2026-08-22. Its
complete evidence is preserved at
`artifacts/android_pose_field_readiness_20260821/dual_paired_passed_000604`.
Both phones ran real 5 Hz on-device inference before the HIL-gated Pixel 6
leader candidate exercised the production authenticated peer-arm client. The
Pixel 5a shadow accepted that arm, both already-open cameras transitioned to
720p240 concurrently, and exactly one Feather event produced two valid local
audio captures. The durable paired record was created and read back on both
phones. The mapped trigger delta was 3.529 ms; the conservative maximum
separation was 10.890 ms with 7.359 ms combined clock uncertainty. Pixel 6 and
Pixel 5a measured 238.876 and 239.353 sensor fps, retained 1.955 and 1.924
seconds of pre-roll, decoded every manifested frame, and retained AprilTag 0
before, at, and after impact. The complete transition-and-capture stage took
14.855 seconds and remained inside its explicit 15-second deadline.

The current evidence-driven refinement plan is:

1. exercise aborted address, arm-without-swing, clear-and-rearm, practice-swing,
   empty-scene, and repeated-setup cases during field use with low-rate preview
   and standby-audio diagnostics enabled;
2. add more reviewed ATL and DTL clips, especially true negative and aborted
   sequences, while keeping held-out validation clips separate from tuning;
3. reconcile provisional strict safe-window labels with the accepted visual
   review before using aggregate pass counts as a release gate; and
4. introduce temporal features or a compact sequence classifier only if the
   collected off-nominal evidence shows the current geometry and controller
   hysteresis are insufficient.

MediaPipe Pose Landmarker documentation:
https://developers.google.com/mediapipe/solutions/vision/pose_landmarker/
