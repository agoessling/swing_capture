# Swing Capture Architecture

## Direction

The first implementation targets the two Daheng
`MER2-160-227U3C` cameras on Linux.

This path already provides deterministic access to every raw frame, explicit
exposure controls, stable device timestamps, serial-number selection, and
electrical frame-trigger inputs. Android high-speed video remains a useful
future capture client, but it adds phone-specific high-speed camera
constraints, thermal behavior, opaque video pipelines, file transfer, and
wireless clock synchronization before it solves the core simulator use case.

## Capture data path

```text
camera A ─ acquisition thread ─ clock fit ─ pooled frame ring ─┐
                                                               ├─ clip planner ─ concurrent VA-API VP9/WebM
camera B ─ acquisition thread ─ clock fit ─ pooled frame ring ─┘       │
                                                                       ├─ atomic session directory
                                                                       └─ catalog + range HTTP ─ review UI
microphone ─ PCM source ─ impact detector ─ host-monotonic trigger estimate ┘
```

Each camera has exactly one dequeue/requeue thread. That thread performs no
demosaicing, encoding, disk I/O, or UI work. It validates metadata, updates
timing metrics, and copies the payload once into a preallocated pool.

Each device clock is fitted independently to the host steady clock. This lets
an audio trigger estimate select frames from both streams. It does not prove
that the two cameras exposed at the same instant.

The hardware-independent version of the trigger, retention, and selection path
is exercised by `//capture/pipeline:capture_pipeline_integration_test`. It
synthesizes an audio strike and two cameras with unrelated clock origins and
frequencies, waits for post-roll, freezes both rings, selects bounded clip
windows, and proves capture can continue without mutating the retained frames.
Application tests separately cover the camera-buffer generations, capture
controller, publisher, persistent catalog, HTTP API, encoded fixture playback,
and browser interactions. The explicit application-flow HIL connects those
boundaries on the production station with both cameras, ALSA, and the Feather
speaker.

## Camera configuration and diagnostics

Machine-local station configuration binds the physical `down_the_line` and
`face_on` roles to camera serial numbers. The hardware tests require an
explicitly verified mapping and record the role beside every serial in their
reports; they no longer accept whichever two devices happen to be attached.

The current Daheng HIL uses deterministic free-run settings: 1440x1080
`BayerRG8`, approximately 227 fps, 500 us fixed exposure, `ExposureAuto=Off`,
24 dB fixed gain, and `GainAuto=Off`. Required values are read back from both
cameras rather than assumed from successful setter calls.

After acquisition stops, diagnostic work runs away from the camera dequeue
threads. One retained raw frame per view is measured for histogram percentiles,
near-black/near-white fraction, and Bayer-phase-aware gradient energy, then
demosaiced to PNG. The JSON stores only the PNG filename relative to the
report; a smoke artifact therefore travels as one self-contained directory:

```text
report.json
frame-FDN22120654.png
frame-FDN23010199.png
```

Transport readiness and image readiness are separate. The 2026-07-26 smoke
transported both streams successfully, but its dark diagnostic views did not
establish image readiness. The 2026-08-09 room-lit station-fixture run replaced
that optical baseline by decoding the same marker cleanly in both views. Image
classification remains diagnostic evidence rather than a transport gate.

The short station-fixture HIL stops and joins both camera producer threads
before stopping either Galaxy stream, then analyzes immutable ring snapshots.
Its LED detector removes a fitted per-frame global illumination change and
requires a localized response above a surrounding background ring. A long
guarded locator pulse chooses one ROI per camera; a later 44.053 ms pulse is
evaluated only there with a scheduled matched window. Qualification requires
nine consecutive lower-threshold supported frames, three nested
high-confidence anchors, and aggregate signal strength. This separates
continuous weak evidence from isolated bright frames while allowing partial
free-running exposure-edge frames. The strongest frames are retained even on
rejection, together with clean images, ROI overlays, and averaged
red-difference diagnostics. AprilTag presence is evaluated on luminance derived
from an offline demosaic, rather than interpreting the color Bayer mosaic as
grayscale. The same `tag36h11` ID must decode in both views; this is a
framing/focus/exposure check, not a pose or geometric-calibration claim.

