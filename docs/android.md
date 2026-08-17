# Android Capture Plan

## Status and intent

Android is the active replacement investigation for the two Daheng cameras,
USB microphone, and Linux capture host. The target station uses two Android
phones as autonomous capture nodes: each phone supplies a rear 240 fps camera,
a microphone, local trigger/retention processing, and a network endpoint from
which the review application can obtain status and completed media.

The repository now contains the production-shaped vertical slice: a continuous
foreground-service capture engine, local audio triggering, bounded encoded
retention, atomic session publication, a single-node HTTP/browser path, and a
browser-owned dual-node coordinator with durable coordination evidence. Short
single-device, sequential two-device, screen-off, concurrent one-event, and
live paired-browser HIL milestones have passed. A requested 15-minute Pixel 6
1080p240 continuous-capture run completed without a frame-continuity failure,
but it failed thermal acceptance after reaching Android thermal status
`SEVERE`. A controlled 15-minute Pixel 6 720p240 run subsequently passed the
same isolation gate without exceeding `MODERATE`. Publication-heavy and
screen-off thermal qualification, the five-minute qualification workflow, and
a 30-minute soak have not passed and are not claimed here.

The existing Daheng implementation remains the qualified behavioral reference
while this work is experimental. It should not be deleted until the Android
path has demonstrated the required capture duration, trigger behavior,
dual-view timing evidence, thermal stability, media publication, and browser
workflow. Sharing a repository does not require the two hardware paths to share
their lowest-level camera implementation.

The Pixel 6 is the reference Android device and is initially assigned the
`down_the_line` role on the development station. The Pixel 5a is initially
assigned `face_on` (also called across-the-line). These are machine-local
defaults, not model-to-role rules. The application must let a user inspect each
preview and assign either connected phone to either role.

## Device support policy

Pixel 6 performance takes priority over Pixel 5a compatibility. The Pixel 5a is
a provisional secondary device, not a performance baseline. In particular:

- 720p240 at 12 Mbit/s is now the standard profile on both nodes because the
  Pixel 6 passed its 15-minute thermal gate there and failed it at 1080p240;
- 1080p240 at 24 Mbit/s remains an explicit Pixel 6 option, not an implicit
  fallback or a requirement imposed by Pixel 5a compatibility;
- compatibility code for the Pixel 5a must remain isolated from the reference
  Pixel 6 path and must not add copies, latency, or failure modes there; and
- Pixel 5a support should be dropped if retaining it prevents a simpler or
  faster Pixel 6 implementation.

Feature and performance probes should normally decide support rather than a
hard-coded model allowlist. A device is nevertheless unsupported if it cannot
meet the product gates. Reasons to drop the Pixel 5a include sustained frame
loss, an unusable thermal limit, camera/audio concurrency failures, inadequate
encoder throughput, insufficient timestamp evidence, or an old Android API or
vendor behavior that would force compromises on the Pixel 6.

An unsupported device should fail clearly during setup. It must not silently
fall back from 240 fps or publish a degraded session without recording that
fact in the session metadata.

Audio detector tuning follows the same isolation rule. `ImpactDetector` defaults
remain unchanged for Pixel 6 and every other or unknown device: the minimum
peak amplitude is 0.015 and all adaptive/noise/confirmation settings use the
default configuration. Only a Google device whose model string is exactly
`Pixel 5a`, compared case-insensitively, gets a 0.012 minimum peak. This policy
is independent of role and capture profile. The separate Pixel 5a
`VOICE_RECOGNITION` microphone-source compatibility branch is likewise
isolated; neither accommodation trades away Pixel 6 performance or tuning.

## Evidence from the initial hardware check

The 2026-08-14 development-station check established the following starting
point:

| Device | Android | Camera2 rear high-speed metadata | Stock 1/8x result | Post-run thermal status |
|---|---:|---|---|---:|
| Pixel 6 (`oriole`) | 16 / API 36 | 1280x720 and 1920x1080 at fixed 120/240 fps | 1920x1080 HEVC, approximately 241 captured fps | 0 (none) |
| Pixel 5a (`barbet`) | 14 / API 34 | 1280x720 and 1920x1080 at fixed 120/240 fps | 1280x720 HEVC, approximately 239 captured fps | 1 (light) |

Both 240 fps stock-camera clips retained the full AprilTag and external
NeoPixel. Both microphones captured the Feather's tone at the optical impact
marker. The host also opened a temporary HTTP listener on each phone over the
station Wi-Fi, proving host-to-phone TCP reachability.

The stock camera writes an approximately 30 fps, eight-times-slowed presentation
file rather than a real-time 240 fps stream. It therefore proves useful device
capability but is not the intended capture or timestamp pipeline. The Pixel 5a
stock app's 720p result also does not prove that its advertised direct
1920x1080-at-240 configuration is sustainable. A Camera2 probe owned by this
repository must answer that question.

