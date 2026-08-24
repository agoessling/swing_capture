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
phone-leader-owned dual-node lifecycle with durable coordination evidence. Short
single-device, sequential two-device, screen-off, concurrent one-event, and
live paired-browser HIL milestones have passed. The paired pose-standby gate
also passed with real 5 Hz inference on both phones, production peer arming,
concurrent warm transition to 720p240, and one Feather event. A requested
15-minute Pixel 6 1080p240 continuous-capture run completed without a
frame-continuity failure, but it failed thermal acceptance after reaching
Android thermal status `SEVERE`. A controlled 15-minute Pixel 6 720p240 run
subsequently passed the same isolation gate without exceeding `MODERATE`.
Publication-heavy and screen-off thermal qualification, the five-minute
qualification workflow, and a 30-minute soak have not passed and are not
claimed here.

The existing Daheng implementation remains the qualified behavioral reference
while this work is experimental. It should not be deleted until the Android
path has demonstrated the required capture duration, trigger behavior,
dual-view timing evidence, thermal stability, media publication, and browser
workflow. Sharing a repository does not require the two hardware paths to share
their lowest-level camera implementation.

The Pixel 6 is the reference Android device and the first field station assigns
it the `face_on` (also called across-the-line) leader role. The Pixel 5a is the
`down_the_line` shadow. These are evidence-backed machine-local defaults, not
model-to-role rules. The application lets a user inspect each preview and
assign either connected phone to either role.

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
`Pixel 5a`, compared case-insensitively, gets a 0.010 minimum peak. This policy
is independent of role and capture profile. The separate Pixel 5a
`VOICE_RECOGNITION` microphone-source compatibility branch is likewise
isolated; neither accommodation trades away Pixel 6 performance or tuning.
The exact-model floor was selected after a concurrent HIL attempt measured a
real 0.011505 Feather peak below the former 0.012 floor; an earlier passing run
had only 0.012238 of margin. The subsequent paired pose gate passed at 0.011139
against 0.010. The adaptive eight-times-noise-floor requirement remains in
force, so this does not make the detector a fixed low-threshold trigger.

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
  the standard 1280x720p240 on either phone; Pixel 6 also exposes an explicit
  1920x1080p240 option. Raw frames never cross Java or JNI;
- a fixed 48 MiB block pool retains at most four seconds or 1,200 compressed
  access units. Triggered snapshots lease pool entries so encoding can continue
  while an earlier swing is muxed;
- local 48 kHz mono `AudioRecord` processing uses an adaptive detector and
  validated BOOTTIME frame-position mapping. During paired pose capture only
  the configured leader makes the terminal impact decision; its authenticated
  impact request freezes the shadow ring. The shadow continuously records a
  bounded ring of validated local candidates. The leader measures the shadow's
  BOOTTIME offset using repeated four-timestamp exchanges, maps its strike into
  the shadow clock, and carries that bounded mapping in the authenticated
  schema-2 request. It waits at
  most 75 ms for late detector evidence, then chooses the closest candidate
  within 80 ms plus measured clock and audio uncertainty. This prevents an
  unrelated Pixel 5a transient from ending one side of the pair early while
  preserving a local strike timestamp on both clips. Clips retain 1.4 seconds
  before and 500 ms after impact, extending the start to the preceding IDR;
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
The first field station uses Pixel 6 as `face_on` and Pixel 5a as
`down_the_line`, but either phone may be assigned either role. The service
snapshots node identity, role, and profile when arming so a later UI change
cannot relabel an in-flight capture.

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
phone reviews the clips. Both capture phones ship the same assets. The browser
Phone setup flow configures one phone or two explicit phone origins through
authenticated, revision-checked setup endpoints. Its identity-binding ceremony shows the stable
node ID, role, current origin, and a human-readable label obtained through each authenticated setup
connection. Android NSD/mDNS advertises and resolves `_swing-capture._tcp`; its expiring TXT metadata
is displayed only as an untrusted address hint. Selecting a hint requires the peer's write-only
control token, and both the browser and leader authenticate the peer identity before the leader
commits the address. The leader durably owns the non-secret identity binding and credential
generation in app-private preferences. Clearing it records a revocation tombstone, and forgetting
revoked metadata requires typing the full peer node ID. Address recovery accepts a changed origin
only when the authenticated stable identity and expected role are unchanged. Live setup preview
reuses the pose standby stream without owning a Camera2 object: at most one detached 640x360 input
per second is encoded when debug evidence is disabled, while the debug path reuses its existing
JPEG. The provider retains one active plus one latest pending frame, rejects callbacks from an old
camera generation, and reports stale or unavailable data while 240 fps owns the camera. The browser
fetches each JPEG with the current Bearer credential, never places credentials in the URL, corrects
portrait sensor rotation, and revokes superseded Blob URLs.
For pose capture, a ready two-phone station has distinct camera roles, exactly
one leader, and one shadow; the leader must resolve its stored peer origin and
verify that peer's distinct node ID, opposite role, and shadow mode. Setup
readiness and updates reject any other pose topology. Only the leader's local
audio detector terminates a paired attempt. The shadow records local detector
candidates for timing but freezes its ring after the authenticated leader
impact request. Transient impact delivery is attempted up to three times; a
separate 1 Hz exchange refreshes a three-sample peer clock estimate. The shadow
keeps 32 audio candidates and selects the event closest to the mapped leader
strike after a bounded 150 ms wait. Estimates older than 10 seconds or wider
than 25 ms are not trusted. The 25 ms value is a provisional, fail-closed implementation bound,
not a calibrated product threshold. Six same-pair/same-network direct-LAN successes retained
9.625--22.628 ms composed mapping uncertainty, while one historical high-jitter batch reached
117.789 ms and was rejected. The exact report-by-report audit and remaining field evidence gap are
recorded in `docs/pair_clock_evidence_20260822.md`. The current bound covers the measured 43 ms best
screen-off phone-to-phone Wi-Fi HTTP round trip while remaining well inside
the separate 80 ms candidate window. With a valid estimate but no candidate in the
80 ms-plus-uncertainty window, peer-arrival time is used instead of an
unrelated transient. If no usable estimate exists, the prior conservative
latest-candidate-within-250-ms policy remains as a fallback before arrival
time. Peer clock age, round-trip bounds, and uncertainty are exposed in pose
status for field diagnosis. Mapping uncertainty composes the clock-offset and
leader audio-timestamp uncertainty with overflow checks; candidate selection
also includes the shadow candidate's local audio uncertainty. The shadow
validates that the mapped target names its own node before use and retains the
accepted mapping, policy decision, selected source, and fallback semantics in
status. Schema-1 requests remain a staged-upgrade fallback.

