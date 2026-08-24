# Current Android dual-phone handoff

Last updated 2026-08-23 18:41 America/Los_Angeles. Read this file, `AGENTS.md`, and
`TODO.md` before changing or testing the repository. This is the exact operational checkpoint for
the next hitting-session preparation pass.

## Current outcome

The current APK, direct-LAN browser workflow, generic pair-network admission, and short production
pose-monitoring ceremony are working on the Pixel 6/Pixel 5a pair. The first final-APK short paired
camera/audio/LED HIL is retained at
`artifacts/android_hil_fixture_retry_localization_failure_20260824T010041Z/`. Both roles completed
capture, exact decode, AprilTag persistence, white-pulse intensity/duration, and operational timing;
only the former spatial-localization rule failed (22 down-the-line and 16 face-on tiles).

Spatial extent is not a hitting-session requirement: the LED exists only to supply a timestamped
fixture pulse, and the retained baselines prove the response is transient rather than global
exposure drift. Retained-media schema 2 therefore keeps peak tile and footprint as diagnostic
telemetry but accepts on intensity, frame count, duration, and optical/audio timing only. A broad
bright-pulse regression passes while weak, short, long, and mistimed pulses still fail. Do not run
a five-minute qualification, 30-minute soak, or OS-reboot ceremony without explicit user approval.

## Workspace and exact APK

- Repository: `/home/agoessling/swing_capture`
- Branch: `android-dual-phone-capture`
- Previous baseline: `2a5c0b69ebeb18e71d0e22d63ce3ee13cb11a704`
  (`Document Android field-readiness and validation workflow`).
- Current implementation commits:
  - `f4ead62` (`Add bounded hitting-session telemetry sidecar`).
  - `1e93b45` (`Harden dual-phone field readiness`).
- These commits contain pair-network health/admission, bounded recent pose metrics, doctor/UI and
  HIL warm-up integration, schema-2 optical acceptance, strict stopped-clean admission, and the
  bounded telemetry sidecar. The worktree should be clean after the documentation commit containing
  this handoff; investigate rather than discarding any later local changes.
- Bazel APK SHA-256 and both installed `base.apk` digests:
  `9d2ad9c12f8ff1846d02ab8018d0a659bf88d313b542824fde0c185d1572a230`.
- Static exact-APK product-floor pass:
  `artifacts/android_product_floor_final_20260823T154000Z/report.json`.
- Final software validation in this worktree:
  - `//tools:pre_field_integration_tests`: 131/131 passed uncached.
  - `bazel test //... --nocache_test_results`: 246/246 passed.
  - `bazel build --config=manual_test_static_check //...`: all 937 targets compiled.
  - `bazel test --config=precommit -c opt //...`: 246/246 passed with all configured formatting,
    lint, type-check, and static-analysis aspects green.

## Phones, roles, and terminal state

| Installed role | Phone | USB serial | Wireless ADB | Direct LAN origin | OS/API |
|---|---|---|---|---|---|
| face-on / pose leader | Pixel 6 | `22181FDF6005QH` | `10.168.168.111:5555` | `http://10.168.168.111:8088` | API 36 |
| down-the-line / shadow | Pixel 5a | `1A011JEG501717` | `10.168.168.241:5555` | `http://10.168.168.241:8088` | API 34 |

After the fixture retry, both stations were not armed, exact-APK admission passed, direct-LAN and
wireless-ADB transport passed, and both screens were noninteractive. This is retained at
`artifacts/field_preflight_after_fixture_retry_20260824T010200Z/report.json`; its accepted third
doctor attempt records `state=ready, armed=false` on both. Services remain reachable on port 8088.
That artifact predates the new strict stopped-clean capture/autonomous/field-recorder contract, so
rerun `--require-stopped-clean` before collection rather than treating it as proof of the expanded
gate. Preserve the terminal state after every physical run.

The next field-collection plan deliberately swaps the phones relative to the installed HIL roles:
Pixel 6 must become down-the-line and Pixel 5a face-on. Do not move into collection with the table
above unchanged. Use the exact non-overwriting plan at
`artifacts/next_field_collection_plan_20260823T211253Z.json`; it requires two complete reviewed,
phone/view-swapped holdout sessions and the full negative-rich scenario coverage.