The short run is not a thermal qualification. The Pixel 5a's light thermal
status after only a few seconds is an early warning and should be tested before
substantial compatibility work is built around it.

## Repository-owned probe status

Phase 1 now has a Bazel-built Android application and a short physical-device
gate. The application persists a random installation identity and configurable
`down_the_line` / `face_on` role, inventories Camera2, microphone, codec,
thermal, and storage capabilities, and can run a simultaneous constrained
high-speed camera, hardware-encoder, and `AudioRecord` probe. Reports are
atomically written under application-private storage.

The 2026-08-14 Pixel 6 runs established an important codec limit. At
1920x1080p240, H.264 at approximately 41.5 Mbit/s lost an encoded batch and was
correctly rejected despite an acceptable average rate. HEVC sustained that
bitrate. H.264 at 24 Mbit/s passed twice with 703 frames inside each three-second
acceptance window, approximately 239.05 measured fps, maximum adjacent
presentation-timestamp deltas of 4.42 and 4.51 ms, encoder EOS, a measured audio
clock within 0.01 percent of 48 kHz, monotonic timestamps, and thermal status 0.
The browser-oriented prototype therefore keeps H.264 but treats 24 Mbit/s as a
measured Pixel 6 profile, not a universal constant.

Camera2 supplies one result for each eight-request high-speed batch on both
development devices, so result callback count appears near 30 Hz. The probe
uses the result frame number to pair each sampled Camera2 sensor timestamp with
the corresponding MediaCodec output ordinal, and rejects the stream unless
the measured offset is stable and the final Camera2 frame accounts for every
encoder output. It uses all MediaCodec presentation timestamps to evaluate the
encoded 240 fps stream. It also rejects an encoded timestamp gap, duplicate or
nonmonotonic timestamp, missing EOS, non-REALTIME camera clock, audio outside
the simultaneous camera window, or an invalid audio clock rate.

The 2026-08-15 Pixel 5a evaluation rejected 1920x1080p240 H.264 at 24 Mbit/s:
it produced 680 frames at approximately 228.49 fps and contained a 71.18 ms
timestamp gap. A separate 1280x720p240 H.264 profile at 12 Mbit/s passed the
canonical gate with 712 total encoder frames, 709 inside the three-second
acceptance window, approximately 239.15 measured fps, a 4.320 ms maximum
adjacent timestamp delta, and thermal status 0. The Qualcomm encoder clock was
approximately 196 seconds behind the Camera2 REALTIME clock, but all 89 batch
samples measured the ordinal mapping within a 989 ns span. The clock mapping is
measured per run; no device- or model-specific offset is hard-coded.

Local debug APK installation on the Pixel 5a initially required Play Protect
confirmation. Disable the Developer options setting **Verify apps over USB**
for unattended HIL; disabling the broader Play Protect scanner should not be
necessary. This affects developer provisioning, not the capture profile.

## Implemented continuous node

Both development phones now have a repository-owned continuous single-node
path with independent profiles:

- a camera-and-microphone foreground service owns a partial wake lock only
  while armed, so capture is independent of the browser and activity lifecycle;
- Camera2 feeds the hardware H.264 encoder directly through an input surface at
  1920x1080p240 on Pixel 6 or 1280x720p240 on Pixel 5a; raw frames never cross
  Java or JNI;
- a fixed 48 MiB block pool retains at most four seconds or 1,200 compressed
  access units. Triggered snapshots lease pool entries so encoding can continue
  while an earlier swing is muxed;
- local 48 kHz mono `AudioRecord` processing uses an adaptive detector and
  validated BOOTTIME frame-position mapping. It freezes 1.4 seconds before and
  500 ms after impact, extending the start to the preceding IDR;
- `MediaMuxer` writes real-time AVC/MP4 and per-frame metadata into an
  application-private sibling directory, then atomically publishes the pair;
- audio HIL retains its exact two-second canonical PCM contract for timing
  qualification. Independently, production keeps a fixed 60-second/5.8 MiB
  mono PCM16 flight recorder on the existing `AudioRecord` stream. A published
  capture detaches up to ten seconds before and two seconds after its marker as
  `diagnostic_audio.wav`; no second microphone recorder is opened;
- each session atomically publishes a canonical diagnostic incident alongside
  its MP4, manifest, and optional WAV. User classification, a bounded note, and
  signed desired-start/visual-impact/audio-impact labels update that incident
  with a temp-write, fsync, and rename;
- a normal arm cycle accepts exactly one swing. After publication the service
  stops capture, and the browser must arm both nodes again with a fresh shared
  session ID. This prevents a later swing from inheriting the first swing's
  cross-phone clock and trigger evidence;
- storage expiry is bounded by count, bytes, and minimum free space and refuses
  to traverse symbolic links or noncanonical session paths; and
- the browser reader accepts one or two canonical roles and frame-steps the
  phone-hosted H.264 media.

