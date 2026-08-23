# Swing Capture web application

This package is the static React and TypeScript station application. Its first
production slice has two modes: live camera setup and recorded-swing review.
Setup provides paired previews and camera controls. Review provides one-shot
audio-trigger arming, a persistent session catalog, dual synchronized encoded
playback, a shared scrubber, playback speed, and exact source-frame stepping.
The review transport keeps play/pause, one-frame controls, speed, and the
playhead together; hovering the playhead opens a seeked thumbnail. With a
transport control focused, Space/K toggles playback, Left/Right or comma/period
steps one source frame, and Home/End jumps to the clip bounds.
The production bundle uses the versioned `/api/v1` station and capture
contracts in `src/api.ts` and `src/review_api.ts`.

Legacy-host setup preview exposes a **Preview timing** disclosure. It reports
both roles' callback, capture-ring sink, sampler, renderer, HTTP, body, and
decode evidence rather than hiding concurrent measurements behind the headline
heuristic. A single credential-free record is retained in tab-scoped
`sessionStorage`, scoped to the current service instance and two camera serials,
capped at 8 KiB, and expired after six hours. Restarting the station service
therefore clears retained evidence even when the same cameras remain attached.
It is diagnostic stage evidence, not causal root-cause proof;
cpp-httplib serialization and socket delivery occur after the server handler's
observable timing boundary.

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
Galaxy SDK. The legacy host station uses concurrent full-resolution Intel
VA-API all-keyframe VP9/WebM; Android nodes publish H.264/MP4. While the legacy
host files are encoding, the application shows
the two exact trigger-nearest full-resolution JPEGs. The physical-artifact
Playwright bridge qualifies the completed files through the same player.

Build and test with Bazel:

```bash
bazel build //web:static_app //web:fixture_demo
bazel build //web:typescript
bazel test //web:component_test //web:review_component_test \
  //web:dual_node_review_api_test //web:live_status_test \
  //web:memory_usage_parser_test //web:browser_test
```

`bazel-bin/web/static_app/` contains the deployable production asset tree.
`bazel-bin/web/fixture_demo/` contains a directly viewable fixture asset tree.
`//capture/service:preview_server` embeds the production tree in its Bazel
runfiles for the legacy host station. The Android APK also packages this exact
tree and serves it from each phone's port 8088, so a NUC is not required for
Android review. The phone's setup activity displays a single-node URL with its
Bearer credential. Dual-node review uses one browser page configured with both
phone origins and tokens.

Android mode exposes a Phone setup tab backed by authenticated
`GET /api/v1/setup` and optimistic, revision-checked `PUT /api/v1/setup`.
Single-node pages configure that phone; a dual-node URL presents both phones
and flags missing or duplicate view assignments. The coordinator refreshes both
live node descriptors before review operations and remaps the two endpoints by
their saved canonical roles, so swapping phone assignments does not require
rewriting an existing dual-node bookmark. A pose-enabled pair must have
distinct camera roles and exactly one leader associated with the other phone,
which must report shadow mode and retain no outbound peer of its own; two
disabled, peer-free pose modes are also valid. Setup
covers the camera role, the standard 720p/240 fps profile, address-trigger mode
and inference delegate, full-frame pose evaluation, debug evidence, and the
leader-to-shadow association. Peer credentials are write-only: GET returns
only whether and where a peer is configured, while PUT explicitly keeps,
clears, or replaces the stored origin/token pair. Swing review reports the live
peer-arm state and the outcome retained in each Android session manifest.
Pending coordination is shown as in progress, accepted and inbound requests
are confirmed, and a rejected or failed peer arm remains a prominent alert
even when the local clip itself reached `ready`.

The setup card can deliberately rotate that phone's own control credential while capture is
stopped. The action stays disabled until the operator enters the complete local node ID, uses the
current Bearer credential and setup revision, shows the new token only in the explicit rotation
result, and immediately updates the browser connection to use it. The old token is rejected.
Remote leaders surface `Peer credential needs re-pairing` after an authentication rejection rather
than continuing to label the stale binding verified; the operator must enter the rotated token and
save the explicit re-pair.

This authentication is deliberately minimal for the trusted local-network
deployment. The first phone-hosted or dual-node navigation carries bootstrap
credentials in cleartext HTTP query parameters. The app immediately consumes
and removes every token from the address and current browser-history entry,
retaining replacements only in tab-scoped `sessionStorage`; a same-tab reload
therefore works without putting a rotated token back into the URL. This limits
routine history, bookmark, and referrer leakage, but it cannot protect the
initial HTTP request, process arguments, proxy logs, or traffic observed by other LAN users.
Every identity and operational metadata read, including `/api/v1/node`, requires Bearer
authorization; only the untrusted `/api/v1/clock` time hint remains public and contains no
control-token material. New API reads fail closed. Native-video and audio URLs carry an HMAC-SHA256
capability scoped to one immutable collection item; it cannot authorize another session and
control-credential rotation invalidates it. Capability-bearing manifests and catalogs are private
and non-cacheable. This prevents anonymous metadata and media reads but not cleartext observation
and replay. Use only an isolated/trusted hitting-area network and do not forward port 8088.
Protected transport and a user-friendly production pairing exchange remain hardening work, not
requirements for the current field prototype.

