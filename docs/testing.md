# Testing Strategy

## Default software suite

```bash
bazel test //...
```

The default suite consists entirely of hardware-independent C++, Java, Python,
and TypeScript/UI tests. The representative targets are listed below.
The current 2026-08-23 canonical checkpoint passed all 236 software tests. The separate
`--config=manual_test_static_check` build selected and compiled all 916 targets without executing
the manual hardware tests.

For Android-only Java iteration, run the checked focused configuration before
the full checkpoint:

```bash
bazel test --config=android_inner //android/app:all
```

It builds only the selected tests and excludes manual tests; it does not build
the APK. After changing HIL-only sources, compile every manual wildcard target
without executing any test:

```bash
bazel build --config=manual_test_static_check //...
```

The definitions, tradeoffs, and recorded timings are in
[`bazel_iteration.md`](bazel_iteration.md).

| Area | Bazel targets |
|---|---|
| Audio and trigger | `//capture/audio:arecord_pcm_source_test`, `//capture/audio:audio_capture_session_test`, `//capture/audio:audio_hil_metrics_test`, `//capture/audio:commanded_click_analyzer_test`, `//capture/audio:commanded_tone_analyzer_test`, `//capture/audio:pcm_wav_test`, `//capture/trigger:impact_detector_test` |
| Storage and timing | `//capture/core:camera_source_test`, `//capture/core:raw_frame_ring_test`, `//capture/core:pooled_raw_frame_ring_test`, `//capture/core:device_clock_mapper_test` |
| Session and clip selection | `//capture/session:capture_session_coordinator_test`, `//capture/clip:clip_window_planner_test` |
| HIL protocol and evidence | `//capture/hil:feather_hil_protocol_test`, `//capture/hil:feather_hil_serial_test`, `//capture/hil:feather_hil_controller_test`, `//capture/hil:hil_metrics_test`, `//embedded/prop_maker:hil_protocol_test` |
| Application capture and publication | `//capture/application:camera_clip_buffer_test`, `//capture/application:audio_impact_monitor_test`, `//capture/application:capture_controller_test`, `//capture/application:clip_session_publisher_test`, `//capture/application:session_catalog_test`, `//capture/encoding:clip_session_test`, `//capture/hil:application_audio_stimulus_test`, `//capture/hil:session_artifact_validator_test` |
| Optical and images | `//capture/optical:april_tag_test`, `//capture/optical:frame_selection_test`, `//capture/optical:led_pulse_test`, `//capture/optical:led_schedule_association_test`, `//capture/image:bayer_rg8_test`, `//capture/image:image_quality_test` |
| Synthetic end to end | `//capture/pipeline:capture_pipeline_integration_test` |
| SDK packaging | `//capture/daheng:daheng_sdk_runtime_test`, `//third_party/daheng:extract_sdk_test` |
| Station configuration | `//station:station_config_test` |
| Setup preview core | `//capture/preview:camera_settings_test`, `//capture/preview:latest_frame_sampler_test`, `//capture/preview:preview_image_test` |
| Setup service | `//capture/service:camera_worker_test`, `//capture/service:preview_api_test`, `//capture/service:synthetic_swing_hil_operation_test`, `//capture/service:synthetic_swing_hil_timeline_test`, `//capture/service:synthetic_swing_station_workflow_test` |
| Android node and coordination | `//android/app:all`, `//android/core/coordination:all`, `//android/core/node:all`, `//android/core/pose:all` |
| Setup and review web UI | `//web:typescript`, `//web:component_test`, `//web:review_component_test`, `//web:live_status_test`, `//web:browser_test` |
| Host tooling | `//tools:android_pair_doctor_test`, `//tools:bazel_bep_summary_test`, `//tools:station_doctor_test`, `//tools:unattended_hil_test` |

The end-to-end fixture synthesizes two cameras with unrelated device clocks
and an audio impact. It verifies post-roll waiting, reference-counted freezing,
bounded strike-relative selection for both views, and continued recording
while the frozen clip remains valid. Session boundary tests distinguish the
backdated strike sample from its later confirmation time, wait one frame
beyond the requested endpoint, and explicitly expire a late request before
pre-roll can be overwritten. The audio source unit test launches a
deterministic fake producer; it does not open host audio hardware.

All test logic above is owned by Bazel. The native and Android application
coverage uses `cc_test` and `java_test`; browser coverage uses Bazel-owned test
bundles and Playwright; the station doctor, SDK extractor, and unattended
runner tests are `py_test` targets.
Shell scripts are not used as test implementations. Synthetic tests cover
timing boundaries and failure paths, not only nominal examples. The wildcard
suite downloads Galaxy Linux SDK
`2.6.2606.9251`, verifies its SHA-256, safely extracts an allowlisted SDK
surface, and tests that the GenTL producer is available through Bazel runfiles.
It does not open a camera. The repository adapter is documented in
[`third_party/daheng/README.md`](../third_party/daheng/README.md).

The service tests use a fake camera adapter and synthetic Bayer frames. They
cover overwrite-latest image publication, reset without sequence reuse,
compressed routine rendering, on-demand full-resolution rendering, monotonic
JPEG/status sequences, measured frame delivery, bounded timeout and
invalid-frame failure, post-stop command rejection, setting
range/increment rejection, stop/configure/start serialization, preview
invalidation, failed-update recovery, capture/session HTTP contracts, session
catalog restart discovery, WebM byte ranges, request content/origin checks,
security headers, malformed requests, and static asset serving.

The synthetic-swing service tests cover the explicitly gated API, serialized
lifecycle and recovery, typed Feather timing receipts, automatic shared
brightness selection across two camera sweeps, white-impact qualification,
manifest evidence, and one-shot ready/unarmed completion. Pre- and post-impact
colors are human review cues and are not exact programmatic gates. No default
test opens the cameras, microphone, or Feather.

The component tests cover dual-view setup rendering, capacity-one paired
polling, apply/revert behavior, disconnected/error states, one-shot capture
transitions, the opt-in synthetic-swing button and progress/error states,
per-role optical/audio evidence labels, synchronized review controls, runtime
schema rejection, and axe accessibility scans. The checksum-pinned Chromium
test decodes real all-intra VP8 fixtures, seeks, steps an exact source frame,
exercises synchronized playback and drift correction, navigates between setup
and review, runs the synthetic workflow against a deterministic fake API, and
retains fixed 1440x1000 and 390x844 screenshots. Four checked-in Playwright baselines compare the
desktop and phone presentations of the synchronized review player and configured two-phone setup
screen. Only the five browser-process timing samples are normalized; their labels and layout plus
all media, controls, and surrounding UI remain in the golden comparison. No default service or
browser test opens physical hardware.

`//web:live_status_test` deterministically exercises Android status version parsing, stale-response
rejection, server-stream restart acceptance, disconnect notification, bounded exponential
backoff, and recovery without wall-clock sleeps. The component contract verifies that credential
rotation is observed by the next authenticated status request. The production-shaped browser test
independently disconnects one phone and freezes one phone's revision, verifies that live controls
become unavailable while the loaded player remains usable, and then verifies automatic recovery.

## Hitting-session incident traceability

This matrix is based on the named assertions in the current test sources, not capability prose.
The production-shaped browser fixture serves the real web bundle from one origin and two independent
Android-shaped node origins.