Role and profile are user configuration, not model inference. New installations
default to 720p240 at 12 Mbit/s on both nodes; 1080p240 remains selectable.
The development assignments use Pixel 6 as `down_the_line` and Pixel 5a as
`face_on`, but either phone may be assigned either role. The service snapshots
node identity, role, and profile when arming so a later UI change cannot relabel
an in-flight capture.

Every encoded frame has a Camera2 REALTIME timestamp derived from a streaming
ordinal calibration. Audio trigger uncertainty, source, sample rate, detector
evidence, actual clip bounds, and shared session ID are persisted in the
manifest. The manifest derives its AVC RFC 6381 codec string from the encoder's
actual profile, level, and codec-specific data rather than hard-coding a Pixel
codec value.

The APK hermetically packages `//web:static_app`, and the foreground service
serves it at `/` on the same port as the node API. The setup activity displays
a selectable single-node review URL containing the local control credential;
the capture phone's display may then remain off while a PC, tablet, or another
phone reviews the clips. Both capture phones ship the same assets. A dual-node
browser URL still supplies both explicit node origins and credentials; peer
discovery and a station setup wizard remain future work.

Until that wizard exists, a dual-node review bookmark has this shape (URL-encode
the two origin values in a real bookmark):

```text
http://<leader-phone>:8088/?dtl_node=http://<dtl-phone>:8088&dtl_token=<dtl-token>&face_node=http://<face-phone>:8088&face_token=<face-token>#review
```

The foreground service exposes a bounded HTTP/1.1 API on port 8088. Mutating
requests require a per-installation 192-bit Bearer credential displayed only
on the phone. Request headers/bodies, client concurrency, paths, storage, and
media ranges are bounded. Read-only metadata and media remain unauthenticated
and cleartext on the trusted LAN so browser `<video>` range requests work;
diagnostic ZIP export is credentialed because it includes microphone evidence.
Production pairing, encrypted transport, and general read authorization remain
open.

The field-diagnostic routes are:

- `POST /api/v1/capture/missed-shot` freezes the currently armed video ring and
  marks the session as an operator-reported miss;
- `POST /api/v1/sessions/{id}/feedback` strictly accepts one classification,
  an optional 500-code-unit note, and optional signed impact-relative timing
  marks; and
- `GET /api/v1/sessions/{id}/diagnostics.zip` creates a bounded, checksummed,
  uncompressed ZIP containing the immutable session artifacts, current
  diagnostic incident, and the durable paired coordination record when one is
  available. The archive never contains the control credential.

The pure diagnostics core also implements a 60-second, 300-entry, 32 MiB ring
for exact compressed 5 Hz preview inputs plus pose/controller decisions. That
ring is not yet populated on device because the low-rate pose model has not
been integrated into the foreground service; manifests state this explicitly
instead of claiming absent preview evidence.
A live Pixel 6 check passed monotonic clock exchange, 401 unauthorized control,
400 malformed authenticated JSON, session/manifest retrieval, and MP4 byte
ranges.

The browser now implements the end-to-end dual-node coordinator. It provisions
one `shared_session_id` to both nodes before arm, collects repeated
four-timestamp samples from `GET /api/v1/clock`, and reads each
role/node/local-trigger/uncertainty report from
`GET /api/v1/capture/trigger-report`. It intersects clock-offset bounds and
explicitly rejects missing, late, replayed, duplicate-role, same-node,
unrelated, ambiguous, or excessive-uncertainty pairs. An accepted alignment is
stored through authenticated `GET`/`POST`
`/api/v1/coordination/{shared_session_id}` as an immutable record on both
phones. The browser recovers that record after reload and refuses conflicting
replicas. Discovery, production pairing/security, setup preview, long-duration
lifecycle, and thermal qualification remain open.

## Validated physical milestones

The successful strict sequential two-phone run is preserved at
[`artifacts/android_dual_hil_20260815T1744Z`](../artifacts/android_dual_hil_20260815T1744Z).
Its aggregate report passed distinct node identities, complete roles, one
shared session ID, and the same `tag36h11` AprilTag ID 0 in both decoded clips.
Each role also passed retained PCM tone analysis, exact MP4 frame-count decode,
and the 20 ms optical/audio correlation bound:

| Role/device | Profile | Retained video | Local audio peak / noise / floor | Retained pre/post | Local frame residual / correlation bound |
|---|---|---|---|---|---|
| `down_the_line`, Pixel 6 | 1920x1080p240, 24 Mbit/s | 545 frames, 6,510,671 bytes, 238.962 measured sensor fps | 0.025604 / 0.001553 / 0.015 | 1.776 s / 0.500 s | 1.498 ms / 9.746 ms |
| `face_on`, Pixel 5a | 1280x720p240, 12 Mbit/s | 512 frames, 3,212,249 bytes, 239.321 measured sensor fps | 0.012970 / 0.000592 / 0.012 | 1.634 s / 0.501 s | 0.439 ms / 14.976 ms |