A dual-node review bookmark has this shape (URL-encode the two origin values in
a real bookmark):

```text
http://<leader-phone>:8088/?dtl_node=http://<dtl-phone>:8088&dtl_token=<dtl-token>&face_node=http://<face-phone>:8088&face_token=<face-token>#review
```

The foreground service exposes a bounded HTTP/1.1 API on port 8088. Mutations and every
operational, setup, pairing, capture, session, diagnostic, and coordination metadata read require a
per-installation 192-bit Bearer credential displayed only on the phone. New API reads fail closed.
Only `/api/v1/clock` remains public as an explicitly untrusted, non-cacheable time hint that never
returns control-token material. Its `node_id` is also only a hint: clock consumers must match it to
the identity previously learned from the Bearer-authenticated pairing endpoint before admitting a
sample. `/api/v1/node` identity requires the destination Bearer credential.
Request headers/bodies, client concurrency, paths, storage, and media ranges are bounded. Because
browser `<video>` range requests cannot attach an Authorization header, an authenticated
manifest/catalog grants an HMAC-SHA256 capability scoped to exactly one immutable session or field
recording; another session, an appended query, or control-credential rotation invalidates it.
Capability-bearing manifests and catalogs use `Cache-Control: private, no-store`. Peer and preflight
clients reject HTTP redirects before sending Bearer credentials to a different origin. Peer requests
can use Bearer directly. Peer configuration and all native peer clients share one exact-origin
security policy: `http://host:port` is classified as `cleartext_trusted_lan_demo`, while
`https://host:port` is classified as `protected_https` and uses the platform HTTPS certificate
validation path. Setup and pairing responses expose that classification. The policy also provides
a fail-closed protected-only boundary, and an active HTTPS identity binding cannot silently move to
HTTP during authenticated address recovery; the operator must revoke and explicitly re-pair.

The current embedded phone server and browser bootstrap still use HTTP, so credentials, hints,
metadata, capabilities, and media bytes remain observable on the trusted demo LAN. Moving the
product to the protected-only boundary will use the station-local CA, per-installation Android
Keystore identity, and attended bootstrap specified in the dependency-ordered closure plan below.
Phone TLS termination, peer and browser migration, rotation/recovery, and the final cleartext-off
cutover remain unimplemented. No permissive trust manager or self-signed-certificate bypass is
present.

`POST /api/v1/control-credential/rotate` is the only network response that returns a local control
token. The request must use the current Bearer token and supply the current setup revision plus the
full local node ID as explicit confirmation. Capture must be stopped. Its one-time response returns
the new 192-bit token and generation so the initiating browser can continue; every prior token is
invalid immediately. Other browsers and any leader that controls this phone must enter the new
token and explicitly re-pair.

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
for every 5 Hz pose/controller row plus sparse preview images. The foreground
service populates that ring when debug evidence is enabled and publishes its
thermally bounded one-Hz JPEGs, priority arm frame, complete pose trace, and
controller decisions through the diagnostic archive. The normal non-debug path
avoids retaining that additional evidence.
A live Pixel 6 check passed monotonic clock exchange, 401 unauthorized control,
400 malformed authenticated JSON, session/manifest retrieval, and MP4 byte
ranges.

The configured phone leader now owns the end-to-end dual-node lifecycle: it creates the shared
session ID, dispatches arm and impact, collects bounded clock/evidence samples, admits the pair,
stores the immutable coordination record locally, and replicates it to the shadow. The browser is
a setup and review client. It can still fetch and validate the same trigger reports and immutable
records after reload, but capture correctness does not depend on an open browser. Pair admission
explicitly rejects missing, late, replayed, duplicate-role, same-node, unrelated, ambiguous, or
excessive-uncertainty evidence. Protected transport and read authorization, long-duration
lifecycle, and thermal qualification remain open. The two-phone discovery,
credential-rotation, and re-pair ceremony has passed its bounded physical gate.

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
role/configuration + HTTP service -------------------------------+-- phone leader lifecycle
                                                               +-- browser setup/review
