# Testing Strategy

## Default software suite

```bash
bazel test //...
```

The default suite consists entirely of hardware-independent C++, Python, and
TypeScript/UI tests:

| Area | Bazel targets |
|---|---|
| Audio and trigger | `//capture/audio:arecord_pcm_source_test`, `//capture/audio:audio_capture_session_test`, `//capture/audio:audio_hil_metrics_test`, `//capture/audio:commanded_click_analyzer_test`, `//capture/audio:commanded_tone_analyzer_test`, `//capture/audio:pcm_wav_test`, `//capture/trigger:impact_detector_test` |
| Storage and timing | `//capture/core:camera_source_test`, `//capture/core:raw_frame_ring_test`, `//capture/core:pooled_raw_frame_ring_test`, `//capture/core:device_clock_mapper_test` |
| Session and clip selection | `//capture/session:capture_session_coordinator_test`, `//capture/clip:clip_window_planner_test` |
| HIL protocol and evidence | `//capture/hil:feather_hil_protocol_test`, `//capture/hil:feather_hil_serial_test`, `//capture/hil:feather_hil_controller_test`, `//capture/hil:hil_metrics_test`, `//embedded/prop_maker:hil_protocol_test` |
| Application capture and publication | `//capture/application:camera_clip_buffer_test`, `//capture/application:audio_impact_monitor_test`, `//capture/application:capture_controller_test`, `//capture/application:clip_session_publisher_test`, `//capture/application:session_catalog_test`, `//capture/encoding:clip_session_test`, `//capture/hil:application_audio_stimulus_test`, `//capture/hil:session_artifact_validator_test` |
| Optical and images | `//capture/optical:april_tag_test`, `//capture/optical:frame_selection_test`, `//capture/optical:led_pulse_test`, `//capture/optical:led_schedule_association_test`, `//capture/image:bayer_rg8_test`, `//capture/image:image_quality_test` |
| Synthetic end to end | `//capture/pipeline:capture_pipeline_integration_test` |
| SDK packaging | `//capture/daheng:daheng_sdk_runtime_test`, `//third_party/daheng:extract_sdk_test` |
| Station configuration | `//station:station_config_test` |
| Setup preview core | `//capture/preview:camera_settings_test`, `//capture/preview:latest_frame_sampler_test`, `//capture/preview:preview_image_test` |
| Setup service | `//capture/service:camera_worker_test`, `//capture/service:preview_api_test`, `//capture/service:synthetic_swing_hil_operation_test`, `//capture/service:synthetic_swing_hil_timeline_test`, `//capture/service:synthetic_swing_station_workflow_test` |
| Setup and review web UI | `//web:typescript_typecheck_test`, `//web:component_test`, `//web:review_component_test`, `//web:browser_test` |
| Host tooling | `//tools:station_doctor_test`, `//tools:unattended_hil_test` |

The end-to-end fixture synthesizes two cameras with unrelated device clocks
and an audio impact. It verifies post-roll waiting, reference-counted freezing,
bounded strike-relative selection for both views, and continued recording
while the frozen clip remains valid. Session boundary tests distinguish the
backdated strike sample from its later confirmation time, wait one frame
beyond the requested endpoint, and explicitly expire a late request before
pre-roll can be overwritten. The audio source unit test launches a
deterministic fake producer; it does not open host audio hardware.

All test logic above is owned by Bazel. The application tests are `cc_test`
binaries; the station doctor, SDK extractor, and unattended-runner tests are
`py_test` targets.
Shell scripts are not used as test implementations. Synthetic tests cover
timing boundaries and failure paths, not only nominal examples. The wildcard
suite downloads Galaxy Linux SDK
`2.6.2606.9251`, verifies its SHA-256, safely extracts an allowlisted SDK
surface, and tests that the GenTL producer is available through Bazel runfiles.
It does not open a camera. The repository adapter is documented in
[`third_party/daheng/README.md`](../third_party/daheng/README.md).

The service tests use a fake camera adapter and synthetic Bayer frames. They
cover overwrite-latest image publication, reset without sequence reuse,
compressed routine rendering, on-demand full-resolution rendering, monotonic
JPEG/status sequences, measured frame delivery, bounded timeout and
invalid-frame failure, post-stop command rejection, setting
range/increment rejection, stop/configure/start serialization, preview
invalidation, failed-update recovery, capture/session HTTP contracts, session
catalog restart discovery, WebM byte ranges, request content/origin checks,
security headers, malformed requests, and static asset serving.

