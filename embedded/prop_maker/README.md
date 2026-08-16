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
SC-HIL/1 <id> CALIBRATE
SC-HIL/1 <id> SWING <brightness>
```

For example, the combined fixture uses a long locator pulse, a ten-frame
qualification pulse at 227 fps, and a conservative audio stimulus:

```text
SC-HIL/1 101 LED 100000 300000
SC-HIL/1 102 LED 200000 44053
SC-HIL/1 103 TONE 100000 20000 2000 10
```

`QUERY` returns the firmware version, protocol version, capability names, all
accepted numeric ranges, the device monotonic time, and the exact synthetic
fixture fields `fixture_neopixel_gpio=21`,
`fixture_neopixel_color_order=rgb`, `shared_power_gpio=23`, and
`prepare_timeout_us=10000000`. A valid stimulus first
returns an `ACK` containing its accepted and scheduled device times. After the
stimulus it returns `EVENT ... START` and `EVENT ... DONE` records with actual
device monotonic start/end times, scheduling lateness, and elapsed time. START
and DONE are deliberately emitted together after the timing-critical stimulus;
the earlier ACK gives the host its schedule without serial I/O lengthening an
LED pulse or starving audio. Rejected input returns a framed `ERR` with a stable
error code and device time. Lines longer than 160 bytes are discarded through
the following newline, after which parsing resumes.

The host must complete `QUERY` negotiation before using `CALIBRATE` or
`SWING`. Firmware `prop-maker-hil-5` advertises the exact
`query,led,tone,calibrate,swing` capability set and the fixed calibration
brightness candidates `1,2,3,4,6,8,12,16`. These low levels replace the v3
set after level 16 already produced unsafe saturation and bloom on the physical
two-camera fixture. `SWING` rejects every
brightness that is not in that advertised set, and rejects the command with
`not_prepared` unless a successful, unexpired `CALIBRATE` has prepared the
shared fixture rail. The command grammar and capability names remain unchanged
from the original version-1 wire contract.

The parser enforces these limits before an output is enabled:

- lead: at most 2,000,000 us;
- LED duration: 100 through 1,000,000 us;
- tone lead: 20,000 through 2,000,000 us;
- tone duration: 1,000 through 250,000 us;
- tone frequency: 100 through 10,000 Hz;
- tone level: 1 through 125 permille of signed 16-bit full scale.

## Synthetic swing sequence

`CALIBRATE` presents each advertised white NeoPixel brightness for 70 ms, for
a total sweep of 560 ms. Before the probes, firmware starts silent I2S, raises
the GPIO23 shared external-power rail, explicitly latches the GPIO21 fixture
pixel off, and then quiesces I2S without cutting power. Its ACK gives the exact
candidate order, device start schedule, step cadence, and the same
`fixture_neopixel_gpio`, `shared_power_gpio`, and `prepare_timeout_us` contract.
Each `STEP` event retains the scheduled and
actual device times and lateness for one candidate. At the end, the pixel is
latched off and I2S remains inactive, while GPIO23 stays high so the host can
arm the cameras and microphone without another amplifier power-on transition.
`DONE` reports `power_on_us`, the exact `prepared_until_us` device time,
`pixel_off=1`, `i2s_inactive=1`, `rail_powered=1`, and `prepared=1` rather than
making a contradictory all-outputs-inactive claim.
The deadline is ten seconds after the rail was first raised, so the completed
560 ms sweep leaves a little over nine seconds for host arming. If that deadline
expires before `SWING`, firmware latches the pixel off again before quiescing
I2S and taking GPIO23 low. These records let the host align the existing
low-rate camera previews with every candidate and select one brightness that is
visible but not clipped in both views. Timing-critical step events are emitted
after the sweep. A parser error or an unrelated `LED` or `TONE` stimulus also
invalidates the prepared state and performs the ordered power-down first.

`SWING <brightness>` consumes the prepared rail as a one-shot authorization and
runs one deterministic fixture action after a 20 ms start lead. The rail stays
high for the complete pre-impact, white/tone, and post-impact sequence. The
selected calibration candidate is the maximum channel value for the entire
action: every pre/post RGB state is a full-value hue with at least one channel
at the selected value, no channel exceeds it, and the white impact reaches it
on all three channels. Integer channel quantization necessarily repeats colors
at the lowest levels. The deterministic pre/post colors are human playback cues
for recognizing sequence direction and phase; their observed color count and
chroma are not programmatic HIL qualification gates. Logical RGB channels are
serialized in `R,G,B` wire-byte order for the fixture pixel. The action consists
of:

- 60 color steps at 20 ms each (1.2 s) before impact;
- a white impact marker lasting 20,000 us, approximately four to five periods
  at 227 fps;
- a 10 ms, 2 kHz, 125-permille speaker tone commanded on the same device
  timeline as the white marker, after amplifier warmup; and
- 25 color steps at 20 ms each (500 ms) after impact.

The fixed 125-permille HIL level uses the firmware's conservative ceiling. It
gives the production impact detector useful margin in the qualified fixture
geometry while the retained PCM gate still rejects clipping.

The ACK contains the planned sequence, impact, post-impact, and completion
device times plus every fixed sequence parameter, `prepared=1`,
`rail_powered=1`, and both GPIO assignments. The resulting typed `PHASE
phase=pre`, `IMPACT`, `PHASE phase=post`, and `DONE` events retain the actual
device timestamps. Every step must be no more than 2 ms late, and the white
pixel and tone command times may differ by no more than 250 us. `IMPACT`
reports both command and end timestamps so the host can validate the optical
and audio durations. `DONE` repeats the accepted phase boundaries and confirms
that the pixel, amplifier, shared rail, and I2S outputs are inactive. After the
tone drains, firmware quiesces I2S without cutting the rail so the white and
post-impact markers remain powered. After the final marker it latches the pixel
off, quiesces I2S, and only then takes GPIO23 low. The same ordered shutdown is
completed before reporting a timing or impact-command failure. Parser errors
also invalidate a prepared rail and force that shutdown. `DONE` retains
`outputs_inactive=1` for compatibility and reports `pixel_off=1`,
`i2s_inactive=1`, `rail_powered=0`, `prepared=0`, and both GPIO assignments.

## Output and timing assumptions

The onboard red LED is GPIO13 and is initialized off. Its `LED` pulse uses the
RP2040 monotonic timer rather than a loop count. `CALIBRATE` and `SWING` drive
the screw-terminal external NeoPixel data output on GPIO21 at 800 kHz with a
second PIO instance. That physical fixture pixel consumes RGB wire-byte order,
which differs from the common GRB convention and is advertised explicitly by
`QUERY`. The external pixel is powered by GPIO23 and is independent of both the
GPIO13 LED and the unused onboard NeoPixel on GPIO4.

The speaker terminal is driven by the onboard MAX98357 I2S amplifier. The
firmware transmits identical left/right 16-bit square-wave samples at 32 kHz
through GPIO16 data, GPIO17 BCLK, and GPIO18 LRCLK. A Pico PIO state machine
generates I2S and a DMA channel supplies the bounded static sample buffer. Tone
duration is therefore quantized upward to the next 31.25 us sample; the ACK and
DONE records include the sample count and rate.

GPIO23 powers the amplifier and also the external NeoPixel and servo rails. It
is initialized low. A standalone `TONE` retains its original bounded power
cycle: the minimum 20 ms lead lets firmware wait until 15 ms before the
scheduled start, start silent I2S, raise GPIO23, and supply ten milliseconds of
silence for amplifier settling. After playback, GPIO23 returns low and all I2S
pins return to low GPIO outputs before `DONE`. The synthetic workflow instead
raises the same rail during `CALIBRATE`, retains it only for the advertised
prepared window, and consumes that preparation with `SWING`; it never
power-cycles the amplifier at the impact boundary. The device START time is the
DMA trigger time; analog onset has additional unmeasured MAX98357 and speaker
latency, which the combined audio/optical workflow measures.

The documented hardware assumes a correctly connected 4--8 ohm speaker. The
125-permille software ceiling is intentionally conservative relative to the
board amplifier's default 9 dB gain. Firmware does not drive the servo signal,
but its power terminal necessarily shares the bounded GPIO23 rail interval with
the amplifier and external NeoPixel.
