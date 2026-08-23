# Current Android dual-phone handoff

Last updated 2026-08-23 09:40 America/Los_Angeles. Read this file, `AGENTS.md`, and
`TODO.md` before changing or testing the repository. This is an operational checkpoint, not a
claim that the prototype is ready for a hitting session.

## Objective and product decisions

The active objective is to finish the two-Android-phone prototype and make the next hitting
session produce useful, recoverable evidence. Each phone is a 720p240 camera, microphone, capture
node, and HTTP media server. The Pixel 6 is the pose leader in the currently installed
configuration; the Pixel 5a is its shadow. A desktop browser loads the review UI directly from a
phone. The NUC is a development and browser client, not a required media relay.

Decisions that should survive a new agent/session:

- Production capture remains 720p240 with the screen off. Do not reduce Pixel 6 performance to
  accommodate the Pixel 5a. A Pixel-5a-only fallback is acceptable only if it is isolated; dropping
  Pixel 5a support is preferable to compromising Pixel 6.
- Pose perception uses the full frame. Do not restore a user-configured ROI; multiple golfers are
  unlikely and ROI setup/moved-tripod invalidation is not worth the coupling.
- Keep the production pose model at Lite 640x360 until held-out lifecycle evidence justifies a
  change. GPU was the fastest measured delegate on both current phones.
- Network readiness must be vendor- and topology-neutral. Do not hard-code Orbi behavior, require
  equal BSSIDs, or require 5 GHz. Measure phone-to-phone reachability, latency, jitter, timeouts,
  and transfer behavior. BSSID, band, RSSI, and link rate are diagnostics. A future UI should show
  good/degraded/unusable, warn and allow an override for degraded service, and fail only when
  coordination is not viable.
- Authentication is intentionally prototype-grade on the trusted local network for now. Do not
  expand TLS work ahead of field readiness. Do not persist Bearer credentials in repository files
  or retained evidence; read them from the app at execution time.
- Debug-mode low-rate preview/audio and problem tagging are intended to make field failures
  reproducible. Do not require the golfer to manually tag every ordinary swing.
- Use Bazel as the only build/test interface. Do not install a system `adb`; the repository supplies
  it through Bazel.
- Do not run lint/format during the ordinary iteration loop. Do not run ASan/UBSan until immediately
  before a commit or to investigate a sanitizer-relevant failure. Do not run the five-minute
  qualification or 30-minute soak without explicit user permission.

## Workspace state

- Repository: `/home/agoessling/swing_capture`
- Branch: `android-dual-phone-capture`
- HEAD: `7ffffb3c879a89e328b17578e49415718e73a657`
  (`Add on-device pose standby and phone-hosted review`)
- The worktree is intentionally very dirty: at this checkpoint `git status --short` reported 147
  modified and 209 untracked paths. These include the Android, web, HIL, evidence, and test work
  from the current development effort. Do not reset, clean, discard, or broadly rewrite them.
- Current Bazel APK SHA-256:
  `9b1389f483a5cc5269065525231631a0196e757f2e9a0837bca16506ed487e70`.
  Both installed base APKs matched this digest in the retained product-floor and pair evidence.
- The last broad validation before the most recent pair-doctor route change passed:
  `//tools:pre_field_integration_tests` 120/120 uncached, `//...` 236/236, and manual static
  compilation 916/916. The focused post-change test `//tools:android_pair_doctor_test` passed.
  Repeat the three canonical validations after the remaining changes; do not infer that the newest
  Python edit is covered by the older broad run.

## Current hardware and network

| Role | Phone | USB serial | Fixed LAN origin | Wireless ADB | OS/API |
|---|---|---|---|---|---|
| face-on / ATL leader | Pixel 6 | `22181FDF6005QH` | `http://10.168.168.111:8088` | `10.168.168.111:5555` | API 36 |
| down-the-line shadow | Pixel 5a | `1A011JEG501717` | `http://10.168.168.241:8088` | `10.168.168.241:5555` | API 34 |

The NUC is `10.168.168.175` on SSID `Hybrid Pi`. At the checkpoint, USB and wireless ADB were
authorized for both phones. The phones are physically connected to the NUC and positioned at the
AprilTag/Feather LED-and-speaker HIL. The Daheng cameras and their microphones are disconnected.
The Feather speaker is 4 ohm/3 W; the stronger short-pulse drive sounded appropriate after the
earlier low-volume stimulus was corrected.