This result is deliberately scoped as `per_phone_local_sequential_gate`. Its
report says `camera_jobs_concurrent=false` and
`concurrent_capture_validated=false`: it proves both local pipelines and their
cross-node identities/evidence contracts, not that one Feather event was
captured concurrently by both phones.

The subsequent concurrent one-event result is preserved at
[`artifacts/android_dual_hil_concurrent_20260815T105807Z/report.json`](../artifacts/android_dual_hil_concurrent_20260815T105807Z/report.json).
It passed with `camera_jobs_concurrent=true`,
`single_feather_swing_count=1`, and
`coordination_scope=one_event_dual_phone_concurrent`. Pixel 6 captured the
`down_the_line` 1920x1080p240 profile at 24 Mbit/s; Pixel 5a captured the
`face_on` 1280x720p240 profile at 12 Mbit/s. Their optical/audio total bounds
were 6.377 ms and 14.100 ms respectively, both below the 20 ms acceptance
limit. Both clips decoded exactly and retained `tag36h11` ID 0 before, at, and
after impact. The admitted coordination record was created and read back at
revision 1 on both phones, proving durable replication rather than only
browser-memory association.

After 720p240 became the standard on both roles, the concurrent gate passed
again with one Feather event. Its complete evidence is preserved at
[`artifacts/android_dual_hil_concurrent_720p_20260815T142302Z`](../artifacts/android_dual_hil_concurrent_720p_20260815T142302Z).
Both nodes captured 1280x720p240 H.264 at 12 Mbit/s, decoded exactly, retained
`tag36h11` ID 0 before/at/after impact, detected the localized LED sequence,
and replicated the accepted coordination record. Pixel 6 and Pixel 5a measured
239.042 and 239.338 sensor fps; their conservative local optical/audio bounds
were 5.659 and 16.394 ms, within the 20 ms gate.

The successful Pixel 6 screen-off run is preserved at
[`artifacts/android_screen_off_hil_20260815T1745Z`](../artifacts/android_screen_off_hil_20260815T1745Z).
After full pre-roll and a one-second quiet interval, the runner confirmed the
device was non-interactive and its display was off, sent the 20 ms, 2 kHz,
125-permille HIL-only tone, and received a successful `local_audio` retained
session. The clip contains 652 frames and 7,734,588 encoded bytes with 2.221 s
of actual pre-roll, 0.503 s of post-roll, and a 0.550 ms local nearest-frame
residual. Its trigger measured 0.023285 peak over 0.000579 noise at the unchanged
Pixel 6 floor of 0.015. The retained WAV contains 96,001 mono 48 kHz samples
(192,046 bytes). Unconditional cleanup woke the display and force-stopped the
app successfully.

The same short screen-off gate passed after 720p240 became standard, without a
profile override. Evidence is preserved at
[`artifacts/android_screen_off_720p_20260815T142216Z`](../artifacts/android_screen_off_720p_20260815T142216Z).
The runner confirmed non-interactive/display-OFF state before the Feather tone,
reconfirmed that capture remained armed, and validated the retained 720p MP4
and manifest.

The requested Pixel 6 15-minute continuous-capture evidence is preserved at
[`artifacts/android_soak_20260815T193311Z`](../artifacts/android_soak_20260815T193311Z).
It ran the 1920x1080p240, 24 Mbit/s camera/encoder, microphone, impact detector,
and bounded retention ring for 900.192 device-monotonic seconds. Automatic
publication was suppressed during this specific between-shot isolation, then
one explicit terminal trigger published a valid 494-frame, 1920x1080 H.264 MP4.
The final clip spans 2.066 seconds; retained sensor deltas remained between
3.945 and 4.419 ms. Aggregate encoded and audio rates were 239.077 fps and
48,000.44 Hz, 215,817 access units were encoded, the ring remained about
10.9 MiB, Java heap peaked at 86,023,392 bytes, and native heap peaked at
34,493,520 bytes.

That run is a thermal failure, not a continuity pass for production. It began
already warm after earlier iterations: battery temperature rose from 30.1 to
35.3 degrees C and thermal headroom from 0.612 to 1.013. Android reported
`NONE` through 120 seconds, `LIGHT` from 150 through 360 seconds, `MODERATE`
from 390 through 720 seconds, and `SEVERE` from 750 seconds through the end.
The target therefore failed its maximum-`MODERATE` acceptance criterion even
though frame cadence remained stable.

A preceding publication-heavy attempt is preserved at
[`artifacts/android_soak_failure_20260815T191620Z`](../artifacts/android_soak_failure_20260815T191620Z).
It allowed the production detector to publish three incidental clips, then
failed after 5 minutes 27 seconds when Camera2/MediaCodec ordinal correlation
shifted by one 240 fps frame. That failure is retained as real evidence: it
suggests concurrent clip publication and false triggers reduce the available
margin further. A separate ambient-trigger artifact is preserved at
[`artifacts/android_soak_incidental_trigger_20260815T190727Z`](../artifacts/android_soak_incidental_trigger_20260815T190727Z).

