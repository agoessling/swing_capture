# Timing calibration evidence

`//android/dual_hil:timing_calibration_score` validates and independently recomputes evidence for
the two open timing decisions. It does not operate hardware and never infers that a physical setup
was truthful merely because its JSON is internally consistent. Every report therefore contains
`physical_truth_inferred: false`.

Run it after copying a reviewed evidence document and its hash-pinned raw artifacts into the
retained calibration directory:

```bash
bazel run //android/dual_hil:timing_calibration_score -- \
  evidence.json /absolute/path/to/retained-artifact-root > score.json
```

Every artifact reference contains a safe root-relative path and lowercase SHA-256. The scorer
rejects absolute paths, `.`/`..`, symlink traversal, missing/non-regular files, and digest changes;
declared hashes alone never make evidence eligible. It also rejects unknown/missing fields,
noncanonical integer encodings, duplicate trials or raw artifacts, reversed bounds, inconsistent
physical labels, impossible latency arithmetic, malformed review states/timestamps, and unreviewed
or under-covered claims. Nanosecond values are canonical decimal strings so JavaScript number
precision cannot silently alter them.

## Pair-clock threshold evidence

An `evidence_kind: pair_clock_threshold` document declares a candidate clock-uncertainty threshold,
the product's maximum acceptable physical alignment error, a calibrated simultaneous-alignment
reference instrument, a strict approved/needs-review record, and an array of trials. Each trial
pins both the validated phone archive and an independent physical-reference artifact by path and
SHA-256. It records:

- network and phone/view assignment, contention, and screen-off duration;
- mapping age/uncertainty and round-trip bounds from the retained schema-2 request;
- selected-candidate residual and cross-phone trigger-separation evidence when available; and
- measured physical alignment error and its uncertainty.

The physical interval must sit wholly below or wholly above the product alignment limit. A trial
whose interval crosses the boundary is diagnostic, not threshold-selection evidence. The reviewed
`accept`/`reject` label must agree with that interval; a transport acceptance is not allowed to label
itself.

The scorer applies the candidate threshold and the existing 10-second freshness rule, then reports
false accepts and false rejects against the physical reference. `threshold_selection_eligible` also
requires at least 20 trials, including 10 physical accepts and 5 physical rejects, two networks, two
phone/view assignments, contended and uncontended trials, and a screen-off trial of at least 30
minutes. These are evidence-accounting floors, not a statement that 20 samples characterize every
network tail.

The existing six successful direct-LAN reports remain useful operational evidence, but they lack
an independent physical alignment artifact and therefore cannot be converted into selection labels
for this scorer. That is intentional: their 9.625--22.628 ms composed uncertainties constrain an
unsupported threshold change, but do not measure the estimator's actual error.

## Absolute audio latency evidence

An `evidence_kind: absolute_audio_latency` document has one of two scopes:

- `source_acoustic_emission` uses a calibrated physical acoustic-emission sensor. It can calibrate
  acoustic propagation plus the phone microphone/input/timestamp path, but cannot establish exact
  ball-contact time.
- `instrumented_ball_contact` uses an independently calibrated contact sensor at the ball event.
  Only this scope can make `absolute_ball_impact_claim_eligible` true.

The root pins the reference instrument's calibration record and raw reference artifact. A human
review records whether the physical reference setup was actually verified. Each trial pins its raw
artifact and records device/role, microphone distance and uncertainty, speed of sound and
uncertainty, the reference and phone audio events, the phone-minus-reference clock offset, and all
timestamp/mapping uncertainties.

For every trial the scorer recomputes:

```text
mapped phone event = phone audio event - phone-minus-reference clock offset
propagation delay  = microphone distance / speed of sound
device latency     = mapped phone event - reference event - propagation delay
```

It derives conservative propagation bounds from the distance and sound-speed intervals, then adds
reference, phone, clock-mapping, and propagation uncertainties. Negative derived device latency is
rejected. Per-device summaries contain the median/range and maximum uncertainty. Coverage requires
at least five trials per device, both camera roles, and at least 500 mm of microphone-distance span.

`component_calibration_eligible` additionally requires an approved human review and verified
reference setup. `absolute_ball_impact_claim_eligible` requires all of that plus the instrumented
ball-contact scope. Neither boolean replaces inspection of the raw waveforms, reference trace,
fixture geometry, mounting repeatability, or applicability to the intended hitting environment.
