# Embedded firmware workflow

The HIL fixture uses an Adafruit Feather RP2040 Prop-Maker. Firmware is built
with the official Raspberry Pi Pico SDK Bazel rules and programmed with the
official picotool binary built by Bazel from source. Bazel 9.2 is the only
user-facing build interface.

## Reproducible dependencies

`MODULE.bazel` pins Pico SDK and picotool 2.3.0 through the Bazel Central
Registry. The SDK module downloads a checksum-pinned Arm GNU 13.2 toolchain and
pins TinyUSB by commit. `MODULE.bazel.lock` records the resolved module and
repository inputs.

Pico SDK 2.3.0 contains a one-line stale Bazel dependency name in its USB stdio
target. `third_party/pico_sdk/pico_usb_reset_dependency.patch` changes that
dependency to the `pico_usb_reset` library used by the same release's source
tree and CMake definition. Keeping this as a narrow module patch lets the
firmware retain USB serial and picotool's automatic reset interface.

The custom transition in `embedded/rp2040/defs.bzl` cross-compiles only the
firmware dependency for RP2040. UF2 conversion uses picotool in Bazel's host
execution configuration. Consequently both commands work without global
platform flags:

```bash
bazel build //embedded/prop_maker:diagnostic_firmware
bazel run //embedded/prop_maker:flash_diagnostic
```

RP2040 firmware participates in ordinary `//...` builds. The repository's
ASan and UBSan configurations exclude targets tagged `no-host-sanitizer`,
because host sanitizer runtimes cannot be linked by the Arm cross-toolchain.
The host C++ tests remain instrumented by those configurations.

The build artifact is:

```text
bazel-bin/embedded/prop_maker/diagnostic_firmware.uf2
```

## One-time Linux access

Install the repository-owned udev rule once on each capture host:

```bash
sudo ./tools/setup_prop_maker_host.sh
```

Reconnect the board if permissions on an existing device do not change. The
rule grants the active desktop user and `plugdev` group access to the RP2040
ROM bootloader, Pico SDK USB stdio/reset interfaces, and the two Adafruit USB
product IDs used by this board's factory and CircuitPython images. It does not
install SDK files or tools globally.

The rule also grants `plugdev` access to the CDC serial device created by the
diagnostic and HIL firmware. Use the stable `/dev/serial/by-id/usb-Raspberry_Pi_Pico_*`
link rather than assuming the board is always assigned `/dev/ttyACM0`.

## Initial programming and recovery

The factory firmware does not necessarily expose the reset interface understood
by picotool. To enter the RP2040 ROM bootloader:

1. Hold the board's **BOOT** button.
2. Tap and release **RESET**.
3. Release **BOOT**.
4. Run `bazel run //embedded/prop_maker:flash_diagnostic`.

The target asks picotool to load, verify, and execute the UF2. It fails rather
than choosing arbitrarily if more than one RP-series device is available;
picotool selection arguments can be forwarded after `--` when needed.

The diagnostic image toggles GPIO13, the onboard red LED, every 500 ms and
writes a heartbeat over USB serial. It intentionally leaves GPIO23 low, keeping
power disabled for the speaker amplifier, servo header, and external NeoPixel
terminal. This remains the minimal recovery image even though the separate HIL
stimulus image now exists.

If application firmware is invalid, the BOOT/RESET sequence always returns the
RP2040 to its ROM bootloader. A UF2 can then be flashed again with the same Bazel
target.

## Versioned HIL stimulus image

Build the HIL image without accessing the board:

```bash
bazel build //embedded/prop_maker:hil_firmware
```

Programming it is a separate, explicit operation:

```bash
bazel run //embedded/prop_maker:flash_hil
```

That operation replaces the diagnostic heartbeat image with the `SC-HIL/1`
USB CDC stimulus image and resets the Feather. It does not modify a camera or
the host. The serial device can briefly disappear and re-enumerate during the
reset.

The HIL image initializes every output inactive. An accepted LED request pulses
the onboard GPIO13 LED. An accepted tone request briefly raises GPIO23 and
therefore powers the MAX98357 speaker amplifier, the external NeoPixel rail,
and the servo rail together; it then drives a bounded I2S tone on GPIO16--18
before returning the shared power rail and I2S pins low. Nothing should be
connected to the external powered rails unless that temporary power-up is
known to be safe.

The complete commands, negotiated bounds, response records, timing semantics,
and recovery behavior are documented in
[`embedded/prop_maker/README.md`](../embedded/prop_maker/README.md). Build and
host-side parser tests do not access or flash the board.

## Board configuration

`embedded/prop_maker/board.h` records the board-specific configuration used by
the Pico SDK:

- 8 MiB GD25Q64C or W25Q64JV-compatible QSPI flash;
- onboard red LED on GPIO13 and status NeoPixel on GPIO4;
- STEMMA QT I2C1 on GPIO2/GPIO3;
- external NeoPixel data on GPIO21 and external power enable on GPIO23;
- I2S data, bit clock, and word select on GPIO16/GPIO17/GPIO18;
- terminal-block button on GPIO19.

The pin assignments are taken from Adafruit's RP2040 Prop-Maker guide and its
CircuitPython board definition:

- <https://learn.adafruit.com/adafruit-rp2040-prop-maker-feather/pinouts>
- <https://github.com/adafruit/circuitpython/tree/main/ports/raspberrypi/boards/adafruit_feather_rp2040_prop_maker>

Do not enable or connect electrical outputs merely because the firmware builds.
The relevant device voltage, polarity, common ground, and load must be verified
before each HIL role is implemented.
