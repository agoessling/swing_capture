# Android pose standby HIL

`android_pose_standby_hil_test` qualifies one explicitly selected phone at a
time. It installs the Bazel-signed APK with Bazel's hermetic ADB, configures the
standard `720p240` profile and pose `shadow` / `gpu_preferred` mode, and observes
the real Camera2-to-MediaPipe standby path for a bounded two-second interval.

The test requires:

- pose phase `monitoring` while the node remains armed;
- at least 4 successful inferences per second;
- zero failed inferences;
- drop accounting bounded by the two-image latest-frame pipeline;
- no more than a 20% dropped-frame fraction after the two-frame allowance; and
- a recorded actual CPU or GPU delegate; and
- ready, advancing standby audio with a non-null source, no discontinuities,
  event drops, or error, and no more than two startup timestamp rejections.

Battery temperature, battery level, voltage, and Android thermal status are
captured before and after. `report.json` is written to Bazel undeclared test
outputs throughout the run, including partial failures and disarm/force-stop
cleanup evidence.

Run the phones sequentially; the test never guesses among attached devices:

```bash
bazel test //android/pose_hil:android_pose_standby_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=<adb-serial> \
  --test_output=streamed --nocache_test_results
```

The capture role defaults to `down_the_line`. To qualify the face-on station,
add `--test_env=SWING_CAPTURE_ANDROID_ROLE=face_on`. Each invocation has a
15-second main-stage deadline and is tagged `manual`, `local`, `exclusive`, and
`requires-android-phone`.

## Warm retained capture

`android_pose_warm_retained_hil_test` enables debug preview evidence, waits for
the status endpoint to report three fully encoded one-Hz JPEGs (not merely
three completed pose inferences), and then posts an authenticated synthetic
leader candidate to `/api/v1/capture/pose-arm`. This forces the production
`PoseStandbyEngine` to transfer its existing `WarmCameraLease` into constrained
high-speed capture. The external arm is claimed before transfer, but its local
controller timestamp is committed only after transfer has drained the final
standby inference; an older Camera2 sensor timestamp therefore cannot arrive
after the lifecycle event. The test waits for a full high-speed pre-roll, posts
a manual retained trigger, and validates the atomically published session. Its
readiness sub-deadline includes a separate 250 ms arm-request budget and
requires the phone to accept the pose arm with at least 8.5 seconds of the
15-second stage remaining for the camera transition, retained capture,
publication, and artifact pulls. The report records the measured reserve.

```bash
bazel test //android/pose_hil:android_pose_warm_retained_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=<adb-serial> \
  --test_output=streamed --nocache_test_results
```

In addition to the 15-second main-stage bound, the warm test requires:

- at least 450 retained 1280×720 / 240 fps frame records;
- at least 1.3 seconds of pre-roll and 450 ms of post-roll;
- a Camera2-to-encoder startup ordinal shift between -32 and 32, preserved in
  the report;
- at least 16 timestamp pairs with no more than 100 µs offset span;
- an absolute `encoder_to_sensor_offset_ns` no larger than 1 ms, which rejects
  a one-frame ordinal shift at 240 fps;
- a valid MP4 matching its manifest byte count; and
- at least three byte-exact indexed JPEGs with matching pose NDJSON rows.

The report, manifest, MP4, MJPEG, and NDJSON trace are preserved in Bazel
undeclared test outputs. The NDJSON contains every retained 5 Hz pose
observation. JPEGs are attached to a sparse subset at the bounded 1 Hz debug
cadence (plus the arm frame), so missed decisions remain diagnosable without
paying continuous 5 Hz JPEG encoding cost. The manifest reports both
`observation_count` and `jpeg_frame_count`. The current status schema does not
expose the standby camera ID separately, so the test proves use of the
same-camera transition by the authenticated pose-arm production path rather
than by comparing two reported camera identifiers.

The arm frame has priority over ordinary pending JPEG work. Transfer waits for
that copied frame only within the existing total three-second inference and
camera handoff deadline, and snapshots evidence after a successful attach. A
flush failure or timeout is counted explicitly in pose metrics and cancels the
best-effort evidence worker, but does not delay the 240 fps transition beyond
the deadline or retain a Camera2 `Image`.

