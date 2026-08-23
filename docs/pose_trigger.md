# Low-rate pose trigger prototype

## Objective

The pose trigger is a thermal optimization, not the impact clock. Both phones
observe low-resolution standby streams at about 5 fps. When the configured
leader sees a coherent approach to address, it requests that both phones start
their existing 720p240 pipelines. The leader microphone determines the terminal
impact; the shadow retains its own local audio candidate for timing evidence but
freezes only when the leader reports that impact.

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
- `inside_hitting_region` is retained as diagnostic/replay metadata for schema
  compatibility. The production controller no longer gates on it.

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
  --hitting-region=0,0,1,1 \
  --projection=atl
```

The host landmarker decodes in presentation order rather than seeking to each
5 Hz deadline. It records both the ideal sampling deadline and the actual
decoded presentation timestamp, and supplies the latter to MediaPipe's video
tracker. This keeps variable-frame-rate source timing intact through landmark
CSV and controller replay. Inputs whose OpenCV backend reports missing,
non-increasing, or millisecond-ambiguous timestamps are rejected rather than
being relabeled with a nominal frame rate.

The replay converter still accepts an explicit region so old evidence remains
reproducible. Production and current field evaluation use the full frame. The
batch corpus evaluator hard-wires `0,0,1,1`; legacy per-clip rectangles remain
optional manifest provenance and cannot change evaluation or annotation results.

### ROI and ball policy

The field recording showed no useful rejection supplied by the configured ROI,
while one valid DTL setup fell outside it. A tripod move would also invalidate
that calibration. The controller therefore uses full-frame person, address, and
motion evidence and the setup UI no longer asks the user to draw a hitting-area
box. This does not add inference work: the pose model already processes the same
input image.

The first on-device prototype also does not run a golf-ball detector
continuously. At roughly 360p the ball occupies very few pixels, may be occluded
by the club, and is easily confused with tees and background detail. The
recorded practice swing occurred with the normal hitting setup present, so ball
presence would not have disambiguated intent. Ball or club-head detection
remains an evidence-driven later option, not an arm prerequisite.

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
| Active scene-clear stop interval | 1 s |
| Rearm cooldown | 2 s |

At a strict 5 fps cadence, 400 ms means three qualifying observations. The
controller tolerates one isolated missed inference but resets on a larger gap.
It emits one `START_HIGH_SPEED` command. While pose observations continue, an
engaged golfer extends a 15-second active-evidence lease, including motion that
no longer looks like address. The lease cannot cross the absolute 30-second
thermal hard cap. A capture completion, expired lease, hard cap, or continuously
missing-person evidence moves the active controller to a waiting-for-reset
state. After capture, one observed non-address or moving pose records that the
previous swing cycle ended. That fact survives while the two-second cooldown
finishes; a golfer who has already begun the next address is not permanently
latched out. A fresh three-observation qualification is still required before
another arm.

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

with no arm request before the safe window or inside a `must_not_arm` interval. The closed
five- and 30-cycle physical qualifications expose startup p95 and exact maximum; at those sample
counts nearest-rank p99 is necessarily the maximum, so maximum is the conservative observed tail
until a larger representative corpus can estimate p99 independently.

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
policy and a historical 800 ms provisional Pixel 6 startup budget. It is preserved at
`artifacts/pose_trigger_corpus_evaluation_20260817T2308Z/report.json`. The arm
times below are controller decisions; changing the startup budget changes only
the estimated-ready time, not those decisions. Current paired-pipeline evidence
measures 1.279 s to the first usable encoded frame on Pixel 6, so the 800 ms
estimated-ready values are not a current acceptance claim.

| Clip | View | Provisional safe arm | Controller arm | Strict label result |
| --- | --- | ---: | ---: | --- |
| IN01 | ATL | 7.0 s | 7.433 s | pass |
| IN02 | ATL | 8.0 s | 8.609 s | pass |
| C01 | DTL | 11.0 s | 11.400 s | pass |
| D01 | DTL | 10.0 s | 8.000 s | early |
| D03 | ATL | 19.0 s | 11.417 s | early |
| D04 | DTL | 13.0 s | 12.608 s | early |
| EE01 (E_E) | ATL | 12.0 s | 2.800 s | early |

The DTL policy now tolerates far-side joint occlusion while ATL retains
bilateral landmark requirements. That turns the former DTL no-arms into visible
decisions. The 2026-08-22 Lite full-frame rerun under
`artifacts/pose_model_corpus_compare_20260822/lite/` has a strict replay score
of 3/7 because the current label contract
treats any arm before `safe_arm_start_ms` as a failure, including D04's 392 ms
offset. It should not be reported as model accuracy. After reviewing the
earlier annotated set, the human reviewer judged its displayed arm points
reasonable for starting high-speed capture. That review predates the current
full-frame rerun, which materially moved EE01 to 2.8 seconds, so the current
annotated outputs require a fresh spot check. Neither result is a field-accuracy
claim: these are selected positive instructional clips, the acceptable early
boundary has not yet been reconciled, and the set does not measure false arms
during ordinary field use.

The retained EE01 trace explains the 2.8-second change without suggesting a Lite regression. At
2.4/2.6/2.8 seconds Lite reports person confidence 0.986/0.987/0.986, address confidence
0.566/0.681/0.653, and motion 0.232/0.247/0.366. All three samples satisfy the current thresholds,
so the controller correctly completes its three-sample/400 ms dwell. Full and Heavy make the same
2.8-second decision, and frame review shows a real address-like setup immediately before the golfer
makes a complete practice swing. The observations are unchanged from the older ROI run; those
samples were simply outside its rectangle. Even that older run armed at 9.2 seconds, still before
the provisional 12-second safe label.

This makes the strict EE01 result causally ambiguous as an address-perception score: pose at address
cannot know whether the upcoming motion will contact the ball. It remains a valid lifecycle and
thermal-efficiency challenge. Human review must decide whether the safety policy should arm on any
valid address-like setup, or whether suppressing a practice-swing attempt is required; it must also
set an acceptable high-speed-duty/false-attempt budget and the earliest acceptable final-shot arm.
Those decisions, plus representative complete lifecycles, must precede any label, threshold, or
model change.

The retained strict boundaries remain useful as conservative regression
challenges until they are reconciled in a separate labeling pass. In
particular, they keep the narrow-stance, practice-swing, and early-setup periods
visible instead of silently redefining them after seeing controller output.
Deterministic controller and complete-session replay tests now cover aborted
address, no-swing timeout and restart, clear-and-rearm, practice swing, empty
scene, walk-through, post-shot finish reset, and repeated setup. The remaining
gap is representative perception evidence: whether MediaPipe emits the assumed
person/address/motion values for those cases across views, golfers, framing,
and lighting. A temporal setup-phase feature or a small sequence model remains
an evidence-driven option if those cases produce unacceptable false arms. A
continuously running ball detector is still unlikely to solve the
practice/setup cases because the ball is already present.

## First field-session evidence

The first continuous field recording is 247 seconds long and contains 12
reviewed ball impacts plus one practice swing before S01. Replaying the complete
5 Hz observation sequence matters more than replaying 12 isolated clips: the
controller is blind while Camera2 owns the camera at 240 fps, and a false arm
can therefore affect the next real swing.

With the full frame, production thresholds, 800 ms first-video-frame budget,
2.45 s retained-history budget, one-second completion, and 800 ms restart, the
direct per-view pose replay is:

| Authority view | Real swings captured | Attempts | 240 fps time | Minimum video lead before takeaway | Minimum full-history lead before impact |
| --- | ---: | ---: | ---: | ---: | ---: |
| Pixel 6 ATL | 12/12 | 12 | 72.630 s | 1.394 s | 0.768 s |
| Pixel 5a DTL | 11/12 | 13 | 135.426 s | — | — |

The ATL result is a 29.4% high-speed duty cycle for this session. The DTL view
mistakes some post-shot finishes for a new address, creates two no-impact
attempts, and is still in an active/restart lifecycle at S03. This is evidence
for making the Pixel 6 ATL view the initial leader, not for weakening Pixel 6
behavior to accommodate the Pixel 5a. The roles remain configurable, and a
future session must repeat the comparison before view selection becomes an
automatic policy. Device, view, mounting location, distance, and room acoustics
are confounded in this recording; it does not show that Pixel 6 hardware is
intrinsically better at pose or audio. A phone-swap A/B recording is required
to separate those effects.

Recreating production audio behavior from each pose-arm time gives a second,
independent result. Both continuous detectors have a candidate within 100 ms of
all 12 impacts, but the first terminal candidate is the target on only 7/12
Pixel 5a windows and 11/12 Pixel 6 windows. The Pixel 6 exception is the known
practice swing. A single amplitude cutoff cannot safely reject it: its transient
is stronger than several valid strikes.

The paired implementation therefore makes the leader's detector authoritative.
The shadow detector cannot terminate capture, but it retains a bounded ring of
validated local candidates. A background 1 Hz, three-sample clock exchange maps
the leader strike into the shadow's BOOTTIME domain on the leader. The leader
composes clock-offset uncertainty with its audio timestamp uncertainty and
sends the mapped target, bounds, age, RTTs, sample count, and target node ID in
an authenticated schema-2 impact request. The shadow verifies its node ID, the
current shared session ID, and the 10-second/15-millisecond policy before using
the mapping. Status evidence is session-bound so a prior capture's accepted
mapping cannot satisfy a later HIL run. After waiting at most
75 ms for detector evidence, the shadow selects the closest candidate within
80 ms plus bounded clock and audio uncertainty. With a valid mapping and no
match it safely falls back to peer-arrival time, not an unrelated precursor.
When clock evidence is missing or stale, the earlier newest-candidate-within-
250-ms schema-1 rule remains a conservative fallback before arrival time. A
schema-2 mapping outside policy uses that same explicit fallback; a mapping for
another node is rejected.

An exact event replay using the ATL pose lifecycle captures S01 through S12
with one separate practice-swing attempt and 28.19% high-speed duty. The old
shadow latest-candidate rule timestamps 11/12 clips within 100 ms; the bounded
clock-mapped ring reaches 12/12, with 82.729 ms maximum absolute error in this
recording. Hard consensus at 25 ms one-way network latency also captures 12/12
with one false attempt, but adds up to 30.125 ms terminal latency and makes
capture depend on both microphones. It is therefore rejected. These are
development-set analysis results, not product-acceptance statistics.

A small pose-threshold grid reduced ATL high-speed time by only 1.398 seconds
while retaining 12/12 on this development session. That gain is too small to
justify overfitting production thresholds. A conservative 120 Hz high-pass plus
robust background envelope retains 12/12 candidates on both phones and scores
11/12 first-terminal per view, but changes the complete ATL lifecycle from
28.19% to 28.35% duty and reduces minimum pre-takeaway video lead from 995 ms to
796 ms. A fitted temporal classifier reaches 23/24 first-terminal decisions but
falls to 22/24 at either adjacent threshold; spectral templates reach only
20/24 or 21/24. None replaces the current production detector. The
higher-value next evidence is a phone/view swap and a negative-rich holdout
containing quiet impacts, practice swings, mat strikes, speech, footsteps, club
drops, and aborted addresses.

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

The current complete paired-LAN path now retains each startup milestone in the
session manifest and rejects non-monotonic derivations or any startup continuity
reset. The short S06 run preserved at
`artifacts/android_pcm_paired_s06_startup_pass_20260823T043404Z` measured:

| Device | Arm to engine start | Arm to first camera frame | Arm to first usable encoded frame | Encoded frame to full pre-roll | Arm to full pre-roll |
| --- | ---: | ---: | ---: | ---: | ---: |
| Pixel 6 | 531.876 ms | 1,066.450 ms | 1,213.343 ms | 2,464.215 ms | 3,677.558 ms |
| Pixel 5a | 648.127 ms | 1,956.545 ms | 2,111.294 ms | 2,445.991 ms | 4,557.285 ms |

Both startups were continuous and both captures passed decoded 720p240,
AprilTag persistence, optical/audio correlation, direct-LAN coordination, and
cleanup. This is one bounded sample, not the slower-node p99 or a contention
qualification. It does prove that roughly 2.45 seconds of the reported delay is
the deliberate full-history requirement. An opt-in retention prototype can
accept an IDR-backed continuous startup window with explicitly reported actual
pre-roll, but production admission remains on the full window until repeated
startup evidence and reviewed address-to-takeaway timing establish a safe
minimum. Truncation cannot recover a backswing that began before the first
usable encoded frame, which is the material Pixel 5a risk.

Run the short gate sequentially:

```bash
bazel test //android/hil:android_warm_high_speed_transition_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_ROLE=face_on \
  --test_env=SWING_CAPTURE_ANDROID_PROFILE=720p240 \
  --test_output=streamed --nocache_test_results