The synthetic-swing service tests cover the explicitly gated API, serialized
lifecycle and recovery, typed Feather timing receipts, automatic shared
brightness selection across two camera sweeps, white-impact qualification,
manifest evidence, and one-shot ready/unarmed completion. Pre- and post-impact
colors are human review cues and are not exact programmatic gates. No default
test opens the cameras, microphone, or Feather.

The component tests cover dual-view setup rendering, capacity-one paired
polling, apply/revert behavior, disconnected/error states, one-shot capture
transitions, the opt-in synthetic-swing button and progress/error states,
per-role optical/audio evidence labels, synchronized review controls, runtime
schema rejection, and axe accessibility scans. The checksum-pinned Chromium
test decodes real all-intra VP8 fixtures, seeks, steps an exact source frame,
exercises synchronized playback and drift correction, navigates between setup
and review, runs the synthetic workflow against a deterministic fake API, and
retains a fixed 1440x1000 screenshot. No default service or browser test opens
physical hardware.

AddressSanitizer and UndefinedBehaviorSanitizer variants run with:

```bash
bazel test --config=asan //...
bazel test --config=ubsan //...
```

## Explicit hardware tests

Physical-device tests are Bazel `cc_test` targets tagged `manual`, `local`,
and `exclusive`. They are intentionally excluded from wildcard test runs:

```bash
bazel test //capture/daheng:dual_camera_smoke_hil_test \
  --test_output=streamed --nocache_test_results
bazel test //capture/daheng:dual_camera_qualify_hil_test \
  --test_output=streamed --nocache_test_results
bazel test //capture/daheng:dual_camera_soak_hil_test \
  --test_output=streamed --nocache_test_results
```

The shortest direct production-encoder check opens only the render node, not
the cameras, microphone, or Feather:

```bash
bazel test //capture/encoding:vaapi_vp9_encoder_hil_test \
  --test_output=streamed --nocache_test_results
```

It requires the Intel iHD driver and `render`-group access, encodes a small
synthetic Bayer clip through direct VA-API, and parses the result to prove VP9,
geometry, timestamps, frame count, and an all-keyframe stream.

The camera tests require `SWING_CAPTURE_STATION_CONFIG` to name a valid local
station file whose down-the-line and face-on roles have been physically
verified. The local `.bazelrc.local` supplies that environment variable. Each
camera object in `report.json` records both `role` and `serial`; an absent,
invalid, unverified, missing, or duplicate assignment fails before capture.

The stages run for 15 seconds, 5 minutes, and 30 minutes. They require:

- the persistent 2000 MB `usbfs_memory_mb` configuration installed by
  `sudo ./tools/setup_daheng_host.sh`;
- exactly two selected cameras with read/write access;
- reported USB root-controller topology; distinct controllers are preferred,
  while a shared controller must prove capacity in the full-rate test itself;
- verified 1440x1080 `BayerRG8` free-run configuration near 227 fps;
- exposure and gain automation disabled, fixed 500 us exposure, and fixed
  24 dB gain with exact read-back;
- no incomplete frames, capture timeouts, frame-ID gaps, or timestamp resets;
- a host receive interval no longer than 10 nominal frame periods, allowing
  bounded Linux scheduling delay without calling it camera loss;
- a stricter device timestamp interval no longer than 4 nominal frame periods;
- exact payload accounting;
- a full two-second frozen snapshot per camera while capture continues from
  reserve blocks.

The smoke test writes the following repository-relative Bazel undeclared
outputs (qualification and soak substitute their target names):

```text
bazel-testlogs/capture/daheng/dual_camera_smoke_hil_test/test.outputs/report.json
bazel-testlogs/capture/daheng/dual_camera_smoke_hil_test/test.outputs/frame-FDN22120654.png
bazel-testlogs/capture/daheng/dual_camera_smoke_hil_test/test.outputs/frame-FDN23010199.png
```

The report's `diagnostic_frame_path` values are just the PNG filenames,
relative to the report. Along with the demosaiced PNGs, the report contains
raw Bayer image-quality metrics and a classification. The classification is
advisory and deliberately independent of the transport result.