| Incident or field contract | Deterministic regression and inspected assertion | Retained physical evidence |
|---|---|---|
| Missed-shot action returned 401 or only one phone retained the diagnostic | `//web:browser_test`, `stale missed-shot credentials report 401 and recover after correction`, proves a stale face-on credential fails the read-only two-phone preflight before either diagnostic POST, preserves both armed states, and succeeds after reopening with the current credential. The distinct `partial missed-shot acceptance safely disarms both phones and retries in one tab` case injects face-on HTTP 409 only after both preflights pass: DTL accepts retention, both authenticated diagnostic POSTs are observed, the coordinator sends an authenticated convergence disarm to each phone, clears stale session ownership, reaches “Not armed,” and successfully rearms and saves on both phones in the same tab. | The original hitting-session failure was observed interactively; no machine-readable artifact was retained. The after-preflight branch is deterministic transport fault injection rather than physical evidence. |
| “Both phones must be armed” and stale browser ownership | `//web:browser_test`, `production-shaped missed-shot rejection has no mutations before arm and retry`, plus `//web:dual_node_review_api_test`, `rejectsStaleBrowserArmBeforeEitherMissedShotMutation` and `rejectsReplacedBrowserSessionBeforeEitherMissedShotMutation`: the coordinator re-reads both live statuses and the shared-session ID before issuing either diagnostic POST. | No fault-injected phone artifact is required for the pure coordinator race; the historical UI failure itself was not retained. |
| Stale or two-shadow phone topology | `//tools:android_pair_doctor_test`, `test_two_shadow_phones_fail_before_field_use`, rejects the exact role-complementary but leaderless topology and its absent authenticated binding. The same gate now rejects stale APKs and absent current-boot/device-admission contracts per phone. `//android/app:node_setup_policy_test` rejects a leader without a reachable peer, a leader targeting another leader, and any outbound peer on a shadow. | `artifacts/android_pair_preflight_web_20260822.json` retains the real two-shadow rejection. `artifacts/android_pair_preflight_current_apk_20260823.json` proves the restored face-on leader/down-the-line shadow binding and exact current APK on both phones. |
| Missing DTL clip, encoding placeholder, and black/empty playback | `//android/app:autonomous_pair_peer_client_test` requires exact HTTP 200 authenticated HEAD responses for both the manifest and expected role MP4; a manifest-only response remains unpublished. This avoids the prior 64 KiB control-response limit rejecting real frame-rich Android manifests retained at roughly 99--147 KiB. `//android/core/coordination:autonomous_pair_lifecycle_test` proves neither local-first nor peer-first publication can expose a pair before both clips publish. `//web:browser_test`, `production-shaped UI recovers the historical missing down-the-line clip`, then verifies the presentation boundary: DTL encoding shows no video and an explicit pending message, missing publication shows the exact DTL error, and later publication restores two players. The same target checks both origins and 206/416 range behavior. Chrome, installed Firefox/geckodriver, and checksum-pinned Playwright WebKit each have a fail-closed H.264 gate covering Range, decoded nonblack pixels, exact presentation, GOP seeks, and playback. | `artifacts/android_browser_hil_20260815T110817Z/test.log` records the live two-phone review test passing; `outputs/android-dual-node-review.png` retains the rendered 1440x1000 review. `artifacts/browser_system_qualification_20260823T0045Z/firefox-h264-exact-frame.json` retains Firefox's ten exact presentations. `//web:webkit_h264_browser_test` passes the same two-origin production test through the checksum-pinned Ubuntu 24 compatibility closure. |
| One phone rejects or fails after accepting continuous field-recording start | `//web:browser_test`, `partial field-recording start rolls back and retries without reload`, retains the synchronous 409 case. The separate accepted-start case returns 202 from both starts, reports face-on's asynchronous pipeline error through status polling, stops both accepted nodes, and retries without reload. `//web:dual_node_review_api_test` independently checks the exact two-start/two-stop sequence and fresh-ID retry. The Android fault gate waits until both 202 responses are observed before releasing the one-shot failure, while its strict validator rejects an early/synchronous failure, a missing role-specific terminal error, incomplete rollback, or a failed retry. | `artifacts/android_field_recording_partial_start_hil_pass_20260823T015457/report.json` physically proves synchronous DTL-202/face-on-409 rollback and retry. `artifacts/android_field_recording_async_start_failure_hil_pass_20260823T091443/report.json` physically proves the distinct post-202 path: both starts were accepted, the face-on terminal failure surfaced, both nodes rolled back, a fresh-ID retry published on both, and all nine cleanup actions passed in 11.898 seconds. |
| Historical catalog hydration starves field controls | `//web:dual_node_review_api_test`, `loadsRichAndroidCatalogWithoutHistoricalManifestRequests`, presents 35 complete pairs plus incomplete/duplicate cases from current compact Android summaries, requires zero historical manifest and coordination GETs, and then requires exactly two of each only when one shot is selected. `historical catalog hydration cannot starve field-recording controls` preserves the bounded two-request fallback for legacy nodes and starts both controls while that fallback remains blocked. `//web:review_component_test` additionally holds both session history and the field-recording catalog indefinitely: live capture and both role cards must still render, Start must become enabled, status polling must continue, and the pending catalog must be requested only once. Simultaneous descriptor and capture-status consumers coalesce without caching across later polls. | The first direct-LAN phone-browser attempt exposed connection starvation. A later current-worktree run with 40 retained sessions and 47 pre-existing field recordings exposed the remaining UI-level coupling. `artifacts/android_field_recording_browser_hil_pass_20260823T132412Z/report.json` passes after decoupling live status from history: the explicit historical baseline was ready in 3.474 seconds, the camera stage completed in 5.546 seconds, both starts/stops returned 202, and both nodes published. |
| One phone disconnects while stopping a field recording | `//web:browser_test`, `field recording recovers a disconnected phone during stop without reload`, proves the read-only preflight fails before either stop mutation, both recorders remain active, the role-specific error is accessible, and a retry after reconnection stops each phone exactly once without reloading. | This is deterministic transport fault injection; no repeated real recording or physical disconnection is needed to prove coordinator mutation ordering. |
| One phone fails its stop after both stop preflights pass | `//web:browser_test`, `field recording retries an after-preflight partial stop without reload`, makes DTL become idle while face-on returns 503, exposes the split state and role-specific error, then retries safely and converges both nodes without reload. | This is deterministic after-mutation transport fault injection; no physical recording is needed to prove retry ownership. |
| Stop was accepted but publication takes longer than the first UI poll window | `//web:browser_test`, `field recording keeps both nodes stopping until asynchronous publication completes`, holds both accepted stops in `stopping`, requires one stop mutation per phone and a stable stopping state, then releases publication and starts the completed library. The physical browser harness separately allows eight seconds for this bounded convergence. | `artifacts/android_field_recording_browser_hil_failed_20260823T103018Z/report.json` records both 202 stops, both completed bundle origins, and all four successful MP4/WAV Range reads; the target's overall timeout interrupted only final cleanup. |
| Direct phone-to-phone discovery, auth, clock, arm, impact, publication, and rearm | `//android/app:lan_node_discovery_registry_test`, `peer_pairing_binding_test`, and `peer_credential_status_policy_test` cover discovery lifetime, authenticated identity/address replacement, credential generations, revocation, and re-pair. `//android/app:peer_clock_client_test`, `peer_clock_synchronizer_test`, and `//android/core/coordination:clock_exchange_estimator_test` cover direct non-cacheable HTTP exchange, redirect refusal, authenticated identity matching, bounded bodies, invalid and expired sample rejection, bounded clock mapping, and clock-jump withdrawal. `//android/app:pose_peer_arm_client_test` covers authenticated HTTP arm/impact, readiness and transient retries, cancellation across lifecycle replacement, non-retry of 401, session identity, mapped-impact fallback, and redirect refusal. `//android/app:automatic_trigger_readiness_gate_test` requires local and peer encoded pre-roll readiness in either callback order and keeps the no-impact timeout disabled for an unconfirmed peer. `//android/app:autonomous_pair_peer_client_test` now checks destination Bearer auth on peer arm, trigger, manifest HEAD, MP4 HEAD, and coordination replication while rejecting manifest-only publication; `//android/core/coordination:autonomous_pair_lifecycle_test` covers publication ordering, partial publication, restarts, backlog, conflict fail-closed, and local/peer rearm. `//android/dual_hil:concurrent_hil_validation_test` rejects corrupt identities, sessions, trigger reports, clock/mapped-impact evidence, startup milestones, missing capture media, or a LAN report that used ADB reverse. | `artifacts/android_discovery_pairing_pass_20260822T195604/report.json` records mutual discovery, authenticated binding, wrong/old-token rejection, rotation/re-pair, and passed cleanup. `artifacts/android_pcm_paired_s06_startup_pass_20260823T043404Z/report.json` records the current APK crossing the two-phone full-pre-roll barrier, `wifi_lan_direct`, no ADB reverse, five mapped-clock samples per phone, accepted peer arm, schema-2 mapped impact, two decoded 720p240 AVC sessions, monotonic startup milestones with zero resets, 55 validated artifact files, visually nominal pre/white/post diagnostics, persisted coordination, monitoring rearm, and complete cleanup. Restart/rearm and Wi-Fi/peer-restart recovery are retained in `artifacts/android_autonomous_restart_pass_20260822T174922/report.json` and `artifacts/android_autonomous_disturbance_pass_20260822T175253/report.json`. |
| Unsupported phone and OS-reboot expectations are discovered before capture | `//android/app:device_capability_policy_test` covers every API, permission, camera/profile, encoder, PCM, GLES, and failed-probe rejection; it also proves a Pixel-5a-shaped 1080p rejection cannot mutate a separately assessed Pixel-6-shaped phone. `//tools:android_capability_report_test` rejects stale, incomplete, cross-node, pose-coupled, screen-on, and APK-mismatched evidence. `//android/app:unattended_recovery_policy_test` covers boot-event classification, the prohibition on boot-triggered capture, locked boot, unlocked-but-not-launched, permission loss, and running-service readiness. `//android/dual_hil:os_reboot_ceremony_validation_test` rejects unchanged boot IDs, missing pre-launch absence, ADB-launched activity, configuration drift, missing peer/monitoring evidence, incomplete disarm, and a recovered leader clock that is missing, malformed, mapped to the wrong peer, has no samples, is older than 10 seconds, or exceeds 25 ms uncertainty. The manual `//android/dual_hil:dual_phone_os_reboot_ceremony_hil_test` is the only gate that can turn those contracts into physical reboot evidence. Setup/status expose `device_admission`, `reboot_recovery`, and the durable marker; arming checks local admission before coordination or camera mutation. | `artifacts/android_product_floor_exact_apk_pass_20260823T072145Z/report.json` passes the independent 720p240, hardware AVC, audio, GLES, Lite 640x360/5 Hz, screen-off, and exact installed/Bazel APK checks on Pixel 6/API 36 and Pixel 5a/API 34 at SHA-256 `328203380f02db24ec7d7707476cab361d3164476d5c9187549897aa64cad39a`. `artifacts/android_pair_preflight_current_apk_20260823.json` proves both operator-started services were unlocked and ready on the cited current boot. The physical OS-reboot target has been statically compiled but deliberately not executed; first-unlock/foreground-launch/peer-clock-recovery/rearm evidence and long thermal qualification remain open. |
| Cleanup after a primary failure | `//android/dual_hil:hil_cleanup_evidence_test` crosses primary pass/fail with cleanup pass/fail, preserves the primary diagnostic, accepts a recorded failed attempt followed by successful restoration, and fails unresolved obligations. `//android/dual_hil:configuration_recovery_journal_test` rejects unsafe/secret-bearing journals and verifies ordered, hashed, idempotent restoration and atomic owner-only journal replacement. | `artifacts/android_discovery_pairing_failure_20260822T194808/report.json` and `artifacts/android_pcm_paired_s06_lan_latency_failure_20260822T202518/report.json` retain failed primary outcomes with `cleanup.passed=true`; the later passing discovery and paired-LAN reports also retain complete restoration evidence. |
| Retained white marker is visually clean but outside the strict point-timing gate | `//android/dual_hil:rgb_swing_analysis_test` reproduces the −20.832 ms first-white-frame result with six valid white frames and an adjacent-frame interval of −24.999 to −16.665 ms. It remains rejected against the unchanged ±20 ms point limit while proving the delta, duration, frame-count, and localization checks pass. `//android/dual_hil:dual_session_validation_test` composes the retained 277.241 µs trigger uncertainty and 293 µs maximum PTS residual, independently reproducing the later conservative lower bound of −25.570 ms against its unchanged ±25 ms limit. `//android/dual_hil:retained_media_analysis_test` requires the completed analysis to serialize every policy limit and independent result before the HIL reports failure. | `artifacts/android_hil_inner_timing_failure_20260823T081020Z` retains nominal pre/white/post images from both phones; the down-the-line point estimate exceeded the strict limit by 832 µs, and its uncertainty-expanded interval independently exceeded the conservative limit by 570 µs. Future runs also retain `<role>/retained-media-analysis.json`, so this boundary failure cannot collapse into an unstructured exception before node evidence is published. This does not calibrate absolute ball-impact timing or weaken either timing gate. |