The short gates remain valid functional evidence, but the 15-minute result
means indefinite 1080p240 arming on an uncooled Pixel 6 is not qualified.

The controlled 720p240 comparison is preserved at
[`artifacts/android_soak_720p_20260815T135742Z`](../artifacts/android_soak_720p_20260815T135742Z).
It began cooled at thermal status `NONE` and 26.7 degrees C battery temperature,
then ran 1280x720p240 H.264 at 12 Mbit/s for 900.234 device-monotonic seconds.
All 31 telemetry samples were complete, aggregate encoded and audio rates were
239.064 fps and 47,998.18 Hz, and one explicit terminal trigger published a
valid 497-frame, 1280x720 H.264 MP4. The approximately four-second encoded ring
peaked at 5,962,367 bytes, half the 1080p run's peak of 11,924,536 bytes.

The 720p run first reported `LIGHT` at 300 seconds and `MODERATE` at 630
seconds, compared with 150 and 390 seconds at 1080p. It never reached `SEVERE`,
ended at 34.6 degrees C and headroom 0.978, maintained frame cadence throughout,
and passed the maximum-`MODERATE` acceptance criterion. The different starting
battery temperatures make this evidence operational rather than a calorimetric
comparison, but the delayed thermal transitions and halved ring size establish
720p240 as the better current continuous-arm profile on Pixel 6.

## Target node architecture

Each phone runs the same application and owns only its local hardware:

```text
Camera2 constrained high-speed session -- camera timestamps -- encoded rolling retention
                                                               |
AudioRecord -- local timestamps -- impact detector -------------+-- frozen clip + evidence
                                                               |
role/configuration + HTTP service -------------------------------+-- station browser/coordinator
```

The camera, microphone, encoder, and retention loop must continue without a
browser connection. A transient Wi-Fi delay must not cause a missed swing.
Network messages associate the two completed views with one session; they
should not be the mechanism that starts high-speed capture after impact.

The implementation evaluates local impact detection on both phones. Each node
has the best local relationship between its microphone and camera timestamps
and retains enough pre- and post-impact data without waiting for the peer. The
coordinator reconciles the two trigger reports and rejects a pair whose timing
evidence does not agree. A single designated trigger phone remains an option,
but only after measured network and acoustic latency show that it improves the
result.

Raw 1080p frames are too expensive for a long 240 fps ring, so the implemented
node uses continuous hardware encoding with bounded encoded retention and an
explicit memory budget. It retains per-frame capture timestamps independently
of presentation timestamps. The stock slow-motion file format is not the
internal timing model.

## Android platform boundary

The application needs a small Android-owned layer for:

- `Camera2` capability discovery and
  `createConstrainedHighSpeedCaptureSession`;
- `AudioRecord` configuration and timestamp collection;
- `MediaCodec` input surfaces, output draining, and codec capability probes;
- camera/microphone runtime permissions and the camera/microphone foreground
  service types;
- network discovery, pairing, and authenticated local service lifecycle; and
- power, screen-lock, reboot, and thermal callbacks.

Portable trigger, clip-planning, validation, and session-schema code should be
shared with the existing C++ core where that produces a clean tested boundary.
JNI must not be introduced merely to reuse a small amount of code; the Android
framework and lifecycle boundary should remain easy to diagnose.

The implemented camera/microphone foreground service and armed-only partial
wake lock are the screen-off mechanism. The short screen-off HIL has confirmed
that local audio capture succeeds while the Pixel 6 is non-interactive with its
display off. This is not a reboot or long-duration lifecycle qualification:
Android still requires a physical first unlock after reboot before
credential-encrypted application state is available. The Developer options
`Stay awake` setting must be disabled for normal station operation. The app does
not request a display wake lock, so the normal system timeout or a press of the
power button turns the panel off while the foreground service keeps capture
running. A normal Android app cannot force an immediate real screen-off without
device-administrator/device-owner privileges; this project deliberately avoids
that security-sensitive provisioning and does not mislabel zero brightness as
screen-off.

## Roles and station configuration

The canonical roles remain `down_the_line` and `face_on`. User-facing text may
describe `face_on` as across-the-line, but stored manifests and APIs should use
one stable name.

Role and profile assignment are persisted app configuration, not an inference
from model, USB port, IP address, discovery order, or which phone detected
impact first. The current UI allows either canonical role and either supported
profile; it rejects changing capture identity while a capture is starting,
armed, retaining post-roll, or publishing. A future pairing/setup flow should:

1. discover or pair both application installations;
2. show a live preview and stable installation identity for each node;
3. let the user assign each distinct node to one role;
4. reject missing or duplicate roles; and
5. persist and include the verified mapping in every session manifest.

