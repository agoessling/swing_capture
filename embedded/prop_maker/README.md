# Prop-Maker HIL firmware

`//embedded/prop_maker:hil_firmware` is the device-side foundation for the
camera/audio fixture. It is separate from the heartbeat-only
`diagnostic_firmware`, so the latter remains a minimal programming and recovery
image. Building either image does not access the Feather. Programming
`hil_firmware` is a separate explicit action through
`//embedded/prop_maker:flash_hil`.

## USB CDC protocol

The protocol is newline-delimited printable ASCII. Every command begins with
the versioned `SC-HIL/1` prefix and a nonzero unsigned 32-bit request ID. ID 0
is reserved for unsolicited device events. `\r\n` and `\n` line endings are
accepted.

```text
SC-HIL/1 <id> QUERY
SC-HIL/1 <id> LED <lead_us> <duration_us>
SC-HIL/1 <id> TONE <lead_us> <duration_us> <frequency_hz> <level_permille>
```

For example, the combined fixture uses a long locator pulse, a ten-frame
qualification pulse at 227 fps, and a conservative audio stimulus:

```text
SC-HIL/1 101 LED 100000 300000
SC-HIL/1 102 LED 200000 44053
SC-HIL/1 103 TONE 100000 20000 2000 10
```

`QUERY` returns the firmware version, protocol version, capability names, all
accepted numeric ranges, and the device monotonic time. A valid stimulus first
returns an `ACK` containing its accepted and scheduled device times. After the
stimulus it returns `EVENT ... START` and `EVENT ... DONE` records with actual
device monotonic start/end times, scheduling lateness, and elapsed time. START
and DONE are deliberately emitted together after the timing-critical stimulus;
the earlier ACK gives the host its schedule without serial I/O lengthening an
LED pulse or starving audio. Rejected input returns a framed `ERR` with a stable
error code and device time. Lines longer than 160 bytes are discarded through
the following newline, after which parsing resumes.

The parser enforces these limits before an output is enabled:

- lead: at most 2,000,000 us;
- LED duration: 100 through 1,000,000 us;
- tone lead: 20,000 through 2,000,000 us;
- tone duration: 1,000 through 250,000 us;
- tone frequency: 100 through 10,000 Hz;
- tone level: 1 through 125 permille of signed 16-bit full scale.

## Output and timing assumptions

The onboard red LED is GPIO13 and is initialized off. Its pulse uses the
RP2040 monotonic timer rather than a loop count.

The speaker terminal is driven by the onboard MAX98357 I2S amplifier. The
firmware transmits identical left/right 16-bit square-wave samples at 32 kHz
through GPIO16 data, GPIO17 BCLK, and GPIO18 LRCLK. A Pico PIO state machine
generates I2S and a DMA channel supplies the bounded static sample buffer. Tone
duration is therefore quantized upward to the next 31.25 us sample; the ACK and
DONE records include the sample count and rate.

GPIO23 powers the amplifier and also the external NeoPixel and servo rails. It
is initialized low and is raised only after a TONE command passes every bound.
The minimum 20 ms tone lead lets the firmware wait until 15 ms before the
scheduled start, then supply ten milliseconds of silent I2S for amplifier
settling. GPIO23 returns low and all I2S pins return to low GPIO outputs before
DONE is emitted. The device START time is the DMA trigger time; analog onset
has additional unmeasured MAX98357 and speaker latency, which a later combined
audio/optical calibration must measure.

The documented hardware assumes a correctly connected 4--8 ohm speaker. The
125-permille software ceiling is intentionally conservative relative to the
board amplifier's default 9 dB gain. Firmware does not use the servo or external
NeoPixel outputs, but their shared rail will be powered during the accepted
tone's warmup and playback interval.
