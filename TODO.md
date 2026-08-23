# TODO

Last reconciled against source and retained evidence on 2026-08-23. Completed capabilities are
kept in the audit at `docs/todo_audit_20260822.md`; this file contains only work that is still
actionable or requires new physical/human evidence.

The operational checkpoint for a new agent/session is `docs/CURRENT_HANDOFF.md`. It records the
exact APK/device/network state, the post-reconnect direct-LAN pass, current monitoring and timing
failures, and the ordered next-session commands. Read it before acting on older evidence below.

## Pose arming and device performance

- Before each hitting session, use `//tools/field_preflight:android_field_preflight` first in
  stopped state and then with `--require-monitoring` after arming. The retained APK with SHA-256
  `785602e479836330ebdf62aca1d9fafd88206346f334e1aa960e4ba7b18b4733` passed that complete
  ceremony on its first strict attempt at
  `artifacts/field_preflight_monitoring_current_apk_pass_20260823/report.json`. Its nested doctor
  report proves the Pixel 6 face-on leader and Pixel 5a down-the-line shadow are armed, records
  53/51 successful pose inferences with 199/179 ms p95, retains standby audio on both, and measures
  a fresh 13.502 ms peer-clock uncertainty. The current Bazel APK and both installed APKs now share
  SHA-256 `9b1389f483a5cc5269065525231631a0196e757f2e9a0837bca16506ed487e70`, and fresh static
  admission passes at
  `artifacts/android_product_floor_current_apk_20260823T0855Z/report.json`. The stopped/monitoring
  ceremony was rerun after the host Wi-Fi repair: stopped admission passed at
  `artifacts/field_preflight_stopped_current_apk_after_lan_repair_20260823T0928Z/report.json`, but
  monitoring admission failed its unchanged 200 ms pose p95 bound (Pixel 6 207--208 ms and Pixel
  5a 309--334 ms across retained attempts) and suffered one transient Pixel 6 wireless-ADB evidence
  timeout. See `docs/CURRENT_HANDOFF.md`; the older monitoring pass remains historical.
- Exercise aborted address, arm-without-swing, clear-and-rearm, practice-swing, empty-scene,
  walk-through, and repeated-setup cases with reviewed low-rate preview/audio evidence. Expand the
  held-out ATL/DTL corpus and reconcile provisional safe-arm labels before treating aggregate
  replay scores as a release gate. The corpus evaluator now deterministically uses the full frame
  even when legacy per-clip ROI provenance exists, so this collection does not require ROI setup.
  The current seven-clip comparison gives Lite, Full, and Heavy
  the same 3/7 strict classification, with all four failures caused by provisional early-arm
  windows. Freshly review the current full-frame annotations, especially EE01's 2.8-second arm;
  the prior favorable review covered an older run. Trace inspection shows that Lite, Full, and
  Heavy all arm on the same genuine address-like setup before a practice swing, and that ROI
  removal—not different perception—made this attempt visible. The review therefore must decide the
  causal product policy and acceptable false-arm/high-speed-duty budget rather than calling 2.8
  seconds a model error. Compare Lite and the
  performance-feasible Full 640x360 candidate again only on the expanded, reconciled complete
  lifecycles before considering a model change. The combined-trigger replay now proves the entire
  state-machine sequence—including no scene exit, practice motion during camera-blind capture, a
  no-impact timeout, and three subsequent captures—so the remaining gap is real-frame perception
  and reviewed labels rather than arm/rearm lifecycle logic.
- Run simultaneous Pixel 6 and Pixel 5a motion workloads long enough to qualify steady-state p95,
  worst-case inference/decision age, latest-frame drop behavior, and thermal/CPU/GPU telemetry.
  The provisional bound remains inference p95 at or below 200 ms with no unexplained steady-state
  outlier above 400 ms.
