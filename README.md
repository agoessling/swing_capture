# Swing Capture

Linux-first dual high-speed camera capture and golf swing review.

Design and validation details live in
[`docs/architecture.md`](docs/architecture.md) and
[`docs/testing.md`](docs/testing.md).
For provisioning a fresh Linux capture host, follow
[`docs/headless_setup.md`](docs/headless_setup.md).

## Current milestone

The current milestone proves simultaneous full-resolution capture from two
Daheng `MER2-160-227U3C` cameras through the Galaxy Linux SDK. The capture
path now includes independently fitted device clocks, preallocated
freeze/continue frame rings, audio impact detection, strike-relative clip
planning, persistent machine-local device/role selection, and machine-readable
HIL evidence. A synthetic end-to-end test drives those pieces from an audio
impact through a retained dual-view clip while capture continues.

A headless setup service now streams responsive, bounded-rate compressed
640x480 previews from the two full-speed acquisition loops, renders an explicit
full-resolution focus view on demand, and serves a React/TypeScript camera setup
UI. Clip encoding and the review application remain future milestones.

## Build

The repository uses Bazel 9.2, Bzlmod, and C++23. Bazelisk reads
`.bazelversion`.

```bash
bazel test //...
```

This runs the hardware-independent C++, Python, and TypeScript/UI suite. It
covers the station doctor, SDK extractor, unattended HIL runner, synthetic
capture pipeline, setup-preview service, station configuration, HIL protocol
and analysis components, and Daheng runtime packaging. Bazel
downloads checksum-pinned CPython 3.11.14 for every Python target, so those
targets do not use the host Python runtime. The Daheng repository bootstrap
does require a broadly compatible host `python3` to unpack the vendor's
self-extracting payload. Bazel downloads and packages the resulting Galaxy SDK
as a normal repository dependency, but no default test opens a physical
device. Targets that access cameras or audio devices are tagged `manual` and
are not selected by `//...`.

Hardware builds automatically download the checksum-pinned Galaxy Linux SDK:

```bash
bazel run //capture/daheng:camera_probe
```

The repository uses Daheng Galaxy Linux SDK `2.6.2606.9251`. Bazel verifies
the vendor archive's SHA-256, extracts its self-extracting payload without
running the vendor installer, and exposes only the required headers, shared
libraries, and GenTL producer. The vendor-specific repository rule and archive
adapter live under [`third_party/daheng`](third_party/daheng/README.md); the
GenTL producer is delivered as a Bazel runfile rather than an absolute build
cache path compiled into the application.

One-time host access setup requires root privileges:

```bash
sudo ./tools/setup_daheng_host.sh
```

This installs a Daheng USB udev rule and a systemd oneshot that applies
Daheng's 2000 MB `usbfs_memory_mb` setting immediately and after every boot.
The value matches the configuration performed by the vendor installer, but
the repository does not install the Galaxy SDK or its kernel modules
system-wide. If udev does not update an already connected device, reconnect
the camera once.

The hardware probe first reports whether Linux can see and open each Daheng USB
device. It then prints camera identity, negotiated capture settings, frame
counts, payload failures, frame-ID gaps, device timestamp span, host elapsed
time, measured frame rate, and representative-frame image diagnostics.

## Machine-local station configuration

Physical tests read stable identifiers from an ignored `.station.local.conf`.
The file records down-the-line and face-on camera serials, a named ALSA device,
and the Feather's `/dev/serial/by-id` path. `.bazelrc.local` passes its absolute
path to Bazel tests through `SWING_CAPTURE_STATION_CONFIG`; neither file belongs
in Git. Camera HIL refuses to start until `camera.roles_verified = true` records
that a person has physically confirmed both views.

Run the read-only readiness workflow before HIL:

```bash
bazel run //tools:station_doctor -- --json artifacts/station/doctor.json
```

The doctor validates the schema and role gate, exact installed udev/service
files, active `plugdev` membership, persistent USB buffering, camera serials,
SuperSpeed access and root-controller topology, the configured ALSA capture
node, and the stable Feather serial link. See
[`docs/headless_setup.md`](docs/headless_setup.md) for the file format and setup
sequence.

## Headless camera setup UI

Run the camera setup service from the repository root:

```bash
bazel run //capture/service:preview_server
```

