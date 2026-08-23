# Headless capture and review service

`//capture/service:preview_server` is the sole camera-owning process for the
headless setup, capture, and review UI. It opens the serials assigned in
`.station.local.conf`, runs one acquisition/command thread per camera, and
serves the production web bundle on port 8080.

The HTTP API is versioned at `/api/v1`:

- `GET /api/v1/status` returns both configured camera roles, observed stream
  rates, latest completed-preview sequence, camera setting ranges/read-backs,
  and image-quality guidance.
- `GET /api/v1/cameras/{role}/preview` returns the latest completed,
  compressed, fit-within 640x480 JPEG. `full=1` selects the cached full-sensor
  PNG for manual focus. `sequence` is a browser cache buster, not a historical
  frame request. The legacy `.png` route remains available during migration.
- `PATCH /api/v1/cameras/{role}/settings` accepts numeric `exposure_us` and
  `gain_db` fields and returns the camera's complete read-back status.
- `/api/v1/capture/*` arms the one-shot microphone trigger and reports capture
  progress. `/api/v1/sessions/*` serves the persistent catalog, manifests,
  byte-range full-resolution VP9 media, and provisional trigger-nearest JPEGs.
- `GET /api/v1/events` is a server-sent event stream for capture, early-image,
  and session changes. The browser keeps a slow recovery poll but does not wait
  for it during the normal path.

The service is intentionally conservative:

- HTTP threads never call the SDK.
- Raw and rendered preview state is overwrite-latest rather than queued, and
  demosaic/encode work runs on a dedicated renderer rather than an SDK owner.
- Routine previews are bounded at about 30 frames per second. Full-resolution PNG
  encoding runs on demand on the same latest-only renderer and is cached until
  a newer routine preview is visible.
- Routine rendering preserves full-frame quality analysis but samples the
  Bayer mosaic directly to the fitted output geometry before demosaic, avoiding
  both full-resolution RGB work and a separate RGB resize pass.
- Settings are validated before the camera is stopped.
- A failed update attempts to restore and restart the prior complete profile.
- Camera acquisition remains full rate. The browser permits only one paired
  download/decode at a time, atomically swaps both roles, retains the last successful
  pair on failure, and collapses pending work to the newest sequences.
- Settings are session-local for this milestone.
- Production publication prepares frames on a bounded CPU worker pool while
  two Intel VA-API VP9 contexts encode concurrently. Completed sessions remain
  atomic; the exact impact JPEG pair is visible during encoding.
- The listener has no authentication or TLS and belongs only on a trusted
  station network.

The normal Bazel configuration is `-O2 -g` with unstripped symbols. A synthetic
1440x1080 Bayer benchmark is available as
`bazel run //capture/preview:preview_benchmark`. On the station NUC, the
software JPEG path measured about 5.3 ms total per routine frame; a bounded
dual-camera check produced 28--29 preview frames per second, 39--41 KiB per
image, while both acquisition loops remained near 227 fps.

The current camera defaults are 500 us exposure and 24 dB gain. The API
reports the exact device read-back, and either setting remains adjustable for
the lifetime of the service.

An occasional 3--4 second gap in visible paired-preview updates was observed
on 2026-08-08 after the sustained lockups were addressed. Both views resumed
without intervention. The old run did not preserve enough evidence to assign
that gap to a stage, so its root cause remains unknown; it must not be described
as a camera, renderer, network, or browser failure based on that observation
alone.

Current builds preserve stage evidence for the next occurrence without
attaching a debugger. Each camera's `preview_performance` status reports the SDK
callback-receipt frame ID and age, the capture-ring sink and preview-sampler
completion frame IDs and ages, latest published sample sequence and age,
rendered-source and render-completion ages, render-queue time, renderer stage,
and pending-work state. This distinction matters when the rendered sequence is
unchanged: a callback newer than the completed sink identifies host capture-ring
work; a completed sink newer than the sampler identifies sampling work; equal
stage IDs plus an old callback points upstream of the callback; and fresh
capture/sampling with an old rendered source points at the renderer. The browser
polls these values even when it correctly skips a duplicate preview sequence.

Every preview response also echoes the requested and served sequences and
includes capture/sink/sample/render snapshots, backend/observable response
preparation times, and `Server-Timing`. The handler cutoff includes content
copying and non-timing headers. The subsequent cpp-httplib serialization and
socket write happen outside the route and are not server-observable. The browser independently times response headers, response
body consumption, both image decodes, paired-loader queue wait, total
request-to-presentation time, and the interval since the prior presented pair.
Its **Preview timing** disclosure retains the most recent pair above 500 ms and
the most recent server-side capture/sink/sample/render threshold crossing. The
server-side record is a single size-bounded browser-session entry scoped to the
current station-service instance and two camera serials. It survives a same-tab
reload, clears on station mismatch or service restart, expires after six hours,
and records recovery so a later same-sequence crossing can replace it.

The attribution labels have deliberately narrow meanings:

| Evidence at the threshold crossing | Attribution |
| --- | --- |
| Callback frame is newer than sink completion for at least 500 ms | Host capture-ring sink |
| Sink completion is newer than sampler completion for at least 500 ms | Preview sampler |
| Callback, sink, and sampler IDs agree while callback age is at least 500 ms | Camera acquisition or work upstream of callback receipt |
| Capture and sink are fresh but latest published sampled input is old | Latest-frame sampling cadence |
| Capture and sampling are fresh but rendered source is old | Preview rendering |
| Server handler time dominates | HTTP handler |
| Header wait minus reported handler time dominates | HTTP dispatch, host scheduling, or network transport |
| Response-body time dominates | Response-body delivery |
| Decode time dominates | Browser image decode |
| A newer pair waited behind an in-flight pair | Browser paired-loader backpressure |
| Only the presented-pair interval is long | Status polling or browser scheduling |

These labels are evidence-based heuristics rather than causal proof. Concurrent
bottlenecks remain visible for both roles, while the headline names the largest
observed threshold crossing. The HTTP/transport label cannot distinguish the
listener's pre-handler queue, post-handler response serialization, host
scheduling, and the LAN because those intervals do not share a clock. If it recurs, retain
a screenshot of **Preview timing** and the `/api/v1/status` response before
restarting the service. Reproducing the original gap and narrowing that one
combined interval further still requires the two Daheng cameras or an
equivalent traffic-level trace; no such hardware run was performed while the
cameras were disconnected.

Run and validate with:

```bash
bazel test //capture/service/...
bazel run //capture/service:preview_server
```

Stop it before direct camera HIL. The unattended runner and preview server
share `artifacts/hil/hardware.lock`. An installed deployment can set
`SWING_CAPTURE_HARDWARE_LOCK` (or pass `--hardware-lock`) to one provisioned
runtime path; the runner must use the same environment value.
