# Swing Capture

Linux-first dual high-speed camera capture and golf swing review.

Design and validation details live in
[`docs/architecture.md`](docs/architecture.md) and
[`docs/testing.md`](docs/testing.md).
The active two-phone replacement investigation is documented in
[`docs/android.md`](docs/android.md).
For provisioning a fresh Linux capture host, follow
[`docs/headless_setup.md`](docs/headless_setup.md).

## Current application

The first headless application continuously acquires full-resolution frames
from two Daheng `MER2-160-227U3C` cameras through the Galaxy Linux SDK. Its
React/TypeScript station UI combines camera setup with one-shot swing capture
and dual-view review. Arming starts a continuous ALSA monitor that applies the
adaptive impact detector through trigger acceptance and post-roll. An accepted
audio impact freezes one generation of each preallocated camera buffer and
rotates acquisition onto the other generation, so both cameras keep running
while the completed clip is encoded.

The production publisher retains the full 1440x1080 Bayer geometry, demosaics
and converts frames to NV12 on a bounded CPU worker pool, and overlaps that
work with queued Intel GPU VA-API VP9 low-power encoding. Both views encode
concurrently. Every encoded frame is a keyframe so browser seeking and exact
source-frame stepping remain deterministic. The WebMs are video-only:
microphone samples are used for trigger detection and are not encoded. A
deterministic software VP8 backend remains for hermetic fixtures and
encoder-independent tests. Before video encoding, the publisher renders the
two exact trigger-nearest frames as full-resolution JPEGs so the review page
can show useful evidence after post-roll without waiting for both videos. The
complete session is still atomically published only after both media files and
the manifest are ready.

The persistent on-disk catalog is cached in memory at startup and refreshed
after publication; media routes support byte ranges for browser seeking. A
server-sent event endpoint notifies connected browsers when capture, session,
or early-impact-image state changes. The UI uses a 15-second poll only as a
safety net while that event stream is connected.

Capture is deliberately one-shot: ALSA monitoring stops after the rings rotate;
after publication the application enters `ready`, releases the four raw rings,
and requires an explicit re-arm for the next swing. The audio-derived trigger
time is an uncalibrated estimate; ALSA/device latency and the physical sound
path are not yet characterized well enough to call it the exact ball-impact
time.

An explicitly enabled synthetic-swing HIL mode exercises that same production
capture path without asking for a real swing. It automatically evaluates the
external screw-terminal fixture NeoPixel brightness candidates
`1,2,3,4,6,8,12,16` in both camera previews, arms one capture, and runs
the Feather's deterministic optical and audio sequence. The resulting session
keeps the per-camera optical white-impact check and the signed offset from that
frame to the audio-trigger estimate as durable evidence. The surrounding
colors are human playback cues rather than programmatic acceptance gates. The
offset is measured evidence, not calibrated true-impact timing.

## Build

The repository uses Bazel 9.2, Bzlmod, and C++23. Bazelisk reads
`.bazelversion`.

```bash
bazel test //...
```

The Android replacement path is also built entirely through Bazel:

```bash
bazel build //android/app:swing_capture
bazel run //tools/android:adb -- devices -l
```

This is now a production-shaped continuous capture node rather than a
visible-activity or synthetic-trigger-only probe. An armed foreground service
owns Camera2, the microphone, a hardware H.264 encoder, and a bounded encoded
pre-roll ring; a local audio impact freezes a real MP4 plus timestamped manifest
without depending on the activity or browser lifecycle. Roles and capture
profiles are configured independently. The Pixel 6 keeps the detector default
floor of 0.015, while an isolated exact-model Pixel 5a policy uses 0.012; the
Pixel 5a accommodation does not change Pixel 6 tuning.

