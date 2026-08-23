# Direct-LAN pair-clock evidence — 2026-08-22

## Scope

This audit covers every retained aggregate report whose `pose_transition.peer_transport` is
`wifi_lan_direct`, plus the failed direct-LAN attempts that retained a leader `peer_clock` status.
It does not mix in ADB-reverse paired runs or the host-to-phone coordination clocks: those are
different transport paths and estimators. All six successful direct-LAN reports use the same
Pixel 6 leader, Pixel 5a shadow, station LAN origins, field-recorded S06 replay, and test day. They prove
that the current policy can work in that environment, not that its product threshold is calibrated.

## Current limits and metric meanings

The production phone-to-phone path has two related checks in `PeerClockSynchronizer` and
`CaptureForegroundService`:

- a peer-clock snapshot must be at most 10 seconds old and have at most 25,000,000 ns uncertainty;
- the mapped impact must also be at most 10 seconds old, and its composed uncertainty (peer-clock
  snapshot plus leader audio timestamp uncertainty) must be at most 25,000,000 ns.

The host HIL coordinator independently maps both phone trigger reports into the host clock. Its
limits are 10,000,000 ns per mapped report, 20,000,000 ns combined, and 50,000,000 ns conservative
maximum trigger separation. These values validate retained coordination evidence; they are not
the production peer-clock limit.

## Successful direct-LAN captures

All values below are exact nanoseconds from the retained JSON. `Peer snapshot uncertainty` is the
leader snapshot used to construct the schema-2 impact request. `Mapped impact uncertainty` adds
the leader's local audio timestamp uncertainty. `Combined HIL uncertainty` and `maximum mapped
separation` come from the independent host coordination record.

| Retained report | Peer snapshot uncertainty | Mapped impact uncertainty | Mapping age at send | Combined HIL uncertainty | Maximum mapped separation |
|---|---:|---:|---:|---:|---:|
| `android_pcm_current_apk_paired_pass_20260823T002901` | 9,331,716 | 9,625,081 | 2,245,028,810 | 10,092,541 | 16,805,805 |
| `android_pcm_paired_s06_lan_pass_20260822T164310` | 16,160,382 | 16,455,703 | 6,238,817,549 | 9,176,775 | 12,206,124 |
| `android_pcm_paired_s06_lan_pass_20260822T180238` | 18,361,026 | 18,671,032 | 136,671,468 | 10,455,750 | 14,742,356 |
| `android_pcm_paired_s06_lan_preview_pass_20260822T200115` | 22,340,881 | 22,628,383 | 7,181,649,539 | 9,771,683 | 14,834,739 |
| `android_pcm_paired_s06_lan_readiness_pass_20260822T210126` | 21,452,165 | 21,752,098 | 3,061,230,755 | 12,179,559 | 15,807,264 |
| `android_pcm_paired_s06_startup_pass_20260823T043404Z` | 19,214,822 | 19,521,074 | 2,105,783,448 | 9,448,970 | 15,474,392 |

The largest successful production mapping used 22,628,383 ns of the provisional 25,000,000 ns
limit, leaving 2,371,617 ns. The oldest successful mapping was 7,181,649,539 ns of the 10-second
freshness limit. The largest independent HIL combined uncertainty was 12,179,559 ns of 20,000,000
ns, and the largest conservative separation was 16,805,805 ns of 50,000,000 ns.

Regenerate the bounded operational inventory directly from retained aggregates instead of
manually counting directories:

```bash
bazel run //tools:pair_clock_operational_summary -- artifacts
```

The tool validates successful direct-LAN schema/policy/coordination evidence and recomputes the
peer-snapshot component. Its output deliberately sets `physical_alignment_reference_present` and
`threshold_selection_eligible` false: transport evidence cannot become calibration truth through
aggregation.

## Relevant failed attempts

No retained direct-LAN failure proves that a usable clock mapping was rejected at the current
25 ms boundary:

- `android_pcm_paired_s06_lan_failed_mapping_20260822T175739` first retained a 20,265,612 ns
  snapshot, then observed a 117,789,376 ns snapshot from a batch spanning 235,578,751 to
  1,780,074,255 ns round trips. The request fell back to schema 1 with
  `mapping_within_policy=false`; the then-current HIL validator expected schema-2 mapped fields.
  This is evidence of a real high-jitter tail and correct conservative rejection, not evidence for
  raising the limit. Current source also preserves a still-fresh overlapping good snapshot rather
  than replacing it with one high-jitter batch.
- `android_pcm_paired_s06_lan_failed_no_trigger_20260822T175932` retained a 20,543,580 ns snapshot
  at 9,936,846,441 ns age before it aged beyond 10 seconds. The replay produced no leader audio
  trigger, so no mapped impact was attempted; the stale post-failure status does not attribute the
  miss to clock policy.
- The retained optical-correlation, monitoring-transition, HTTP-stage-deadline, and common-state
  transition failures had in-policy leader snapshots. Their diagnostics identify non-clock causes.
- The missing-origin and missing-PCM-environment attempts ended before clock evidence existed.

## Decision status

Keep 25 ms as the explicit provisional, fail-closed implementation bound. The evidence does not
support lowering it: doing so below 22,628,383 ns would reject one of only six successful mappings.
It also does not support raising it: the only wider retained snapshot was an extreme failed jitter
batch, not a correctly aligned field capture. Select a product threshold only after collecting
multiple sessions across representative field networks, phone/view swaps, network contention,
long-running screen-off operation, and both successful and rejected mappings. Record the snapshot
uncertainty, composed mapping uncertainty, mapping age, round-trip interval, selected shadow audio
candidate residual, conservative cross-phone trigger separation, and capture outcome for each event.

Durable capture manifests now retain the original schema-2 request even when
policy selects the schema-1 fallback. Peer-impact mapping schema 2 records the
`mapping_policy`, effective request schema, fallback semantics, actual selection
source, original mapping age/uncertainty/round-trip interval, and residual. A
rejected mapping is therefore calibration evidence instead of an ephemeral
live-status-only observation.