It listens on port 8080 on all interfaces so another computer on the trusted
station network can open `http://<station-address>:8080/`. The service opens
both configured cameras exclusively, keeps their acquisition loops at the
camera's full configured rate, and renders up to about 30 compressed 640x480
routine JPEGs per second on a separate latest-only renderer thread. A
full-resolution focus image is encoded only when its link is requested. Browser
polling fetches the two routine images as one capacity-one pair, atomically
swaps both views after both downloads and browser decodes complete, and skips superseded pairs
without cancelling downloads or forming a queue. It does not control or block
the capture cadence. The status API includes source/render age, encoded size,
and per-stage quality/Bayer/resize/encode timings for diagnosing stalls.

Exposure and gain changes are validated against each camera's read-back range,
then performed on that camera's owner thread as a stop/configure/start cycle.
They are session-local in this milestone. Stop the preview service before
running a direct camera HIL target. The service and unattended HIL runner share
`artifacts/hil/hardware.lock`, which prevents those two entry points from
opening the cameras concurrently. Installed services can set
`SWING_CAPTURE_HARDWARE_LOCK` to a provisioned runtime path; the unattended
runner must receive the same value.

## Prop-Maker firmware

The Adafruit Feather RP2040 Prop-Maker firmware also uses Bazel as its only
build and programming entry point. Bazel downloads the pinned Pico SDK 2.3.0,
Arm GNU toolchain, TinyUSB sources, and picotool; no Arduino CLI, CMake wrapper,
or system cross-compiler is required.

```bash
bazel build //embedded/prop_maker:diagnostic_firmware
bazel build //embedded/prop_maker:hil_firmware
sudo ./tools/setup_prop_maker_host.sh  # one time per host
bazel run //embedded/prop_maker:flash_diagnostic
```

The diagnostic image blinks only the onboard red LED and writes heartbeats to
USB serial. The initial factory image may require entering BOOTSEL manually:
hold **BOOT**, tap **RESET**, release **BOOT**, and rerun the flash target.
Firmware built by this repository enables picotool's USB reset interface, so
later flashes can normally reboot and program the board without button presses.
The separate HIL image implements the negotiated `SC-HIL/1` LED/tone protocol;
it is programmed only through the explicit
`//embedded/prop_maker:flash_hil` target. A tone temporarily enables GPIO23,
which powers the speaker amplifier and the external NeoPixel and servo rails
together, so review the exact flash disclosure in the embedded guide first.
See [`docs/embedded.md`](docs/embedded.md) for the complete workflow and board
configuration evidence.

## Hardware-in-the-loop tests

Hardware tests are explicit Bazel targets because they require two locally
attached cameras and exclusive access to them. They are tagged `manual`,
`local`, and `exclusive`, so ordinary `bazel test //...` remains deterministic
and hardware-independent. The canonical hardware tests are compiled C++
`cc_test` targets:

```bash
bazel test //capture/daheng:dual_camera_smoke_hil_test \
  --test_output=streamed --nocache_test_results
bazel test //capture/daheng:dual_camera_qualify_hil_test \
  --test_output=streamed --nocache_test_results
bazel test //capture/daheng:dual_camera_soak_hil_test \
  --test_output=streamed --nocache_test_results
```

These targets open the two station-assigned camera roles by serial number,
record each role in the report, and configure full-resolution `BayerRG8`
capture at approximately 227 fps with exposure and gain automation disabled, a
fixed 500 us exposure, and fixed 24 dB analog gain. They exercise a
two-second raw frame ring per camera, freeze the full retained window without
copying its payloads, and continue recording from a preallocated reserve. At
the current 1,555,200-byte frame size, the active and reserve pools use about
1.41 GB per camera (2.82 GB total); measured process RSS after the dual-camera
smoke test is about 3.0 GB.

Each run publishes a report and one demosaiced diagnostic image per camera at
repository-relative paths such as:

```text
bazel-testlogs/capture/daheng/dual_camera_smoke_hil_test/test.outputs/report.json
bazel-testlogs/capture/daheng/dual_camera_smoke_hil_test/test.outputs/frame-FDN22120654.png
bazel-testlogs/capture/daheng/dual_camera_smoke_hil_test/test.outputs/frame-FDN23010199.png
```

The report stores each PNG as a relative `diagnostic_frame_path`, so a report
and its images can be moved together. The image-quality section records raw
histogram percentiles, black/white fractions, and Bayer-aware gradient energy.