ADB serials are useful development identities but are not an appropriate
production API. Each app installation should create a stable random node ID,
present a human-readable device label, and support deliberate re-pairing or
identity reset. The current development mapping is:

```text
down_the_line = Pixel 6, ADB serial 22181FDF6005QH
face_on       = Pixel 5a, ADB serial 1A011JEG501717
```

## Timing and synchronization

Two phones do not share a sensor clock. Wall-clock or Wi-Fi arrival times are
not adequate frame-alignment evidence. The implementation must preserve:

- Camera2 sensor timestamps and the reported timestamp-source capability;
- audio frame positions and timestamps from each phone;
- the local monotonic times of trigger observation, confirmation, and clip
  boundaries;
- repeated clock-exchange samples between nodes, including round-trip bounds;
  and
- post-capture optical/audio correlation evidence where available.

The design should first establish camera-to-audio mapping independently on
each device, then estimate cross-phone alignment with bounded uncertainty.
AprilTag presence remains a focus/framing check. The Feather NeoPixel and tone
remain the deterministic HIL stimulus for measuring whether the claimed
cross-phone and audio-to-frame alignment is defensible.

The Feather is not the limiting clock in current evidence: its white and tone
commands differed by only 1 microsecond in both concurrent runs. The Android
local bound is dominated by the optical edge interval. A decoded lit tile does
not identify the exact rolling-shutter row exposure, so the analyzer brackets
onset with the adjacent retained sensor timestamps, an approximately 8.3 ms
interval at 240 fps. AudioRecord-to-BOOTTIME uncertainty contributed only
0.328-0.396 ms and decoded-media PTS residual contributed 0.193-0.324 ms in the
all-720p run. The Pixel 5a also observed the speaker later relative to its local
trigger, producing the wider 16.394 ms bound. Cross-phone association then adds
separate Wi-Fi clock-exchange uncertainty; it does not improve either local
optical edge.

The Daheng path has a simpler measurement chain: raw Bayer frames, fixed 500 us
exposure, no codec reorder, and both cameras plus microphone terminate at one
host. It can therefore associate raw exposure evidence without Android's
Camera2-to-MediaCodec ordinal bridge. It is not a calibrated sub-millisecond
ground truth, however: its own reports retain about 2.4 ms camera clock-mapping
uncertainty and assume up to one nominal frame of unobservable camera/USB
delivery latency. Electrical hardware triggering has not been configured. The
Pixel 6's current 5.659 ms conservative local bound is therefore competitive
with the qualified Daheng evidence; Pixel 5a remains materially worse.

## Network and browser service

Both phones serve data directly on the trusted station LAN. The current API
provides stable identity/status/session/manifest/media routes, bounded byte
ranges and storage, Bearer-authenticated state changes, a coordinator-supplied
shared session ID, clock samples, trigger reports, and immutable app-private
coordination records. The browser supports both current paths:

```text
http://<web-app-address>/?node=http://<phone-address>:8088
http://<web-app-address>/?dtl_node=http://<dtl-address>:8088&dtl_token=<dtl-token>&face_node=http://<face-address>:8088&face_token=<face-token>
```

The single-node form lists and reviews one phone's sessions. The dual-node form
requires exactly one endpoint for each role, arms both with one shared ID,
performs repeated clock exchange, admits the trigger pair, composes the
platform-neutral two-view manifest, and writes the accepted coordination
record to both phones. Authenticated coordination `POST` is create-only: an
identical retry is idempotent and conflicting content is rejected. It still
needs:

- an authenticated pairing step and credential rotation/revocation;
- TLS or another protected transport plus authorization for read-only media;
- readiness, thermal, and storage detail beyond the current capture counters;
- reconnectable status updates over WebSocket or server-sent events;
- discovery and setup preview.

The existing browser review UI consumes platform-neutral session metadata and
encoded media; it does not depend on Camera2 objects or Android lifecycle
state.

## Bazel and repository layout

Bazel remains the only supported build and test entry point. The repository
already pins the Android SDK and platform tools through Bzlmod. ADB is available
without a system installation:

```bash
bazel run //tools/android:adb -- devices -l
```

The Android application is built as normal Bazel targets with pinned
dependencies. Host-side protocol, schema, timing, and media tests remain
hermetic. Tests that install an APK or touch a phone must be tagged `manual`,
`local`, and `exclusive`, and the ordinary `bazel test //...` suite must remain
hardware-independent.

Build the current app with:

```bash
bazel build //android/app:swing_capture
```

Run the short physical gate on exactly one phone with:

```bash
bazel test //android/hil:android_high_speed_probe_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_ROLE=down_the_line \
  --test_env=SWING_CAPTURE_ANDROID_PROFILE=720p240 \
  --test_output=streamed --nocache_test_results
```