The 2026-07-26 physical smoke classified both connected views
`underexposed_or_obscured`; one camera was nearly black. This is a current
image-readiness failure even though the frame transport checks passed. Before
real swing acceptance, inspect the PNGs and correct obstruction, framing,
lighting, lens/iris state, or exposure as appropriate.

That retained artifact is historical rather than the current physical view.
The 2026-08-09 lit-room station-fixture run retained a new baseline with the
same `tag36h11` ID 0 decoded in both cameras, a localized red response to the
commanded Feather pulse visible in both, and the Feather speaker audible beside
the USB microphone. The selected optical regions appear to be reflections on
the nearby surface rather than direct localization of the LED package. This is
useful optical/audio HIL evidence, but it is not pose or geometric-calibration
evidence.

The 2026-07-26 five-minute qualification passed and exited cleanly after
capturing 68,061 complete frames per camera at 226.872 fps. Both cameras
reported zero timeouts, incomplete frames, and frame-ID gaps. The maximum
device-frame interval was 4.408 ms, and the maximum host-delivery intervals
were 16.339 ms and 15.462 ms. This is the regression baseline for the SDK
lifetime, dual-controller transport, and freeze/continue retention path.

The current MS-01 wiring places both 5 Gb/s camera links on the same PCH xHCI
root controller. A later 15-second dual-camera run with the topology gate
disabled sustained 226.87 fps per camera with no incomplete frames, timeouts,
or frame-ID gaps. Static topology is therefore reported as an operational
warning rather than a hard failure; qualification remains the capacity gate.

### Audio HIL

The physical microphone test is also a Bazel `cc_test`, tagged `manual`,
`local`, and `exclusive`:

```bash
bazel test //capture/audio:audio_hil_test \
  --test_output=streamed --nocache_test_results
```

It captures at least three seconds from the station file's stable
`hw:CARD=<id>,DEV=<number>` selection at 32 kHz signed 16-bit PCM using the
configured hardware channel count and mono channel selection. It reports
sample/block counts, peak and RMS amplitude,
clipping, detected impacts, and timestamp-model limitations at:

```text
bazel-testlogs/capture/audio/audio_hil_test/test.outputs/audio_hil_summary.json
```

The target has explicit pass/fail checks for source completion, sample count,
an elapsed-to-captured-duration ratio from 0.75 through 1.75, RMS amplitude of
at least 0.0001 and peak amplitude of at least 0.001, and no more than 0.1%
clipped samples. The 2026-07-26 live baseline passed all checks with 96,256
samples, a cadence ratio of 1.122, 0.00448 normalized RMS, 0.0675 peak, and no
clipping.

The current `arecord` backend estimates the first block time from host read
completion, then advances by sample count. The report therefore cannot yet
bound ALSA buffering or device latency.

### Combined station-fixture HIL

The shortest combined fixture check is an explicit manual target:

```bash
bazel test //capture/hil:station_fixture_hil_test \
  --test_output=streamed --nocache_test_results
```

It takes the same exclusive hardware lock as the setup-preview service and the
unattended runner. If the preview service owns the cameras, the fixture test
fails at the lock rather than opening a second camera job. The Feather must
already be running the separately flashed `SC-HIL/1` image; building or testing
the host target never flashes it.

The run starts both full-rate camera streams and microphone capture before
issuing any stimulus. It first requests a 300 ms onboard LED locator pulse.
Eight guaranteed-OFF frames and at least 24 frames from the guarded stable
interior of that pulse identify one response ROI independently in each camera.
The run then leaves a 200 ms OFF interval and requests a 44,053 us qualification
pulse (ten nominal periods at 227 fps), with analysis locked to the locator ROI.
Its response is evaluated over a command-relative nine-through-eleven-frame
matched window. At least nine consecutive frames must meet the support gates of
mean red excess 2.0 and changed-red fraction 0.10. At least three supported
frames must also meet all high-confidence gates, including mean red excess 4.0,
and the complete window must retain aggregate mean red excess 3.0 and
changed-red fraction 0.20. This prevents three isolated bright frames from
carrying a result. A free-running exposure can straddle either edge, so partial
edge frames may fall below support. The span and device-local duration remain
acceptance gates. Equality between the two cameras is not treated as
synchronization evidence. The same high-quality AprilTag identity must also be
visible in both views.