The application expects capture/session endpoints for arming, missed-shot
preservation, session discovery, manifests, media, feedback, and diagnostic
ZIP export. Encoded-video responses must implement HTTP byte ranges; Chromium
cannot seek or step the clip from a non-range 200-only route.

Android diagnostic controls use these authenticated routes:

- `POST /api/v1/capture/missed-shot` with `{}`;
- `POST /api/v1/sessions/{id}/feedback` with schema-v1 classification, optional
  bounded note, and optional signed microsecond timing labels; and
- `GET /api/v1/sessions/{id}/diagnostics.zip`.

The review panel exposes those actions after playback. In dual-node mode it
fans feedback out to both local session IDs and downloads role-prefixed ZIPs
from both phones.

Session summaries may identify `session_kind` as `capture` or `standby_diagnostic`; omitting it
retains the legacy capture meaning. The single-node review page clearly labels a ready standby
diagnostic as “Diagnostics only,” never sends it through the clip-manifest/video parser, and offers
classification, notes, and ZIP export without video-relative timing marks. The browser-side
dual-node coordinator filters standby diagnostics before loading or pairing manifests; use a
phone's single-node page or the field feedback collector to inspect/export that node-local evidence.

The legacy host production API also exposes `/api/v1/events` as a server-sent event stream.
Session/capture changes trigger an immediate coalesced refresh; a slow
15-second poll remains only as recovery if an event is missed. This removes
the former one-second session-discovery delay without coupling the UI to camera
objects. Android's embedded node server has four request workers and does not expose that event
stream. Android review therefore uses independent, short authenticated status requests for each
phone instead of occupying workers with persistent streams. The reconnect controller sends the
current Bearer credential on every request, applies bounded exponential backoff after a failure,
and checks the server stream ID, status revision, and monotonic generation time before accepting a
response. Live capture controls are invalidated while either phone is disconnected or stale, but
an already-loaded review remains available.

The legacy host server writes atomically published session directories under
`artifacts/sessions/` by default. Pass `--sessions-root <absolute-path>` to use
a deployment volume instead. Each complete session contains `manifest.json`,
`down_the_line.webm`, and `face_on.webm`. The catalog scans compatible complete
sessions at startup and refreshes after publication, so recorded sessions
remain available after a service restart.

For an installed station, use a durable writable path outside a disposable
checkout or Bazel output tree. Back up recordings that must be retained and
monitor available space: the legacy host slice does not provide backup,
retention policy, quota enforcement, or automatic free-space recovery. Android
uses its separate app-private bounded retention policy. The host HTTP listener
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
rejection, phone setup/association, secure token non-readback, and axe
semantic-accessibility scans. `//web:browser_test` runs
against a checksum-pinned Chromium headless shell downloaded by Bazel. It
serves the fixture app with byte-range media support, verifies real VP8
decoding, synchronized play/pause/speed/step behavior, navigation back to
setup, timeline thumbnail and keyboard behavior, responsive two-phone setup,
and fixed 1440×1000 and 390×844 screenshots. It also keyboard-opens the legacy
preview-timing disclosure at 1440×1000 and writes the expanded diagnostic as a
test-output screenshot. Four checked-in Playwright goldens make those
desktop and phone presentations a pixel-comparison gate for both the synchronized review player
and the configured two-phone setup screen. The review golden preserves the browser-delivery panel
but normalizes its five process-specific timing samples before comparison; media, labels, layout,
and controls remain unmasked. Its opt-in artifact path also loads
the exact hardware-produced VP9 manifest and WebMs from a completed application
HIL run through the production bundle. Screenshots are published as Bazel undeclared test outputs
when that output directory is available.

Run just the visual comparisons while iterating with:

```bash
bazel test //web:browser_test --nocache_test_results \
  --test_arg='--grep=golden image visual regression'
```

Treat baseline replacement as a reviewed UI change; do not use snapshot updates to accept an
unexplained diff.

