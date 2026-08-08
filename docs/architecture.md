# Swing Capture Architecture

## Direction

The first implementation targets the two Daheng
`MER2-160-227U3C` cameras on Linux.

This path already provides deterministic access to every raw frame, explicit
exposure controls, stable device timestamps, serial-number selection, and
electrical frame-trigger inputs. Android high-speed video remains a useful
future capture client, but it adds phone-specific high-speed camera
constraints, thermal behavior, opaque video pipelines, file transfer, and
wireless clock synchronization before it solves the core simulator use case.

## Capture data path

```text
camera A ─ acquisition thread ─ clock fit ─ pooled frame ring ─┐
                                                               ├─ clip planner ─ encoder
camera B ─ acquisition thread ─ clock fit ─ pooled frame ring ─┘       │
                                                                       └─ review UI
microphone ─ PCM source ─ impact detector ─ host-monotonic strike time ┘
```

Each camera has exactly one dequeue/requeue thread. That thread performs no
demosaicing, encoding, disk I/O, or UI work. It validates metadata, updates
timing metrics, and copies the payload once into a preallocated pool.

Each device clock is fitted independently to the host steady clock. This lets
an audio strike timestamp select frames from both streams. It does not prove
that the two cameras exposed at the same instant.

The hardware-independent version of the trigger, retention, and selection path
is exercised by `//capture/pipeline:capture_pipeline_integration_test`. It
synthesizes an audio strike and two cameras with unrelated clock origins and
frequencies, waits for post-roll, freezes both rings, selects bounded clip
windows, and proves capture can continue without mutating the retained frames.

## Camera configuration and diagnostics

The current Daheng HIL uses deterministic free-run settings: 1440x1080
`BayerRG8`, approximately 227 fps, 4000 us fixed exposure, `ExposureAuto=Off`,
0 dB fixed gain, and `GainAuto=Off`. Required values are read back from both
cameras rather than assumed from successful setter calls.

After acquisition stops, diagnostic work runs away from the camera dequeue
threads. One retained raw frame per view is measured for histogram percentiles,
near-black/near-white fraction, and Bayer-phase-aware gradient energy, then
demosaiced to PNG. The JSON stores only the PNG filename relative to the
report; a smoke artifact therefore travels as one self-contained directory:

```text
report.json
frame-FDN22120654.png
frame-FDN23010199.png
```

Transport readiness and image readiness are separate. The 2026-07-26 smoke
transported both streams successfully, but classified both images
`underexposed_or_obscured`, with one view almost entirely black. The camera
positions, obstructions, lens/iris state, lighting, and exposure need physical
correction before collecting representative swings. Image classification is
currently diagnostic evidence, not a transport gate.

## Retention and memory

A camera ring owns an active pre-impact window plus reserve blocks. Freezing a
window increments block references; it does not copy frame payloads. Capture
continues into the reserve and then reuses unpinned active blocks. Pool
exhaustion is an explicit error rather than an unbounded allocation or a
silent frame drop.

The measured full-resolution payload is 1,555,200 bytes. At 226.86 fps:

| Allocation | Per camera | Two cameras |
|---|---:|---:|
| Two-second active window | about 706 MB | about 1.41 GB |
| Active window plus full reserve | about 1.41 GB | about 2.82 GB |

The dual-camera freeze/continue HIL measured about 2.99 GB process RSS. On the
current 30 GB host this is substantial but not prohibitive. A later
range-freeze API can retain only the requested pre-strike interval when lower
memory is important.

## Trigger and clip sequence

1. Both cameras and the audio source run continuously.
2. The impact detector reports both the peak sample's host-monotonic strike
   timestamp and the later sample timestamp at which that peak was confirmed.
   Session call ordering uses confirmation time; clip selection uses strike
   time.
3. Capture continues through the configured post-impact interval plus one
   frame-boundary margin. The active ring is sized for the complete
   pre-plus-post clip plus leading and trailing margins, so the desired
   boundary frames cannot be overwritten during this wait.
4. The coordinator freezes each completed ring window by reference.
5. The clip planner maps device timestamps to host time and selects the fixed
   pre/post interval for both views.
6. An asynchronous worker demosaics and encodes the selected frames.
7. The live rings continue recording while the completed clip becomes
   available to the review UI.

Only one payload copy occurs on the acquisition path. Encoding policy and
hardware acceleration will be selected after raw capture, triggering, and
timing are proven with real swings. Steps 1-5 have implementation and synthetic
coverage; clip encoding and UI publication in steps 6-7 are not implemented
yet.

If scheduling reaches the latest safe freeze time after the leading margin has
fallen out of retention, the coordinator reports an explicit expired outcome
instead of emitting a clip request that the planner cannot fulfill.

## Synchronization

Free-running capture is enough to develop transport, triggering, clip
selection, and the UI. It is not the final synchronization mechanism.

The intended final setup feeds one shared electrical frame pulse into `Line0`
on both cameras. Both streams must be armed before the pulse train begins.
An optical sync HIL should put a flashed LED in both views and measure the
observed frame offset. The model-specific connector pinout and voltage limits
must be verified before wiring.

Free-run qualification also separates two kinds of apparent stalls. Host
receive intervals may reach 10 nominal frame periods to tolerate bounded Linux
scheduling delay while the SDK queue preserves every frame. Device timestamp
intervals may reach only 4 periods because they describe camera cadence
directly. Both thresholds are recorded and checked independently.

## Audio timing

The first physical audio backend launches `arecord` directly, captures the
C925e at 32 kHz stereo, and selects channel 0 for the impact detector. It uses
host read completion minus the first block duration as its initial timestamp,
then preserves time by sample-count continuity. That is sufficient for early
trigger and clip-pipeline iteration, but ALSA buffering and device latency are
not yet measured. A direct ALSA timestamp backend or a physical audio/optical
calibration HIL is required before claiming absolute strike alignment.

## UI boundary

The capture engine should expose immutable clip metadata and media rather than
camera SDK objects. The review UI can then be built and tested entirely from
fixture clips:

- synchronized side-by-side playback;
- frame stepping and a shared scrubber;
- impact marker and address/impact positions;
- drawing overlays that remain tied to a view;
- capture health and clear degraded-state messages.

Component fixtures, browser interaction tests, and screenshot comparisons
will make visual iteration independent of attached cameras.
