# Paired hitting-session evidence sidecar

This bounded host tool records debugging evidence alongside, but independently from, a two-phone
hitting session. It performs only authenticated `GET` requests and read-only ADB operations. It
never arms, stops, configures, deletes, or exports recordings.

Run it from the workspace before the operator begins the first slate, using the two currently
connected ADB serials and credential-free direct-LAN application origins:

```bash
bazel run //tools/session_sidecar:session_sidecar -- \
  --node 10.168.168.111:37123=http://10.168.168.111:8088 \
  --node 10.168.168.241:42491=http://10.168.168.241:8088 \
  --output-dir artifacts/hitting_sidecar_YYYYMMDDTHHMMSSZ \
  --duration-seconds 3600 \
  --interval-seconds 1
```

The output directory must not already exist. Control credentials are read in memory through
`adb exec-out run-as`; they must not be supplied in arguments or origins. Redirects are rejected,
response bodies containing credentials or media capabilities are omitted, diagnostics and logcat
are redacted, and `inventory.json` SHA-256 inventories every other retained file after a final
credential scan.

`status-snapshots.jsonl` contains timestamped raw JSON values from each node's capture and
field-recording status endpoints. Initial and final diagnostics retain bounded `dumpsys power`,
`dumpsys battery`, `dumpsys thermalservice`, and `cmd wifi status` output. Each phone also gets a
bounded logcat tail. Defaults are 1 Hz, 8 MiB of logcat per phone, and 256 MiB total; duration is
always explicit and cannot exceed 12 hours. `Ctrl-C` finalizes the available evidence and inventory.

Limitations:

- This does not copy field MP4/WAV/manifests or diagnostic ZIPs off either phone. Export and hash
  those separately after the session.
- Logcat is a bounded tail. Once full, its oldest lines are discarded.
- HTTP or ADB failures are retained as evidence; they do not authorize a retry or phone mutation.
- Status and device diagnostics are telemetry, not F01--F14 operator labels or impact ground truth.
- Wi-Fi diagnostics can contain local SSID, BSSID, and IP information. Treat the directory as field
  evidence even though control and media credentials are excluded.