## Implemented integration behavior

- The pose status exposes a fixed 150-sample recent window in addition to lifetime metrics. The
  doctor gates the actual production delegate/model/size (`gpu`, Lite, 640x360), 100--150 recent
  samples, recent p95 at most 200 ms, recent maximum at most 400 ms, zero failures, and zero rejected
  timestamps. Lifetime startup samples remain observable but do not poison a warmed steady-state
  gate.
- The pose leader measures three application clock requests plus a representative `/app.js`
  transfer in both phone-to-phone directions. The reverse callback is authenticated and constrained
  to the accepted literal IPv4 peer, and redirects/timeouts fail closed.
- Status publishes Good/Degraded/Unusable with hysteresis, age/staleness, both directional samples,
  latency, jitter, timeout counts, throughput, and issues. Unknown, stale, unmeasured, malformed, or
  unusable health blocks external leader arming. Degraded service requires a one-shot explicit
  override in both the UI and wire request; the override is not persisted. Shadow, disabled-mode,
  disarm, and internal recovery paths do not incorrectly depend on leader admission.
- Setup/review UI and the strict pair doctor consume the same producer schema; the doctor applies
  additional evidence-consistency checks for field admission. Topology metadata remains
  diagnostic only; BSSID equality, frequency band, RSSI, router vendor, and router credentials are
  not release gates.
- The production paired HIL now waits within its existing 15-second budget for stabilized network
  health before external arm. It accepts Good normally, Degraded only with explicit override, and
  never overrides Unusable. Raw/stabilized transition consistency is retained as evidence. The
  five-minute/30-minute report validator now requires that admission snapshot, exact LAN
  leader/shadow topology, and accepted-state/override consistency as well.
- `--require-stopped-clean` is now distinct from monitoring admission. It requires capture to be
  unarmed with no active/shared session; autonomous pose state to be stopped with no active shared
  capture, replication backlog, trigger, or publication work; and the separate field recorder to
  be idle/ready with no active recording or injected fault. Every attempt retains recursively
  credential-redacted raw capture and field-recording status, and malformed or inconsistent status
  fails closed.
- `//tools/session_sidecar:session_sidecar` is a bounded read-only collector for a hitting session.
  It samples both authenticated capture and field-recording status streams, retains bounded logcat
  plus initial/final power, battery, thermal, and Wi-Fi diagnostics, refuses overwrite, scans for
  leaked tokens, and writes a SHA-256 inventory. It never arms, stops, configures, deletes, or
  exports media; collect MP4/WAV/manifests separately.

## Strongest current physical evidence

### Product floor and stopped ceremony

- `artifacts/android_product_floor_final_20260823T154000Z/report.json`: both exact installed APKs,
  rear 720p240 capture, hardware AVC, PCM audio, GLES, Lite/GPU pose configuration, and screens-off
  requirements passed.
- `artifacts/field_preflight_stopped_final_apk_20260823T153900Z/report.json`: final-APK stopped
  admission passed on attempt 3 after the expected two-round recovery hysteresis. Attempts 1--2
  retained valid measured health while stabilized state was still Unusable/recovering.

### Direct-LAN Chrome release gate

`artifacts/browser_direct_lan_final_apk_20260823T153900Z/` is the current no-relay release artifact.
Its retained report records 2.826-second page readiness, a 5.674-second camera stage, 3.953-second
start convergence, and 0.948-second stop convergence. Both real
origins returned the required APIs; arm/start/stop mutations returned 202; all four MP4/WAV range
paths returned 206; both new 720x1280 H.264 clips were nonblack and advanced; accessibility, page,
and unexpected-request errors were empty; cleanup and screen sleep passed. The retained screenshot
was inspected and showed a measured Degraded/recovering state, both directions, and the required
one-shot acknowledgement control.

### Armed monitoring ceremony