```

The camera, microphone, encoder, and retention loop must continue without a
browser connection. A transient Wi-Fi delay must not cause a missed swing.
Network messages associate the two completed views with one session; they
should not be the mechanism that starts high-speed capture after impact.

The leader's local audio detector is authoritative for capture termination. The shadow keeps its
own detector active only to select a bounded clock-mapped local acoustic candidate; if none is
available, it records the mapped peer-arrival fallback explicitly. Each phone still retains enough
pre- and post-impact data to tolerate transport delay. Pair admission reconciles both reports and
rejects timing evidence that does not agree. The `field_readiness_v2` holdout remains necessary
before this policy is treated as production-final: at least two fully reviewed phone/view-swapped
sessions must repeat every required category across two sessions.

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

The reboot boundary is now an explicit product policy rather than an implied
`START_STICKY` guarantee. The application and camera/microphone service remain
outside direct boot. One narrowly scoped direct-boot-aware receiver observes
locked-boot, completed-boot, and first-unlock broadcasts and writes only a
non-secret marker to device-protected storage. It never starts the service,
camera, microphone, networking, or capture. After an OS reboot the operator
must unlock the phone once and open Swing Capture in the foreground; the
activity then starts the camera/microphone foreground service using the already
implemented permission ceremony. `START_STICKY` remains best-effort
process-recreation behavior only. A zero-touch reboot mode would be a separate
device-owner/kiosk architecture and is not silently enabled by this APK.

Both `/api/v1/setup` and `/api/v1/status` expose `reboot_recovery`, including
the selected `operator_launch_after_first_unlock` mode, the latest durable boot
observation, current unlock/service readiness, and any remaining operator
action. This reporting and its pure policy/manifest tests are software-complete;
a physical reboot, first-unlock, launch, peer-recovery, and rearm ceremony has
not been qualified for this revision.

The explicit physical gate for closing that evidence gap is
`//android/dual_hil:dual_phone_os_reboot_ceremony_hil_test`. This target really reboots both
phones; it is not part of the normal HIL loop and must be run only with the phones locally
accessible to an operator. Use two USB ADB connections, because a wireless-debugging endpoint may
not survive an OS reboot. Before running it, leave the configured production pair stopped in
`READY`, verify the LAN origins, and keep both screens accessible:

```bash
bazel test //android/dual_hil:dual_phone_os_reboot_ceremony_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_DTL_SERIAL=1A011JEG501717 \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN=http://10.168.168.111:8088 \
  --test_env=SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN=http://10.168.168.241:8088 \
  --test_env=SWING_CAPTURE_APPROVE_OS_REBOOT_CEREMONY=I_UNDERSTAND_THIS_REBOOTS_BOTH_PHONES_AND_REQUIRES_LOCAL_UNLOCK \
  --test_output=streamed --nocache_test_results
```

The gate first refuses any node that is armed, not complementary and authentically paired, or not
running the byte-exact Bazel APK. The APK check is read-only: a mismatch fails rather than
installing. It then asks the operator not to launch the app, issues the two reboot commands, and
allows at most 120 seconds for Android boot plus first unlock. After unlock it requires both the
capture service and direct-LAN API to remain absent, then prompts the operator to open Swing
Capture on both phones. The runner never sends an activity-start command. A second 120-second
watchdog bounds this human stage; all subsequent API, rearm, peer-clock, and cleanup stages retain
the ordinary 15-second bound.

A passing `report.json` requires changed Linux boot IDs, advancing device-protected boot-marker
counts, foreground activity on both phones, exact durable node/configuration/pairing fields across
reboot, the recovered leader binding, real 5 Hz monitoring on both nodes, and successful terminal
disarm. The recovered leader clock must identify the durable shadow node, contain at least one
sample, be no older than 10 seconds, and have uncertainty no greater than 25 ms. Missing,
malformed, stale, wider, or wrong-peer clock evidence fails the software validator. The report
retains pre/post/cleanup setup and status documents under the Bazel
undeclared test outputs. No APK is installed, no configuration endpoint is called, no private file
is removed, and cleanup proves both phones finish `READY` and unarmed. Static compilation or the
software validator does not qualify the physical ceremony; only a reviewed passing artifact from
this explicitly run target can close the TODO.

## Minimum Android device floor

The APK and runtime admission check now use one explicit minimum floor:

- Android API 34 or newer;
- granted camera and microphone runtime permissions;
- a rear camera with `REALTIME` timestamps, supported sensor orientation, a
  16:9 YUV standby size of at least 640x360, and a fixed 240 fps range for the
  selected 720p or 1080p profile;
- a hardware H.264 encoder that advertises the selected size at 240 fps;
- a valid 48 kHz mono PCM16 `AudioRecord` buffer configuration; and
- OpenGL ES 3.1 or newer for the production pose GPU platform.

Arming now fails before changing coordination or camera state when the local
phone does not meet that floor. The setup and status APIs expose a structured
`device_admission` result, and the operator capability inventory retains the
full `product_floor_assessment`. The inference contract is Lite at 640x360 and
5 Hz, with delegate selection scoped to one node. The capability policy does
not receive a manufacturer or model string, so an eventual Pixel 5a fallback
cannot mutate the Pixel 6 assessment or select a lower pair-wide path. Passing
this static admission is necessary but not thermal, inference-latency, or
long-duration physical qualification.

After launching the exact APK, retain and validate both fresh inventories with the Bazel-provided
ADB tool:

```bash
bazel run //tools:android_capability_report -- \
  --serial 22181FDF6005QH --serial 1A011JEG501717 \
  --output-dir artifacts/android_product_floor_current
```