- Measure worst-case address-to-ready latency under contention. Drop Pixel 5a support if it cannot
  meet the selected latency/accuracy bound without reducing Pixel 6 performance, as previously
  agreed. One current paired-LAN sample measured arm-to-first-usable-frame at 1.279 s on Pixel 6
  and 2.118 s on Pixel 5a, versus 3.735 s and 4.552 s to the full pre-roll barrier. This isolates
  the approximately 2.45-second retained-history cost but is not a contention bound. The closed
  qualification reports now preserve per-role startup p95 and exact maximum across every
  concurrent cycle. With only five or 30 physical samples, nearest-rank p99 is necessarily the
  maximum, so use the exact maximum as the conservative observed tail rather than claiming a
  separately estimated p99.

## Timing and qualification

- Collect at least two fully reviewed phone/view-swapped, negative-rich audio holdout sessions
  containing quiet real impacts, practice swings, mat strikes, waggles, speech, footsteps, club
  drops, and aborted addresses.
  The current 247-second development recording ranks hypotheses but cannot select a production
  threshold: the conservative envelope still fires early on the practice swing, and the temporal
  classifier's adjacent thresholds regress. Keep the ATL leader's current production detector
  until a complete-lifecycle holdout shows a real improvement. The collection/readiness contract
  and non-overwriting `//tools/field_evidence:audio_holdout_gate` now make that future comparison
  leakage-resistant: `field_readiness_v2` hash-verifies all media; requires at least two complete,
  reviewed phone/view-swapped sessions; requires two qualifying occurrences of every required pose
  and audio-negative category across at least two sessions; and applies the same two-occurrence,
  two-session rule to quiet real impacts. The gate rejects a detector lock that does not predate
  capture, scores every qualifying session, and gates target/quiet-impact recall, all non-target
  candidates, every required arm attempt, terminal behavior, captured swings, false attempts, and
  high-speed duty. The exact production
  Android detector/config replay emitter and same-holdout baseline/candidate comparator are now
  implemented and covered by the pre-field suite. The production baseline and fixed-memory
  `robust_hp120_x12` candidate are frozen before capture under
  `tools/field_evidence/policy_locks/`; the candidate emitter covers complete continuous and
  reset-from-arm replay and rejects stale implementation digests. The future physical recordings,
  human labels, generated holdout predictions/evaluations, and comparison result remain
  outstanding; no result is inferred from the current development set and production remains
  unchanged. The exact next-session checklist is retained at
  `artifacts/next_field_collection_plan_20260823.json`; it assigns Pixel 6 to down-the-line and
  Pixel 5a to face-on and provides an ordered F01--F14 operator sequence, neutral-gap/slating rules,
  and every lifecycle, hard-negative, quiet-impact, review, and diversity requirement for the next
  session. One completed sequence cannot by itself satisfy the version-2 readiness policy.
- Calibrate end-to-end audio/device and acoustic latency before describing the review marker as
  exact ball impact. The legacy ALSA and Android Camera2 paths currently retain bounded evidence,
  but neither makes an absolute strike-time claim. The deterministic evidence contract and scorer
  in `docs/timing_calibration_evidence.md` now separate speaker-emission/device-path calibration
  from an instrumented ball-contact claim; the hash-pinned physical trials and review are still
  required.
- Run the five-minute Android qualification or 30-minute soak only when explicitly requested.
  Closed manual targets now exercise the complete current trigger/publication/rearm workload with
  screen-off and periodic telemetry. Every cycle now retains both MP4s independently and must pass
  ffprobe timeline validation, exact decode, white-marker timing, persistent pre/marker/post
  AprilTag checks, and final artifact traversal, so a good last cycle cannot hide broken earlier
  media. Neither long target has been executed for this revision. The short
  physical pass at `artifacts/android_pcm_current_apk_paired_pass_20260823T002901/report.json` is
  bound to APK SHA-256 `74115ca3e6a8ded19a1c995ea0b9f8be65bcfbe266d401330e1e028da3cf594b`
  and is historical rather than present-revision evidence. Existing long evidence still does not
  qualify both phones.
