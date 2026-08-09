# Headless setup-preview service

`//capture/service:preview_server` is the sole camera-owning process for the
headless setup UI. It opens the serials assigned in `.station.local.conf`, runs
one acquisition/command thread per camera, and serves the production web bundle
on port 8080.

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
without intervention. This remains an instrumentation/performance follow-up;
it is not yet attributed to capture, rendering, HTTP delivery, or the browser.

Run and validate with:

```bash
bazel test //capture/service/...
bazel run //capture/service:preview_server
```

Stop it before direct camera HIL. The unattended runner and preview server
share `artifacts/hil/hardware.lock`. An installed deployment can set
`SWING_CAPTURE_HARDWARE_LOCK` (or pass `--hardware-lock`) to one provisioned
runtime path; the runner must use the same environment value.