Each phone serves its sessions to the existing browser over bounded HTTP. The
browser also has a two-node coordinator that assigns one shared session ID,
estimates clock-offset bounds, admits exactly one `down_the_line` and one
`face_on` trigger, combines both clips, and replicates an immutable coordination
record to both phones. Short physical milestones are preserved under
[`artifacts/android_dual_hil_20260815T1744Z`](artifacts/android_dual_hil_20260815T1744Z)
and
[`artifacts/android_screen_off_hil_20260815T1745Z`](artifacts/android_screen_off_hil_20260815T1745Z).
The sequential run remains useful local-pipeline evidence, and the subsequent
[`artifacts/android_dual_hil_concurrent_20260815T105807Z/report.json`](artifacts/android_dual_hil_concurrent_20260815T105807Z/report.json)
passed the one-event concurrent gate: one Feather swing drove Pixel 6
1080p240/24 Mbit/s and Pixel 5a 720p240/12 Mbit/s captures, with 6.377 ms and
14.100 ms optical/audio timing bounds, persistent `tag36h11` ID 0, and the
durable coordination record verified on both phones. The paired live H.264
browser path also passed; its preserved screenshot is
[`artifacts/android_browser_hil_20260815T110817Z/outputs/android-dual-node-review.png`](artifacts/android_browser_hil_20260815T110817Z/outputs/android-dual-node-review.png).
That live H.264 gate uses
`--test_env=SWING_CAPTURE_CHROME_EXECUTABLE=/usr/bin/google-chrome` because the
bundled Chromium headless shell lacks proprietary H.264 decoding.
A requested 15-minute Pixel 6 1080p240 isolation completed 215,817 encoded
frames at 239.077 fps and published a valid terminal clip, but failed thermal
acceptance after reaching Android status `SEVERE`; evidence is preserved at
[`artifacts/android_soak_20260815T193311Z`](artifacts/android_soak_20260815T193311Z).
A controlled 720p240/12 Mbit/s comparison then passed the same 15-minute gate
at 239.064 fps without exceeding `MODERATE`; it delayed the first `LIGHT` and
`MODERATE` samples from 150/390 seconds to 300/630 seconds and halved the
encoded-ring footprint. Evidence is preserved at
[`artifacts/android_soak_720p_20260815T135742Z`](artifacts/android_soak_720p_20260815T135742Z).
New installations and Android HIL now use 720p240/12 Mbit/s as the standard on
both roles; 1080p240 remains an explicit option. Normal operation leaves the
display free to sleep while the foreground service holds only a partial CPU
wake lock.
The standard Pixel 6 screen-off gate and a concurrent one-event two-phone gate
then passed at 720p240 on both nodes. The latter retained persistent AprilTag
ID 0, localized Feather LED/tone evidence, 5.659/16.394 ms conservative local
timing bounds, and the durable replicated coordination record; evidence is at
[`artifacts/android_dual_hil_concurrent_720p_20260815T142302Z`](artifacts/android_dual_hil_concurrent_720p_20260815T142302Z).
A publication-heavy precursor also exposed a one-frame Camera2/encoder ordinal
shift after 5 minutes 27 seconds. Five-minute qualification and a 30-minute
soak remain unclaimed. Commands, contracts, metrics, and remaining risks are
documented in [`docs/android.md`](docs/android.md). All phone HIL targets are
manual, local, and exclusive, so the ordinary software-only suite does not
select them.

This runs the hardware-independent C++, Python, and TypeScript/UI suite. It
covers the station doctor, SDK extractor, unattended HIL runner, synthetic
capture pipeline, setup-preview service, one-shot capture controller, session
publisher and catalog, software fixture encoder, React review application,
Playwright browser workflow, station configuration, HIL protocol and analysis
components, and Daheng runtime packaging. Bazel
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

Production clip encoding also requires the Intel iHD VA-API runtime, access to
`/dev/dri/renderD128` through the `render` group, and VP9 low-power encode
support. Bazel pins the libva headers used to compile the direct API boundary;
the render node, `libva.so.2`, `libva-drm.so.2`, and GPU driver are host runtime
dependencies. The exact provisioning and `vainfo` check are in
[`docs/headless_setup.md`](docs/headless_setup.md).

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

## Headless station UI

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
swaps both views after both downloads and browser decodes complete, and skips
superseded pairs without cancelling downloads or forming a queue. It does not
control or block the capture cadence. The status API includes source/render
age, encoded size, and per-stage quality/Bayer/resize/encode timings for
diagnosing stalls. The physical application-flow HIL also requests a
full-resolution preview from each still-running camera after clip publication,
qualifying the setup-preview copy path as part of the production station
workflow.