Both phones were explicitly disarmed for this handoff. Their authenticated status settled at
`state=ready, armed=false`; do not mistake `ready` for the older doctor's `stopped` wording. The
services were still reachable on port 8088. Both screens were then explicitly put to sleep and
verified as `mWakefulness=Dozing`. Every later HIL or preflight cleanup must restore and verify that
state rather than assume it.

At the latest inspection:

- NUC and Pixel 6 used BSSID `28:94:01:70:7c:b6` on 5 GHz.
- Pixel 5a used BSSID `2e:94:01:70:59:ca` on 2.4 GHz.
- NUC signal was 65% at 540 Mbit/s. Pixel 6 RSSI was -67 dBm. Pixel 5a RSSI was a strong -55 dBm,
  but its host RTT had occasional large spikes. Different BSSIDs prove different radios, not
  necessarily different physical Orbi satellites. Do not diagnose weak RF or a different satellite
  from these fields alone.

## Network incident and current resolution

The NUC originally could not reach the Pixel 6 even though the app and routes were healthy. The
asymmetric ARP/HTTP evidence was consistent with stale mesh/AP L2 forwarding state. While the user
was at the local console, they ran the previously supplied one-shot `systemd-run`/`nmcli` reconnect.
The NUC associated to `28:94:01:70:7c:b6`; host-to-both and both phone-to-phone directions then
recovered.

A separate false diagnostic was found on Pixel 5a: `ping -I wlan0` fails before transmission with
`SO_BINDTODEVICE: Operation not permitted`, even though ordinary ping works. The current
`tools/android_pair_doctor.py` first requires `ip route get <peer>` to identify the exact peer,
`dev wlan0`, and the phone's expected LAN source, then runs ordinary ping. It fails closed for a
wrong/malformed route or failed ping. `tools/android_pair_doctor_test.py` covers the change and its
focused Bazel test passed.

The strict current-APK pair doctor passed every identity, role, binding, exact-APK, web hosting,
CORS, resource, host-to-phone, and both phone-to-peer check after the host reconnect:

- `artifacts/android_pair_preflight_post_wifi_reconnect_20260823T163223Z.json`

This does not yet qualify browser recording under load, and it does not prove that the Pixel 5a's
latency spikes are harmless. The production solution is end-to-end health measurement and a useful
warning, not topology-specific repair.

## Strongest retained current evidence

- Static exact-APK product floor passed both devices:
  `artifacts/android_product_floor_current_apk_20260823T0855Z/report.json`.
- Stopped/unarmed field ceremony passed on the first attempt after LAN repair:
  `artifacts/field_preflight_stopped_current_apk_after_lan_repair_20260823T0928Z/report.json`.
- The complete exact-APK Chrome recording/publication/range/decode/cleanup flow passed through a
  deliberately temporary USB relay:
  `artifacts/android_field_recording_browser_hil_current_apk_relay_pass_20260823T0904Z/`.
  Both new 720x1280 H.264 clips were nonblack and advanced, all four MP4/WAV requests returned 206,
  there were no accessibility/page/unexpected-request errors, and screen-sleep cleanup passed. This
  validates the app and browser flow but is not qualifying direct-LAN evidence.
- `web/src/review.e2e.spec.ts` was stabilized by waiting for the mobile grid CSS, normalizing timing,
  and then scrolling. The unchanged golden passed 20/20 stress runs.
- `web/android_field_recording_browser_hil_report_validation.cc` now requires integer
  `phone_preparation_elapsed_ms` in `[0, 15000)`; malformed/missing/boundary tests pass. This fixed
  schema drift exposed by the real relay run.
- Direct field-recording partial-start and post-202 asynchronous failure recovery have physical
  evidence; see the entries in `docs/todo_audit_20260822.md`. Do not reimplement that recovery.

## Unresolved failures and blockers

### 1. Monitoring performance and wireless diagnostic stability

The stopped preflight passed, but the current-APK monitoring preflight failed after three retained
attempts:

- `artifacts/field_preflight_monitoring_current_apk_after_lan_repair_20260823T0930Z/report.json`
- Attempt 1: Pixel 6 110 successful/0 failed pose inferences, p95 208 ms, max 228.447 ms; Pixel 5a
  101/0, p95 309 ms, max 340.556 ms. The provisional p95 limit is 200 ms.
- Attempt 2: the Pixel 6 wireless-ADB `getprop` probe timed out after four seconds, leaving incomplete
  pair evidence. Its nested direct-LAN application probes still passed.
- Attempt 3: Pixel 6 206/0, p95 207 ms, max 228.447 ms; Pixel 5a 195/0, p95 334 ms, max 345.266 ms.
  The attempt also lost Pixel 6 BSSID/host/peer diagnostic evidence under concurrent collection.

Do not weaken the 200 ms gate from these results. First determine the delegate actually selected at
runtime, whether the Pixel 5a fell back from GPU, whether inference scheduling is sustainable at
five Hz, and whether concurrent wireless ADB/diagnostic work perturbed only evidence collection or
the app itself. Keep Pixel 6 policy independent of any Pixel 5a fallback.

### 2. Direct-LAN browser release evidence

The relay browser flow passes, and the raw direct-LAN pair matrix now passes, but
`//web:prototype_browser_release_gate` has not been rerun over the repaired direct network. This is
the most valuable next integration test because it exercises recording, publication, four media
range paths, real Chrome H.264 decode/playback, two phone origins, and cleanup together.

### 3. Short optical/audio timing boundary

The latest retained short paired HIL failure is:

- `artifacts/android_hil_inner_timing_failure_20260823T081020Z/report.json`

Its down-the-line first-white point was -20.832 ms against the unchanged +/-20 ms point bound. The
quantized optical-onset interval was [-24.999, -16.665] ms; the independent uncertainty-expanded
lower bound missed the conservative -25 ms boundary by 0.570 ms. Images were otherwise nominal.
Rerun the shortest HIL after the camera/speaker repositioning. Preserve and inspect `report.json`,
both roles' three PNGs, retained-media analysis, and clips. Do not tune a limit from one fixture
sample.

### 4. Generic network-health UX

The current pair doctor proves reachability but the product still needs a topology-neutral setup
experience. Add bounded application-level bidirectional sampling and expose good/degraded/unusable
with hysteresis. Record latency/jitter/timeout/throughput outcomes, while BSSID/band/RSSI remain
diagnostic annotations. Cover client isolation, delay/loss, roaming, and transient disconnects with
deterministic tests. A degraded warning should be overrideable; unreachable coordination should
fail. Do not make this work depend on router credentials or Orbi APIs.

### 5. Documentation drift

Parts of `TODO.md`, `docs/testing.md`, and `docs/todo_audit_20260822.md` still describe direct LAN as
blocked before the host reconnect. Update them only after retaining the direct-LAN browser result
and the next monitoring result, so they cite the complete outcome rather than another transient
checkpoint.

## Recommended execution order

1. Verify USB/wireless ADB, direct HTTP, roles, current APK hash, unarmed state, screens, and thermal
   state. Preserve a fresh stopped preflight directory; never overwrite an artifact directory.
2. Inspect live pose status/logcat and the delegate selection on each phone. Reproduce the monitoring
   measurement without concurrent heavy ADB work, then with the actual field preflight. Fix a real
   scheduling/delegate/measurement issue with deterministic coverage. Do not hide a Pixel 5a miss or
   slow Pixel 6 to equalize the phones.
3. Implement the smallest generic network-health measurement/status/warning slice needed for field
   diagnosis. Keep topology data advisory. If this changes the APK, deploy that exact APK to both
   devices and rerun the product-floor report.
4. Run the direct-LAN Chrome release gate against the two real origins. Copy its undeclared outputs
   to a unique `artifacts/` directory before another invocation overwrites `bazel-testlogs`. Inspect
   `browser-hil-launcher-report.json`, `report.json`, and
   `android-field-recording-browser-hil.png`; also inspect/probe both newly published clips.
5. Run the shortest paired pose/audio/LED HIL, alone, using the existing fixture. Inspect all output
   artifacts rather than accepting only the Bazel exit code.
