# Android field preflight

`//tools/field_preflight:android_field_preflight` closes the mechanical gap between remembering
two phone addresses and running the strict pair doctor. It either uses an explicitly provisioned
stable ADB endpoint or resolves the current TLS ADB connect port for each DHCP-reserved host
through `adb mdns services`, reconnects exactly those two
endpoints, verifies that both transports are authorized and both users are unlocked, optionally
performs the documented foreground activity launch, and retries the current-APK doctor while
service and peer recovery converge. The preparation steps run concurrently on the independent
phones while the retained evidence remains in command-line order.

It never reboots or unlocks a phone, changes role/pair/credential configuration, arms capture, or
claims camera, microphone, thermal, or real-swing evidence. An ADB activity launch is a development
convenience, not proof that a human completed the production reboot ceremony on the device.
The unlock probe checks primary-user credential-encrypted storage availability, which remains true
after the screen is deliberately put back to sleep; it does not bypass the first unlock after boot.
Relative evidence paths are resolved against Bazel's invoking workspace, not the binary runfiles
tree.

With the phones stopped, reconnect and launch both services before inspecting setup:

```bash
bazel run //tools/field_preflight:android_field_preflight -- \
  --node 10.168.168.111:5555=http://10.168.168.111:8088 \
  --node 10.168.168.241:5555=http://10.168.168.241:8088 \
  --expected-role 10.168.168.111=face_on \
  --expected-role 10.168.168.241=down_the_line \
  --launch-after-unlock \
  --sleep-screen-after-launch \
  --require-stopped-clean \
  --evidence-dir artifacts/field_preflight_before_arm
```

Immediately before hitting, after both phones have been armed in the hosted UI, retain the stricter
admission result:

```bash
bazel run //tools/field_preflight:android_field_preflight -- \
  --node 10.168.168.111:5555=http://10.168.168.111:8088 \
  --node 10.168.168.241:5555=http://10.168.168.241:8088 \
  --expected-role 10.168.168.111=face_on \
  --expected-role 10.168.168.241=down_the_line \
  --require-monitoring \
  --evidence-dir artifacts/field_preflight_armed
```

The tool requires the ADB identity and application origin to name the same fixed host, refuses USB
serials and duplicate nodes, checks both phones even if one is locked, and refuses to overwrite an
existing evidence directory. Discovery fails closed when a host has no connect advertisement or
more than one distinct connect endpoint. A full `HOST:PORT` remains accepted only for an explicitly
provisioned stable ADB endpoint; a DHCP reservation by itself does not stabilize this port.
The current pair uses USB-provisioned `adb tcpip 5555`, so the examples use that explicit endpoint;
after either phone reboots, reprovision it over USB before relying on this form. With Android's
secure Wireless debugging enabled instead, omit `:5555` and let the tool resolve the advertised
TLS connect port.
When strict admission fails, the CLI prints the bounded credential-redacted failures from the final
doctor attempt (for example, an exact-APK mismatch) as well as retaining the complete child report.
This keeps a routine preflight failure actionable without asking the operator to inspect JSON first.
Both `--expected-role` values are mandatory and bind the intended camera view to the stable host,
not its transient ADB port. Admission fails even when the doctor finds complementary roles if those
roles are attached to the wrong phones. `report.json` records the expected and observed bindings,
each endpoint's discovery source, bounded attempts, and limitations. It also copies the final
strict doctor's `lan_diagnostics`: both current BSSIDs and the required directed host-to-phone and
phone-to-peer reachability matrix. All matrix edges must pass; wireless or USB ADB health, a
host-side USB forward, and one-sided ARP evidence are diagnostic context rather than LAN admission.
Each underlying
credential-redacted pair-doctor report is retained alongside it. Every node row in those reports
also retains a recursively credential-redacted `capture_status` diagnostic snapshot, including
numeric pair-network, pose/audio, thermal/storage, capture-ring, and autonomous-publication state.
The same row retains the complete authenticated `field_recording_status`, so an active recorder or
its stop-and-publish transition cannot be hidden by an otherwise stopped capture engine.

`--require-stopped-clean` and `--require-monitoring` are mutually exclusive. The stopped-clean gate
requires both nodes to be unarmed in a stopped/setup-ready state, with no active local/shared
session, no autonomous replication backlog, no pending trigger/publication flags, and no active or
fault-injected field recorder. Use it for pre-session and terminal evidence; a generic doctor pass
deliberately remains non-mutating.

Before mDNS or phone operations begin, the wrapper durably publishes a failing `report.json`.
Every later checkpoint and sanitized child artifact is written through a unique temporary file,
fsynced, atomically replaced, and followed by a parent-directory fsync. An interruption therefore
leaves either the preceding complete checkpoint or the following complete checkpoint, never a
plausibly passing partial document. Resolution and unexpected preparation failures are retained in
credential-redacted form. The wrapper independently hashes the Bazel APK and admits a successful
child doctor only when its expected digest and both installed APK digests match that value.

`--sleep-screen-after-launch` sends Android's explicit `KEYCODE_SLEEP` after the activity launch
and then requires each phone to report `Asleep`, `Dozing`, or `mInteractive=false`. It is rejected
unless `--launch-after-unlock` is also present, so a stale pre-launch power check cannot satisfy the
ceremony.