Exposure and gain changes are validated against each camera's read-back range,
then performed on that camera's owner thread as a stop/configure/start cycle.
They are session-local. The same UI can arm the microphone-triggered capture,
show capture state, discover sessions stored under `artifacts/sessions/` by
default, and review a completed session. Pass `--sessions-root` to choose a
different persistent catalog root. Arming allocates two 448-frame,
full-resolution raw rings per camera: approximately 2.596 GiB of payload across
the four rings, before SDK, preview, encoder, and other process memory.

Synthetic-swing HIL controls are disabled and absent from the UI by default.
Start the service with the explicit `--enable-hil-controls` option to expose
the button and `POST /api/v1/hil/synthetic-swing` endpoint:

```bash
bazel run //capture/service:preview_server -- --enable-hil-controls
```

One click owns calibration, arming, stimulus, post-roll, encoding, and session
publication. Capture finishes `ready` and unarmed; explicitly re-arm before a
normal next swing, or start another synthetic run to let that operation perform
its own one-shot arm.

Stop the service before running a direct camera HIL target. The service and
unattended HIL runner share `artifacts/hil/hardware.lock`, which prevents those
two entry points from opening the cameras concurrently. Installed services can
set `SWING_CAPTURE_HARDWARE_LOCK` to a provisioned runtime path; the unattended
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
The separate HIL image implements the negotiated `SC-HIL/1` fixture protocol;
it is programmed only through the explicit
`//embedded/prop_maker:flash_hil` target. Its synthetic-swing mode drives the
external screw-terminal NeoPixel on GPIO21. Calibration raises the shared
GPIO23 power rail and keeps it prepared through the immediately following
swing; that rail powers the speaker amplifier, external NeoPixel, and servo
terminal together. Every completion, failure, or preparation timeout turns the
fixture pixel off, quiesces I2S, and lowers the rail. Review the exact flash
disclosure in the embedded guide first.
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
that a conservative 20 ms, 2 kHz, 125-permille speaker tone has the expected
energy, frequency content, and duration above the microphone noise floor. It retains
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

The shortest complete application check exercises the explicitly enabled
synthetic-swing operation through the production station backend, real
microphone trigger, one-shot ring rotation, hardware dual-view encoding,
atomic publication, catalog and HTTP byte-range serving, and the
post-publication setup-preview path:

```bash
bazel test //capture/hil:application_flow_hil_test \
  --test_output=streamed --nocache_test_results
```

It has an internal 15-second workflow deadline. Through
`POST /api/v1/hil/synthetic-swing`, it first sweeps eight shared brightness
candidates for both views, then runs 1.2 seconds of stepped pre-impact color, a
20 ms white impact marker with a simultaneous 10 ms 2 kHz tone at 125 permille,
and 0.5 seconds of stepped follow-through color. The application must finish
`ready` and unarmed after exactly one accepted audio trigger. Automated acceptance checks
the white marker in both cameras, the audio trigger, retained-frame continuity,
encoded media, HTTP byte ranges, and the camera-to-Feather timestamp correction.
The surrounding colors are deliberately a human playback cue for watching and
frame stepping; their exact hues are not machine-qualified. The report,
published and independently downloaded session trees, and one full-resolution
diagnostic image per camera are preserved. The audio offset is useful measured
optical/audio evidence, but ALSA, camera, amplifier, and acoustic latency are
not calibrated, so it is not a true-impact timing claim.

Published sessions also retain a versioned pipeline profile from audio
confirmation through capture freeze, analysis, early-impact rendering,
per-view image conversion and codec/WebM encoding, and the last pre-manifest
snapshot. The review page combines that backend profile with manifest-fetch
and decoded-impact-frame presentation timing, making post-impact latency
regressions visible in both physical HIL reports and normal recorded sessions.

The latest 2026-08-09 low-latency production run passed at the normal 500 us
exposure and 24 dB gain. Both full-resolution VP9 clips retained 433 contiguous
all-keyframe frames at approximately 226.87 fps. The exact trigger-nearest JPEG
pair was server-ready 538 ms after audio confirmation, including the required
500 ms post-roll. Concurrent hardware media encoding took 2.901 seconds and
the complete profile snapshot was captured 3.440 seconds after confirmation.
Pinned Chromium decoded the physical files, sought and stepped exactly, and
presented both impact frames 398 ms after beginning its manifest request. The
report, media, camera images, and browser evidence are retained under
`artifacts/hil/application-flow/20260809T230608Z-fullres-low-latency-pass/`.

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
