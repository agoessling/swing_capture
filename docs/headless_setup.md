# Headless capture host setup

This is the bootstrap checklist for a fresh Linux host connected to the two
Daheng cameras, USB microphone, and Adafruit Feather RP2040 Prop-Maker.
Machine-specific device selections do not belong in Git.

## Base system

Install Git, Bazelisk, a C/C++ host toolchain, Python 3 for unpacking the
vendor SDK, ALSA utilities, and USB utilities. On Ubuntu or Debian, the
non-Bazel packages are:

```bash
sudo apt update
sudo apt install alsa-utils build-essential git python3 usbutils
```

Install Bazelisk as `bazel` using its upstream release or package for the host.
The checked-in `.bazelversion` selects the repository's Bazel version; Bazel
downloads the pinned Python, Arm, Pico SDK, Daheng SDK, and lint toolchains.
Keep at least 20 GB free for the first build: the pinned LLVM distribution and
embedded toolchains expand substantially before Bazel can reuse them from its
local cache.

Clone the repository and first validate it without touching hardware:

```bash
git clone https://github.com/agoessling/swing_capture.git
cd swing_capture
bazel build //...
bazel test //... --test_output=errors
```

The committed module graph pins `bazel_devtools` to an immutable Git commit.
Developers changing both repositories may temporarily add this ignored local
override to `.bazelrc.local`:

```text
common --override_module=bazel_devtools=/absolute/path/to/bazel_devtools
```

Do not commit `.bazelrc.local`.

## Device permissions and USB buffering

The setup scripts require the `plugdev` group. Minimal installations may need
to create it first:

```bash
getent group plugdev >/dev/null || sudo groupadd --system plugdev
```

Install the checked-in host rules:

```bash
sudo ./tools/setup_daheng_host.sh
sudo ./tools/setup_prop_maker_host.sh
```

Confirm that the account which will run capture belongs to it:

```bash
id
getent group plugdev
```

If needed, add the account and then log out and back in before testing:

```bash
sudo usermod -aG plugdev "$USER"
```

On a headless host, the capture account must also have read/write access to the
configured ALSA capture nodes. Desktop logind ACLs may grant that access; if
they do not, add the account to `audio` and log out and back in:

```bash
sudo usermod -aG audio "$USER"
```

Reconnect the USB devices after installing the rules. Confirm the persistent
Daheng USB buffer setting and its boot service:

```bash
cat /sys/module/usbcore/parameters/usbfs_memory_mb
systemctl is-enabled swing-capture-usbfs-memory.service
systemctl is-active swing-capture-usbfs-memory.service
```

The value must be `2000`, and both systemd queries must report success.

## Hardware inventory

Record stable identifiers before assigning camera roles:

```bash
lsusb -t
ls -l /dev/serial/by-id/
arecord --list-devices
bazel run //capture/daheng:camera_probe -- \
  --duration-seconds 1 --require-camera-count 2
```

The camera inventory must show both expected serial numbers at USB SuperSpeed.
Distinct USB root controllers are preferred for full-rate dual capture, but
connector topology alone does not establish capacity. When both SuperSpeed
links share a root controller, preserve a full-rate dual-camera smoke and
qualification report proving that the actual controller sustains the load.
Use a stable ALSA card ID and the Feather's `/dev/serial/by-id/` link instead
of volatile numeric device names such as `hw:0` or `/dev/ttyACM0`.

The audio HIL reads its stable ALSA identifier from the machine-local station
file rather than using a numeric card index or a checked-in device name.

## Station configuration and doctor

Create an ignored `.station.local.conf` at the repository root after recording
the stable identifiers:

```text
schema_version = 1
camera.roles_verified = false
camera.down_the_line.serial = FDN00000001
camera.face_on.serial = FDN00000002
audio.alsa_device = hw:CARD=microphone,DEV=0
audio.channel_count = 1
audio.selected_channel = 0
feather.serial_path = /dev/serial/by-id/usb-Raspberry_Pi_Pico_SERIAL-if00
```

Set `audio.channel_count` to the microphone's hardware capture-channel count
and `audio.selected_channel` to the zero-based channel retained as mono. The
station doctor records these choices; `arecord --dump-hw-params --format
S16_LE --rate 32000 -D <device> /dev/null` reports the hardware channel range.