- Decide from field/thermal evidence whether Pixel 5a remains supported and whether Pixel 6
  720p240 stays within the accepted thermal state under production workload with the screen off.
- Retain fresh, exact-APK-bound `product_floor_assessment` evidence after a material APK change and
  on every newly proposed device. `//tools:android_capability_report` host-hashes the Bazel APK and
  each phone's sole installed `base.apk`, records the expected and installed digests, and fails the
  pair unless both match. The Pixel 6/Pixel 5a pair passed all capability and digest checks for the
  current APK at `artifacts/android_product_floor_current_apk_20260823T0855Z/report.json`; the
  Bazel APK and both installed APKs have SHA-256
  `9b1389f483a5cc5269065525231631a0196e757f2e9a0837bca16506ed487e70`. This closes static admission
  for that exact APK only; subsequent material APK changes require a new report, and no static
  report closes the long-duration thermal qualification.

## Product and protocol follow-ups

- Before declaring a revision prototype-release-ready, run and review
  `//web:prototype_browser_release_gate`. Desktop Google Chrome and the combined exact-frame AVC
  plus exact-APK/direct-LAN phone-browser boundary are already selected; Firefox and WebKit remain
  optional compatibility candidates rather than release blockers. The physical half now also
  requires Chrome to decode both MP4s created by that invocation, present a nonblack frame, and
  advance playback by at least 0.1 seconds, with role/origin/shared-recording identity checked in
  the retained report. Two consecutive exact-APK Chrome runs at
  `artifacts/android_field_recording_browser_hil_relay_range_pass1_20260823T071927Z` and
  `artifacts/android_field_recording_browser_hil_relay_range_pass2_20260823T072013Z` passed the
  final fresh-decode/cancellation schema and show that Chrome decoder release does not strand the
  phone media worker. They used an explicitly temporary, non-qualifying relay for the Pixel 6
  face-on origin, so they are cleanup evidence rather than release evidence. The direct-LAN attempt
  at `artifacts/android_browser_hil_direct_lan_preflight_failure_20260823T072340Z` reached DTL in
  951 ms but recorded five 500 ms Pixel 6 transport timeouts through 2.901 seconds and stopped
  before Playwright; exact-APK verification and both screen-sleep cleanup actions still passed.
  Pixel 6-to-host Wi-Fi reachability was restored by the user's local-console NUC Wi-Fi reconnect,
  and the post-repair directed pair matrix passes at
  `artifacts/android_pair_preflight_post_wifi_reconnect_20260823T163223Z.json`. Rerun the direct-LAN
  combined browser gate now that its former transport blocker is gone. The prior combined
  checkpoint lacks `fresh_media_decode`, and no relay or earlier component pass substitutes for a
  reviewed invocation against the proposed release candidate.
  The current exact APK also passes the complete Chrome/camera/publication/decode flow through an
  explicitly temporary diagnostic relay at
  `artifacts/android_field_recording_browser_hil_current_apk_relay_pass_20260823T0904Z`: both
  recordings published, all four MP4/WAV ranges returned 206, both 720x1280 H.264 clips presented
  nonblack frames and advanced playback, accessibility/page/request checks were clean, and both
  screens returned to sleep. That run exposed and then regression-covered launcher-report schema
  drift, but it deliberately does not qualify the direct-LAN edge.
- Replace topology-specific network assumptions with a generic field-health contract. The prior
  host-to-Pixel-6 failure was repaired by the user's one-shot NUC Wi-Fi reconnect; the strict
  current-APK pair report now passes host-to-both and both phone-to-peer directions at
  `artifacts/android_pair_preflight_post_wifi_reconnect_20260823T163223Z.json`. Pixel 5a still showed
  high host-RTT variance despite strong RSSI. Equal BSSID or 5 GHz must not become product gates:
  different BSSIDs identify different radios but not necessarily different physical mesh nodes,
  and real deployments will use diverse routers. Measure bounded application-level bidirectional
  reachability, latency, jitter, timeouts, and representative transfer behavior; expose
  good/degraded/unusable with hysteresis; let users override a degraded warning; and fail only when
  coordination is not viable. Retain BSSID, band, RSSI, and link rate as diagnostic context. Cover
  client isolation, delay/loss, roaming, and transient disconnects deterministically. The doctor
  now validates `ip route get <peer>` provenance before ordinary ping because Pixel 5a rejects
  `ping -I wlan0` with `SO_BINDTODEVICE: Operation not permitted`; its focused regression passes.