Scoped-media authorization is also part of the deterministic incident gate. The Java protocol
requires Bearer authorization for session manifests and field-recording catalogs, grants only a
collection-scoped native MP4/WAV capability, and rejects cross-session, ambiguous, and pre-rotation
capabilities while retaining Bearer access for peers. The production browser requires the scoped
query on both origins, exercises 206/416 Range responses through it, rejects a capability-free
request, and treats a stale field credential as a read-only failure before any mutation.

Remaining physical, product-policy, and external-toolchain evidence is deliberately narrower than
deterministic correctness:

- desktop Google Chrome is the selected prototype browser, and every proposed release revision
  requires a reviewed passing `//web:prototype_browser_release_gate` artifact. The target combines
  the proprietary-AVC exact-frame gate with the exact-APK, direct-LAN phone-hosted browser HIL and
  requires Chrome to visibly decode both freshly published phone MP4s. Two consecutive temporary
  relay passes validate the final decode/cancellation schema and media-worker cleanup, but the
  direct-LAN attempt stopped before Playwright when Pixel 6-to-host Wi-Fi remained unreachable, so
  the current contract still needs a reviewed direct-LAN invocation;
- Firefox and WebKit remain passing compatibility candidates through
  `//web:browser_h264_candidate_matrix`, not prototype release blockers; and
- repeat the shortest relevant paired-capture HIL after materially changing the APK, phone OS, or
  network topology. The retained reports prove the cited revisions, not arbitrary future installs.

AddressSanitizer and UndefinedBehaviorSanitizer variants run with:

```bash
bazel test --config=asan //...
bazel test --config=ubsan //...
```

## Explicit hardware tests

Physical-device tests are Bazel `cc_test` targets tagged `manual`, `local`,
and `exclusive`. They are intentionally excluded from wildcard test runs:

```bash
bazel test //capture/daheng:dual_camera_smoke_hil_test \
  --test_output=streamed --nocache_test_results
bazel test //capture/daheng:dual_camera_qualify_hil_test \
  --test_output=streamed --nocache_test_results
bazel test //capture/daheng:dual_camera_soak_hil_test \
  --test_output=streamed --nocache_test_results
```

The shortest direct production-encoder check opens only the render node, not
the cameras, microphone, or Feather:

```bash
bazel test //capture/encoding:vaapi_vp9_encoder_hil_test \
  --test_output=streamed --nocache_test_results
```

It requires the Intel iHD driver and `render`-group access, encodes a small
synthetic Bayer clip through direct VA-API, and parses the result to prove VP9,
geometry, timestamps, frame count, and an all-keyframe stream.

The camera tests require `SWING_CAPTURE_STATION_CONFIG` to name a valid local
station file whose down-the-line and face-on roles have been physically
verified. The local `.bazelrc.local` supplies that environment variable. Each
camera object in `report.json` records both `role` and `serial`; an absent,
invalid, unverified, missing, or duplicate assignment fails before capture.

The stages run for 15 seconds, 5 minutes, and 30 minutes. They require:

- the persistent 2000 MB `usbfs_memory_mb` configuration installed by
  `sudo ./tools/setup_daheng_host.sh`;