Each frame is corrected with a fitted global affine illumination model, and a
candidate region must exceed its surrounding background ring. This rejects
additive and multiplicative room-light flicker while retaining a dim localized
pulse. AprilTag detection runs on deterministic luminance converted from the
demosaiced Bayer frame; treating the color Bayer mosaic itself as grayscale is
not valid for this check.

The detected LED edges are compared with the Feather acknowledgement schedule
using robust camera-device-to-host-receipt mapping. That comparison records one
nominal frame of fixed delivery latency as an operational assumption and is
diagnostic, not an acceptance gate: fixed sensor/readout/USB/SDK latency is not
observable from receipt timestamps alone. The accepted claim is that the long
locator and short qualification produce a consistent localized optical
response in the command-relative windows, together with separately verified
Feather command and device durations. The ROI may be a reflection of the
emitter rather than the LED package itself. This does not prove absolute
camera-to-Feather timing or synchronization between the two cameras.

It then requests a conservative 20 ms, 2 kHz speaker tone and verifies energy,
signal-to-noise ratio, 2 kHz spectral concentration, frequency error, active
duration, and clipping in a bounded command-relative window. Synthetic tests
reject an ambient impulse, amplifier power-on pop, wrong-frequency tone, and
short or overlong stimulus. This proves the expected tone was audible but is
not calibrated acoustic latency. The microphone sample position is
block-granular, and the reported delay also contains USB serial, firmware
scheduling, ALSA/pipe buffering, amplifier, speaker, and acoustic delay.

The undeclared output directory retains `station-fixture-report.json`, the
captured `speaker-microphone.wav`, and clean pre-flash AprilTag, locator-peak,
and qualification-peak PNGs for both camera roles. Cyan ROI overlays and
averaged stable-ON-minus-OFF red-difference PNGs make both accepted and rejected
optical evidence inspectable. The camera rings preallocate 512 full frames each
so bounded serial delays and sequential shutdown cannot overwrite the short
run; the exact allocation is recorded. Reports are written incrementally, and
failure paths retain completed Feather records, audio diagnostics/WAV, and the
latest available clean camera images. Presence of the AprilTag is a
framing/focus/exposure sanity check only; it makes no pose or
geometric-calibration claim.

The 2026-08-09 room-lit run passed in 6.0 seconds of test time. Down-the-line
captured 418 frames and face-on 414, both at about 227.4 fps with zero timeouts
and frame-ID gaps. Down-the-line had 10 consecutive supported and 10
high-confidence frames; face-on had 10 consecutive supported and 7
high-confidence frames. Both qualification windows covered 10 frame positions,
or 44.078 ms in each device-clock domain. Both decoded `tag36h11` ID 0 with
hamming 0 and decision margins 81.6/84.9. The audio check measured exactly 2
kHz, 17.7 dB SNR, and no clipping. The complete artifact is preserved under
`artifacts/hil/station_fixture/20260809T085952-supported-frame-final-pass/`.

### Synthetic-swing application-flow HIL

The shortest physical application check is a separate explicit target:

```bash
bazel test //capture/hil:application_flow_hil_test \
  --test_output=streamed --nocache_test_results
```

It is tagged `manual`, `local`, and `exclusive`, uses the shared hardware lock,
and has an internal 15-second workflow deadline. Stop the hosted preview service
before running it. The test starts the production station backend with HIL
controls explicitly enabled on an ephemeral loopback port, verifies both
configured camera roles, and starts the one-at-a-time operation through
`POST /api/v1/hil/synthetic-swing` with an empty JSON object. The ordinary
application and UI keep this endpoint disabled unless the server is started
with `--enable-hil-controls`.

The operation first establishes an OFF baseline, raises the shared GPIO23
fixture rail, and presents the white candidates
`1,2,3,4,6,8,12,16` for 70 ms each on the external screw-terminal
NeoPixel driven by GPIO21. Preview samples
from both roles must locate the response and produce one common level that is
visible without clipping in either camera. Once the station reports armed, the
Feather runs 60 scaled RGB states at 20 ms each (1.2 seconds), a 20 ms white
impact marker with a simultaneous 10 ms 2 kHz speaker tone, and 25 more 20 ms
states (0.5 seconds). The existing adaptive microphone detector—not the manual
capture endpoint—must accept exactly one tone trigger and drive post-roll,
dual-view encoding, and atomic publication.