The same hermetic target now also starts two production-shaped Android-node
origins. It rejects missing and incorrect bearer credentials on every exposed
mutation (including missed-shot/tag and feedback), verifies partial-arm
rollback plus an immediate retry, distinguishes a peer still encoding from a
peer whose clip is truly missing, rejects mismatched live shared-session IDs,
and checks valid and unsatisfiable MP4 byte ranges. Before any irreversible two-phone arm, tag,
manual-trigger, or feedback fan-out, the coordinator authenticates both credentials through the
lightweight read-only pairing-identity route so one stale token cannot partially mutate the other
phone or incur the full setup/peer-health path. The checked-in
`fixtures/production_h264` production clips are real 90-frame AVC/H.264 MP4s with the Android
one-second inter-frame GOP (IDR frames 0, 30, and 60); their metadata, retained all-intra
comparison files, and reproduction commands are stored beside them.

`//web:dual_node_review_api_test` additionally retains a deliberately stale
browser-owned arm session while one phone reports disarmed. Saving a missed
shot must revalidate both live phones and issue zero diagnostic mutations,
rather than partially tagging the still-armed phone. It also proves one stale credential causes
zero tag mutations and succeeds after the corrected credential is supplied.

Current Android `GET /api/v1/sessions` entries include compact
`android_capture` node, shared-session, role, and coordination-availability
metadata. The dual-node catalog groups those summaries without downloading
every historical manifest or immutable coordination record; opening one shot
hydrates and validates only that pair. Legacy nodes without compact metadata
retain the bounded two-request-per-origin manifest fallback. The deterministic
catalog regression covers 35 complete pairs, an active one-sided publication,
duplicate roles, missing coordination, exact selected-pair hydration, and a
refresh with no additional historical fetches.

The pinned Chromium remains the hermetic default but does not ship proprietary
H.264 decoding. A local Bazel companion uses installed Google Chrome and makes
H.264 support mandatory, then verifies both remote origins decode nonblack
pixels, seek backward and forward across one-second GOP boundaries, play, and advance one exact
frame. It also runs the fixed-viewport
screenshot and axe accessibility assertions:

```bash
bazel test //web:system_h264_browser_test --test_output=errors
```

The Playwright configuration accepts `SWING_CAPTURE_BROWSER_NAME` (`chromium`,
`firefox`, or `webkit`) and `SWING_CAPTURE_BROWSER_EXECUTABLE`. The repository
pins Chromium and WebKit; stock Firefox is not compatible with Playwright's patched
Firefox transport, so a separate local gate uses the installed geckodriver
without downloading another browser:

```bash
bazel test //web:system_firefox_h264_browser_test \
  --test_output=errors --nocache_test_results
```

That target is intentionally fail-closed. It requires Firefox to advertise and
decode AVC, issue HTTP byte ranges for both one-second-GOP fixtures, produce
nonblack pixels, report the exact requested media time through
`requestVideoFrameCallback` after forward/backward GOP seeks, reproduce the
reverse-stepped frame, visibly distinguish requested frames, and play. Firefox
153.0.4 advances and paints a paused seek but does not enqueue a post-seek
video-frame callback. The player therefore waits for the ordinary callback and,
only on Firefox after a completed paused seek stalls, briefly plays from the
preceding frame at 0.25x and pauses on the exact requested presentation. A
one-second deadline prevents that recovery from running away. The gate proves
that the original paused-seek pixel hash equals the callback-confirmed target,
in addition to exact `mediaTime`, nonblack pixels, Range, GOP, repeatability,
and playback assertions. Both fixtures and all ten requested presentations
passed with Firefox 153.0.4/geckodriver 0.37.0 on 2026-08-23. Chrome retains its
ordinary callback path and passes the same production H.264 player suite.
Playwright 1.52's checksum-pinned WebKit 18.4 revision 2158 also passes the
two-origin H.264 production test through checksum-pinned Ubuntu 24 compatibility
libraries extracted into the Bazel test temporary directory:

```bash
bazel test //web:webkit_h264_browser_test \
  --test_output=errors --nocache_test_results
```

The closure does not modify host packages or substitute host-soname symlinks.
Run all three current candidates serially through one analysis-checked inventory with:

```bash
bazel test //web:browser_h264_candidate_matrix \
  --test_output=errors --nocache_test_results
```

The aggregate is tagged `manual`/`local` and makes an omitted candidate an analysis error; it does
not itself declare product support. The prototype supports desktop Google Chrome. Firefox and
WebKit remain passing compatibility candidates but are not release blockers. Before a prototype
release, run the Chrome codec gate together with the exact-APK, direct-LAN two-phone browser flow:

```bash
bazel test //web:prototype_browser_release_gate \
  --test_output=streamed --nocache_test_results
```