- exactly two selected cameras with read/write access;
- reported USB root-controller topology; distinct controllers are preferred,
  while a shared controller must prove capacity in the full-rate test itself;
- verified 1440x1080 `BayerRG8` free-run configuration near 227 fps;
- exposure and gain automation disabled, fixed 500 us exposure, and fixed
  24 dB gain with exact read-back;
- no incomplete frames, capture timeouts, frame-ID gaps, or timestamp resets;
- a host receive interval no longer than 10 nominal frame periods, allowing
  bounded Linux scheduling delay without calling it camera loss;
- a stricter device timestamp interval no longer than 4 nominal frame periods;
- exact payload accounting;
- a full two-second frozen snapshot per camera while capture continues from
  reserve blocks.

The smoke test writes the following repository-relative Bazel undeclared
outputs (qualification and soak substitute their target names):

```text
bazel-testlogs/capture/daheng/dual_camera_smoke_hil_test/test.outputs/report.json
bazel-testlogs/capture/daheng/dual_camera_smoke_hil_test/test.outputs/frame-FDN22120654.png
bazel-testlogs/capture/daheng/dual_camera_smoke_hil_test/test.outputs/frame-FDN23010199.png
```

The report's `diagnostic_frame_path` values are just the PNG filenames,
relative to the report. Along with the demosaiced PNGs, the report contains
raw Bayer image-quality metrics and a classification. The classification is
advisory and deliberately independent of the transport result.

The 2026-07-26 physical smoke classified both connected views
`underexposed_or_obscured`; one camera was nearly black. This is a current
image-readiness failure even though the frame transport checks passed. Before
real swing acceptance, inspect the PNGs and correct obstruction, framing,
lighting, lens/iris state, or exposure as appropriate.

That retained artifact is historical rather than the current physical view.
The 2026-08-09 lit-room station-fixture run retained a new baseline with the
same `tag36h11` ID 0 decoded in both cameras, a localized red response to the
commanded Feather pulse visible in both, and the Feather speaker audible beside
the USB microphone. The selected optical regions appear to be reflections on
the nearby surface rather than direct localization of the LED package. This is
useful optical/audio HIL evidence, but it is not pose or geometric-calibration
evidence.

The 2026-07-26 five-minute qualification passed and exited cleanly after
capturing 68,061 complete frames per camera at 226.872 fps. Both cameras
reported zero timeouts, incomplete frames, and frame-ID gaps. The maximum
device-frame interval was 4.408 ms, and the maximum host-delivery intervals
were 16.339 ms and 15.462 ms. This is the regression baseline for the SDK
lifetime, dual-controller transport, and freeze/continue retention path.

The current MS-01 wiring places both 5 Gb/s camera links on the same PCH xHCI
root controller. A later 15-second dual-camera run with the topology gate
disabled sustained 226.87 fps per camera with no incomplete frames, timeouts,
or frame-ID gaps. Static topology is therefore reported as an operational
warning rather than a hard failure; qualification remains the capacity gate.

### Audio HIL

The physical microphone test is also a Bazel `cc_test`, tagged `manual`,
`local`, and `exclusive`:

```bash
bazel test //capture/audio:audio_hil_test \
  --test_output=streamed --nocache_test_results
```

It captures at least three seconds from the station file's stable
`hw:CARD=<id>,DEV=<number>` selection at 32 kHz signed 16-bit PCM using the
configured hardware channel count and mono channel selection. It reports
sample/block counts, peak and RMS amplitude,
clipping, detected impacts, and timestamp-model limitations at:

```text
bazel-testlogs/capture/audio/audio_hil_test/test.outputs/audio_hil_summary.json
```

The target has explicit pass/fail checks for source completion, sample count,
an elapsed-to-captured-duration ratio from 0.75 through 1.75, RMS amplitude of
at least 0.0001 and peak amplitude of at least 0.001, and no more than 0.1%
clipped samples. The 2026-07-26 live baseline passed all checks with 96,256
samples, a cadence ratio of 1.122, 0.00448 normalized RMS, 0.0675 peak, and no
clipping.

The current `arecord` backend estimates the first block time from host read
completion, then advances by sample count. The report therefore cannot yet
bound ALSA buffering or device latency.

### Combined station-fixture HIL

The shortest combined fixture check is an explicit manual target:

```bash
bazel test //capture/hil:station_fixture_hil_test \
  --test_output=streamed --nocache_test_results
```

It takes the same exclusive hardware lock as the setup-preview service and the
unattended runner. If the preview service owns the cameras, the fixture test
fails at the lock rather than opening a second camera job. The Feather must
already be running the separately flashed `SC-HIL/1` image; building or testing
the host target never flashes it.

The run starts both full-rate camera streams and microphone capture before
issuing any stimulus. It first requests a 300 ms onboard LED locator pulse.
Eight guaranteed-OFF frames and at least 24 frames from the guarded stable
interior of that pulse identify one response ROI independently in each camera.
The run then leaves a 200 ms OFF interval and requests a 44,053 us qualification
pulse (ten nominal periods at 227 fps), with analysis locked to the locator ROI.
Its response is evaluated over a command-relative nine-through-eleven-frame
matched window. At least nine consecutive frames must meet the support gates of
mean red excess 2.0 and changed-red fraction 0.10. At least three supported
frames must also meet all high-confidence gates, including mean red excess 4.0,
and the complete window must retain aggregate mean red excess 3.0 and
changed-red fraction 0.20. This prevents three isolated bright frames from
carrying a result. A free-running exposure can straddle either edge, so partial
edge frames may fall below support. The span and device-local duration remain
acceptance gates. Equality between the two cameras is not treated as
synchronization evidence. The same high-quality AprilTag identity must also be
visible in both views.

Each frame is corrected with a fitted global affine illumination model, and a
candidate region must exceed its surrounding background ring. This rejects
additive and multiplicative room-light flicker while retaining a dim localized
pulse. AprilTag detection runs on deterministic luminance converted from the
demosaiced Bayer frame; treating the color Bayer mosaic itself as grayscale is
not valid for this check.

The detected LED edges are compared with the Feather acknowledgement schedule
using robust camera-device-to-host-receipt mapping. That comparison records one
nominal frame of fixed delivery latency as an operational assumption and is
diagnostic, not an acceptance gate: fixed sensor/readout/USB/SDK latency is not
observable from receipt timestamps alone. The accepted claim is that the long
locator and short qualification produce a consistent localized optical
response in the command-relative windows, together with separately verified
Feather command and device durations. The ROI may be a reflection of the
emitter rather than the LED package itself. This does not prove absolute
camera-to-Feather timing or synchronization between the two cameras.

It then requests a conservative 20 ms, 2 kHz speaker tone at the shared
125-permille HIL level and verifies energy, signal-to-noise ratio, 2 kHz
spectral concentration, frequency error, active
duration, and clipping in a bounded command-relative window. Synthetic tests
reject an ambient impulse, amplifier power-on pop, wrong-frequency tone, and
short or overlong stimulus. This proves the expected tone was audible but is
not calibrated acoustic latency. The microphone sample position is
block-granular, and the reported delay also contains USB serial, firmware
scheduling, ALSA/pipe buffering, amplifier, speaker, and acoustic delay.

The undeclared output directory retains `station-fixture-report.json`, the
captured `speaker-microphone.wav`, and clean pre-flash AprilTag, locator-peak,
and qualification-peak PNGs for both camera roles. Cyan ROI overlays and
averaged stable-ON-minus-OFF red-difference PNGs make both accepted and rejected
optical evidence inspectable. The camera rings preallocate 512 full frames each
so bounded serial delays and sequential shutdown cannot overwrite the short
run; the exact allocation is recorded. Reports are written incrementally, and
failure paths retain completed Feather records, audio diagnostics/WAV, and the
latest available clean camera images. Presence of the AprilTag is a
framing/focus/exposure sanity check only; it makes no pose or
geometric-calibration claim.