Capture is one-shot: publication transitions to `ready` with capture disarmed
and ALSA stopped, while retaining the source-ready flag, counters, adaptive
audio evidence, and accepted trigger for inspection. The HIL validates that
state, downloads the session, then explicitly posts `armed:false` and requires
the application to normalize from `ready` back to `setup`. A following normal
swing requires an explicit re-arm; another synthetic request performs its own
one-shot arm.

The HIL downloads the manifest and both production all-keyframe VP9/WebM assets back through
the HTTP API, checks browser byte-range behavior, parses the WebM structure,
and independently validates role/serial identity, frame continuity, device and
impact-relative timestamps, impact-frame selection, frame rate, dimensions,
frame/keyframe counts, and file sizes. Synthetic `hil_evidence` additionally
must contain the selected brightness and exact sequence constants. For each
camera role it qualifies the white-impact observation, requires its encoded
frame index, and checks the signed offset from that optical frame to the
audio-trigger estimate. The surrounding stepped colors are retained as a human
playback and frame-stepping cue, not checked as exact programmatic states. The
validator also requires the signed mapped-time correction and
residual uncertainty learned from the calibration sweep, bounds the provisional
audio offset, and checks that the two camera offsets agree. It fetches a
full-resolution PNG from each
still-running camera before disarming. Bazel undeclared outputs retain the
incrementally written `application-flow-report.json`, the production
`station-sessions/` directory, the independently fetched `http-session/`
directory, and `diagnostic-down_the_line.png` and
`diagnostic-face_on.png`.

The 2026-08-09 impact-only run passed at the production camera profile of
500 us and 24 dB. The shared selector chose brightness 1. Down-the-line had
four stable, matching white frames with 5.08% maximum saturation and 1.71%
maximum bloom; face-on had three with 3.91% saturation and 1.79% bloom. Both
matching fractions were 1.0. The clips retained 433 and 434 contiguous frames
at approximately 226.87 fps, and the complete operation took 14.75 seconds.
Pinned Chromium subsequently passed against the downloaded physical session.
Evidence is preserved under
`artifacts/hil/application-flow/20260809T182443Z-impact-only-pass/`.

A following profiling run retained 433 contiguous frames from each camera at
approximately 226.87 fps and passed the same physical application HIL. Audio
confirmation to the pre-manifest profile snapshot was 9.517 seconds. Its major
stages were 502.4 ms of required post-roll, 226.7 ms stopping ALSA, 7.6 ms of
prepublication optical analysis, and 8.778 seconds of sequential dual-view
media encoding. Down-the-line encoding took 4.371 seconds and face-on took
4.406 seconds. Across both views, VP8 consumed 5.159 seconds, fitted Bayer
demosaic 3.001 seconds, and RGB-to-I420 conversion 564 ms; muxing,
finalization, and verification were comparatively negligible.

Pinned Chromium then decoded the two physical WebMs and presented both impact
frames 127.9 ms after receiving the manifest response, or 140.2 ms after its
manifest request began. The artifact-only replay cannot reconstruct the live
server response timestamp, so it deliberately leaves the combined
audio-confirmation-to-browser bound unavailable; the deployed server supplies
that timestamp on live responses. The report, physical media and images,
browser screenshot, and structured browser timing are preserved under
`artifacts/hil/application-flow/20260809T200712Z-pipeline-profile-pass/`.

The following production revision replaced sequential software VP8 with two
concurrent direct VA-API VP9 encodes. A fresh physical HIL retained 433
contiguous frames per camera at approximately 226.87 fps and passed the media,
range, optical, audio, and session checks. Hardware media encoding took 3.005
seconds total, down from 8.778 seconds for the preceding software baseline.
The profile snapshot was 3.735 seconds after audio confirmation: 512.2 ms was
the required post-roll wait, 216.3 ms was ALSA shutdown, 8.8 ms was optical
analysis, and image conversion plus media publication dominated the remainder.
For the two concurrent views, fitted Bayer demosaic took 1.528/1.535 seconds,
RGB-to-NV12 conversion 314/309 ms, and VA-API codec submission/readback
1.099/1.117 seconds. The complete physical HIL took 10.47 seconds.

