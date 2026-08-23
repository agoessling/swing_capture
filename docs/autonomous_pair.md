# Autonomous two-phone lifecycle

The configured leader owns the capture station after an operator starts it. The browser remains a
setup and review client; it is not the authority for shared swing IDs, peer arm, impact delivery,
pair admission, coordination-record publication, or rearm. The shadow accepts only authenticated,
bounded leader requests and continues to serve its own immutable media.

## Lifecycle

`AutonomousPairLifecycle` is a synchronized, side-effect-free state machine. Production code
executes the actions it returns and reports each result back to the machine. Its normal path is:

```text
stopped -> starting station -> monitoring -> arming swing -> capturing
        -> waiting for evidence -> admitting pair -> storing local record
        -> replicating record -> rearming -> monitoring
```

Station start and stop are idempotent at the shadow. A leader pose claim creates exactly one shared
session ID, then the existing direct arm and impact paths carry that ID to the shadow. The leader
polls the shadow's trigger report and published manifest, admits the pair only with a fresh bounded
clock estimate, stores the immutable coordination record locally first, and then replicates it.
The pose status response exposes the current lifecycle state, evidence flags, last outcome, and a
diagnostic string under `autonomous_pair`. It also exposes whether startup restored a durable
checkpoint, the recovery diagnostic, replication backlog depth, and the last backlog session and
outcome so restart qualification does not depend on logcat timing.

Before executing any lifecycle action, the leader atomically replaces an app-private checkpoint
containing the complete state-machine generation and, once admitted, its pending immutable record.
Arm or capture interruption resumes bounded evidence collection for the same shared-session ID; it
never starts a replacement clip under a new ID. An admission interrupted before its immutable
record became durable fails closed. Local records enter a separate create-only backlog before peer
replication, survive rearm and process death, and are removed only after the peer accepts the exact
record. A retry is idempotent; a different record already present under the same shared ID is
terminal and neither side is overwritten.

## Failure semantics

- A lost arm acknowledgement is not treated as proof that the peer failed to arm. The leader sends
  impact and polls for bounded same-session evidence; that evidence can confirm the arm.
- An explicit arm rejection degrades to local capture. Transport loss after a confirmed arm does
  not revoke the active session.
- Wi-Fi loss, missing peer publication, stale clock evidence, and unavailable peer replication keep
  valid local media reviewable and rearm the leader in degraded mode after the evidence deadline.
- A peer restart that reports another shared session, duplicate clips for one shared session,
  malformed identity/timing evidence, or a conflicting immutable record is terminal. The station
  does not pair or silently overwrite evidence after one of these conflicts.
- The local immutable coordination record is authoritative once stored. Peer replication failure
  cannot roll it back.

Hermetic tests use a pure fake clock/state driver and a loopback authenticated HTTP peer. They cover
lost/retried/duplicate station requests, lost arm responses, mid-capture transport loss, peer
restart and split sessions, stale clock state, partial publication, one-phone degraded capture,
duplicate local clips, local-only replication, and conflicting replicas.

## Restart qualification

The deterministic manual target below uses two phone HTTP nodes and low-rate pose standby, but no
high-speed capture or Feather stimulus. It seeds HIL-gated published evidence under one shared ID,
force-stops and restarts the leader, requires the same ID and local immutable record to recover,
observes peer replication clear the backlog, and requires both phones to return to pose monitoring.
Each physical stage is bounded to 15 seconds. Immutable-conflict and corrupt/missing-checkpoint
behavior remain hermetic software tests because deliberately putting a phone into a conflicting
terminal state adds no physical coverage.

```bash
bazel test //android/dual_hil:dual_phone_autonomous_restart_recovery_hil_test \
  --test_output=streamed --nocache_test_results \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_SERIAL=<pixel-6-adb-serial> \
  --test_env=SWING_CAPTURE_ANDROID_DTL_SERIAL=<pixel-5a-adb-serial> \
  --test_env=SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN=http://<pixel-5a-ip>:8088
```

The target snapshots and restores both complete node-configuration generations, writes its report
and before/after status evidence to Bazel undeclared test outputs, removes only its unique HIL
coordination records after a successful durable stop, and preserves failure evidence for diagnosis.
The companion disturbance target uses USB ADB so it can briefly disable and restore the shadow's
Wi-Fi without losing control of either phone. It requires the leader to preserve the seeded local
record and backlog across its own restart, replicate that exact record after Wi-Fi returns, detect
a force-stopped peer app, recover the authenticated peer after restart, and automatically return
both phones to pose monitoring. Every stage has the same 15-second bound, and no camera capture or
Feather stimulus is required.

```bash
bazel test //android/dual_hil:dual_phone_autonomous_disturbance_hil_test \
  --test_output=streamed --nocache_test_results \
  --test_env=SWING_CAPTURE_ANDROID_FACE_ON_SERIAL=<pixel-6-usb-adb-serial> \
  --test_env=SWING_CAPTURE_ANDROID_DTL_SERIAL=<pixel-5a-usb-adb-serial> \
  --test_env=SWING_CAPTURE_ANDROID_DTL_LAN_ORIGIN=http://<pixel-5a-ip>:8088
```

This target is manual, local, and exclusive. It snapshots and restores both complete node
configurations, always re-enables shadow Wi-Fi on exit, removes only its unique synthetic
coordination record after a successful durable stop, and publishes the checkpoint, local-only,
replication, outage, restart, and rearm status responses with `report.json` in Bazel undeclared
test outputs. The bounded physical run passed on 2026-08-22; retained evidence is under
`artifacts/android_autonomous_disturbance_pass_20260822T175253`.