The 2026-08-09 room-lit run passed in 6.0 seconds of test time. Down-the-line
captured 418 frames and face-on 414, both at about 227.4 fps with zero timeouts
and frame-ID gaps. Down-the-line had 10 consecutive supported and 10
high-confidence frames; face-on had 10 consecutive supported and 7
high-confidence frames. Both qualification windows covered 10 frame positions,
or 44.078 ms in each device-clock domain. Both decoded `tag36h11` ID 0 with
hamming 0 and decision margins 81.6/84.9. The audio check measured exactly 2
kHz, 17.7 dB SNR, and no clipping. The complete artifact is preserved under
`artifacts/hil/station_fixture/20260809T085952-supported-frame-final-pass/`.

### Synthetic-swing application-flow HIL

The shortest physical application check is a separate explicit target:

```bash
bazel test //capture/hil:application_flow_hil_test \
  --test_output=streamed --nocache_test_results
```

It is tagged `manual`, `local`, and `exclusive`, uses the shared hardware lock,
and has an internal 15-second workflow deadline. Stop the hosted preview service
before running it. The test starts the production station backend with HIL
controls explicitly enabled on an ephemeral loopback port, verifies both
configured camera roles, and starts the one-at-a-time operation through
`POST /api/v1/hil/synthetic-swing` with an empty JSON object. The ordinary
application and UI keep this endpoint disabled unless the server is started
with `--enable-hil-controls`.

The operation first establishes an OFF baseline, raises the shared GPIO23
fixture rail, and presents the white candidates
`1,2,3,4,6,8,12,16` for 70 ms each on the external screw-terminal
NeoPixel driven by GPIO21. Preview samples
from both roles must locate the response and produce one common level that is
visible without clipping in either camera. Once the station reports armed, the
Feather runs 60 scaled RGB states at 20 ms each (1.2 seconds), a 20 ms white
impact marker with a simultaneous 10 ms 2 kHz speaker tone, and 25 more 20 ms
states (0.5 seconds). The speaker tone uses the fixed 125-permille
synthetic-swing level. The existing adaptive microphone detector—not the
manual capture endpoint—must accept exactly one tone trigger and drive
post-roll, dual-view encoding, and atomic publication.

Capture is one-shot: publication transitions to `ready` with capture disarmed
and ALSA stopped, while retaining the source-ready flag, counters, adaptive
audio evidence, and accepted trigger for inspection. The HIL validates that
state, downloads the session, then explicitly posts `armed:false` and requires
the application to normalize from `ready` back to `setup`. A following normal
swing requires an explicit re-arm; another synthetic request performs its own
one-shot arm.

The HIL downloads the manifest and both production all-keyframe VP9/WebM assets back through
the HTTP API, checks browser byte-range behavior, parses the WebM structure,
and independently validates role/serial identity, frame continuity, device and
impact-relative timestamps, impact-frame selection, frame rate, dimensions,
frame/keyframe counts, and file sizes. Synthetic `hil_evidence` additionally
must contain the selected brightness and exact sequence constants. For each
camera role it qualifies the white-impact observation, requires its encoded
frame index, and checks the signed offset from that optical frame to the
audio-trigger estimate. The surrounding stepped colors are retained as a human
playback and frame-stepping cue, not checked as exact programmatic states. The
validator also requires the signed mapped-time correction and
residual uncertainty learned from the calibration sweep, bounds the provisional
audio offset, and checks that the two camera offsets agree. It fetches a
full-resolution PNG from each
still-running camera before disarming. Bazel undeclared outputs retain the
incrementally written `application-flow-report.json`, the production
`station-sessions/` directory, the independently fetched `http-session/`
directory, and `diagnostic-down_the_line.png` and
`diagnostic-face_on.png`.

The 2026-08-09 impact-only run passed at the production camera profile of
500 us and 24 dB. The shared selector chose brightness 1. Down-the-line had
four stable, matching white frames with 5.08% maximum saturation and 1.71%
maximum bloom; face-on had three with 3.91% saturation and 1.79% bloom. Both
matching fractions were 1.0. The clips retained 433 and 434 contiguous frames
at approximately 226.87 fps, and the complete operation took 14.75 seconds.
Pinned Chromium subsequently passed against the downloaded physical session.
Evidence is preserved under
`artifacts/hil/application-flow/20260809T182443Z-impact-only-pass/`.

A following profiling run retained 433 contiguous frames from each camera at
approximately 226.87 fps and passed the same physical application HIL. Audio
confirmation to the pre-manifest profile snapshot was 9.517 seconds. Its major
stages were 502.4 ms of required post-roll, 226.7 ms stopping ALSA, 7.6 ms of
prepublication optical analysis, and 8.778 seconds of sequential dual-view
media encoding. Down-the-line encoding took 4.371 seconds and face-on took
4.406 seconds. Across both views, VP8 consumed 5.159 seconds, fitted Bayer
demosaic 3.001 seconds, and RGB-to-I420 conversion 564 ms; muxing,
finalization, and verification were comparatively negligible.

Pinned Chromium then decoded the two physical WebMs and presented both impact
frames 127.9 ms after receiving the manifest response, or 140.2 ms after its
manifest request began. The artifact-only replay cannot reconstruct the live
server response timestamp, so it deliberately leaves the combined
audio-confirmation-to-browser bound unavailable; the deployed server supplies
that timestamp on live responses. The report, physical media and images,
browser screenshot, and structured browser timing are preserved under
`artifacts/hil/application-flow/20260809T200712Z-pipeline-profile-pass/`.

The following production revision replaced sequential software VP8 with two
concurrent direct VA-API VP9 encodes. A fresh physical HIL retained 433
contiguous frames per camera at approximately 226.87 fps and passed the media,
range, optical, audio, and session checks. Hardware media encoding took 3.005
seconds total, down from 8.778 seconds for the preceding software baseline.
The profile snapshot was 3.735 seconds after audio confirmation: 512.2 ms was
the required post-roll wait, 216.3 ms was ALSA shutdown, 8.8 ms was optical
analysis, and image conversion plus media publication dominated the remainder.
For the two concurrent views, fitted Bayer demosaic took 1.528/1.535 seconds,
RGB-to-NV12 conversion 314/309 ms, and VA-API codec submission/readback
1.099/1.117 seconds. The complete physical HIL took 10.47 seconds.

Pinned Chromium replayed the exact hardware-produced VP9 files and passed
decode, seeking, exact stepping, synchronized playback, and evidence rendering.
The report, media, full camera images, browser screenshot, and structured
profiles are preserved under
`artifacts/hil/application-flow/20260809T211742Z-vaapi-vp9-pass/`.

The low-latency revision retained full 1440x1080 output, overlapped bounded CPU
frame preparation with queued VA-API submissions, requested ALSA shutdown as
soon as the trigger was accepted, added exact early-impact JPEGs, and replaced
one-second session polling with server-sent events. A fresh physical run passed
with 433 contiguous frames per view at approximately 226.87 fps. The early
JPEG pair was server-ready 538.2 ms after audio confirmation, of which roughly
510.9 ms was required post-roll. Audio join consumed 0.004 ms. Concurrent
full-resolution encoding took 2.901 seconds, and the pre-manifest snapshot was
3.440 seconds after confirmation. Pinned Chromium loaded, decoded, sought, and
presented both physical impact frames 398.3 ms after starting its manifest
request. Evidence is preserved under
`artifacts/hil/application-flow/20260809T230608Z-fullres-low-latency-pass/`.

After a physical run, the pinned-Chromium bridge can replay the exact preserved
manifest and WebMs without touching hardware:

```bash
bazel test //web:browser_test \
  --test_output=streamed --nocache_test_results \
  --test_env=SWING_CAPTURE_REQUIRE_HIL_ARTIFACT=1 \
  --test_env=SWING_CAPTURE_HIL_SESSION_DIR=/absolute/path/to/http-session
```