`artifacts/field_preflight_monitoring_final_apk_retry_20260823T154400Z/report.json` passed strict
wireless-ADB pair admission on its first attempt. The matching detailed exact-APK report is
`artifacts/field_preflight_monitoring_final_apk_retry_20260823T154400Z/doctor_attempt_1.json`; it
proves:

- Pixel 6 leader: GPU/Lite 640x360, 1009 successful and zero failed lifetime inferences, 150 recent
  samples, recent p95 140 ms, recent max 156.548 ms, zero recent deadline misses and zero rejected
  timestamps.
- Pixel 5a shadow: GPU/Lite 640x360, 1005 successful and zero failed lifetime inferences, 150 recent
  samples, recent p95 150 ms, recent max 209.274 ms, one recent deadline miss and zero rejected
  timestamps; the p95 and 400 ms outlier gates passed.
- Bidirectional application-level pair health was Good and both stations were armed during the
  evidence collection. The leader peer-clock mapping was 1.524 seconds old with 15.901 ms
  uncertainty.

Preserve `artifacts/field_preflight_monitoring_final_apk_20260823T154100Z/`: attempt 1 records the
expected sub-100-sample warm-up, and later attempts retain Pixel 5a hosted-asset/thermal-service
timeouts. Also preserve `artifacts/android_pair_monitoring_final_apk_20260823T154300Z.json`, whose
first USB retry timed out on the Pixel 5a descriptor before the next invocation passed. These are
intermittent diagnostic-collection incidents, not grounds to weaken admission. Both stations were
explicitly disarmed after the passing report.

### Short paired camera/audio/LED HIL

The original network-health build exposed a real integration gap: arm ran before health warm-up and
failed before capture. Preserve
`artifacts/android_hil_network_warmup_failure_20260823T145902Z/`. The HIL readiness wait described
above fixes that path.

The first physical run with the fix cleared network admission but Pixel 6 recent pose p95 was 202 ms;
preserve `artifacts/android_hil_network_pass_pose_latency_failure_20260823T150853Z/`.

The predecessor retry completed capture but failed the former fixture-localization rule:
`artifacts/android_hil_network_capture_image_failure_20260823T151028Z/`.
It was captured with the immediately preceding APK (`25202df9...`), before the final scheduler/UI
integration edits; it remains the latest valid fixture diagnosis, not final-APK timing admission.

- Network admission completed after 12.797 seconds and 24 polls. Stabilized state was Degraded with
  an explicit override while raw state had recovered to Good; both directions completed.
- Down-the-line decoded exactly 597/597 frames; its three diagnostic frames retained the required
  AprilTag. Optical/audio offset was -18.491 ms with onset interval [-22.658, -14.263] ms, six white
  frames, and 25.071 ms duration. All timing checks passed. Only localization failed: 13 tiles
  versus the then-current maximum of 12.
- Face-on decoded exactly 615/615 frames; all three diagnostic AprilTags passed. Optical/audio
  offset was -13.736 ms with interval [-17.916, -9.503] ms, six white frames, 25.105 ms duration,
  and eight localized tiles; the complete retained-media analysis passed.
- All six PNGs were visually inspected. The down-the-line LED reflection spreads over the metal
  fixture surface. Cleanup restored all 10/10 obligations.

The final-APK retry is
`artifacts/android_hil_fixture_retry_localization_failure_20260824T010041Z/`. It used exact APK
`9d2ad9c...`, decoded exactly 658/658 down-the-line and 644/644 face-on frames, retained all six
AprilTags, and passed both roles' pulse intensity, five-frame/~20.9 ms duration, and point-timing
checks (-18.957 ms down-the-line and -11.182 ms face-on). Under the former spatial rule it reported
22 and 16 tiles. Network admission was explicitly overridden Degraded after 13.828 seconds; capture
and cleanup still completed, with all 10/10 obligations restored. All six PNGs, both analyses,
MP4s, WAVs, and the root report were inspected.