The collector is read-only, overlaps the independent pulls, refuses to overwrite existing
evidence, and emits a credential-redacted pair report. Its Bazel target supplies the current APK;
the collector pulls and host-hashes each installed monolithic `base.apk` and requires both digests
to match that Bazel artifact. Pixel 6/API 36 and Pixel 5a/API 34 pass the capability floor,
screen-off and per-node pose-policy isolation, and the installed/Bazel identity checks at
`artifacts/android_product_floor_exact_apk_pass_20260823T072145Z/report.json`. The expected APK and
both installed APKs have SHA-256
`328203380f02db24ec7d7707476cab361d3164476d5c9187549897aa64cad39a`. Re-run the collector for any
new phone or materially changed APK; do not interpret this static admission as a thermal soak.

The camera, codec, audio-format, API, and OpenGL portions are probed once when
the node service starts. Camera and microphone grants are overlaid from the
current Android permission state before every arm decision and every setup or
status response. This keeps those common checks cheap while preventing a
service-lifetime permission snapshot from contradicting current readiness.

## Roles and station configuration

The canonical roles remain `down_the_line` and `face_on`. User-facing text may
describe `face_on` as across-the-line, but stored manifests and APIs should use
one stable name.

Role and profile assignment are persisted app configuration, not an inference
from model, USB port, IP address, discovery order, or which phone detected
impact first. The current Phone setup UI allows either canonical role and
supported profile; it rejects changing capture identity while a capture is
starting, armed, retaining post-roll, or publishing. It manually associates
explicit phone origins and tokens and validates the configured peer descriptor.
The browser pairing foundation now:

1. models expiring NSD/mDNS observations without trusting advertised identity as authentication;
2. verifies stable installation identity, role, and current origin through an authenticated setup
   connection before user confirmation;
3. lets the user assign each distinct node to one role and rejects duplicate identities or roles;
4. retains only non-secret binding metadata with a human-readable label and credential generation;
5. requires explicit credential rotation, address recovery, re-pair, revocation, and full-node-ID
   reset transitions.

The Android NSD/mDNS adapter feeds this abstraction, while the verified binding is stored in the
leader's credential-encrypted app-private preferences and mirrored without secrets in browser-local
UX metadata. The Phone setup UI also supports deliberate rotation of the local installation's
global Bearer credential. Rotation is accepted only with the currently valid credential, the
current setup revision, a capture-safe runtime state, and exact confirmation of the full local node
ID. It atomically advances the credential generation and setup revision, returns the new token only
from that explicit action, and immediately rejects the old token. Ordinary GET, status, identity,
discovery, and diagnostic artifacts expose only non-secret generation or health metadata. A leader
whose peer rejects a stored token reports `re_pair_required` and requires the operator to enter the
peer's current token; transient network loss reports `unavailable` without silently revoking or
verifying it. A discovery advertisement alone can never update a binding.

The bounded two-phone qualification is explicit and excluded from the default suite:

```bash
bazel test //android/dual_hil:dual_phone_discovery_pairing_hil_test \
  --test_output=streamed --nocache_test_results
```

It uses the two serials and LAN origins from the station environment, installs and starts the final
APK on both phones, snapshots and restores each complete private node configuration, and bounds the
mutual-discovery/authenticated-pairing stage to 15 seconds. Its undeclared `report.json` and redacted
JSON artifacts prove that NSD observations remained untrusted, the leader had no active binding
before the setup mutation, the authenticated peer became the durable binding afterward, and a bad
peer credential was rejected without advancing the setup revision. The target also rotates the
shadow's own credential, requires its old token and the leader's stale binding to fail, then enters
the new token through an explicit authenticated re-pair and requires healthy redacted setup again.
The bounded physical run passed on 2026-08-22. Its retained
`artifacts/android_discovery_pairing_pass_20260822T180317/report.json` proves mutual untrusted
discovery, authenticated mutation-only binding, bad-credential rejection, own-credential rotation,
immediate old-token rejection, stale-leader `re_pair_required`, explicit re-pair with the new token,
artifact redaction, and exact restoration of both private configuration generations.

Every dual-phone physical target now creates `cleanup.json` before its first phone mutation and
merges the finalized evidence into the aggregate `report.json`. The cleanup report registers each
required package stop, ADB forward/reverse removal, app-private temporary-file removal, complete
node-configuration restore, and temporary Wi-Fi re-enable. It retains every attempt as `restored`
or `failed`, including a failed checked attempt followed by a successful unwind retry, while its
obligation summary reports the final restored and unresolved counts. A cleanup failure turns an
otherwise successful physical result red. When the physical stage and cleanup both fail,
`primary_outcome` and the aggregate diagnostic preserve the physical failure and exit code while
the independent `cleanup` object carries the restoration failure; cleanup can no longer replace
the evidence needed to diagnose the original fault.

ADB serials are useful development identities but are not an appropriate
production API. Each app installation should create a stable random node ID,
present a human-readable device label, and support deliberate re-pairing or
identity reset. The current development mapping is:

```text
down_the_line = Pixel 5a, ADB serial 1A011JEG501717
face_on       = Pixel 6, ADB serial 22181FDF6005QH
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
http://<web-app-address>/?node=http://<phone-address>:8088&node_token=<credential-shown-on-phone>
http://<web-app-address>/?dtl_node=http://<dtl-address>:8088&dtl_token=<dtl-token>&face_node=http://<face-address>:8088&face_token=<face-token>
```