This is an artifact-based browser test: it serves the production bundle and a
read-only facade over the supplied directory. It does not launch or validate a
live production station server. The browser must decode both clips, seek and
step exactly, and retain `hil-review-session.png` at a fixed viewport.

The per-camera audio offset remains provisional: the current `arecord` model
contains ALSA, pipe, scheduling, amplifier, speaker, and acoustic latency, and
the cameras add uncalibrated exposure/readout/transport delay. It measures the
relationship between the two observed markers but is not calibrated true
club/ball impact timing.

### Android dual-phone HIL

The Android replacement has separate explicit manual targets for transport,
production peer coordination, discovery/pairing, restart recovery, and network
disturbance. All are tagged `manual`, `local`, and `exclusive`; no wildcard
test installs an APK or opens a phone camera. The default software suite
instead exercises their report validators with synthetic success and fault
fixtures. After changing HIL-only code, compile the complete manual surface
with `--config=manual_test_static_check` before selecting a physical target.

The shortest complete production-path check is:

```bash
bazel test //android/dual_hil:dual_phone_paired_pose_arm_lan_hil_test \
  --test_output=streamed --nocache_test_results
```

For repeated development against an unchanged installed APK, select the
fail-closed exact-match lane:

```bash
bazel test --config=android_hil_inner \
  //android/dual_hil:dual_phone_paired_pose_arm_lan_hil_test \
  --test_output=streamed --nocache_test_results
```

It skips installation only when the Bazel APK and the phone's sole installed
`base.apk` have identical SHA-256 values. A missing, unreadable, split, or
different install is replaced and the resulting identity is verified. The
plain target always reinstalls and remains the clean checkpoint.

The clean target installs the same APK on the Pixel 6-family face-on leader and Pixel 5a
down-the-line shadow, proves independent Bearer enforcement over each phone's
LAN address, establishes an authenticated peer binding, and maps the pair's
monotonic clocks. Both phones then enter five-Hz pose monitoring. The gate
requires the GPU delegate, at least two successful post-warm-up inferences per
phone, inference p95 no greater than 200 ms, no steady-state inference above
400 ms, and internally consistent latest-frame scheduling/drop counters. Cold
model warm-up is reported separately and is not misclassified as recurring
five-Hz inference latency.

While pose monitoring owns Camera2, the HIL fetches each authenticated setup
preview and checks a fresh bounded JPEG, portrait rotation metadata,
`Cache-Control: no-store`, `nosniff`, and rejection without the current
credential. After transition to 720p240 ownership it requires the same route
and setup metadata to report explicit `high_speed_capture` unavailability,
rather than serving a stale preview or a black placeholder.

The Feather then replays a checked-in case cut from the retained field PCM at
the same instant as its white optical marker. The leader's production audio
detector must publish one coordinated session; the shadow retains its local
candidate as diagnostic/fallback evidence. Both 720p240 AVC clips are pulled,
decoded, and checked for continuity, frame rate, timing bounds, session/role
identity, AprilTag visibility, and optical marker evidence. `report.json`, both
setup-preview JPEGs, per-node status/clock/trigger evidence, clips, and all
diagnostic PNGs must be inspected; a passing process exit alone is
insufficient.

The post-stimulus observer reports `neither`, `leader_only`, `shadow_only`, or
`both` and retains each role's production trigger source. `both` completes
immediately, while `neither` remains a 750 ms fast failure. A partial pair is
allowed a bounded 10.1-second quiescence interval covering all three peer
delivery attempts, their separate connect/read timeouts, retry gaps, and a
scheduling margin before any missed-shot salvage is permitted. This prevents
the diagnostic salvage request from making the peer capture-active while the
leader's asynchronous impact request is still in flight. Required-positive
scoring passes only for `both`; a trigger on either role makes a diagnostic
negative fail.

The discovery target begins from the phones' actual durable configuration,
then creates an explicit unpaired fixture. It covers authenticated NSD identity
binding, wrong-token rejection, credential rotation invalidating the old
token, stale-peer detection, reset/re-pair, secret redaction, and exact
configuration restoration. This prevents `adb install -r` preserving a prior
valid pair from accidentally turning the test into a nominal-only exercise.
The autonomous restart and disturbance targets separately cover durable local
evidence, replication backlog, app-process restart, Wi-Fi interruption, peer
restart, recovery, and re-arm. Each physical stage has its own 15-second
deadline; the five-minute qualification and 30-minute soak remain explicit
operator-only runs.

Every configuration-mutating dual-phone target publishes `cleanup.json` before
mutation, updates it after each restoration attempt, and merges cleanup into
the final report. A primary pass is converted to failure if cleanup is
incomplete, while a primary failure remains the reported root cause. Recovery
metadata must never contain the private preferences or Bearer credentials.

### Android pair preflight

`//tools:android_pair_doctor_current_apk` is the normal read-only field-session gate. It builds the
current APK into its runfiles and requires both phones' sole installed `base.apk` files to match it
byte-for-byte. It does not install, start, stop, arm, pair, or otherwise change either phone. It
checks authorized ADB access, the installed
debuggable application, direct Wi-Fi API reachability and bearer enforcement, complementary roles,
the one-leader/one-shadow topology, the leader's authenticated binding to the current shadow
identity/address/credential generation, setup readiness, 720p240 selection, and conservative
battery, thermal, and free-storage thresholds. Credential material is never included in terminal or
JSON output. It records each phone's current BSSID and a directed matrix containing host-to-phone
public-clock HTTP plus both phone-to-peer Wi-Fi ICMP probes. Every edge is required: an authorized
ADB connection, a working USB port forward, or one-sided neighbor/ARP evidence cannot qualify a
direct-LAN origin. It also fetches the phone-hosted review document, JavaScript, and stylesheet directly
from each LAN origin, rejects redirects or missing bundle markers, verifies their content/cache and
`nosniff` policies, and exercises the browser CORS preflight needed for Authorization, media Range,
and mutating cross-origin requests. A healthy JSON API alone therefore cannot admit a phone whose
review UI is missing or unusable from its peer's origin.

Run it before moving the phones into the hitting area, and again over wireless ADB after moving
them. The stricter second invocation also proves both stations are already monitoring, each pose
path has produced inference decisions with p95 no greater than 200 ms, no maximum above 400 ms,
and no failed or rejected-timestamp decisions, the leader can currently reach the bound shadow,
and its peer-clock snapshot is no older than ten seconds and no wider than the current 25 ms
policy. It also requires a healthy standby audio trigger input with retained frames on both
phones. An `armed=true` flag alone is deliberately insufficient:

```bash
bazel run //tools:android_pair_doctor_current_apk -- \
  --node 22181FDF6005QH=http://10.168.168.111:8088 \
  --node 1A011JEG501717=http://10.168.168.241:8088 \
  --json artifacts/android_pair_preflight_usb.json

bazel run //tools:android_pair_doctor_current_apk -- \
  --node 10.168.168.111:5555=http://10.168.168.111:8088 \
  --node 10.168.168.241:5555=http://10.168.168.241:8088 \
  --require-wireless-adb --require-monitoring \
  --json artifacts/android_pair_preflight_field.json
```

The exact wireless ADB ports can differ after Android restarts. Use the serials printed by
`bazel run //tools/android:adb -- devices -l`; the doctor deliberately does not pair or reconnect a
transport. Evidence collection overlaps the independent phones, retains every per-node collection
failure in one redacted report, and preserves its BSSID inventory and directed edges under
`lan_diagnostics`. It uses a bounded five-second network deadline so first-launch
capability work on the older phone does not create a three-second false timeout. Its parser and
policy are hermetically covered by `//tools:android_pair_doctor_test`.

For the normal field ceremony, use the bounded wrapper instead of transcribing temporary ADB ports.
It performs one `adb mdns services` discovery for the two DHCP-reserved hosts, fails on missing or
ambiguous connect advertisements, reconnects and checks both phones concurrently, optionally
launches the foreground activity and sleeps the screens, and then invokes the exact-APK doctor:

```bash
bazel run //tools/field_preflight:android_field_preflight -- \
  --node 10.168.168.111=http://10.168.168.111:8088 \
  --node 10.168.168.241=http://10.168.168.241:8088 \
  --expected-role 10.168.168.111=face_on \
  --expected-role 10.168.168.241=down_the_line \
  --launch-after-unlock --sleep-screen-after-launch \
  --evidence-dir artifacts/field_preflight_before_arm

bazel run //tools/field_preflight:android_field_preflight -- \
  --node 10.168.168.111=http://10.168.168.111:8088 \
  --node 10.168.168.241=http://10.168.168.241:8088 \
  --expected-role 10.168.168.111=face_on \
  --expected-role 10.168.168.241=down_the_line \
  --require-monitoring \
  --evidence-dir artifacts/field_preflight_armed
```

The wrapper also requires the expected role for each stable phone host and rejects swapped camera
assignments even when the pair has complementary roles. It never pairs, unlocks, reboots,
configures, or arms a phone and refuses to overwrite evidence. The first-unlock and on-device
production launch steps remain human actions; an ADB foreground launch is only a development
convenience. See `tools/field_preflight/README.md`.

The unqualified `//tools:android_pair_doctor` target remains available for diagnosing an
intentionally stale installation. It reports each installed SHA-256 and the missing current
contracts instead of aborting its parser, but it cannot admit a field session because it has no
expected APK artifact. Use `//tools:android_pair_doctor_current_apk` for admission.

Collect the larger per-phone capability inventories after starting the current application and
before proposing a new device:

```bash
bazel run //tools:android_capability_report -- \
  --serial 22181FDF6005QH --serial 1A011JEG501717 \
  --output-dir artifacts/android_product_floor_current
```

The collector is read-only, runs the two ADB pulls concurrently, refuses to replace an existing
evidence directory, redacts credentials, rejects stale reports, host-hashes each installed
monolithic `base.apk` against the Bazel-supplied current APK, and verifies the complete camera,
encoder, audio, API/OpenGL, pose-isolation, and screen-off product floor. The capability portion of
`artifacts/android_product_floor_exact_apk_pass_20260823T072145Z/report.json` passes on Pixel 6/API
36 and Pixel 5a/API 34. Its expected Bazel APK and both installed `base.apk` files share SHA-256
`328203380f02db24ec7d7707476cab361d3164476d5c9187549897aa64cad39a`. This is exact-APK static
admission, not motion, thermal, or long-duration qualification.

Before deploying another software revision, the fast hermetic checkpoint for the integration
contracts that previously escaped into the hitting area is:

```bash
bazel test //tools:pre_field_integration_tests
```

It currently lists 118 targets covering credential bootstrap/rotation, scoped native-media authorization,
stale ownership and partial mutation, the real Java node HTTP protocol, atomic field-recording publication, hosted UI/CORS and setup
preview, peer readiness/auth/clock/publication/recovery ordering, read-only APK identity, reboot
evidence including bounded post-reboot peer-clock validation, exact-APK-bound product-floor parsing,
the production field-preflight executable and host-to-camera-role association,
`field_readiness_v2` acquisition/readiness, exact production audio
prediction plus fixed-memory candidate replay, immutable policy locks, same-holdout comparison,
and the audio holdout CLI, trigger and pose lifecycle replay, diagnostic archive/store/publication
and collector crash recovery, retained-media and HIL report validation, timing/pose-qualification
evidence, cleanup, full and isolated TypeScript checks, and production-shaped browser coverage.
The browser harness additionally covers post-202 stop/publication failure and stale bookmark roles
after the phones are physically swapped. It does not contact the phones and therefore complements
rather than replaces the current-APK doctor and the shortest relevant physical gate. The current
120-target aggregate passed 120/120 uncached in 21.58 seconds on 2026-08-23, from 08:06:54 through
08:07:15; `//web:browser_test` was the 19.78-second critical path. This is a current software
checkpoint, not physical phone, camera, microphone, or network evidence.

The prior 2026-08-23 combined prototype-browser checkpoint completed its physical camera stage in
5.546 seconds against the direct-LAN Pixel 5a/Pixel 6 origins, observed both 202 starts/stops and
both published bundles, fetched bounded video and audio ranges from each phone with four HTTP 206
responses, and returned both nodes to `ready`. Its launcher verified the same Bazel APK SHA-256
`8454c21a0b2582916e6e25ed06affc207f0fcdf4be7e49159fa24264485b5346` on both phones, retained no
credentials, and restored screen-sleep state; the companion system-Chrome gate passed the fixture
contract. That phone report has no `fresh_media_decode` evidence and is no longer eligible under
the current validator. The current physical half requires installed Google Chrome to decode both
newly published MP4s from their direct phone origins, validate their complementary roles and shared
recording identity, present a nonblack frame, and advance playback by at least 0.1 seconds.
`artifacts/android_field_recording_browser_hil_relay_range_pass1_20260823T071927Z` and
`artifacts/android_field_recording_browser_hil_relay_range_pass2_20260823T072013Z` are consecutive
exact-APK passes under the final schema. Each decodes both fresh 720x1280 MP4s, advances them by
more than 0.2 seconds, records Chrome's allowlisted decoder-release `net::ERR_ABORTED`, retains no
unexpected request/page/accessibility failure, and restores both nodes and screen-sleep state. The
second clean pass after the first cancellation validates media-worker cleanup. These runs are
explicitly non-qualifying because the Pixel 6 face-on origin used a temporary relay.

The sequential direct-LAN retry is retained at
`artifacts/android_browser_hil_direct_lan_preflight_failure_20260823T072340Z`. The Pixel 5a DTL
origin became HTTP-ready in 951 ms, while the Pixel 6 face-on origin returned five 500 ms transport
timeouts through 2.901 seconds. The launcher now prepares both phones concurrently: each per-phone
task performs the exact-APK check or install, starts the application, reads its token, and probes
the authenticated `/api/v1/node` endpoint. The report records the shared wall time as
`phone_preparation_elapsed_ms`.

The parallel retry at
`artifacts/android_browser_hil_direct_lan_parallel_preflight_failure_20260823T072703Z` records
`phone_preparation_elapsed_ms: 3353`; DTL became ready in 946 ms, while the Pixel 6 repeated five
500 ms transport timeouts through 2.902 seconds. The whole Bazel target measured 4.72 seconds,
compared with 5.67 seconds for the preceding sequential failure. Both retries failed before
Playwright and therefore produced no browser or fresh-decode evidence. Both installed APKs still
matched SHA-256 `328203380f02db24ec7d7707476cab361d3164476d5c9187549897aa64cad39a`, and both screen-sleep
cleanup actions passed. The shorter failed preflight does not close the release gate: restore Pixel
6-to-host Wi-Fi reachability, then run and review `//web:prototype_browser_release_gate` for the
current and every changed release candidate.

The current-revision physical checkpoint is retained at
`artifacts/android_pcm_current_apk_paired_pass_20260823T002901`: both roles
triggered automatically in 621 ms, both 720p240 clips decoded to their exact
manifested frame counts, all six inspected AprilTag/LED diagnostic frames were
nominal, and all cleanup obligations were restored.

The current-APK independent one-second field-recording transport checkpoint is retained at
`artifacts/android_field_recording_direct_lan_current_apk_20260823T015651/report.json`. It ran
directly against both phones' Wi-Fi LAN origins without ADB forwarding. The Bazel artifact and both
installed monolithic APKs matched SHA-256
`13c6c3e3012b86c13042d718803213e41f487123cb8c025a54e75707facef162`, so installation was skipped.
Both origins returned 204 to the browser CORS preflight with `Access-Control-Allow-Origin: *` and
returned 401 for unauthenticated control. The target assigned one shared recording ID, started both
recorders, stopped and atomically published both bundles, cross-checked listing and manifest
identity/lengths, and fetched bounded MP4 and WAV ranges with valid `ftyp` and `RIFF` signatures.
The complete target and cleanup took 12.657 seconds and restored both recorders and both screen-sleep
states. It does not claim full media decode, swing content, or optical alignment.