The two serial assignments are only a draft while
`camera.roles_verified = false`. Put an identifiable target in one view or
inspect a one-camera diagnostic, physically confirm both positions, correct
the mapping if necessary, and only then set the value to `true`.

Pass the absolute path into physical Bazel tests with the already ignored
`.bazelrc.local`:

```text
test --test_env=SWING_CAPTURE_STATION_CONFIG=/absolute/path/to/swing_capture/.station.local.conf
```

Run the doctor and preserve its report before opening devices:

```bash
bazel run //tools:station_doctor -- --json artifacts/station/doctor.json
```

Every check must pass before HIL. In particular, the two selected cameras must
be accessible at 5 Gb/s; the named ALSA capture nodes and stable Feather link
must be read/write; and the installed rules, service, active usbfs value, and
verified role mapping must match. The doctor reports shared root-controller
topology with an instruction to prove capacity through the full-rate camera
HIL rather than failing solely from the static port layout.

## Bring-up order

Run the shortest checks first and preserve their output when a stage fails:

```bash
bazel run //capture/daheng:camera_probe -- \
  --duration-seconds 1 --require-camera-count 2
bazel test //capture/daheng:dual_camera_smoke_hil_test \
  --test_output=streamed --nocache_test_results
bazel test //capture/audio:audio_hil_test \
  --test_output=streamed --nocache_test_results
bazel build //embedded/prop_maker:diagnostic_firmware
bazel run //embedded/prop_maker:flash_diagnostic
```

Inspect the camera `report.json` and both diagnostic PNGs even when the smoke
test passes. Run the five-minute qualification only when the operator
explicitly requests it:

```bash
bazel test //capture/daheng:dual_camera_qualify_hil_test \
  --test_output=streamed --nocache_test_results
```

Likewise, run the 30-minute soak only when the operator explicitly requests
milestone acceptance. Detailed artifact paths, thresholds, and unattended
operation are documented in
[`testing.md`](testing.md); Feather recovery details are in
[`embedded.md`](embedded.md).

## Run the setup preview

After the doctor and camera smoke checks succeed, start the setup UI in a
persistent terminal such as tmux:

```bash
tmux new-session -s swing-preview
bazel run //capture/service:preview_server
```

From another computer on the same trusted network, open:

```text
http://<capture-host-address>:8080/
```

The service binds to `0.0.0.0` by default. Use `--bind 127.0.0.1` when access
should be local-only, or `--port <port>` to select another port. This initial
service has no authentication or TLS and must not be exposed directly to an
untrusted network.

The service reads the same station path as HIL from
`SWING_CAPTURE_STATION_CONFIG`; `--station-config <absolute-path>` overrides
it. Exposure and gain changes last for the running session and are reset to the
current deterministic defaults when the service restarts. Lens focus remains a
physical adjustment.

Routine polling receives up to about 30 compressed 640x480 JPEGs per second. The
browser fetches both roles as one capacity-one pair, retains the last good pair
while a slow download or browser decode finishes, atomically swaps both views, and skips
superseded pairs. Full-resolution images are encoded on demand; use each card's
link only while checking fine focus. `/api/v1/status` exposes source/render age,
encoded size, and per-stage render timings when diagnosing a delayed update.
`Ctrl-C` and `SIGTERM`
stop HTTP acceptance, join both camera workers, stop their streams, and destroy
the Galaxy SDK before the process exits.

Only one process may own the cameras. Exit the preview service before running
a direct Bazel camera HIL target. The preview service and
`//tools:run_unattended_hil` both take `artifacts/hil/hardware.lock`, so those
two supported long-running entry points fail safely instead of overlapping.
For an installed service outside the repository, provision one writable lock
path and pass it with `--hardware-lock <path>` or
`SWING_CAPTURE_HARDWARE_LOCK`; give the unattended runner the same environment
value.

## Transfer discipline

Use Git to move changes between development machines. Keep one authoritative
working branch and avoid parallel uncommitted edits on both hosts. Hardware
reports, Bazel output trees, IDE metadata, local overrides, and downloaded SDK
archives are ignored and should not be transferred through the repository.
