# Swing Capture web application

This package is the static React and TypeScript station application. Its first
production slice has two modes: live camera setup and recorded-swing review.
Setup provides paired previews and camera controls. Review provides one-shot
audio-trigger arming, a persistent session catalog, dual synchronized encoded
playback, a shared scrubber, playback speed, and exact source-frame stepping.
The production bundle uses the versioned `/api/v1` station and capture
contracts in `src/api.ts` and `src/review_api.ts`.

Each arm accepts at most one trigger. After the trigger, post-roll, and encoding
complete, capture is unarmed and the operator explicitly re-arms for the next
swing. The displayed audio-trigger estimate frame is the retained camera frame
nearest the current microphone timestamp estimate. It is useful for review
navigation, but it is not calibrated ball-contact timing: ALSA buffering,
microphone/device latency, and fixed camera transport latency have not yet been
measured end to end.

The fixture bundle injects `FakeStationApi` and `FakeReviewApi`. Checked-in
software-encoded, all-intra VP8/WebM clips and deterministic per-frame timing
metadata exercise the same review player without cameras, a microphone, or the
Galaxy SDK. Production sessions use concurrent Intel VA-API all-keyframe
VP9/WebM, and the physical-artifact Playwright bridge qualifies those files
through the same player.

Build and test with Bazel:

```bash
bazel build //web:static_app //web:fixture_demo
bazel test //web:component_test //web:review_component_test //web:browser_test
```

`bazel-bin/web/static_app/` contains the deployable production asset tree.
`bazel-bin/web/fixture_demo/` contains a directly viewable fixture asset tree.
`//capture/service:preview_server` embeds the production tree in its Bazel
runfiles and serves it together with the `/api/v1` station endpoints. The
application expects capture/session endpoints for arming, manual diagnostics,
session discovery, manifests, and media. WebM responses must implement HTTP
byte ranges; Chromium cannot seek or step the clip from a non-range 200-only
route.

The server writes atomically published session directories under
`artifacts/sessions/` by default. Pass `--sessions-root <absolute-path>` to use
a deployment volume instead. Each complete session contains `manifest.json`,
`down_the_line.webm`, and `face_on.webm`. The catalog scans compatible complete
sessions at startup and refreshes after publication, so recorded sessions
remain available after a service restart.

For an installed station, use a durable writable path outside a disposable
checkout or Bazel output tree. Back up recordings that must be retained and
monitor available space: the current slice does not provide backup, retention
policy, quota enforcement, or automatic free-space recovery. The HTTP listener
has no authentication or TLS. Bind it only to localhost or a trusted station
network; do not expose it directly to an untrusted network or the public
Internet.

The synthetic-swing HIL control is an operator diagnostic, not a normal capture
control. It is absent unless capture status explicitly reports HIL as enabled;
the production server reports that only when it was started with
`--enable-hil-controls`. The button sends an empty JSON object to
`POST /api/v1/hil/synthetic-swing`. One backend-owned operation samples the
external screw-terminal fixture NeoPixel candidates
`1,2,3,4,6,8,12,16` in both camera previews,
selects a visible non-clipping brightness, arms capture, and runs this fixture
timeline:

- 1.2 seconds of 60 stepped pre-impact RGB states;
- a 20 ms white impact state with a 10 ms, 2 kHz Feather speaker tone; and
- 0.5 seconds of 25 stepped post-impact RGB states.

The UI shows calibration, stimulus, capture, encode, and ready/error progress
and opens the published session automatically. Capture ends `ready` and
unarmed; the operator must re-arm before a normal swing, while another
synthetic request owns its own one-shot arm. The manifest retains the exact
per-camera white-impact check, source frame, camera profile, schedule-mapping
diagnostics, and signed audio-trigger-estimate offset. The stepped pre/post
colors remain timeline context for human playback; they are deliberately not
an 86-state automated acceptance gate. The review player labels the automated
white check and the uncalibrated audio estimate separately. The offset is a
measured relationship between markers, not calibrated true-impact timing:
camera transport, ALSA, amplifier, speaker, and acoustic latency remain
uncharacterized.

The two component tests cover setup and capture status, image refresh URLs,
apply/revert behavior, disconnected controls, session transitions, exact
timeline behavior, one-shot re-arming, the HTTP request shapes, runtime schema
rejection, and axe semantic-accessibility scans. `//web:browser_test` runs
against a checksum-pinned Chromium headless shell downloaded by Bazel. It
serves the fixture app with byte-range media support, verifies real VP8
decoding, synchronized play/pause/speed/step behavior, navigation back to
setup, and a fixed 1440×1000 screenshot. Its opt-in artifact path also loads
the exact hardware-produced VP9 manifest and WebMs from a completed application
HIL run through the production bundle. Screenshots are published as Bazel undeclared test outputs
when that output directory is available.

The manual browser bridge can validate the exact output directory preserved by
`//capture/hil:application_flow_hil_test` without touching hardware again. Pass
the absolute directory that directly contains `manifest.json` and the two
WebMs:

```bash
bazel test //web:browser_test \
  --test_output=streamed --nocache_test_results \
  --test_env=SWING_CAPTURE_REQUIRE_HIL_ARTIFACT=1 \
  --test_env=SWING_CAPTURE_HIL_SESSION_DIR=/absolute/path/to/session
```

That target serves the production bundle and a read-only API facade over the
supplied files; it does not start the production station server or touch
hardware. It requires both clips to decode and support seeking, performs an
exact frame step and playback check, and preserves `hil-review-session.png` as
a Bazel undeclared test output. The bridge runs only when the explicit
`SWING_CAPTURE_REQUIRE_HIL_ARTIFACT=1` test environment opt-in is present; the
default invocation remains fully hermetic and skips that one external-artifact
case.

When a manifest contains `pipeline_profile`, the review page shows capture,
publication, and per-view encoder timings. It also measures manifest fetch and
the presentation time of each role's impact frame with
`requestVideoFrameCallback` (or a seeked-plus-two-paint fallback). Live station
responses carry `X-Swing-Capture-Server-Monotonic-Ns`, so the page can report a
bounded audio-confirmation-to-both-frames-displayed interval without comparing
unrelated browser and server clock epochs. Playwright preserves the structured
numbers as `browser-pipeline-profile.json`; a supplied physical HIL artifact is
written separately as `hil-browser-pipeline-profile.json`.