6. Run `//tools:pre_field_integration_tests`, then `//...`, then the manual static build. Avoid
   format/lint/sanitizers during this loop.
7. With phones unarmed, run the stopped field preflight. Arm through the hosted UI, allow warm-up,
   and run the monitoring preflight. A hitting-session go decision requires reviewed passes, not
   merely responsive ADB.
8. Provide the user the phone-hosted URL, current credential, role mapping, and a concise operator
   checklist. Verify screens off, charging, storage headroom, diagnostic capture, and problem-tag
   behavior before moving the phones.
9. During the next field collection follow
   `artifacts/next_field_collection_plan_20260823.json`. It intentionally asks for phone/view swap,
   quiet impacts, practice swings, mat strikes, waggles, speech, footsteps, club drops, aborted
   address, neutral separation, and slates. Reconcile that plan's Pixel 6 DTL / Pixel 5a face-on
   assignment with the currently installed opposite role configuration before leaving the HIL.

Use subagents for bounded software audits/tests that do not compete for the physical cameras or a
shared Bazel server. Run only one camera/HIL job at a time, and avoid parallel Bazel output bases;
prior parallel builds increased memory pressure and contributed to iteration/OOM risk.

## Commands

Stopped field preflight using the currently provisioned stable wireless-ADB endpoints:

```bash
bazel run //tools/field_preflight:android_field_preflight -- \
  --node 10.168.168.111:5555=http://10.168.168.111:8088 \
  --node 10.168.168.241:5555=http://10.168.168.241:8088 \
  --expected-role 10.168.168.111=face_on \
  --expected-role 10.168.168.241=down_the_line \
  --launch-after-unlock --sleep-screen-after-launch \
  --evidence-dir artifacts/field_preflight_before_arm_UNIQUE
```

Armed/monitoring field preflight:

```bash
bazel run //tools/field_preflight:android_field_preflight -- \
  --node 10.168.168.111:5555=http://10.168.168.111:8088 \
  --node 10.168.168.241:5555=http://10.168.168.241:8088 \
  --expected-role 10.168.168.111=face_on \
  --expected-role 10.168.168.241=down_the_line \
  --require-monitoring \
  --evidence-dir artifacts/field_preflight_armed_UNIQUE
```

Direct-LAN Chrome release gate with USB ADB as the command channel and Wi-Fi origins for all app and
media traffic:

```bash
bazel test //web:prototype_browser_release_gate \
  --test_output=streamed --nocache_test_results \
  --test_env=SWING_CAPTURE_ANDROID_DTL_SERIAL=1A011JEG501717 \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_SERIAL=22181FDF6005QH \
  --test_env=SWING_CAPTURE_ANDROID_DTL_NODE_URL=http://10.168.168.241:8088 \
  --test_env=SWING_CAPTURE_ANDROID_FACE_NODE_URL=http://10.168.168.111:8088
```

The browser HIL outputs are under
`bazel-testlogs/web/android_field_recording_browser_hil_test/test.outputs/`. Copy that whole
directory to a new artifact directory immediately after the run.

Shortest complete production paired HIL:

```bash
bazel test --config=android_hil_inner \
  //android/dual_hil:dual_phone_paired_pose_arm_lan_hil_test \
  --test_output=streamed --nocache_test_results
```

Canonical validation after software changes:

```bash
bazel test //tools:pre_field_integration_tests --nocache_test_results
bazel test //...
bazel build --config=manual_test_static_check //...
```

Do not paste credentials into shell history or this document. The Bazel HIL/preflight launchers read
the private `control_token` from each app's `shared_prefs/node_configuration.xml` through
`run-as`; the hosted setup UI can display the prototype credential when the user needs it.

## Completed subagent work at this checkpoint

- `browser_hil_schema_fix`: launcher report schema fix and focused tests complete.
- `pair_doctor_route_fix`: Wi-Fi route-provenance probe and focused tests complete.
- `bazel_loop_audit`: diagnosed the pre-reconnect L2/network condition. Its old conclusion that the
  direct-LAN blocker was active is superseded by the user's host reconnect and the passing
  `android_pair_preflight_post_wifi_reconnect_20260823T163223Z.json` artifact.

No subagent is still running.