Robust batch clock fitting maps camera device ticks to normal host receipt
times while rejecting queued-delivery outliers. A constant camera/readout/USB
delivery delay is not observable in those samples. The LED-versus-Feather
schedule comparison therefore records its one-frame delivery assumption and
remains diagnostic; station acceptance instead gates locator consistency and
the short pulse's integrated response and duration in its command-relative
retained window, plus independently verified Feather command/device timing.

## Headless station and setup preview

The setup UI is a mode of the same implemented station service, not a second
camera-owning application:

```text
Daheng camera ─ owner/acquisition thread ─ 227 fps frame dequeue
                                             │
                                             └─ overwrite-latest sampler (about 30 fps)
                                                         │
                                                         └─ latest-only renderer
                                                                    │
                                                         compressed 640x480 JPEG
                                                         + on-demand full-size PNG
                                                                    │
browser ─ capacity-one paired fetch + atomic swap ─ HTTP API + React/TypeScript UI ┘
```

Each camera remains owned by exactly one native thread. HTTP handlers consume
immutable raw or rendered snapshots and never call the Galaxy SDK. Sampling
copies only an admitted frame and retains at most the latest raw snapshot;
one dedicated renderer per camera similarly caches only the latest completed
routine JPEG and an on-demand full-resolution PNG. Slow browsers keep one
two-camera pair in flight, replace at most one pending pair with newer
sequences, retain the last successful pair on failure, and cannot apply
backpressure to high-rate acquisition. The two views become visible in one
atomic UI update after both downloads and browser decodes complete. Routine
quality measurement still examines the full Bayer payload, while preview
rendering samples the Bayer mosaic directly to the fitted geometry before
demosaic. Full-resolution demosaic and encoding run only after an explicit
request and are collapsed onto that same capacity-one renderer. The physical
application-flow HIL fetches a full-resolution preview from each camera after
the session has been encoded and published, qualifying this copy/render path
while the production station remains open.

The versioned `/api/v1` contract exposes station status, one latest-image route
per configured role, exposure/gain updates, capture arm/manual/status routes,
and session discovery, manifest, and media routes. The additional
`POST /api/v1/hil/synthetic-swing` operation is authorized only when the
station is started with `--enable-hil-controls`; capture status version 2
reports whether that operation is enabled, busy, or at a
calibration/stimulus/capture/encode stage. A settings update is serialized
through the owning camera thread and
rejected while capture is armed. Because the current Daheng wrapper only
configures a stopped stream, the worker stops that camera, applies the full
deterministic profile with the requested values, verifies read-back, restarts,
and attempts to restore the prior profile if any step fails.

Routine setup polling uses a compressed fit-within 640x480 JPEG. The explicit
manual-focus link selects the cached full-sensor render for the same preview
sequence. Recorded review is a separate data path: it uses persisted encoded
media plus per-frame timestamp metadata rather than polling individual still
images.

## Retention and memory

A pooled camera ring owns preallocated frame blocks. Freezing a window
increments block references; it does not copy frame payloads. Pool exhaustion
is an explicit error rather than an unbounded allocation or a silent frame
drop.

The measured full-resolution payload is 1,555,200 bytes. At 226.86 fps:

| Allocation | Per camera | Two cameras |
|---|---:|---:|
| Two-second active window | about 706 MB | about 1.41 GB |
| Active window plus full reserve | about 1.41 GB | about 2.82 GB |

The generic freeze/continue camera HIL uses the active-plus-reserve layout in
the table and measured about 2.99 GB process RSS. The production application
instead arms two 448-frame generations per camera with no additional reserve:
four rings hold 2,786,918,400 payload bytes, approximately 2.596 GiB total.
SDK buffers, ring metadata, preview snapshots, encoder working memory, and the
rest of the process are additional. The application rotates to the prepared
generation before encoding, then releases all four rings when the one-shot
session reaches `ready`. A later range-freeze API can reduce the retained
pre-trigger allocation if memory becomes constrained.

## Trigger and clip sequence

1. Both camera owner threads run continuously for setup preview. Arming allocates
   two 448-frame raw generations per camera and starts continuous ALSA capture;
   full-rate camera frames are copied into the active generation once.