The synchronous partial-start target passed the same current APK at
`artifacts/android_field_recording_partial_start_hil_pass_20260823T015457/report.json`. In 9.022
seconds it proved DTL 202/face-on 409, one-node rollback and publication, a fresh shared-ID retry on
both nodes, and all seven recorder, injected-fault, ADB-forward, and screen-state cleanup actions.
The preserved pre-fix report at
`artifacts/android_field_recording_partial_start_hil_failed_20260823T015026/report.json` found that
Camera2's expected `REASON_FLUSHED` callback during explicit stop was being treated as a recorder
failure. The correction ignores only a flushed request after explicit stop; an unexpected flush or
any other capture failure remains fatal.

The separate post-202 target passed at
`artifacts/android_field_recording_async_start_failure_hil_pass_20260823T091443/report.json`.
Both starts returned 202 before the face-on one-shot gate was released; the recorder then exposed
the exact injected asynchronous terminal error, both nodes rolled back, and a fresh-ID retry
published on both. The 11.898-second Bazel target restored all nine cleanup obligations, both
screens to Dozing, and left no ADB forwards.

The direct-LAN `//web:android_field_recording_browser_hil_test` loads the actual production app from
the Pixel 5a, addresses both phone APIs without a fake or ADB forwarding, injects credentials from
`adb run-as` only into the child browser environment, asserts the credential-free URL/UI/axe
contract, records and stops both phones, opens the completed library, and Range-fetches both MP4
and WAV outputs. Current phones expose compact role/identity/coordination summaries in the session
list, so the catalog does not fetch historical manifests or coordination records; only a selected
shot is fully hydrated. Legacy nodes retain a bounded two-manifest fallback that cannot starve
pairing or control. The deterministic tests cover both paths.

The retained current-APK hosted-UI, mutation, publication, Range, and cleanup pass is
`artifacts/android_field_recording_browser_hil_pass_20260823T104442Z/report.json`: the page was ready
in 2.784 seconds, start converged in 4.956 seconds, stop/publication converged in 1.936 seconds, and
the camera stage totaled 7.657 seconds. Both origins accepted start and stop, published the new
shared recording, and served MP4/WAV ranges with 206; accessibility, page, request, terminal-ready,
and cleanup checks passed. The paired launcher report proves one common byte-exact APK SHA-256
`785602e479836330ebdf62aca1d9fafd88206346f334e1aa960e4ba7b18b4733`, a non-timeout Playwright
exit, and both role-specific screen-sleep restorations. The retained screenshot was visually
inspected and shows the real phone-hosted review page with both recorders ready and the completed
bundle listed. That older artifact predates mandatory fresh-phone-MP4 decode evidence and must not
be cited as a pass of the current browser release gate.

The earlier `artifacts/android_pair_preflight_web_20260822.json` run correctly rejected a stale
two-shadow configuration. The current exact-APK field preflight at
`artifacts/field_preflight_current_apk_pass_20260823/report.json` passes every stopped-state check on
the restored Pixel 6 face-on leader and bound Pixel 5a down-the-line shadow: both installed hashes
equal the Bazel artifact, the current boot is ready, the complete product floor is admitted, and
each phone directly serves the current 451-byte document, 364,500-byte script, 31,364-byte
stylesheet, and CORS policy. The production wrapper ran over stable wireless ADB and proves the
explicit stable-host assignments in addition to retaining the nested strict-doctor report. Its
immediately preceding run correctly failed because both phones carried the same older APK; after a
bounded exact-identity update, the next run passed on its first doctor attempt. The CLI now surfaces
credential-redacted child-check failures such as that APK mismatch instead of printing only a
generic admission failure. Arm both and rerun with `--require-monitoring` before the next hitting
session; that requirement is not inferred from a stopped, cool-device check and is proven
separately by the subsequent strict checkpoint.

The post-catalog-change stopped-state run at
`artifacts/field_preflight_lazy_catalog_pass_20260823/report.json` independently passed on its first
doctor attempt with the exact APK hash used by the green browser HIL. The subsequent strict
monitoring ceremony passed on its first attempt at
`artifacts/field_preflight_monitoring_current_apk_pass_20260823/report.json`: both nodes were armed,
pose p95 was 199 ms on Pixel 6 and 179 ms on Pixel 5a with no failed or rejected-timestamp
decisions, both standby-audio paths retained frames, the authenticated peer was reachable, and the
fresh peer-clock uncertainty was 13.502 ms. The phones were then disarmed and returned to Dozing.
The doctor samples live status before APK hashing and authenticated setup collection so its own
diagnostic workload cannot inflate this short observation; a deterministic operation-order test
guards that measurement boundary.

### Station doctor

`//tools:station_doctor` is a read-only host/configuration workflow. It
inventories devices through sysfs, procfs, and stable `/dev` links, compares
the installed udev and systemd files byte-for-byte with the repository, checks
the live USB buffer/service state and effective access, and can retain a JSON
report:

```bash
bazel run //tools:station_doctor -- --json artifacts/station/doctor.json
```

Its deterministic parser, inventory, and readiness policy live in
`//tools:station_doctor_test`; the C++ consumers independently exercise the
same strict schema in `//station:station_config_test`.

### Optional unattended runner

`//tools:run_unattended_hil` is an operations runner, not a test
implementation:

```bash
bazel run //tools:run_unattended_hil -- smoke
bazel run //tools:run_unattended_hil -- qualify
bazel run //tools:run_unattended_hil -- soak
```

It invokes the corresponding Bazel `cc_test` and adds a host lock,
duration-plus-grace watchdog, heartbeat/status JSON, durable logs, atomic
result publication, and automatic single-camera diagnostics after a
dual-camera failure. By default the lock is
`artifacts/hil/hardware.lock`; `SWING_CAPTURE_HARDWARE_LOCK` selects the same
provisioned absolute lock path used by an installed preview service.

## HIL ladder

1. Pure unit tests with synthetic clocks, audio, frames, and faults.
2. Process/backend integration against deterministic fake producers.
3. Roughly fifteen-second-or-shorter physical fixture or transport check.
4. Five-minute qualification only when the operator explicitly requests it.
5. Thirty-minute soak only when the operator explicitly requests milestone
   acceptance.
6. Shared-flash optical timing test after electrical frame triggering exists.
7. Recorded real-swing replay through trigger, clip, encoding, and UI.

The replay fixture is important for autonomous iteration: one carefully
captured real session can exercise most of the application repeatedly without
asking a person to swing a club.

## Evidence

Every HIL artifact should contain:

- requested and read-back camera configuration;
- camera serial and USB topology;
- frame, payload, timeout, and gap counts;
- host and device frame rates and independently gated maximum intervals;
- clock-fit drift and residual error;
- retention capacity, reserve, frozen size, and process memory;
- fixed exposure/gain requests and exact camera read-backs;
- raw image-quality metrics plus a relative demosaiced PNG from each view;
- explicit checks with failure messages;
- runner exit state, timestamps, log path, and timeout state.

The current audio artifact includes format, selected device/channel, amplitude,
clipping, impact counts, adaptive noise/threshold values, and its provisional
timestamp model. Later audio evidence should add measured device/buffer
latency, noise-floor distributions, candidate impacts, and calibrated strike
timestamps. UI evidence includes fixed-viewport browser screenshots, automated accessibility
results, and checked-in golden-image comparisons for the deterministic review and phone-setup
fixtures.

## Human-only acceptance points

Automation should leave only a few deliberate tasks for the developer:

- verify safe trigger wiring and the optical sync result;
- provide representative real golf swings and simulator-room audio;
- judge whether impact detection misses or falsely triggers;
- approve playback feel and visual design using concrete screenshot builds.

Everything else should be reproducible from Bazel targets and retained
artifacts.