The user decided that spatial spread is not an acceptance criterion for this timestamp-only HIL
stimulus. Retained-media schema 2 removes the maximum-tile policy/check fields and retains
`peak_tile` plus `localized_response_tile_count` as diagnostic-only evidence. Run exactly one
shortest HIL with the integrated host validator to create a fresh schema-2 aggregate; no fixture
adjustment or policy-limit tuning is required first.

## Still open before declaring full field qualification

1. Obtain one fresh short paired HIL aggregate under retained-media schema 2; inspect the timing,
   media, audio, AprilTag, network, and cleanup evidence while treating spatial spread as diagnostic.
2. Physically swap/configure Pixel 6 to down-the-line and Pixel 5a to face-on for collection, then
   repeat exact-role strict stopped-clean and monitoring preflights.
3. Collect and completely review at least two hash-pinned, phone/view-swapped, negative-rich field
   sessions using the retained version-2 plan. Current development media do not satisfy this gate.
4. Run the five-minute qualification or 30-minute soak only if the user explicitly requests that
   specific duration. Pixel 5a support and sustained thermal behavior remain undecided.
5. Run the physical OS-reboot ceremony only with explicit user approval. It has never been executed
   for this revision; first-unlock and foreground human launch remain unproven.
6. TLS/protected transport, representative client-isolation/topology/roaming/loss coverage, pair
   clock threshold calibration, and human launch ceremony remain product follow-ups.

## Next commands

First verify exact APK, stopped/clean state, roles, direct LAN, and screens while retaining a new
directory:

```bash
bazel run //tools/field_preflight:android_field_preflight -- \
  --node 10.168.168.111:5555=http://10.168.168.111:8088 \
  --node 10.168.168.241:5555=http://10.168.168.241:8088 \
  --expected-role 10.168.168.111=face_on \
  --expected-role 10.168.168.241=down_the_line \
  --require-stopped-clean \
  --launch-after-unlock --sleep-screen-after-launch \
  --evidence-dir artifacts/field_preflight_before_session_UNIQUE
```

Run the bounded read-only telemetry sidecar before the first slate and leave it running through
final recording publication/export:

```bash
bazel run //tools/session_sidecar:session_sidecar -- \
  --node 22181FDF6005QH=http://10.168.168.111:8088 \
  --node 1A011JEG501717=http://10.168.168.241:8088 \
  --output-dir artifacts/hitting_sidecar_UNIQUE \
  --duration-seconds 3600 --interval-seconds 1
```

The sidecar never arms, stops, configures, deletes, or exports media. It retains bounded dual status
snapshots, logcat tails, power/battery/thermal/Wi-Fi diagnostics, a token scan, and SHA-256
inventory. Export both complete field MP4/WAV/manifests and any diagnostic ZIPs separately before
clearing or changing phone state.

To create the fresh schema-2 short production HIL aggregate, run:

```bash
bazel test --config=android_hil_inner \
  //android/dual_hil:dual_phone_paired_pose_arm_lan_hil_test \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_DTL_SERIAL=1A011JEG501717 \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_LAN_ORIGIN=http://10.168.168.111:8088 \
  --test_env=SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN=http://10.168.168.241:8088 \
  --test_env=SWING_CAPTURE_PCM_REPLAY_MANIFEST="$PWD/android/dual_hil/field_pcm_replay_cases.json" \
  --test_env=SWING_CAPTURE_PCM_REPLAY_WAV="$PWD/artifacts/field_recording_95482d93-f024-400e-9532-5ba7082de05e/face_on_pixel6_audio.wav" \
  --test_env=SWING_CAPTURE_PCM_REPLAY_CASE=S06-representative \
  --test_output=streamed --nocache_test_results
```

After a new APK or browser-contract change, repeat product-floor and direct-LAN Chrome evidence. For
software changes, use:

```bash
bazel test //tools:pre_field_integration_tests --nocache_test_results
bazel test //...
bazel build --config=manual_test_static_check //...
bazel test --config=precommit -c opt //...
```

Do not paste credentials into commands, reports, or this document. The launchers read the private
control tokens at runtime. Use one Bazel/HIL job at a time when physical cameras are involved, and
always restore both phones to `ready`, `armed=false`, screens asleep.