- Physically run and review
  `//android/dual_hil:dual_phone_os_reboot_ceremony_hil_test` once on the Pixel 6/Pixel 5a pair.
  The manual/local/exclusive gate is implemented and statically checked, but deliberately has not
  rebooted either phone. It requires two USB ADB connections, literal reboot approval, first
  unlock, a foreground operator launch, node API availability, exact durable configuration and
  binding recovery, successful monitoring rearm, and terminal disarm. Its software validator now
  requires the recovered leader clock to identify the durable shadow node, contain at least one
  sample, be no older than 10 seconds, and have uncertainty no greater than 25 ms; missing,
  malformed, stale, wider, or wrong-peer clock evidence fails closed.
  Its read-only APK gate fails rather than installing, and its report retains explicit pre/post and
  cleanup evidence. The direct-boot-aware receiver records only a non-secret boot marker in
  device-protected storage; it deliberately never starts the service, camera, or microphone. True
  zero-touch reboot recovery would require a separately approved device-owner/kiosk architecture.
- Select a physical pair clock-uncertainty acceptance threshold from representative field evidence.
  The current 25 ms bound is a provisional fail-closed implementation policy, not a calibrated
  product limit. Six same-pair/same-network direct-LAN successes used 9.625--22.628 ms composed
  mapping uncertainty and 0.137--7.182 s mapping age; one historical high-jitter batch reached
  117.789 ms and was rejected. Collect multiple networks, contention, long-running screen-off
  operation, phone/view swaps, and accepted/rejected field events before changing the bound; see
  `docs/pair_clock_evidence_20260822.md`. Future selection evidence should use the independent
  physical-reference/scoring contract in `docs/timing_calibration_evidence.md`; current transport
  acceptance must not label itself as clock-error ground truth.
- Add TLS or another authenticated protected transport before using an untrusted network. Session
  manifests and field-recording catalogs now require Bearer authorization; native MP4/WAV requests
  use HMAC-SHA256 collection-scoped capabilities that cannot cross sessions and are invalidated by
  control-credential rotation. Capability-bearing catalogs and manifests are private and
  non-cacheable. All identity and operational metadata reads, including `/api/v1/node`, now fail
  closed on Bearer auth; only the deliberately untrusted `/api/v1/clock` time hint remains public.
  Authenticated peer and preflight clients reject redirects so credentials stay on their configured
  phone origin. Credentials and capabilities are still observable and replayable over cleartext
  HTTP, and the manual bootstrap/pairing exchange remains a prototype concern. The architecture is
  now selected and dependency-ordered in
  `docs/android.md#protected-transport-closure-plan`: use a station-local CA, non-exportable
  per-phone Android Keystore identities, ordinary browser/peer certificate validation, an attended
  single-use bootstrap, explicit pin/root rotation and recovery, and a final cleartext-off release
  cutover. That plan closes the design-choice gap, not this implementation/evidence TODO.

## Deferred legacy-host follow-ups

- Reproduce the occasional 3--4 second paired-preview update gap if the Daheng/NUC path becomes
  active again. The old trace cannot identify its cause, but current status, HTTP, browser, and
  persisted UI telemetry now partition acquisition, sampling, rendering, handler/transport/body,
  decode, loader backpressure, and browser scheduling. Retain that evidence before changing the
  legacy path.
- Drawing/annotation tools remain an optional review-UI feature. Golden-image comparison is now a
  deterministic browser gate at both 1440x1000 and 390x844 for synchronized playback and configured
  two-phone setup; baseline replacement requires an explicitly reviewed UI change.