bazel test //android/hil:android_warm_high_speed_transition_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=1A011JEG501717 \
  --test_env=SWING_CAPTURE_ANDROID_ROLE=down_the_line \
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
phone role, leader/shadow mode, delegate policy, and debug evidence; pose
decisions use the full frame. A leader decision sends one authenticated pose-arm
candidate to its configured peer without blocking local capture, and peer
outcome is retained in status evidence. The controller implements an
active-evidence lease, absolute thermal cap, no-swing stop, and temporal
reset-plus-cooldown rearm policy.

The first delegate invocation is an explicit cold warm-up. Until it succeeds,
status reports `pose.phase=warming_up`; its landmark frame seeds temporal motion
state but cannot arm the controller. Its duration and success/failure counters
are reported separately, so model/delegate initialization remains observable
without polluting the steady-state latency distribution. `monitoring` therefore
means one warm-up completed successfully and live frames are reaching the
controller.

The pose status exposes fixed-memory, one-millisecond-resolution p50, p90, p95,
p99, and maximum measurements for inference duration and end-to-end age from
the Camera2 frame timestamp through the controller decision. It also counts
inferences over the nominal 200 ms frame period, outliers over 400 ms,
timestamp-domain rejections, and scheduled/dropped frames. This makes a long
tail visible even when the mean remains below the 5 Hz budget; histogram values
are conservative inclusive bucket upper bounds.