The 2026-08-09 room-lit station-fixture run replaced the earlier dark image
baseline. Both retained views decode the printed `tag36h11` ID 0 without a bit
correction, with decision margins of 48.3 and 50.3. Image-quality histograms
remain diagnostic rather than a transport gate.

The stages run for 15 seconds, 5 minutes, and 30 minutes respectively.
Qualification requires at least 99% of the requested frame rate with no
incomplete frames or frame-ID gaps. Host delivery and device timing use
separate stall gates: 10 frame periods for host scheduling and 4 frame periods
for device timestamps (about 44 ms and 18 ms at 227 fps).

The 2026-07-26 five-minute qualification completed cleanly, including SDK
shutdown. Each camera delivered 68,061 complete frames at 226.872 fps with
zero timeouts, incomplete frames, or frame-ID gaps. The largest device-frame
interval was 4.408 ms; the largest host-delivery intervals were 16.339 ms and
15.462 ms.

The microphone path has its own explicit Bazel HIL target:

```bash
bazel test //capture/audio:audio_hil_test \
  --test_output=streamed --nocache_test_results
```

It exercises the station-configured ALSA device at 32 kHz using its configured
hardware channel count and mono channel selection, and writes
`bazel-testlogs/capture/audio/audio_hil_test/test.outputs/audio_hil_summary.json`.
It rejects source errors, short captures, implausible real-time cadence,
silence, and more than 0.1% clipped samples. The 2026-07-26 live run passed all
five checks with 96,256 samples, 0.00448 normalized RMS, 0.0675 peak, and no
clipped samples.
Its timestamps are derived from host read completion and sample continuity;
ALSA/device latency is not yet measured.

The shortest combined station-fixture check verifies the scene the operator
can see in the setup preview:

```bash
bazel test //capture/hil:station_fixture_hil_test \
  --test_output=streamed --nocache_test_results
```

It first requests a 300 ms Feather LED locator pulse, averages a guarded stable
ON interval against a guaranteed-OFF baseline, and chooses one localized
response per camera. It then locks a 44,053 us qualification pulse to that ROI
and checks its response across a scheduled nine-through-eleven-frame matched
window. At least nine consecutive frames must meet the lower support threshold,
at least three of those must also meet the nested high-confidence threshold,
and the complete window must pass aggregate signal gates. Partial free-running
exposure-edge frames may fall below support. Whole-frame room-light variation
is corrected and a local background ring is subtracted in both stages. It also
verifies that the same AprilTag identity is present in both views and checks
that a conservative 20 ms, 2 kHz speaker tone has the expected energy,
frequency content, and duration above the microphone noise floor. It retains
an incremental JSON report, a WAV, clean tag/locator/qualification PNGs, ROI
overlays, and averaged red-difference images for both roles, including partial
evidence on failure. The selected ROI can be an optically useful reflection
rather than the LED package itself. This target requires the HIL image to have
been explicitly flashed beforehand and shares the camera hardware lock with
the preview service.
AprilTag presence is a framing/focus/exposure sanity check, not pose or
geometric calibration.

The 2026-08-09 lit-room run passed: both cameras had zero timeouts and frame-ID
gaps. Down-the-line had 10 consecutive supported and 10 high-confidence frames;
face-on had 10 consecutive supported and 7 high-confidence frames. Both
qualification windows covered 10 frame positions. Both cameras decoded
`tag36h11` ID 0 with zero corrected bits, and the microphone recorded the
commanded 2 kHz tone at 17.7 dB SNR with no clipped samples. The selected
optical regions appear to be nearby reflections rather than direct localization
of the Feather LED package. Camera receipt times are also compared with the
Feather schedule, but that comparison is an explicit non-gating diagnostic:
fixed camera/readout/USB delivery latency has not been calibrated, so absolute
camera-to-Feather edge association is not claimed.

For unattended operation,
`bazel run //tools:run_unattended_hil -- smoke|qualify|soak` invokes the
corresponding Bazel test. The Python runner supplies operational concerns: a
host lock, external watchdog, periodic machine-readable status, durable reports
under `artifacts/hil/runs/`, and automatic per-camera diagnostics after a
dual-camera failure. It is not a test implementation.

The transport and station-fixture tests do not prove exposure synchronization.
The current USB cameras have independent clocks; true time synchronization
will require a verified shared electrical frame trigger. The optical pulse is
retained as visibility and independently mapped timing evidence only.