The single-node form lists and reviews one phone's sessions. The dual-node form
requires exactly one endpoint for each role, arms both with one shared ID,
performs repeated clock exchange, admits the trigger pair, composes the
platform-neutral two-view manifest, and writes the accepted coordination
record to both phones. Authenticated coordination `POST` is create-only: an
identical retry is idempotent and conflicting content is rejected. Setup and capture status also
publish an additive `operational_health` object with Android thermal status/headroom, power-save
mode, usable storage, the 2 GiB retention reserve, per-domain readiness, and combined readiness.
Severe thermal status and sub-reserve storage enter the existing setup readiness issue list.
Capture status also publishes a per-server stream ID, strictly increasing revision, and monotonic
generation timestamp with `Cache-Control: no-store`. The browser probes each phone independently
with the current Bearer credential, rejects stale revisions, and reconnects with bounded
exponential backoff. A disconnected or stale phone disables live capture actions without
discarding an already-loaded review. Before use outside the trusted prototype LAN it still needs:

- provisioned TLS identities (or another authenticated protected transport), phone-side
  termination, enforcement of the existing protected-only policy boundary, and a user-friendly
  production pairing exchange;

### Protected transport closure plan

This TODO is not closed by accepting an `https://` string or by adding a permissive trust manager.
The current boundary spans five components: `NodeHttpServer` accepts raw `ServerSocket` connections,
Android NSD constructs `http://` origins, peer clients use `HttpURLConnection`, the embedded browser
bootstraps Bearer credentials from an HTTP URL, and the APK declares
`android:usesCleartextTraffic="true"`. All five must migrate together. Until the final enforcement
step, cleartext remains an explicitly labeled `cleartext_trusted_lan_demo` mode and must never be an
automatic fallback from a failed protected connection.

The selected identity model is a station-local certificate authority, not a self-signed leaf on
each phone. The station CA private key belongs to an operator-controlled provisioning tool and is
never installed in the APK. Each app installation generates a non-exportable server private key in
Android Keystore and submits a certificate-signing request containing its durable node ID. The
issued leaf certificate must contain:

- a station-controlled, stable DNS SAN used by browsers and peer clients, with a reserved IP SAN
  only when the station deliberately operates by fixed address;
- a Swing Capture node-ID SAN URI so the authenticated certificate identity can be compared with
  the API's durable node ID;
- `serverAuth` extended-key usage, the station CA identifier, serial number, validity interval, and
  a SHA-256 SPKI fingerprint recorded in the provisioning manifest; and
- a bounded lifetime selected with a renewal window long enough to complete an attended station
  rotation before expiry.

The station DNS or address reservation is part of provisioning. NSD remains an untrusted discovery
hint and cannot define a certificate identity. The station CA public certificate is installed
explicitly in the selected Chrome host's operating-system trust store and as an app-private trust
anchor on each capture phone. Installing or replacing that anchor requires an out-of-band operator
check of its SHA-256 fingerprint. A browser certificate warning, an all-trusting `TrustManager`, a
custom hostname verifier that returns true, and trusting a root learned from NSD or the candidate
peer are all prohibited.

Implement the migration in this dependency order:

1. **Freeze the provisioned identity and evidence schemas.** Add versioned, strict parsers for the
   CA identity, node certificate chain, node-ID binding, DNS/IP SANs, SPKI fingerprint, validity,
   and current/next pin generations. Reject unknown critical fields, weak keys/signatures,
   mismatched node IDs, expired or not-yet-valid certificates, and arithmetic overflow in validity
   calculations. Keep private-key operations behind an Android Keystore adapter and make the
   policy testable with a synthetic clock and checked-in test certificates whose private keys are
   test-only.
2. **Add phone-side TLS termination without changing the HTTP application protocol.** Put socket
   creation behind a small listener factory, build the production `SSLContext` from the
   Keystore-backed leaf and provisioned chain, and run the existing bounded parser and twelve-client
   executor only after a successful TLS handshake. Bound handshake time and input before assigning
   a normal request worker. Initially expose this on a separate, explicitly configured HTTPS port
   so the trusted-LAN demo listener remains diagnosable; never redirect credential-bearing HTTP to
   HTTPS. Preserve all current body, header, Range, concurrency, storage, and shutdown bounds.
3. **Centralize protected peer connections.** Replace direct connection creation in
   `PosePeerArmClient`, `AutonomousPairPeerClient`, the authenticated setup probe, and
   `PeerClockClient` with one connection factory. It must perform ordinary chain and hostname
   validation against the provisioned station CA, then require the paired node-ID/SPKI binding.
   Redirects stay disabled. No Bearer header or body may be transmitted until the TLS peer is
   accepted; certificate, hostname, node-ID, pin, or expiry failure is terminal rather than
   retryable over HTTP. The public clock response remains an untrusted hint and is admitted only
   after the existing authenticated node-identity comparison.
4. **Bind discovery and pairing to the protected identity.** Advertise the transport kind, stable
   protected origin, CA identifier, and leaf SPKI fingerprint in NSD as hints. Extend the durable
   pairing binding with the authenticated CA ID, certificate node ID, current pin, optional next
   pin, pin generation, and certificate expiry. Address recovery may change an IP hint only after
   TLS proves the same certificate identity. A changed certificate identity cannot be adopted from
   discovery or from a Bearer-authenticated response alone.
5. **Replace URL-token bootstrap in the browser.** First install and verify the station CA on the
   selected Chrome host. The phone then displays a QR code or short attended setup payload
   containing the HTTPS origin, node ID, CA fingerprint, leaf SPKI fingerprint, and a single-use,
   short-lived bootstrap code—not the long-lived control credential. After normal browser TLS
   validation, an HTTPS bootstrap exchange consumes that code once and installs the existing
   origin-keyed tab credential without placing it in the URL, history, referrer, logs, or an NSD
   record. The dual-node setup performs this ceremony independently at both origins. Native video
   Range requests continue to use collection-scoped capabilities, but the manifest, capability,
   query, and media bytes are then protected by TLS.
