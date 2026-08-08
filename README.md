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
planning, and machine-readable HIL evidence. A synthetic end-to-end test drives
those pieces from an audio impact through a retained dual-view clip while
capture continues.

Product services, clip encoding, and the review application remain future
milestones.

## Build

The repository uses Bazel 9.2 and Bzlmod. Bazelisk reads `.bazelversion`.

```bash
bazel test //...
```

This runs 14 hardware-independent C++ `cc_test` targets plus two Python tests
covering the SDK extractor and unattended HIL runner. It includes the synthetic
capture-pipeline, audio-HIL-metrics, and Daheng runtime-packaging tests. Bazel
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

## Prop-Maker firmware

The Adafruit Feather RP2040 Prop-Maker firmware also uses Bazel as its only
build and programming entry point. Bazel downloads the pinned Pico SDK 2.3.0,
Arm GNU toolchain, TinyUSB sources, and picotool; no Arduino CLI, CMake wrapper,
or system cross-compiler is required.

```bash
bazel build //embedded/prop_maker:diagnostic_firmware
sudo ./tools/setup_prop_maker_host.sh  # one time per host
bazel run //embedded/prop_maker:flash_diagnostic
```

The diagnostic image blinks only the onboard red LED and writes heartbeats to
USB serial. The initial factory image may require entering BOOTSEL manually:
hold **BOOT**, tap **RESET**, release **BOOT**, and rerun the flash target.
Firmware built by this repository enables picotool's USB reset interface, so
later flashes can normally reboot and program the board without button presses.
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

These targets open both cameras by serial number, configure full-resolution
`BayerRG8` capture at approximately 227 fps with exposure and gain automation
disabled, a fixed 4000 us exposure, and fixed 0 dB analog gain. They exercise a
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

**Current image warning:** the 2026-07-26 smoke classified both connected views
as `underexposed_or_obscured` (one view was almost entirely black). Transport
passed, but framing, lighting, lens/iris state, and exposure must be corrected
before these cameras are ready to record swings. Image quality is currently an
advisory diagnostic rather than a transport pass/fail gate.

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

It currently exercises the C925e at 32 kHz stereo, selecting channel 0, and
writes
`bazel-testlogs/capture/audio/audio_hil_test/test.outputs/audio_hil_summary.json`.
It rejects source errors, short captures, implausible real-time cadence,
silence, and more than 0.1% clipped samples. The 2026-07-26 live run passed all
five checks with 96,256 samples, 0.00448 normalized RMS, 0.0675 peak, and no
clipped samples.
Its timestamps are derived from host read completion and sample continuity;
ALSA/device latency is not yet measured.

For unattended operation,
`bazel run //tools:run_unattended_hil -- smoke|qualify|soak` invokes the
corresponding Bazel test. The Python runner supplies operational concerns: a
host lock, external watchdog, periodic machine-readable status, durable reports
under `artifacts/hil/runs/`, and automatic per-camera diagnostics after a
dual-camera failure. It is not a test implementation.

This test validates simultaneous transport, not exposure synchronization.
The current USB cameras have independent clocks; true time synchronization
will require a shared electrical frame trigger or an optical-pulse HIL test.