## Standby missed-shot diagnostics

`android_pose_standby_missed_shot_hil_test` qualifies the field-debug path
without entering high-speed capture. It enables debug preview evidence, waits
for joint pose and 48 kHz standby-audio readiness, turns the display off, and
requires both inference and retained audio to keep advancing while the phone is
noninteractive. Before tagging, it also waits for the status counter to confirm
that at least three of the thermally bounded one-Hz preview JPEGs have finished
encoding, while retaining every five-Hz pose observation row. It then posts an authenticated operator tag to
`/api/v1/capture/missed-shot` and waits for the exact two-second audio
post-roll. Android 16 may report `mWakefulness=Dozing` after the sleep key; the
test accepts that logical state only when an independent display-state dump
reports the physical display `OFF` (a `DOZE` display does not qualify). Cleanup
reapplies the sleep key and verifies the display remains physically off for
normal field operation.

```bash
bazel test //android/pose_hil:android_pose_standby_missed_shot_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=<adb-serial> \
  --test_output=streamed --nocache_test_results
```

The bounded validator requires a schema-v1 `standby_diagnostic` manifest,
`operator_tag` event, `user_reported` / `missed_shot` incident, canonical
48 kHz mono PCM16LE WAV, and exactly 96,000 post-marker samples. A full window
contains 480,000 pre-marker samples; a tag issued during startup is explicitly
classified as startup-short and must contain at least 24,000 pre-marker
samples starting at audio frame zero. At least three JPEG preview inputs must
link byte-for-byte to their NDJSON trace. The authenticated diagnostics ZIP
must contain the export manifest and the exact five published session files,
with valid stored-entry headers and CRCs.

`report.json`, the manifest, WAV, incident, MJPEG, NDJSON, ZIP, and raw screen
state evidence are preserved in Bazel undeclared outputs. Cleanup reapplies
sleep, verifies the display remains physically off, disarms capture, force-stops
the app, and removes the ADB forward. The main stage remains bounded to 15
seconds and the target carries the standard
`manual`, `local`, `exclusive`, and `requires-android-phone` tags.

## Standby Feather impact diagnostics

`android_pose_standby_impact_diagnostic_hil_test` keeps the phone in the same
low-rate pose/audio standby mode and commands one bounded Feather tone. It
requires exactly one automatically detected `impact_while_not_armed` session,
then validates the retained audio, pose preview, incident, manifest, and
diagnostics ZIP with the same strict artifact checks as the operator-tag test.

```bash
bazel test //android/pose_hil:android_pose_standby_impact_diagnostic_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=<adb-serial> \
  --test_output=streamed --nocache_test_results
```

The stage is limited to 15 seconds and is additionally tagged
`requires-rp2040`. The existing
`//android/dual_hil:dual_phone_concurrent_hil_test` remains the end-to-end
Feather LED/tone plus printed-AprilTag qualification for the retained 240 fps
videos; this standby target isolates the new low-power missed-impact recorder.

## Paired pose-arm production transition

`//android/dual_hil:dual_phone_paired_pose_arm_hil_test` configures the Pixel 6
down-the-line node as the leader and the Pixel 5a face-on node as its shadow.
Both phones run the real on-device 5 Hz standby inference and standby audio.
The test invokes a launch-gated deterministic candidate endpoint on the leader;
from that point it uses the production peer-arm client and warm Camera2
transition on both phones. It requires both nodes to report a full `720p240`
pre-roll before commanding exactly one Feather LED/tone event, then applies the
same dual AprilTag, audio, retained-frame, timing, and coordination validators
as the concurrent capture HIL.