2. After one second of pre-roll plus a frame margin is present, the controller
   enters `armed`. The impact detector reports both the peak sample's
   host-monotonic trigger estimate and the later timestamp at which that peak
   was confirmed. Only an impact submitted while `armed` is accepted.
3. Capture continues through the fixed 500 ms post-trigger interval plus the
   frame-boundary margin. The controller then changes to `encoding`, freezes
   both completed generations by reference, and rotates each camera onto its
   prepared generation before stopping ALSA.
4. The clip planner independently maps each camera's device timestamps to host
   time and selects 1.4 seconds before through 500 ms after the trigger estimate.
5. Two publication tasks concurrently demosaic and fit the selected BayerRG8
   frames to at most 640x480, convert them to NV12, and encode all-keyframe
   VP9/WebM through the Intel GPU's VA-API low-power path. Camera acquisition
   and the second ring generations continue during this work. A deterministic
   software VP8 implementation remains an injected hermetic-test backend.
6. The publisher writes both media assets and a manifest into a private sibling
   directory, then atomically renames the complete directory into the session
   root. It never exposes a partial session or overwrites an existing one.
7. The in-memory cache of the persistent session catalog refreshes after
   publication. The controller enters one-shot `ready` with ALSA stopped and
   both cameras' raw generations released; an explicit re-arm is required for
   the next swing.

The manifest preserves each selected source frame's frame ID, device timestamp,
trigger-relative time, and media time, along with the mapped nearest-frame
index and view/camera identity. Media routes implement HTTP byte ranges so the
browser can seek and step without downloading a new still for every frame. The
catalog scans and validates existing atomic session directories at startup,
caches that result for requests, and refreshes only after a successful local
publication.

## Pipeline profiling

Published manifests may contain a versioned `pipeline_profile` that follows a
capture from the microphone trigger estimate through the last backend snapshot
before manifest serialization. Capture timings separate confirmation,
acceptance, the scheduled post-roll wait, ring rotation, and ALSA shutdown.
Session timings separate prepublication analysis, publisher planning, output
setup, media encoding, frame metadata, and the snapshot's monotonic timestamp.
Each camera view further separates timeline work, fitted Bayer demosaic,
RGB-to-YUV420 conversion, codec encoding, WebM muxing, finalization, and output
verification. Profile schema 2 uses those backend-neutral stage names; the web
reader retains schema-1 support for already published software-VP8 sessions.

The timing tree is hierarchical: both concurrent view totals are contained in
media encoding wall time, and per-view stages are contained in that view's total.
Consumers must not add parent and child values. Terminal manifest
serialization, file write, atomic rename, catalog refresh, and request handling
occur after the manifest snapshot. Manifest and media responses therefore also
carry `X-Swing-Capture-Server-Monotonic-Ns`, allowing the browser to combine a
same-host backend interval with its own response-to-video-presentation interval
without assuming that the browser and server clocks share an epoch.

The review UI displays the backend profile and records the manifest fetch plus
the actual presentation of both role-specific impact frames. Chromium uses
`requestVideoFrameCallback` when available and a seeked-plus-paint fallback
otherwise. The fetch duration widens the reported audio-confirmation-to-display
bound; it is not hidden inside a single opaque elapsed time.

The lower-level synthetic coordinator also models a late scheduling case: if
the leading margin has fallen out of retention, it reports an explicit expired
outcome instead of emitting a clip request that the planner cannot fulfill.

## Synthetic-swing HIL sequence

The station's synthetic-swing control is an explicitly enabled diagnostic
facade over the production one-shot capture path. It is serialized away from
normal camera settings, arm, and manual-capture operations. One API request
owns this complete state progression:

```text
calibrating ─ arming ─ stimulus ─ capturing ─ encoding ─ ready
```