The target uses Bazel's hermetic ADB and APK runfiles, has a 15-second overall
deadline, and publishes the durable JSON report as a Bazel undeclared test
output. The serial above is development-station evidence, not a product
default.

The profile environment variable defaults to `720p240`; specify `1080p240`
explicitly for the optional Pixel 6 full-HD path. Profiles express requested
capture performance and the runner verifies the exact result. Capture-profile
selection is not inferred from the model. The only current model-specific
behavior is the isolated Pixel 5a audio-source and 0.012 detector-floor policy
described above; Pixel 6 and unknown devices retain the complete default
detector configuration including the 0.015 floor.

The retained-capture gate additionally validates and exports the atomically
published manifest and MP4:

```bash
bazel test //android/hil:android_retained_capture_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_ROLE=down_the_line \
  --test_env=SWING_CAPTURE_ANDROID_PROFILE=720p240 \
  --test_output=streamed --nocache_test_results
```

The production-shaped continuous manual and local-audio gates are:

```bash
bazel test //android/hil:android_continuous_capture_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_ROLE=down_the_line \
  --test_env=SWING_CAPTURE_ANDROID_PROFILE=720p240 \
  --test_output=streamed --nocache_test_results

bazel test //android/hil:android_audio_trigger_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_ROLE=down_the_line \
  --test_env=SWING_CAPTURE_ANDROID_PROFILE=720p240 \
  --test_output=streamed --nocache_test_results

bazel test //android/hil:android_audio_trigger_screen_off_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_ROLE=down_the_line \
  --test_env=SWING_CAPTURE_ANDROID_PROFILE=720p240 \
  --test_output=streamed --nocache_test_results

bazel test //android/hil:android_continuous_15_minute_soak_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_ROLE=down_the_line \
  --test_env=SWING_CAPTURE_ANDROID_PROFILE=720p240 \
  --test_output=streamed --nocache_test_results
```

The audio targets also require the configured Feather RP2040. They validate a
one-second quiet interval and use the HIL-only 20 ms, 2 kHz, 125-permille tone.
Both require `trigger.source=local_audio`, validate the shared session ID, and
export the retained manifest, MP4, and PCM evidence. The screen-off variant
waits for full pre-roll, turns the display off, verifies the phone is
non-interactive and its display is off, rechecks that capture is still armed,
then sends the tone. Cleanup always wakes the display and force-stops the app.
The 15-minute target samples in-process thermal status/headroom, battery,
encoded/audio counters, bounded-ring occupancy, Java/native heap, and usable
storage every 30 seconds. It suppresses automatic publication to isolate the
between-shot continuous workload, explicitly publishes one terminal clip, and
fails if the thermal status exceeds `MODERATE`, aggregate rates leave their
bounds, telemetry is incomplete, or any continuity error occurs.

The strict two-phone sequential gate is:

```bash
bazel test //android/dual_hil:dual_phone_sequential_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_DTL_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_SERIAL=1A011JEG501717 \
  --test_output=streamed --nocache_test_results
```

It also requires `SWING_CAPTURE_STATION_CONFIG` to resolve the Feather. It runs
both 720p240/12 Mbit/s role jobs strictly one after the other under one shared
session ID. For each node it enforces a quiet
pre-stimulus interval, local-audio trigger, bounded pre/post-roll, exact MP4
decode count, commanded-tone PCM evidence, AprilTag persistence, and a bounded
optical/audio correlation. It then requires distinct node identities, complete
roles, the shared ID, and the same AprilTag identity. Every capture, pull,
decode, diagnostic, and audio-analysis stage is bounded to at most 15 seconds.
The current synthetic-swing fixture uses a 10 ms, 2 kHz,
125-permille HIL-only tone concurrent with its 20 ms white optical marker.

The separate target
`//android/dual_hil:dual_phone_concurrent_hil_test` is the physical acceptance
gate for one event captured by both phones. Its implementation arms both nodes,
collects bounded clock exchanges, runs exactly one Feather swing, validates the
pair, and persists the coordination record. The successful physical run at
[`artifacts/android_dual_hil_concurrent_20260815T105807Z/report.json`](../artifacts/android_dual_hil_concurrent_20260815T105807Z/report.json)
passed that complete contract, including durable create/read-back on both
nodes.

After the foreground service has been launched, a development browser can
review its latest sessions at `http://<phone-address>:8088`. Serve the
checked-in web app and point it at that node with a query parameter:

```text
http://<web-app-address>/?node=http://<phone-address>:8088
```

Add `&node_token=<credential-shown-on-phone>` only when exercising mutating
controls. Treat browser history containing that development credential as
sensitive.

For the dual-node coordinator, use both role-specific endpoint and credential
pairs shown in the Network and browser service section. Supplying only one of
`dtl_node` or `face_node` is rejected.