This explicit manual target is the release-artifact boundary: the Chrome fixture half requires
proprietary AVC decode, exact-frame seek/step, Range, playback, screenshot, and accessibility; the
physical half requires one byte-identical APK on both phones and the complete hosted-UI
start/stop/publication/media-range/cleanup flow. Installed Google Chrome must also decode the two
MP4s newly published by that invocation directly from their phone origins, present a nonblack
frame, and advance playback by at least 0.1 seconds. The retained evidence binds complementary
roles, origins, and one shared recording identity and fails closed on missing or malformed decode
fields. It does not run during the ordinary software wildcard. The prior combined artifact at
`artifacts/android_field_recording_browser_hil_pass_20260823T132412Z` predates this
`fresh_media_decode` contract. Two consecutive exact-APK Chrome passes at
`artifacts/android_field_recording_browser_hil_relay_range_pass1_20260823T071927Z` and
`artifacts/android_field_recording_browser_hil_relay_range_pass2_20260823T072013Z` exercise the
final decode/cancellation schema and validate media-worker cleanup, but are explicitly
non-qualifying because the Pixel 6 face-on origin used a temporary relay. The direct-LAN failure at
`artifacts/android_browser_hil_direct_lan_preflight_failure_20260823T072340Z` records DTL ready in
951 ms and five 500 ms Pixel 6 transport timeouts through 2.901 seconds; Playwright never launched,
while exact-APK verification and both screen-sleep cleanup actions passed. Restore Pixel 6-to-host
Wi-Fi reachability, then run and review the combined target for the current and every later proposed
release revision. A later decision may promote Firefox or WebKit without weakening the current
Chrome gate.

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

The manual Android browser target can also qualify live retained H.264 media.
The existing single-node path uses `SWING_CAPTURE_ANDROID_NODE_URL` together
with `SWING_CAPTURE_ANDROID_NODE_TOKEN` (or a `node_token` query on the URL).
The test first loads the APK-served root and assets, waits for an authenticated
same-origin capture-status response, and opens Phone setup to require an
authenticated setup response before it navigates to the host-served review
facade. For a durable two-phone review, export the two node origins and their
32-character control tokens, then explicitly pass the variable names through
Bazel:

```bash
export SWING_CAPTURE_ANDROID_DTL_NODE_URL=http://dtl-phone:8088
export SWING_CAPTURE_ANDROID_DTL_TOKEN=replace_with_dtl_node_token
export SWING_CAPTURE_ANDROID_FACE_NODE_URL=http://face-phone:8088
export SWING_CAPTURE_ANDROID_FACE_TOKEN=replace_with_face_node_token
export SWING_CAPTURE_CHROME_EXECUTABLE=/usr/bin/google-chrome
bazel test //web:android_browser_hil_test \
  --test_output=streamed --nocache_test_results \
  --test_env=SWING_CAPTURE_ANDROID_DTL_NODE_URL \
  --test_env=SWING_CAPTURE_ANDROID_DTL_TOKEN \
  --test_env=SWING_CAPTURE_ANDROID_FACE_NODE_URL \
  --test_env=SWING_CAPTURE_ANDROID_FACE_TOKEN \
  --test_env=SWING_CAPTURE_CHROME_EXECUTABLE
```

All four dual-node variables are an all-or-none contract: the test skips when
none are set and fails before either phone is contacted when the configuration
is partial or malformed. It first loads `/`, `/app.js`, and `/app.css` from
each phone, observes authenticated same-origin capture status, and opens Phone
setup through authenticated `GET /api/v1/setup` to prove the APK-packaged
application and its control-token state are being served. It then
recovers retained pairing evidence from both nodes, loads a ready paired
session, requires both MP4 origins to serve HTTP
206 byte ranges, decodes and seeks exactly two H.264 views, exercises
synchronized stepping and playback, and preserves
`android-dual-node-review.png` at 1440×1000.

Use `/usr/bin/google-chrome` for this live H.264 gate because the bundled
Chromium headless shell lacks proprietary H.264 decoding in this environment.
The passing two-node run is preserved at
[`artifacts/android_browser_hil_20260815T110817Z/outputs/android-dual-node-review.png`](../artifacts/android_browser_hil_20260815T110817Z/outputs/android-dual-node-review.png).
It exercised the concurrently captured Pixel 6 1080p240/24 Mbit/s and Pixel 5a
720p240/12 Mbit/s session whose durable coordination record was present on
both nodes.

When a manifest contains `pipeline_profile`, the review page shows capture,
publication, and per-view encoder timings. It also measures manifest fetch and
the presentation time of each role's impact frame with
`requestVideoFrameCallback` (or a seeked-plus-two-paint fallback). Live station
responses carry `X-Swing-Capture-Server-Monotonic-Ns`, so the page can report a
bounded audio-confirmation-to-both-frames-displayed interval without comparing
unrelated browser and server clock epochs. Playwright preserves the structured
numbers as `browser-pipeline-profile.json`; a supplied physical HIL artifact is
written separately as `hil-browser-pipeline-profile.json`.