Calibration first establishes an OFF baseline, then powers the Prop-Maker's
shared GPIO23 fixture rail and asks the external screw-terminal NeoPixel on
GPIO21 to show the white brightness candidates `1,2,3,4,6,8,12,16` for
70 ms each. The host aligns low-rate preview samples with the Feather's device-time
receipt, locates the same physical response independently in both cameras, and
chooses one shared brightness that is visible without clipping in either view.
The sweep also fits a signed, camera-local correction between mapped camera
receipt time and the Feather schedule. Full-rate analysis adds that correction
to each camera timeline and carries its residual uncertainty into transition
classification. Calibration leaves the pixel off but the shared rail prepared
for the immediately following full-rate stimulus, which uses only the selected
level:

- 60 stepped RGB states at 20 ms each, or 1.2 seconds before impact;
- a 20 ms white optical-impact state and a 10 ms, 2 kHz Feather speaker tone
  commanded together on the RP2040 timeline; and
- 25 more 20 ms RGB states, or 0.5 seconds after impact.

The audio detector accepts the tone through the same normal capture route, so
one synthetic action consumes one arm. Publication leaves capture `ready` and
unarmed. A normal following swing therefore needs an explicit re-arm; another
synthetic request starts and owns its own arm.

The full retained frames, rather than the preview JPEGs, are aligned with the
typed Feather schedule and analyzed inside the calibrated NeoPixel region.
Manifest `hil_evidence` records the selected brightness and stimulus constants.
It machine-qualifies the white impact marker in each role and records
`optical_white_impact_frame_index`, `audio_trigger_estimate_offset_us`, and
`camera_schedule_alignment`. The stepped colors before and after impact exist
for human review and frame stepping; exact palette states are not an acceptance
gate. `camera_schedule_alignment` additionally
preserves each signed mapped-time correction and uncertainty. A positive offset
means the audio trigger estimate is later than that camera's observed white
frame. This signed offset
is measured diagnostic evidence; unmeasured ALSA, camera, amplifier, speaker,
and acoustic delays prevent treating it as calibrated true-impact timing.

## Synchronization

Free-running capture is enough to develop transport, triggering, clip
selection, and the UI. It is not the final synchronization mechanism.

The intended final setup feeds one shared electrical frame pulse into `Line0`
on both cameras. Both streams must be armed before the pulse train begins.
An optical sync HIL should put a flashed LED in both views and measure the
observed frame offset. The model-specific connector pinout and voltage limits
must be verified before wiring.

Free-run qualification also separates two kinds of apparent stalls. Host
receive intervals may reach 10 nominal frame periods to tolerate bounded Linux
scheduling delay while the SDK queue preserves every frame. Device timestamp
intervals may reach only 4 periods because they describe camera cadence
directly. Both thresholds are recorded and checked independently.

## Audio timing

The first physical audio backend launches `arecord` directly, captures the
station-configured stable ALSA card ID and hardware channel count at 32 kHz,
and selects the configured mono channel for the impact detector. It uses host
read completion minus the first block's duration as its initial timestamp, then
preserves time by sample-count
continuity. That is sufficient for early trigger and clip-pipeline iteration,
but ALSA buffering and device latency are not yet measured. A direct ALSA
timestamp backend or a physical audio/optical calibration HIL is required
before claiming absolute strike alignment. Accordingly, manifest time zero and
the review marker are an **audio-trigger estimate**, not a calibrated
ball-impact timestamp.

## UI boundary

The capture engine exposes immutable manifest metadata and WebM assets rather
than camera SDK objects. The implemented React review UI consumes that boundary
for synchronized side-by-side playback, exact source-frame stepping, a shared
scrubber, 0.25x/0.5x/1x/2x playback speeds, the audio-trigger estimate marker,
capture health, and degraded-state messages. Address/impact annotations and
view-bound drawing overlays remain future review features.

The camera-setup and review modes use injectable APIs and checked-in fixture
artwork/media. Component tests cover interaction, runtime contract validation,
and accessibility without the SDK or physical cameras. Bazel supplies a pinned
Chromium to Playwright for real software-fixture VP8 decoding, seeking,
synchronized controls, fixed-viewport screenshots, and an optional
browser-only replay of a preserved HIL session. The production HIL replay also
qualifies the hardware-produced VP9 files. That optional replay serves a
preserved manifest and WebMs through
a read-only test facade; it does not claim to drive a live production server or
rerun physical HIL from the browser.
