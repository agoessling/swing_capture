# Testing Strategy

## Default software suite

```bash
bazel test //...
```

The default suite consists of 14 hardware-independent C++ `cc_test` targets and
two Python tests:

| Area | Bazel targets |
|---|---|
| Audio and trigger | `//capture/audio:arecord_pcm_source_test`, `//capture/audio:audio_hil_metrics_test`, `//capture/trigger:impact_detector_test` |
| Storage and timing | `//capture/core:camera_source_test`, `//capture/core:raw_frame_ring_test`, `//capture/core:pooled_raw_frame_ring_test`, `//capture/core:device_clock_mapper_test` |
| Session and clip selection | `//capture/session:capture_session_coordinator_test`, `//capture/clip:clip_window_planner_test` |
| HIL rules and images | `//capture/hil:hil_metrics_test`, `//capture/image:bayer_rg8_test`, `//capture/image:image_quality_test` |
| Synthetic end to end | `//capture/pipeline:capture_pipeline_integration_test` |
| SDK packaging | `//capture/daheng:daheng_sdk_runtime_test`, `//third_party/daheng:extract_sdk_test` |
| Host tooling | `//tools:unattended_hil_test` |

The end-to-end fixture synthesizes two cameras with unrelated device clocks
and an audio impact. It verifies post-roll waiting, reference-counted freezing,
bounded strike-relative selection for both views, and continued recording
while the frozen clip remains valid. Session boundary tests distinguish the
backdated strike sample from its later confirmation time, wait one frame
beyond the requested endpoint, and explicitly expire a late request before
pre-roll can be overwritten. The audio source unit test launches a
deterministic fake producer; it does not open host audio hardware.

All test logic above is owned by Bazel. The application tests are `cc_test`
binaries; the SDK extractor and unattended-runner tests are `py_test` targets.
Shell scripts are not used as test implementations. Synthetic tests cover
timing boundaries and failure paths, not only nominal examples. The wildcard
suite downloads Galaxy Linux SDK
`2.6.2606.9251`, verifies its SHA-256, safely extracts an allowlisted SDK
surface, and tests that the GenTL producer is available through Bazel runfiles.
It does not open a camera. The repository adapter is documented in
[`third_party/daheng/README.md`](../third_party/daheng/README.md).
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

The stages run for 15 seconds, 5 minutes, and 30 minutes. They require:

- the persistent 2000 MB `usbfs_memory_mb` configuration installed by
  `sudo ./tools/setup_daheng_host.sh`;
- exactly two selected cameras with read/write access;
- distinct USB root controllers by default;
- verified 1440x1080 `BayerRG8` free-run configuration near 227 fps;
- exposure and gain automation disabled, fixed 4000 us exposure, and fixed
  0 dB gain with exact read-back;
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

The 2026-07-26 five-minute qualification passed and exited cleanly after
capturing 68,061 complete frames per camera at 226.872 fps. Both cameras
reported zero timeouts, incomplete frames, and frame-ID gaps. The maximum
device-frame interval was 4.408 ms, and the maximum host-delivery intervals
were 16.339 ms and 15.462 ms. This is the regression baseline for the SDK
lifetime, dual-controller transport, and freeze/continue retention path.

### Audio HIL

The physical microphone test is also a Bazel `cc_test`, tagged `manual`,
`local`, and `exclusive`:

```bash
bazel test //capture/audio:audio_hil_test \
  --test_output=streamed --nocache_test_results
```

It captures at least three seconds from `hw:CARD=C925e,DEV=0` at 32 kHz,
two-channel signed 16-bit PCM and analyzes channel 0. It reports sample/block
counts, peak and RMS amplitude, clipping, detected impacts, and timestamp-model
limitations at:

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
dual-camera failure.

## HIL ladder

1. Pure unit tests with synthetic clocks, audio, frames, and faults.
2. Process/backend integration against deterministic fake producers.
3. Fifteen-second physical transport and retention smoke.
4. Five-minute qualification after meaningful capture-path changes.
5. Thirty-minute soak before declaring a capture milestone stable.
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
