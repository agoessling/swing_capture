# TODO

Last reconciled against source and retained evidence on 2026-08-23. Completed capabilities are
kept in the audit at `docs/todo_audit_20260822.md`; this file contains only work that is still
actionable or requires new physical/human evidence.

The operational checkpoint for a new agent/session is `docs/CURRENT_HANDOFF.md`. It records the
exact APK/device/network state, the post-reconnect direct-LAN pass, current physical evidence,
telemetry procedure, and the ordered next-session commands. Read it before acting on older evidence
below.

## Pose arming and device performance

- Before each hitting session, use `//tools/field_preflight:android_field_preflight` first with
  `--require-stopped-clean` and then with `--require-monitoring` after arming. The stopped-clean
  contract requires unarmed high-speed capture, no active capture or shared session, zero
  autonomous replication backlog/trigger/publication work, and a terminal field recorder; every
  attempt retains credential-redacted raw capture and field-recording status. Run the bounded
  read-only `//tools/session_sidecar:session_sidecar` from before the first slate through final
  publication/export so status history, logcat, and device/network/resource context survive. The
  current Bazel APK and both
  installed APKs share SHA-256
  `9d2ad9c12f8ff1846d02ab8018d0a659bf88d313b542824fde0c185d1572a230`; its static product-floor
  pass is `artifacts/android_product_floor_final_20260823T154000Z/report.json`. Stopped admission
  passed on attempt 3 at
  `artifacts/field_preflight_stopped_final_apk_20260823T153900Z/report.json`. Armed monitoring
  admission passed on its first retry attempt at
  `artifacts/field_preflight_monitoring_final_apk_retry_20260823T154400Z/report.json`; its detailed
  `doctor_attempt_1.json` records the Pixel 6 face-on leader at 150 recent GPU/Lite samples, 140 ms
  p95 and 156.548 ms max, and the
  Pixel 5a down-the-line shadow at 150 samples, 150 ms p95 and 209.274 ms max. Both had zero failed
  inferences and rejected timestamps; network health was Good and peer-clock uncertainty was
  15.901 ms. Preserve the failed predecessor directories: they record the expected short recent
  window during warm-up plus transient Pixel 5a hosted-asset, descriptor, and thermal-service
  timeouts. The latest stopped/screen-off evidence is retained at
  `artifacts/field_preflight_after_fixture_retry_20260824T010200Z/report.json`, but that artifact
  predates the expanded stopped-clean field-recorder checks. Obtain a fresh strict stopped-clean
  report before collection. See `docs/CURRENT_HANDOFF.md`; older APK ceremonies remain historical.
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
  `artifacts/next_field_collection_plan_20260823T211253Z.json`; it assigns Pixel 6 to down-the-line and
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
  qualify both phones. The first final-APK short HIL at
  `artifacts/android_hil_fixture_retry_localization_failure_20260824T010041Z/` reached complete
  exact decode on both roles and passed AprilTag persistence, pulse intensity/frame-count/duration,
  both operational timing bounds, and cleanup. It failed only the former spatial-light rule at 22
  down-the-line and 16 face-on tiles. The LED is a timestamp-only fixture stimulus, so retained-media
  schema 2 removes footprint from acceptance while retaining peak tile and tile count as diagnostic
  telemetry; weak, short, long, and mistimed pulses remain failures. Obtain one fresh shortest HIL
  aggregate under schema 2 before considering a longer qualification; no reflection adjustment or
  spatial-limit tuning is required.
- Decide from field/thermal evidence whether Pixel 5a remains supported and whether Pixel 6
  720p240 stays within the accepted thermal state under production workload with the screen off.
- Retain fresh, exact-APK-bound `product_floor_assessment` evidence after a material APK change and
  on every newly proposed device. `//tools:android_capability_report` host-hashes the Bazel APK and
  each phone's sole installed `base.apk`, records the expected and installed digests, and fails the
  pair unless both match. The Pixel 6/Pixel 5a pair passed all capability and digest checks for the
  current APK at `artifacts/android_product_floor_final_20260823T154000Z/report.json`; the
  Bazel APK and both installed APKs have SHA-256
  `9d2ad9c12f8ff1846d02ab8018d0a659bf88d313b542824fde0c185d1572a230`. This closes static admission
  for that exact APK only; subsequent material APK changes require a new report, and no static
  report closes the long-duration thermal qualification.

## Product and protocol follow-ups

- Keep `//web:prototype_browser_release_gate` as a per-revision release requirement. The current
  exact-APK/direct-LAN candidate now passes the selected desktop-Google-Chrome boundary at
  `artifacts/browser_direct_lan_final_apk_20260823T153900Z`: both newly created 720x1280 H.264 clips
  were nonblack and advanced by more than 0.1 seconds, all four MP4/WAV paths served byte ranges,
  the role/origin/shared-recording identities agreed, page and accessibility errors were empty, and
  both phones returned to the required stopped/screen-sleep state. This completes the direct-LAN
  Chrome release gate for that candidate; relay runs remain diagnostic cleanup evidence only, and
  a material APK or browser-contract change still requires a fresh reviewed invocation. Firefox
  and WebKit remain optional compatibility candidates rather than release blockers.
- Preserve the completed generic field-health contract and extend its physical coverage. The
  phones now measure bounded application-level bidirectional reachability, latency, jitter,
  timeouts, and representative transfer behavior; publish good/degraded/unusable with hysteresis;
  expose both directions and issues in setup/review; require a one-shot operator acknowledgement
  for degraded service; and fail closed when configured coordination is unknown, stale, or
  unusable. Equal BSSID, 5 GHz, router credentials, and vendor topology remain deliberately outside
  the gate; BSSID, band, RSSI, and link rate are diagnostic context only. Deterministic software
  coverage exercises loss, recovery, strict status parsing, dual-node selection, and arm admission.
  The final-APK stopped preflight at
  `artifacts/field_preflight_stopped_final_apk_20260823T153900Z` observed the expected hysteresis:
  its first two attempts were valid but still unusable while recovering, and its third attempt
  passed as viable degraded service with an explicit warning. The later direct-LAN browser run at
  `artifacts/browser_direct_lan_final_apk_20260823T153900Z` showed measured Degraded/recovering
  health with both directions and the one-shot acknowledgement while the complete Chrome flow
  passed. This closes the software slice and shortest
  same-pair/direct-LAN evidence only. Representative client-isolation, topology, injected
  delay/loss, roaming, and transient-disconnect physical coverage remains open, as do long
  monitoring/thermal qualification, multi-session human collection, OS-reboot evidence, and TLS.
  The initial timing-HIL candidate failed before capture while health admission warmed up; the HIL
  now waits within its existing 15-second budget for a stabilized Good or explicitly overridden
  Degraded state and never overrides Unusable. Physical reruns cleared that integration path. The
  final-APK complete capture at
  `artifacts/android_hil_fixture_retry_localization_failure_20260824T010041Z/` passed both roles'
  exact decode, AprilTags, intensity/duration, and timing bounds; only the now-retired spatial rule
  failed. The final immediate-poll scheduler integration has software, prior stopped, browser,
  monitoring, and complete final-APK component evidence, but it still requires a fresh strict
  stopped-clean preflight and one fresh short aggregate under the schema-2 host validator.
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