The deterministic `POST /api/v1/hil/pose-arm` route is unavailable after a
normal application launch. The HIL starts each app with the explicit
`enable_pose_arm_hil=true` activity extra, and force-stop cleanup removes that
process-local permission. The leader reaches the shadow through a test-owned
ADB reverse tunnel, while the actual bearer-authenticated peer protocol is
unchanged. Before installation/configuration the target snapshots each phone's
complete private `node_configuration` generation (including the write-only
peer credential). Successful runs require byte-for-byte restoration after
force-stop and record that result in the aggregate report. Failure cleanup also
attempts restoration and emits an explicit warning if it cannot complete; that
best-effort failure path is not yet represented as structured report evidence.

```bash
bazel test //android/dual_hil:dual_phone_paired_pose_arm_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_DTL_SERIAL=<pixel-6-adb-serial> \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_SERIAL=<pixel-5a-adb-serial> \
  --test_output=streamed --nocache_test_results
```

The target is `manual`, `local`, and `exclusive`; it requires two Android
phones and the RP2040. Every camera-owning stage is bounded to 15 seconds.
The latest passing evidence is preserved at
`artifacts/android_pose_field_readiness_20260821/dual_paired_passed_000604`.
It retained 588 Pixel 6 frames at 238.876 fps and 582 Pixel 5a frames at
239.353 fps, passed exact decode, audio, optical, and AprilTag checks on both
phones, and persisted a paired record with 10.890 ms maximum mapped trigger
separation and 7.359 ms combined uncertainty. Configuration restoration was
checked byte-for-byte and no credential was retained in the artifact tree.

## Phone-hosted root/static smoke

The paired and concurrent dual-phone targets perform a non-camera web-hosting
smoke during setup: `GET /` must return the packaged HTML referring to
`/app.js` and `/app.css`, and `GET /app.css` must return a nonempty asset. To
repeat only that check on an already installed development APK, start the app
normally (do not arm capture), forward the node port, and query the public
static routes:

```bash
adb -s <serial> shell am start -W -n com.agoessling.swingcapture/.MainActivity
adb -s <serial> forward tcp:18088 tcp:8088
curl --fail --show-error http://127.0.0.1:18088/ -o root.html
curl --fail --show-error http://127.0.0.1:18088/app.css -o app.css
adb -s <serial> forward --remove tcp:18088
adb -s <serial> shell am force-stop com.agoessling.swingcapture
```

## Recorded-clip replay HIL

`android_pose_replay_hil_test` installs the same APK, copies one explicitly
selected host MP4 into the app's private `files/pose_replay_hil` directory, and
invokes `PoseReplayHilRunner` through replay-only activity extras. The Android
path decodes at 5 Hz and runs the exact MediaPipe landmarker, observation
extractor, and trigger controller. The host retrieves the bounded schema-v1
report, validates its full trace and metrics contract, and preserves both
`report.json` and `pose_replay_report.json` in Bazel undeclared outputs. The
temporary phone clip and ADB staging copy are removed on success or failure.

The serial and absolute MP4 path are required; the test never selects a phone
or clip implicitly:

```bash
bazel test //android/pose_hil:android_pose_replay_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=<adb-serial> \
  --test_env=SWING_CAPTURE_POSE_REPLAY_CLIP=/absolute/path/address.mp4 \
  --test_output=streamed --nocache_test_results
```

Optional inputs and defaults are:

- `SWING_CAPTURE_ANDROID_ROLE=down_the_line` (`face_on` is also supported);
- `SWING_CAPTURE_POSE_PROJECTION=dtl` (derived from the role when omitted);
- `SWING_CAPTURE_POSE_HITTING_REGION=0.15,0.30,0.85,1.0`;
- `SWING_CAPTURE_POSE_DELEGATE=gpu_preferred` (`cpu_only` and `gpu_required`
  are also supported);
- `SWING_CAPTURE_POSE_EXPECTATION=observe_only` (`require_arm` or
  `require_no_arm` makes the Bazel result enforce that outcome); and
- `SWING_CAPTURE_POSE_MAXIMUM_FRAMES=600`, bounded to 1–1,200 frames.

Replay-specific extras do not modify saved station configuration, do not start
the capture service, and do not require camera or microphone permissions. The
target is tagged `manual`, `local`, `exclusive`, and `requires-android-phone`;
its main-stage deadline is 30 seconds.