The manual browser HIL facade runs the ordinary fixture workflow plus the
environment-enabled live-node case. The live paired H.264 gate requires a host
Chrome with proprietary H.264 support; the bundled Chromium headless shell
lacks that codec in this environment. Pass both node origins, both bearer
tokens, and the Chrome executable explicitly:

```bash
bazel test //web:android_browser_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_DTL_NODE_URL=http://dtl-phone:4315 \
  --test_env=SWING_CAPTURE_ANDROID_DTL_TOKEN=replace_with_dtl_node_token \
  --test_env=SWING_CAPTURE_ANDROID_FACE_NODE_URL=http://face-phone:4315 \
  --test_env=SWING_CAPTURE_ANDROID_FACE_TOKEN=replace_with_face_node_token \
  --test_env=SWING_CAPTURE_CHROME_EXECUTABLE=/usr/bin/google-chrome \
  --test_output=streamed --nocache_test_results
```

All four node variables are an all-or-none contract. The successful live run
decoded, sought, stepped, and played both phone-hosted H.264 views through HTTP
206 byte ranges and preserved
[`artifacts/android_browser_hil_20260815T110817Z/outputs/android-dual-node-review.png`](../artifacts/android_browser_hil_20260815T110817Z/outputs/android-dual-node-review.png).

## Delivery phases and acceptance gates

### 1. Capability probe

- Build and install a minimal Bazel-owned APK.
- Enumerate rear-camera high-speed combinations and encoder capabilities.
- Run a bounded 240 fps stream with camera timestamps and frame counters.
- Test simultaneous `AudioRecord` without encoding or camera stalls.
- Record temperature, thermal status, dropped frames, storage rate, and power.

The short capability gates are complete for Pixel 6 1080p240 at 24 Mbit/s and
Pixel 5a 720p240 at 12 Mbit/s. Pixel 5a 1080p240 was rejected for a real frame
gap. Pixel 6 1080p240 maintained cadence for 15 minutes but reached `SEVERE`;
the controlled Pixel 6 720p240 run maintained the same cadence and completed
the gate at `MODERATE`. Sustained publication-heavy and screen-off behavior
remain qualification gaps, especially for the provisional Pixel 5a path.

### 2. Single-node capture

- Implement continuous bounded retention, local impact detection, and
  deterministic pre/post-roll freezing.
- Preserve real-time per-frame metadata through hardware encoding.
- Publish one session through a local API and review it in the browser.
- Continue capturing correctly when the browser disconnects or the screen
  locks.

Continuous retention, local impact detection, atomic publication, bounded
storage, single-node browser review, and the foreground lifecycle are
implemented. The short screen-off HIL passed on Pixel 6. The non-screen-off
15-minute Pixel 6 1080p240 run maintained continuity but failed thermal
acceptance; the controlled 720p240 run passed without exceeding `MODERATE`.
The publication-heavy precursor lost Camera2/encoder ordinal alignment after
5 minutes 27 seconds. Screen-off thermal qualification, 720p publication-heavy
qualification, reboot recovery, and fault recovery remain.

### 3. Dual-node sessions

- Pair two nodes and configure roles in the UI.
- Associate one physical swing with exactly two clips.
- Measure and report clock uncertainty and audio/optical alignment.
- Handle one missing, late, hot, full, or disconnected node explicitly.

The deterministic clock estimator, trigger-report model, pair admission,
browser coordinator, combined dual-view manifest, and durable record
replication to both phone APIs are implemented and hermetically tested. The
strict sequential physical gate passed both local pipelines and cross-node
identity contracts, and the one-event concurrent physical gate passed both
captures, bounded timing, and durable coordination. The live browser HIL also
passed paired H.264 review. Discovery and production pairing remain.

### 4. Qualification

- Add deterministic software tests for every timing and retention boundary.
- Add short per-device and dual-device Bazel HIL targets.
- Inspect retained reports and both diagnostic images for every physical run.
- Run longer thermal qualification only when explicitly requested.
- Decide whether the Pixel 5a remains supported before its workarounds become a
  permanent architectural constraint.

## Open decisions

- Whether later Pixel 5a thermal and end-to-end trigger qualification justify
  retaining its provisional 720p240 profile.
- Whether the passing Pixel 6 720p240 isolation remains at or below `MODERATE`
  with production trigger/publication load and with the screen explicitly off;
  the corresponding 1080p240 isolation reached `SEVERE`.
- Whether the current one-second AVC GOP and IDR-extended pre-roll provide
  sufficiently exact browser frame access on all target browsers.
- Whether both phones continue detecting impact or one node becomes the
  authoritative trigger after further latency tests; the validated current
  implementation detects locally on both.
- The measured uncertainty threshold for accepting a physical dual-view
  session; the interval-based clock model is implemented.
- Whether the browser-owned coordinator remains the product architecture or
  pairing/session ownership moves to one phone.
- The minimum supported Android/API/device capability set after the Pixel 6
  prototype is qualified.
