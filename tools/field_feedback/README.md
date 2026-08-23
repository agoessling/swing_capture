# Field feedback collector

This Nucky-side tool polls one or two Android capture nodes for newly published sessions. For each
unseen `(node_id, session_id)`, it downloads the bearer-authenticated diagnostics ZIP, validates the
ZIP and every SHA-256 declared by `diagnostic_export.json`, checks the manifest and diagnostic
incident identities, and checks retained shared-session coordination evidence when available.
The coordination check independently revalidates mapped timestamps, composed uncertainty, clock
sample/round-trip bounds, and the derived trigger-separation interval before indexing a compact
timing summary under the shared session. This makes later pair-clock threshold analysis depend on
validated evidence rather than merely a matching session ID and ZIP checksum.
The shared index publishes that summary only when both role archives contain the same coordination
record; a two-manifest association with one-sided evidence remains `coordination_status:
"incomplete"`. On every collector pass, archive-derived timing and peer-impact fields are rebuilt
from the retained ZIP and compared with `index.json`, so the index cannot silently replace the
archive as the evidence authority.

Peer-triggered capture manifests also retain the original peer impact request, whether its mapping
was accepted or rejected, its age/uncertainty/round-trip bounds, the effective fallback policy, and
the selected local candidate residual. The collector independently enforces the 10-second/25-ms
mapping policy and source-specific 80-ms mapped or 250-ms legacy candidate windows before indexing
those fields.

The collector accepts both ordinary capture manifests and
`session_kind: "standby_diagnostic"` manifests. A standby diagnostic is evidence gathered while the
pose standby path is waiting rather than a playable swing: it must contain a canonical mono PCM16
`diagnostic_audio.wav` and `diagnostic_incident.json`, and it does not need a camera view or video
track. Optional pose evidence is validated under `pose_diagnostics/` before it is indexed.

Run one pass:

```bash
bazel run //tools/field_feedback -- \
  --node http://192.168.1.41:8088 --token AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA \
  --node http://192.168.1.42:8088 --token BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB \
  --output artifacts/field_feedback --once
```

Omit `--once` to poll continuously; `--poll-interval-seconds` defaults to 5. Tokens are used only in
the `Authorization` header and are never written to the index or artifact paths. Supplying tokens on
the command line can expose them through shell history or the process list, so use this only on the
controlled Nucky host and clear history as appropriate.

Add `--extract-previews` to materialize an exported `preview_frames.mjpeg` and
`pose_trace.ndjson` beside the ZIP. The collector validates each trace byte span against a complete
JPEG in the concatenated stream and writes a deterministic `frame_index.json`; ffmpeg is not used.
The raw files remain checksum-validated inside the retained ZIP regardless of this option. Trace
records may name the span `offset`/`length`, `byte_offset`/`byte_length`, `mjpeg_offset`/
`mjpeg_length`, their `_bytes` variants, or the standby contract's
`frame_byte_offset`/`frame_byte_length` fields.

`index.json` is atomically written with sorted keys and stable list ordering. Archives live below a
UTC timestamped `artifacts/<timestamp>/<node_id>/` directory. An existing indexed archive is checked
on startup and is never downloaded again; a missing or changed local archive stops collection rather
than silently replacing evidence. Partial shared sessions are retained and become `paired` in the
index after evidence from the other role arrives.

Publication is recoverable across interruption. Before exposing an archive or extracted preview at
its final path, the collector fsyncs a token-free intent under `.transactions/`. Archive, preview, and
index replacements fsync both file contents and their containing directories. On the next run, a
durable archive whose index publication was interrupted is revalidated, any preview is regenerated,
and the index is completed without another phone download. An intent interrupted before the archive
was published is retired and downloaded normally; an intent left after a durable index write is
validated and removed. Unknown, symbolic-link, conflicting, or checksum-changing transaction state
fails closed.

The phone currently exposes sessions as its discovery feed; there is no separate incident-list
route. Each authenticated export contains `diagnostic_incident.json`, so the collector indexes its
classification, feedback, and timing-mark count after validation.

Run hermetic tests with:

```bash
bazel test //tools/field_feedback:collector_test
```