6. **Implement renewal, pin rotation, and recovery before enforcing protected-only mode.** A
   same-key certificate renewal keeps the SPKI pin and advances certificate metadata only after the
   old authenticated channel validates the new chain. A key rotation stages exactly one
   CA-validated next pin, proves possession on the old authenticated channel, and allows a bounded
   current/next overlap before atomically committing the new generation. CA rotation similarly
   requires an explicitly provisioned dual-root overlap; no network response may introduce a new
   root. Expiry, Keystore loss, lost old-key possession, or a node-ID reset fails closed and requires
   local re-provisioning plus explicit peer/browser re-pairing. That recovery also rotates the
   control credential and retains a non-secret revocation tombstone so stale certificates, pins,
   tokens, and address hints cannot restore trust.
7. **Make the release cutover fail closed.** Once phone, peer, browser, rotation, and recovery gates
   pass, remove the HTTP listener from the production configuration, require
   `PeerTransportSecurityPolicy.Requirement.PROTECTED_ONLY` at every persisted/configured peer
   boundary, advertise only HTTPS, and set the release manifest to
   `android:usesCleartextTraffic="false"` with no broad network-security exception. A separately
   named developer/demo target may retain trusted-LAN HTTP, but its package/artifact identity and
   UI must make it ineligible for product-floor or release evidence.

The deterministic evidence must be complete before any physical qualification:

- pure tests cover strict certificate/provisioning parsing, hostname and node-ID matching, expiry,
  current/next pin transitions, CA rotation, revocation, rollback, generation exhaustion, and every
  protected-to-cleartext downgrade path;
- a loopback TLS integration target uses a test CA and real TLS sockets to prove accepted traffic,
  wrong CA, wrong hostname, wrong node ID, stale pin, expired certificate, handshake timeout,
  oversized pre-handshake/application input, redirect rejection, and absence of an HTTP fallback;
- the existing Node HTTP contract is replayed through TLS, including Bearer mutations, public clock
  semantics, CORS, bounded manifests, capability-scoped HEAD/Range media, credential rotation,
  concurrency, and clean listener shutdown;
- browser interaction tests start from HTTPS fixtures and prove one-time bootstrap consumption,
  two-origin setup, reload behavior, URL/history/referrer redaction, stale bootstrap/token failure,
  certificate failure, and exact Range playback without a cleartext request; and
- the manifest policy test requires cleartext disabled in the release APK, while Bazel dependency
  queries ensure the demo listener/configuration cannot enter that artifact.

The physical closure gate must use the exact candidate APK on both supported phones and the
selected installed Chrome. It must retain certificate chains/fingerprints without private keys;
provision both nodes; pair by the attended bootstrap; exercise discovery, setup, arm, peer impact,
publication, coordination, authenticated HEAD/Range retrieval, and playback over direct LAN HTTPS;
then rotate one leaf/pin and prove continued operation. Negative stages must show that an unknown
CA, wrong hostname/node ID, stale pin, expired leaf, replayed bootstrap, and forced HTTP origin all
fail without disclosing a credential or mutating configuration. Separate recovery stages must cover
an IP-address change with stable identity and attended recovery after simulated Keystore identity
loss in a disposable qualification identity slot. A packet capture on the isolated qualification
LAN must show that Bearer credentials, capabilities, metadata, and media payloads are not present in
cleartext; ordinary IP/TLS traffic metadata is not claimed confidential. Only a reviewed report
containing those positive, negative, rotation, recovery, cleanup, and exact-APK results closes the
protected-transport TODO.

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
behavior is the isolated Pixel 5a audio-source and 0.010 detector-floor policy
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
  --test_env=SWING_CAPTURE_ANDROID_DTL_SERIAL=1A011JEG501717 \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_SERIAL=22181FDF6005QH \
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

`//android/dual_hil:dual_phone_paired_pose_arm_lan_hil_test` extends that contract
through the low-rate standby path. The latest direct-LAN passing evidence is
preserved at
[`artifacts/android_pcm_paired_s06_lan_pass_20260822T164310/report.json`](../artifacts/android_pcm_paired_s06_lan_pass_20260822T164310/report.json).
Both phones ran real 5 Hz on-device inference; an explicitly HIL-gated Pixel 6
leader candidate exercised the production authenticated peer client and the
Pixel 5a recorded the inbound arm. Both cameras then transitioned concurrently
to 720p240 and captured one full-scale S06 recorded-impact replay. Pixel 6
retained 561 frames at 239.043 fps and Pixel 5a retained 568 at 239.248 fps.
Both decoded exactly, retained AprilTag 0 across the impact, and passed their
optical/audio bounds at 18.486 and 21.325 ms. The gate assigned Pixel 6
`face_on` leader and Pixel 5a `down_the_line` shadow; their final sources were
`local_audio` and `peer_audio_clock_candidate`. The schema-2 mapped impact was
bound to the current shared session and target node, and the durable
coordination record passed on both nodes with 12.206 ms maximum mapped trigger
separation and 9.177 ms combined pair uncertainty. It used direct Wi-Fi peer
transport with no ADB reverse, and proved that 20 ms high-speed audio reads
avoid the prior phase-dependent `peer_audio_arrival` fallback.

The revised gate uploads a byte-exact native 48 kHz PCM16 window from the field
recording, then replays its gain-scaled samples rather than substituting a
synthetic tone. This preserves digital source identity without claiming
acoustic equivalence through the fixture speaker, room, and phone microphones.
The checked-in `field_pcm_replay_cases.json` identifies each window by source
frame, sample count, marker frame, gain, and expected CRC32; the private source
WAV remains under ignored `artifacts/` storage. Run one physical case at a time:

```bash
bazel test //android/dual_hil:dual_phone_paired_pose_arm_lan_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_DTL_SERIAL=1A011JEG501717 \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN=http://10.168.168.111:8088 \
  --test_env=SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN=http://10.168.168.241:8088 \
  --test_env=SWING_CAPTURE_PCM_REPLAY_MANIFEST="$PWD/android/dual_hil/field_pcm_replay_cases.json" \
  --test_env=SWING_CAPTURE_PCM_REPLAY_WAV="$PWD/artifacts/<field-session>/face_on_pixel6_audio.wav" \
  --test_env=SWING_CAPTURE_PCM_REPLAY_CASE=S06-representative \
  --test_output=streamed --nocache_test_results
```

Required-positive cases are `S11-quiet`, `S06-representative`, and `S03-loud`.
`practice-hard-negative` and `low-transient-negative` are diagnostic cases: a
false acceptance makes matrix qualification fail. An observed no-trigger is
recorded as the expected diagnostic outcome, followed by an explicit disarm
and configuration restore; that path does not fabricate a PNG requirement.
If a diagnostic case does trigger, the runner waits for both sessions and
pulls their manifests, WAVs, and MP4s before publishing a self-identifying
failed report.
The runner uploads and CRC-verifies the selected window, calibrates the fixture,
arms both phones, refreshes calibration after high-speed readiness, and then
requires the playback receipt to say `prepare_source=calibration`. Each
transition, replay/capture, salvage, pull, and analysis stage remains bounded to
15 seconds. If playback does not automatically trigger the Pixel 6, the gate
stays failed, snapshots both nodes' high-speed audio peak/noise/threshold, and
uses a `missed_shot` trigger only to retain both rings for diagnosis. Those
salvage clips are explicitly ineligible for qualification.

The upload receipt CRC identifies the unchanged source window. The playback
receipt separately carries the expected and observed CRC32 of the gain-scaled
mono PCM16LE samples; the host requires those scaled values to match.
The production detector's point timestamp must remain within 20 ms of the
labeled optical strike. Because a 240 fps frame only localizes the LED onset to
an exposure interval, the recorded-PCM gate separately allows a 25 ms maximum
conservative bound after composing camera phase, media-timestamp residual, and
audio-timestamp uncertainty. Both the point offset and expanded interval are
retained in `evidence.json`; this does not move the source marker to the later
microphone peak.
The high-speed `AudioRecord` retains a 200 ms-class internal safety buffer but
processes it through 20 ms blocking reads. This bounds detector delivery across
independent phone read phases; the shadow waits at most 150 ms for its own
clock-matched acoustic candidate before using the explicitly audited
`peer_audio_arrival` fallback.
Confirmed impacts which precede a valid startup clock mapping are held in a
four-event FIFO rather than discarded. Detection accounting and cooldown happen
at confirmation time; delivery waits until both strike and confirmation frames
map through a validated `AudioTimestamp`. This specifically covers devices
whose first timestamp is rejected for clock-rate, uncertainty, or frame-position
consistency without weakening the requirement for mapped trigger time.

`//android/dual_hil:pcm_replay_extract` reproduces a case's exact little-endian
PCM bytes and prints its identity. `//android/dual_hil:pcm_replay_score` consumes
the case manifest plus a schema-2 observations JSON and emits the confusion
matrix and per-row source ID, CRC32, gain, pair outcome, and per-role production
trigger sources. Missing
required positives count as misses. Diagnostic observations are optional, but
any observed diagnostic false trigger keeps the aggregate gate red.

For field-readiness validation over the production phone-to-phone network path,
use the LAN-specific target. ADB is used only for host orchestration;
the target creates no ADB reverse and configures the leader with the Pixel 5a's
actual Wi-Fi origin:

```bash
bazel test //android/dual_hil:dual_phone_paired_pose_arm_lan_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_DTL_SERIAL=1A011JEG501717 \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN=http://10.168.168.111:8088 \
  --test_env=SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN=http://10.168.168.241:8088 \
  --test_env=SWING_CAPTURE_PCM_REPLAY_MANIFEST="$PWD/android/dual_hil/field_pcm_replay_cases.json" \
  --test_env=SWING_CAPTURE_PCM_REPLAY_WAV="$PWD/artifacts/<field-session>/face_on_pixel6_audio.wav" \
  --test_env=SWING_CAPTURE_PCM_REPLAY_CASE=S06-representative \
  --test_output=streamed --nocache_test_results
```

Before arming cameras, it probes both Wi-Fi app origins directly and retains
identity, role, device model, advertised origin, descriptor/status/clock schema
versions, unauthenticated setup rejection, and authenticated setup success.
After both phones enter real 5 Hz monitoring, it also requires each actual Android route to reject
an unauthenticated setup-preview request, serve a fresh bounded JPEG with no-store/nosniff headers
under that phone's Bearer credential, and publish rotation/age/generation metadata. Once 240 fps
owns Camera2, both routes must return 503 with `high_speed_capture` rather than leaking a stale
preview. The JPEGs and high-speed metadata are retained per role and are part of the aggregate
report contract.
The subsequent usable leader-held peer clock, accepted peer arm, schema-2
mapped impact for the current shared session, and exact final trigger sources
prove that peer traffic used the configured LAN origin. ADB reverse is reported
as false, credentials are not retained, and complete private configuration is
restored. The current-revision checkpoint at
`artifacts/android_pcm_current_apk_paired_pass_20260823T002901` passed this exact
target, including direct authenticated setup and clock traffic to both phone origins, paired
automatic trigger in 621 ms, exact retained-media decode, and complete cleanup.