Pinned Chromium replayed the exact hardware-produced VP9 files and passed
decode, seeking, exact stepping, synchronized playback, and evidence rendering.
The report, media, full camera images, browser screenshot, and structured
profiles are preserved under
`artifacts/hil/application-flow/20260809T211742Z-vaapi-vp9-pass/`.

After a physical run, the pinned-Chromium bridge can replay the exact preserved
manifest and WebMs without touching hardware:

```bash
bazel test //web:browser_test \
  --test_output=streamed --nocache_test_results \
  --test_env=SWING_CAPTURE_REQUIRE_HIL_ARTIFACT=1 \
  --test_env=SWING_CAPTURE_HIL_SESSION_DIR=/absolute/path/to/http-session
```

This is an artifact-based browser test: it serves the production bundle and a
read-only facade over the supplied directory. It does not launch or validate a
live production station server. The browser must decode both clips, seek and
step exactly, and retain `hil-review-session.png` at a fixed viewport.

The per-camera audio offset remains provisional: the current `arecord` model
contains ALSA, pipe, scheduling, amplifier, speaker, and acoustic latency, and
the cameras add uncalibrated exposure/readout/transport delay. It measures the
relationship between the two observed markers but is not calibrated true
club/ball impact timing.

### Station doctor

`//tools:station_doctor` is a read-only host/configuration workflow. It
inventories devices through sysfs, procfs, and stable `/dev` links, compares
the installed udev and systemd files byte-for-byte with the repository, checks
the live USB buffer/service state and effective access, and can retain a JSON
report:

```bash
bazel run //tools:station_doctor -- --json artifacts/station/doctor.json
```

Its deterministic parser, inventory, and readiness policy live in
`//tools:station_doctor_test`; the C++ consumers independently exercise the
same strict schema in `//station:station_config_test`.

### Optional unattended runner

`//tools:run_unattended_hil` is an operations runner, not a test
implementation:

```bash
bazel run //tools:run_unattended_hil -- smoke
bazel run //tools:run_unattended_hil -- qualify
bazel run //tools:run_unattended_hil -- soak
```

It invokes the corresponding Bazel `cc_test` and adds a host lock,
duration-plus-grace watchdog, heartbeat/status JSON, durable logs, atomic
result publication, and automatic single-camera diagnostics after a
dual-camera failure. By default the lock is
`artifacts/hil/hardware.lock`; `SWING_CAPTURE_HARDWARE_LOCK` selects the same
provisioned absolute lock path used by an installed preview service.

## HIL ladder

1. Pure unit tests with synthetic clocks, audio, frames, and faults.
2. Process/backend integration against deterministic fake producers.
3. Roughly fifteen-second-or-shorter physical fixture or transport check.
4. Five-minute qualification only when the operator explicitly requests it.
5. Thirty-minute soak only when the operator explicitly requests milestone
   acceptance.
6. Shared-flash optical timing test after electrical frame triggering exists.
7. Recorded real-swing replay through trigger, clip, encoding, and UI.

The replay fixture is important for autonomous iteration: one carefully
captured real session can exercise most of the application repeatedly without
asking a person to swing a club.

## Evidence

Every HIL artifact should contain:

- requested and read-back camera configuration;
- camera serial and USB topology;
- frame, payload, timeout, and gap counts;
- host and device frame rates and independently gated maximum intervals;
- clock-fit drift and residual error;
- retention capacity, reserve, frozen size, and process memory;
- fixed exposure/gain requests and exact camera read-backs;
- raw image-quality metrics plus a relative demosaiced PNG from each view;
- explicit checks with failure messages;
- runner exit state, timestamps, log path, and timeout state.

The current audio artifact includes format, selected device/channel, amplitude,
clipping, impact counts, adaptive noise/threshold values, and its provisional
timestamp model. Later audio evidence should add measured device/buffer
latency, noise-floor distributions, candidate impacts, and calibrated strike
timestamps. UI evidence now includes fixed-viewport browser screenshots and
automated accessibility results; it is not yet a golden-image visual regression
comparison.

## Human-only acceptance points

Automation should leave only a few deliberate tasks for the developer:

- verify safe trigger wiring and the optical sync result;
- provide representative real golf swings and simulator-room audio;
- judge whether impact detection misses or falsely triggers;
- approve playback feel and visual design using concrete screenshot builds.

Everything else should be reproducible from Bazel targets and retained
artifacts.