Short sequential GPU-required HIL after that split passed the hard steady-state
gate on both devices at least once: Pixel 6 measured 175 ms p95 / 174.5 ms exact
maximum after a 155 ms warm-up, and Pixel 5a measured 199 ms p95 / 198.3 ms
exact maximum after a 708 ms warm-up. One immediately preceding Pixel 6 sample
narrowly missed at 202 ms p95 / 201.3 ms exact maximum, with no inference above
400 ms. The data supports GPU on both phones and proves that the old 714 ms
Pixel 5a value was cold initialization in these short runs; it does not yet
replace a motion-and-thermal qualification distribution.

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

`//tools/field_evidence:field_evidence` now supplies the acquisition and review
contract for steps 1--3. It creates a hash-pinned but explicitly unreviewed
two-phone skeleton, keeps DTL and ATL times separate, and refuses to mark the
pose gate ready until both timelines are fully reviewed, the complete lifecycle
taxonomy is labeled with non-diagnostic expectations, a held-out phone/view
swap is present, and at least two golfer, lighting, and framing strata have been
reviewed. This closes the reproducibility gap in how the next corpus is recorded
and labeled; the representative recordings and human review are still required
evidence.

MediaPipe Pose Landmarker documentation:
https://developers.google.com/mediapipe/solutions/vision/pose_landmarker/
