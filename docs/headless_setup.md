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
For full-rate dual capture, put the cameras on distinct USB root controllers,
not merely different connectors on the same hub. Use a stable ALSA card ID and
the Feather's `/dev/serial/by-id/` link instead of volatile numeric device
names such as `hw:0` or `/dev/ttyACM0`.

The current audio HIL target is temporarily fixed to
`hw:CARD=C925e,DEV=0`; confirm that identifier exists on the new host before
running it. Station-level role and device configuration will replace this
hard-coded selection.

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
test passes. After a good smoke result, run the five-minute qualification:

```bash
bazel test //capture/daheng:dual_camera_qualify_hil_test \
  --test_output=streamed --nocache_test_results
```

Use the 30-minute soak only for milestone acceptance. Detailed artifact paths,
thresholds, and unattended operation are documented in
[`testing.md`](testing.md); Feather recovery details are in
[`embedded.md`](embedded.md).

## Transfer discipline

Use Git to move changes between development machines. Keep one authoritative
working branch and avoid parallel uncommitted edits on both hosts. Hardware
reports, Bazel output trees, IDE metadata, local overrides, and downloaded SDK
archives are ignored and should not be transferred through the repository.
