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
| Optical and images | `//capture/optical:april_tag_test`, `//capture/optical:frame_selection_test`, `//capture/optical:led_pulse_test`, `//capture/optical:led_schedule_association_test`, `//capture/image:bayer_rg8_test`, `//capture/image:image_quality_test` |
| Synthetic end to end | `//capture/pipeline:capture_pipeline_integration_test` |
| SDK packaging | `//capture/daheng:daheng_sdk_runtime_test`, `//third_party/daheng:extract_sdk_test` |
| Station configuration | `//station:station_config_test` |
| Setup preview core | `//capture/preview:camera_settings_test`, `//capture/preview:latest_frame_sampler_test`, `//capture/preview:preview_image_test` |
| Setup service | `//capture/service:camera_worker_test`, `//capture/service:preview_api_test` |
| Setup web UI | `//web:typescript_typecheck_test`, `//web:component_test` |
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

The setup-service tests use a fake camera adapter and synthetic Bayer frames.
They cover overwrite-latest image publication, reset without sequence reuse,
compressed routine rendering, on-demand full-resolution rendering, monotonic
PNG/status sequences, measured frame delivery, bounded timeout and invalid-frame failure,
post-stop command rejection, setting range/increment rejection,
stop/configure/start serialization, preview invalidation, failed-update
recovery, HTTP status and PNG headers, malformed requests, and static asset
serving. The web component test covers dual-view rendering, paired polling and
full-resolution URLs, stale-status clearing, backend error details,
apply/revert interaction, disconnected/error states, runtime schema rejection,
capacity-one slow-client backpressure, atomic pair publication, bounded object
URL retention and cleanup, and an axe accessibility scan. No setup test opens
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
issuing either stimulus. It requests a 44,053 us onboard LED pulse (ten nominal
periods at 227 fps), verifies a bounded pulse in both independently clocked
camera streams, and checks that the same high-quality AprilTag identity is
visible in both views. A free-running exposure can straddle either pulse edge,
so nine through eleven visible frames are accepted; equality between the two
cameras is not treated as synchronization evidence.

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
observable from receipt timestamps alone. The accepted claim is a bounded
localized pulse in the command-relative 48-frame window, together with the
separately verified Feather command and device duration. It does not prove
absolute camera-to-Feather timing or synchronization between the two cameras.

It then requests a conservative 20 ms, 2 kHz speaker tone and verifies energy,
signal-to-noise ratio, 2 kHz spectral concentration, frequency error, active
duration, and clipping in a bounded command-relative window. Synthetic tests
reject an ambient impulse, amplifier power-on pop, wrong-frequency tone, and
short or overlong stimulus. This proves the expected tone was audible but is
not calibrated acoustic latency. The microphone sample position is
block-granular, and the reported delay also contains USB serial, firmware
scheduling, ALSA/pipe buffering, amplifier, speaker, and acoustic delay.

The undeclared output directory retains `station-fixture-report.json`, the
captured `speaker-microphone.wav`, and clean pre-flash AprilTag and accepted
peak-LED PNGs for both camera roles. The camera rings preallocate 512 full
frames each so bounded serial delays and sequential shutdown cannot overwrite
the short run; the exact allocation is recorded. Reports are written
incrementally, and failure paths retain completed Feather records, audio
diagnostics/WAV, and the latest available clean camera images. Presence of the
AprilTag is a framing/focus/exposure sanity check only; it makes no pose or
geometric-calibration claim.

The 2026-08-09 room-lit run passed in 8.8 seconds of test time. Down-the-line
captured 304 frames and face-on 299, both at about 227.6 fps with zero timeouts
and frame-ID gaps. The cameras observed 10 and 9 active LED frames respectively
across the same ten-position span, or 44.078 ms in each device-clock domain.
Both decoded `tag36h11` ID 0 with hamming 0 and decision margins 48.3/50.3. The
audio check measured exactly 2 kHz, 20.1 dB SNR, and no clipping. The complete
artifact is preserved under
`artifacts/hil/station-fixture-20260809T014339Z-attempt10/`.

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
clipping, impact counts, and its provisional timestamp model. Later audio
evidence should add measured device/buffer latency, noise-floor distributions,
candidate impacts, and detected strike timestamps. Future UI artifacts should
include browser screenshots at fixed viewport sizes and accessibility results.

## Human-only acceptance points

Automation should leave only a few deliberate tasks for the developer:

- verify safe trigger wiring and the optical sync result;
- provide representative real golf swings and simulator-room audio;
- judge whether impact detection misses or falsely triggers;
- approve playback feel and visual design using concrete screenshot builds.

Everything else should be reproducible from Bazel targets and retained
artifacts.