After the foreground service has been launched, a development browser can
review its latest sessions at `http://<phone-address>:8088`. Serve the
checked-in web app and point it at that node with a query parameter:

```text
http://<web-app-address>/?node=http://<phone-address>:8088
```

The credential is required for node identity, setup, status, preview, session, and control routes.
The bootstrap code consumes the token before application startup,
removes it from the current URL and history entry with `history.replaceState`, and retains it only
in origin-keyed tab `sessionStorage` so same-tab reload and rotation continue without recreating a
credentialed URL. `Referrer-Policy: no-referrer` blocks ordinary referrer disclosure. This reduces
routine history/bookmark leakage; it does not protect the initial cleartext HTTP request, process
arguments, or network observers. Native-video media is now authorized by a collection-scoped
capability obtained only from authenticated metadata, but a cleartext observer can replay that
capability until the control credential rotates. The trusted hitting-area network remains a
prototype boundary.

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
  --test_env=SWING_CAPTURE_ANDROID_DTL_NODE_URL=http://dtl-phone:8088 \
  --test_env=SWING_CAPTURE_ANDROID_DTL_TOKEN=replace_with_dtl_node_token \
  --test_env=SWING_CAPTURE_ANDROID_FACE_NODE_URL=http://face-phone:8088 \
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
5 minutes 27 seconds. Current-workload screen-off thermal and publication-heavy
qualification remain. Process-restart and paired peer-disturbance recovery pass. The selected
OS-reboot policy requires first unlock and an operator foreground launch; that physical ceremony
has not been qualified for this revision.

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
captures, bounded timing, and durable coordination. The paired pose-standby
gate additionally passed real low-rate inference, production peer arming, both
warm transitions, and one-event capture. The live browser HIL also passed paired H.264 review. The
authenticated browser ceremony, Android NSD/mDNS integration, stable binding, credential rotation,
stale-token rejection, and explicit re-pair have all passed the bounded two-phone physical gate.

### 4. Qualification

- Add deterministic software tests for every timing and retention boundary.
- Add short per-device and dual-device Bazel HIL targets.
- Inspect retained reports and both diagnostic images for every physical run.
- Run longer thermal qualification only when explicitly requested.
- Decide whether the Pixel 5a remains supported before its workarounds become a
  permanent architectural constraint.

## Prototype browser release boundary

Desktop Google Chrome is the selected prototype browser, and
`//web:prototype_browser_release_gate` combines the real one-second Android AVC
decode/Range/nonblack/RVFC/seek/step gate with the exact-APK, direct-LAN two-phone hosted-UI HIL.
The physical half also requires installed Google Chrome to decode both MP4s newly published during
that invocation, present a nonblack frame, advance playback, and retain complementary
role/origin/shared-recording evidence.
Installed Firefox 153.0.4 and checksum-pinned WebKit 18.4 revision 2158 remain passing
compatibility candidates through their individual gates and the candidate matrix, but do not block
a prototype release. Two consecutive exact-APK Chrome passes at
`artifacts/android_field_recording_browser_hil_relay_range_pass1_20260823T071927Z` and
`artifacts/android_field_recording_browser_hil_relay_range_pass2_20260823T072013Z` satisfy the final
fresh-decode/cancellation schema and validate media-worker cleanup across decoder release. They are
explicitly non-qualifying because Pixel 6 used a temporary relayed face-on origin. The direct-LAN
failure at `artifacts/android_browser_hil_direct_lan_preflight_failure_20260823T072340Z` records DTL
HTTP-ready at 951 ms while the Pixel 6 face-on origin had five 500 ms transport timeouts through
2.901 seconds. Playwright never launched; exact-APK verification and both screen-sleep cleanup
actions still passed. Restore Pixel 6-to-host Wi-Fi reachability and run the combined gate over both
direct-LAN origins. The current and every later proposed release needs its own reviewed passing
invocation; it cannot be inferred from the relay or an earlier checkpoint.

## Open decisions

- Whether field and longer thermal evidence for the Pixel 5a justify retaining
  its provisional 720p240 profile; its paired pose-to-impact gate now passes,
  but one complete-path sample took 2.118 s from arm to first usable encoded
  frame versus 1.279 s on Pixel 6. Repeated contention evidence and reviewed
  address-to-takeaway timing must decide whether that slower startup is viable.
- Whether to admit an IDR-backed, explicitly truncated startup pre-roll before
  the normal 1.4-second history is available. The fixed-memory prototype and
  deterministic boundary tests exist, but production remains fail-closed on
  the full window because truncation cannot recover motion before Camera2's
  first usable encoded frame.
- Whether the passing Pixel 6 720p240 isolation remains at or below `MODERATE`
  with production trigger/publication load and with the screen explicitly off;
  the corresponding 1080p240 isolation reached `SEVERE`.
- Whether the current leader-authoritative impact policy remains acceptable after the version-2
  phone/view-swapped holdout: at least two fully reviewed sessions with required category
  repetition across both. The shadow detector is retained only for bounded local timestamp
  selection and falls back explicitly to mapped peer arrival.
- The measured uncertainty threshold for accepting a physical dual-view
  session; the interval-based clock model is implemented.
- Whether representative physical evidence admits the provisional Pixel 5a
  under the selected local 720p240 floor; the API/camera/encoder/audio/GLES
  software admission contract itself is no longer open.
