# Bazel iteration performance

## Supported loops

Use the smallest loop that covers the code being changed, then keep the
repository-wide checkpoint:

| Purpose | Command | What it deliberately omits |
|---|---|---|
| Android Java unit inner loop | `bazel test --config=android_inner //android/app:all` | APK assembly, non-test siblings, repository-wide tests, and manual tests |
| One browser behavior | `bazel test //web:browser_test --test_arg='--grep=<unique test text>' --nocache_test_results` | Unmatched Playwright cases and all non-browser tests |
| APK packaging | `bazel build //android/app:swing_capture` | Test execution and repository-wide checks |
| Software checkpoint | `bazel test //...` | Physical tests, manual-test compilation, and pre-commit lint/format aspects |
| Manual-only source check | `bazel build --config=manual_test_static_check //...` | Test execution, including physical HIL execution |
| Android physical inner loop | `bazel test --config=android_hil_inner //android/dual_hil:dual_phone_paired_pose_arm_lan_hil_test --nocache_test_results` | Forced reinstall when the installed monolithic APK is already byte-identical |
| Pre-commit checks | `bazel test --config=precommit -c opt //...` | Physical tests and manual-test compilation |

`bazel test //...` remains the canonical checkpoint after software-only
changes. The Android inner loop is not a replacement for it. Run the explicit
manual static check after changing HIL-only code so that code which is absent
from the normal wildcard graph cannot silently rot. Run a physical test only
through its exact target and only when hardware work is intended.

The browser command passes `--grep` through Bazel to Playwright. Use distinctive
text from the test title so the result cannot silently select an unrelated case,
and inspect Playwright's reported selected/pass/skip counts. Repeated current
47-case runs took 15.6--18.8 seconds inside Playwright and 17.1--20.0 seconds as
a Bazel test; individual cases took approximately 0.001--6.0 seconds. The
focused field-recording lane completed as a Bazel test in 3.6 seconds. This is
an inner loop only: run `//web:browser_test` without a
filter, followed by the normal software checkpoint, before handing off the
change. Do not create Bazel shards for this suite. Its four Playwright workers
already isolate their servers and contexts, while sharding would duplicate
Chromium and server startup.

The seven lint, formatting, and static-analysis aspects run only with the
explicit `precommit` configuration. Plain `bazel test`, including physical HIL
targets, exercises test behavior without paying for unrelated repository-wide
checks. The checked pre-commit hook and hosted CI both select that configuration,
so moving the aspects does not remove the commit-gate coverage. The underlying
`//lint:fail_on_violation` build setting remains aligned across build, run, and
test because changing that Starlark setting discarded the entire analysis
cache.

The pinned bazel-devtools generator currently hard-codes its aspects onto plain
`bazel test` and exposes no check-policy setting. This repository therefore
tracks the pre-commit-only policy in the generated-file state as a deliberate
downstream customization. When the `bazel_devtools` module pin is upgraded,
preserve this policy until its generator supports an equivalent selection.

An isolated warm host-tool transition measured the effect. With the setting
scoped to `test`, build-to-test emitted the cache-discard warning, configured
23,281 targets, spent 0.722 seconds in analysis, and took 0.977 seconds wall
time. Aligning only the setting removed the warning, configured 2,143 targets,
spent 0.225 seconds in analysis, and took 0.471 seconds wall time. The aspects
are now selected independently with `--config=precommit`, so the same stable
build-setting transition is retained in both loops.

## Android HIL compilation and deployment

The nine dual-phone HIL modes share one `dual_android_hil_runner` library and a
tiny common entry point. Previously each `cc_test` compiled the same roughly
6,700-line source independently. Command execution and retained-media analysis
are separate libraries with focused tests; a media-analysis edit no longer
recompiles the orchestration runner. The host-only runner is I/O-bound at run
time, so it compiles at `-O0` and links statically while the extracted media
analysis retains the repository optimization setting. The orchestration-only
library is itself tagged `manual`: an edit is compiled by the selected HIL or
the explicit manual static check, not by the ordinary software wildcard. A
warm canonical checkpoint immediately before that tag spent 8.49 seconds
compiling/linking the runner's unused PIC form; the same checkpoint after the
tag completed in 0.49 seconds with no runner action.

At `-O2`, an edit to the remaining runner took 32.49 seconds and a static build
of the complete `//android/dual_hil:all` graph took 39.87 seconds after the
initial structural split. With the static `-O0` runner, the next real
incremental edit plus the same complete manual graph took 9.06 seconds. This is
a warm incremental comparison, not a cold-output-base benchmark, but it is the
relevant HIL source-edit loop.

The `android_hil_inner` configuration permits one additional optimization. The
runner hashes the Bazel APK and the installed monolithic `base.apk` on each
phone. It skips `adb install` only when the SHA-256 values are identical and
there are no split APKs; otherwise it installs and verifies the final hash.
The current Pixel 6 and Pixel 5a exact-match probes took 68--227 ms, including
68--138 ms measurements inside the integrated HIL setup. The plain HIL target
remains the forced-install checkpoint. Both paths retain per-phone install
decisions and fine-grained stop/wake, configure/forward, API-ready,
peer-configuration, LAN-validation, and clock-exchange timings.

Four focused field-recording HIL binaries supplement the nine shared-runner
modes: an ADB-forward smoke, a direct-LAN smoke, an injected synchronous
partial-start rollback, and an injected post-202 asynchronous start failure.
The retained direct-LAN exact-match run at
`artifacts/android_field_recording_direct_lan_current_apk_20260823T015651/report.json`
completed in 12.657 seconds without installing or using ADB forwarding; both
phones matched current APK SHA-256
`13c6c3e3012b86c13042d718803213e41f487123cb8c025a54e75707facef162`. It also
performed the browser CORS preflight and unauthenticated-401 checks against
both Wi-Fi origins and completed all cleanup.

The partial-start binary passed the same APK in 9.022 seconds at
`artifacts/android_field_recording_partial_start_hil_pass_20260823T015457/report.json`.
It forced and verified both installs, exercised DTL 202/face-on 409, rolled back
the accepted node, retried both nodes with a fresh ID, and restored seven
cleanup actions. The preserved pre-fix
`artifacts/android_field_recording_partial_start_hil_failed_20260823T015026/report.json`
identified an expected Camera2 `REASON_FLUSHED` during explicit stop being
misclassified as fatal. The correction ignores only that explicit-stop flush;
unexpected flushes and non-flush capture failures remain fatal.

The post-202 asynchronous target passed in 11.898 seconds at
`artifacts/android_field_recording_async_start_failure_hil_pass_20260823T091443/report.json`.
Both starts returned 202 before the face-on recorder released its one-shot
accepted-start fault and reported the injected terminal error. The target then
proved two-node rollback, a fresh-ID two-node retry, publication, all nine
cleanup actions, Dozing restoration, and no remaining ADB forwards. Its
internal physical workflow measured 10.677 seconds.

The manual/local/exclusive phone-hosted production browser target also keeps
its physical stage below 15 seconds. Its first run exposed historical manifest
hydration starving live control. Current Android session summaries now include
compact pair identity/role/coordination metadata, so current nodes fetch no
historical manifests or coordination records until a shot is selected; the
legacy fallback remains bounded to two manifest requests per node. A
deterministic 35-pair regression proves catalog hydration cannot starve live
control. The complete current-APK target passes at
`artifacts/android_field_recording_browser_hil_pass_20260823T104442Z/report.json`:
page readiness took 2.784 seconds, the camera start/stop/publication stage took
7.657 seconds, both bundle origins published, four MP4/WAV Range requests
returned 206, and accessibility, page, request, terminal-state, and screen-sleep
cleanup checks all passed.

The browser-HIL launcher now runs each phone's exact-APK check or install,
application start, token read, and authenticated `/api/v1/node` readiness probe
as one concurrent per-phone preparation. The launcher report records the shared
wall time as `phone_preparation_elapsed_ms`. The retained direct-LAN failure-path
measurement at
`artifacts/android_browser_hil_direct_lan_parallel_preflight_failure_20260823T072703Z`
reports 3,353 ms of phone preparation and a 4.72-second Bazel target, compared
with 5.67 seconds for the immediately preceding sequential-preparation failure.
This is an iteration-time comparison on the same blocked topology, not a
passing-browser benchmark: DTL became ready in 946 ms, the Pixel 6 face-on node
returned five 500 ms transport timeouts through 2.902 seconds, and Playwright
never launched. Exact-APK verification and both screen-sleep cleanup actions
still passed.

Independent LAN endpoint validation now runs concurrently for the two phones.
On adjacent physical runs this reduced association wall time from 12,767 ms to
6,527 ms. The LAN-validation portion fell from 8,630 ms to 2,696 ms wall time;
the slower individual phone still took 2,695 ms, so this is overlap rather than
weaker validation. Post-capture retained-media analysis also overlaps the two
independent ffmpeg/ffprobe, optical, and AprilTag pipelines. The current-revision
physical pass completed that stage in 1,709 ms for both phones together.

Three earlier development-lane attempts exercised the exact-APK and association
paths after these changes. Two stopped at the existing pose-latency gate (the
Pixel 6-family p95 varied between 207 and 215 ms) and one reached the stimulus
stage before a node HTTP request exceeded its stage deadline. These are real
qualification failures, not deployment failures. Their retained evidence is in
`artifacts/android_hil_inner_failure_20260823T053006Z` and
`artifacts/android_hil_inner_pose_latency_failure_20260823T053417Z`.

The current-revision S06 replay then passed end to end at
`artifacts/android_pcm_current_apk_paired_pass_20260823T002901`. Its changed APK
installed concurrently in 957 ms and 1,182 ms; complete per-node setup took
2,040 ms and 2,633 ms. Both automatic triggers were observed after 621 ms,
association took 7,087 ms, and the full physical target took 34.4 seconds after
a roughly seven-second incremental APK build. Deployment is therefore not the
dominant current loop cost. The remaining wall time is production-shaped phone
setup, monitoring/high-speed transition, capture/publication, and retained-media
analysis, each with its own 15-second bound.

The next current-APK attempt exposed a preflight-ordering defect: omitted PCM
replay variables were discovered only after phone stop/setup, wasting 12.3
seconds and temporarily mutating both nodes. Paired-pose modes now require all
three PCM variables before constructing the cleanup/mutation guard, and the
quick-start command lists them explicitly. On the corrected development-lane
run, both exact APK checks took only 65--100 ms and skipped installation. The
physical workflow then reached two complete 720p240 captures in 29.9 seconds
but correctly failed its down-the-line optical/audio timing gate: the
conservative onset interval extended beyond the current operational bound.
That evidence is retained at
`artifacts/android_hil_inner_timing_failure_20260823T081020Z`; it is a timing
qualification result, not a build or deployment regression.

## Reproducible profile method

These measurements use Bazelisk 1.29.0 and Bazel 9.2.0. Always collect a
cold/warm/incremental comparison through the checked profile-session runner.
It uses one previously absent, dedicated `--output_base`, runs all three cache
states through that same server, and shuts the server down before returning.
The output base may reuse downloads from the normal repository cache but has no
analysis, action, or output cache when the cold stage starts. The incremental
stage restores the selected file's exact original bytes before shutdown. For an
ordinary Java source it appends a unique, unreferenced package-private class, so
the compiled jar changes and downstream desugar, dex, and APK actions are
measured. Its evidence suffix is `incremental_java_bytecode`. Other supported
languages still receive a comment-only invalidation and use the explicit
`incremental_source_only` suffix; do not present those results as link or
packaging latency.

For example:

```bash
bazel run //tools:bazel_iteration_profile -- \
  --output-base /absolute/new/output-base \
  --evidence-dir artifacts/bazel-iteration/20260822 \
  --name android_unit \
  --incremental-file \
    android/app/java/com/agoessling/swingcapture/NodeConfiguration.java \
  -- test --config=android_inner //android/app:all

bazel run //tools:bazel_bep_summary -- \
  artifacts/bazel-iteration/20260822/android_unit_cold.bep.json \
  artifacts/bazel-iteration/20260822/android_unit_warm.bep.json \
  artifacts/bazel-iteration/20260822/android_unit_incremental_java_bytecode.bep.json
```

The runner serializes profile sessions for this checkout, refuses an existing
output base or evidence file, and prevents the wrapped command from overriding
its resource policy. By default it caps Bazel at four concurrent jobs and four
worker instances per worker key, limits the aggregate retained worker pool to
4 GiB, gives the action scheduler an 8 GiB memory budget, and caps the server
Java heap at 3 GiB. It also sets the isolated server's idle fallback to five
seconds and invokes `shutdown` in a `finally` path after success, failure, or an
ordinary interruption. The short idle timeout remains the fallback if the
runner itself is killed before cleanup can execute.

The resource defaults can be lowered with `--jobs`, `--memory-mib`,
`--server-heap-mib`, and `--total-worker-memory-mib`. Do not raise them without
accounting for the normal Bazel server and other processes on the host. Do not
run raw commands with a series of distinct output bases: their independent
server JVMs and persistent worker pools remain live and can collectively
exhaust RAM even when each command has finished.

This is an observed failure mode, not only a precaution. A 2026-08-22 profile
session left eleven isolated servers alive with three-hour idle timeouts. Bazel
Java processes had accumulated about 25 GiB of resident memory and 6 GiB of
swap before the host exhausted RAM and swap and killed the complete user
session. Reusing one bounded server for each comparison and shutting it down is
therefore part of the validity and safety of the profile, not optional cleanup.

The ordinary shared output base is bounded as well. A live process inventory on
2026-08-23 found the server resident at about 3.0 GiB with an 8.3 GiB maximum
heap, four hour-old desugar workers totaling 1.6 GiB, and two javac workers
totaling 1.0 GiB. The checked startup policy now caps the server heap at 3 GiB
and releases it after 30 idle minutes. Ordinary builds use at most eight jobs,
four instances per ordinary worker key, two instances for the Android
`Desugar` mnemonic, a 12 GiB local action budget, and a 1.5 GiB per-worker
memory limit. Because instance limits apply independently to each worker key,
the aggregate retained worker pool is separately capped at 6 GiB; Bazel evicts
idle workers above that threshold. The mnemonic-specific bound matters because
rules_android starts each desugar JVM with an 8 GiB heap ceiling; a live audit
found several resident simultaneously even though each was still below 1 GiB.
Two retains parallel jar desugaring while preventing four such JVMs from
expanding together. These settings keep warm workers during active development
but bound the failure domain if a compiler or Android desugar worker grows
without limit. They intentionally share one output base; they are not authority
to create parallel Bazel servers for subagents.

The compressed execution profile is suitable for Bazel's trace viewer. Bazel
9.2.0 no longer supplies the old `bazel analyze-profile` subcommand, so the
checked `//tools:bazel_bep_summary` utility extracts stable elapsed, phase,
critical-path, graph, action-cache, runner, and mnemonic fields directly from
the Build Event Protocol JSON. It deliberately excludes environment variables,
headers, and arbitrary command-line option values so the summary is safe to
share.

Run cold comparisons only against a stable source snapshot. The runner guards
against a concurrent profile session, but it cannot stop an editor or another
raw Bazel invocation. A source or BUILD edit that lands after analysis but
before an action consumes its direct inputs can produce a mixed-snapshot
failure; exclude that run rather than presenting its time as a baseline. If
the selected incremental source changes concurrently, the runner refuses to
overwrite it and reports that its temporary marker needs manual inspection.

## 2026-08-22 baseline

A historical ordinary checkpoint after the 2026-08-22 integration-contract work selected 202 software tests.
A warm `bazel test //...` completed in 36.00 seconds; the 35.6-second browser target was the entire
critical path while 200 unaffected tests came from cache. The explicit manual-target static build
then compiled all 812 selected targets in 9.14 seconds without executing hardware. This confirms
that lint/aspect and manual-HIL graph costs are no longer being paid in the ordinary loop.

Those counts and timings are retained as a historical baseline rather than the current inventory.
After the subsequent integration work, the 08:16 2026-08-23 canonical checkpoint passed all 236
software tests in 0.49 seconds with a warm cache, and `--config=manual_test_static_check` selected
and compiled all 916 targets in 1.16 seconds without executing hardware. The focused
`//tools:pre_field_integration_tests` aggregate separately
passed 120/120 uncached in 21.58 seconds from 08:06:54 through 08:07:15;
`//web:browser_test` was its 19.78-second critical path. These are software and static-build
checkpoints, not physical-device evidence.

The browser target had also forced its independent specs through one Playwright worker. They own
separate ephemeral-port servers, and the production integration matrix gives each worker its own
module state, browser context, `ProductionHarness`, and ephemeral static/node servers. Forced
no-cache measurements were 25.5 seconds with two workers, 21.8 seconds with three, and 19.5 seconds
with four. The checked four-worker configuration then passed the full browser target in 16.2
seconds and the complete Bazel invocation in 16.9 seconds. Four remains the local cap rather than a
global fully-parallel default, preserving bounded memory and avoiding unreviewed shared-state races
in future specs.

A subsequent uncached 232-test profile found a separate 11.95-second non-critical teardown in
`//tools/field_feedback:collector_test`: 23 independent `ThreadingHTTPServer` fixtures each paid
Python's default 500 ms `serve_forever` shutdown poll. The test fixture now uses a 10 ms poll; three
uncached focused runs took 0.8, 0.8, and 0.7 seconds, and the next repository run measured 0.6
seconds. The production collector is unchanged. The browser test remains the intentional critical
path at about 15–16 seconds, so this removes wasted aggregate capacity without increasing browser
workers or changing target semantics.

The 2026-08-23 preview-telemetry checkpoint exposed a different source-edit cost. After the
focused test had built the PIC form of `preview_api.cc`, `bazel test //...` still spent 19.59
seconds on the non-PIC form required by the wildcard's standalone `cc_library` output; the full
232-test invocation took 20.10 seconds. `bazel aquery` confirms both `preview_api.cc` and
`camera_worker.cc` have PIC and non-PIC compile actions. This is not a duplicate target or output
base. Forcing PIC globally would collapse those actions, but it would also change capture-runtime
code generation. Making the legacy preview server static was tested and rejected: its first build
compiled 408 additional static dependency actions and took 28.95 seconds. Keep the existing link
policy until a stable-snapshot profile demonstrates a net win without changing capture performance;
use the focused C++ tests while editing and pay the second form once at the canonical checkpoint.

The manual phone-browser HIL now has an isolated TypeScript configuration containing only its
config, contract helper/test, and spec. This prevents a concurrent edit to unrelated browser files
from surfacing as the prior broad-suite TS6053 missing-input flake while retaining the explicit
manual static-build gate for the physical target.

The stable Android unit result predates the explicit pre-commit split. It used
`//android/app:all` with `--build_tests_only --nobuild_manual_tests`, equivalent
to the checked `android_inner` configuration except that all seven aspects were
then still attached to plain tests. It discovered and passed 59 tests. Current
ordinary-test analysis is therefore expected to configure fewer targets than
this retained historical baseline. Its incremental stage used the former
comment-only Java mutation, so the row is retained as source-invalidation
history rather than bytecode-changing edit latency.

| Cache state | Wall | Analysis | Execution | Critical path | Actions executed | Action-cache hits | Tests executed |
|---|---:|---:|---:|---:|---:|---:|---:|
| Cold | 14.102 s | 7.088 s | 9.985 s | 4.345 s | 555 | 0 | 59 |
| Warm | 0.642 s | 0.276 s | 0.197 s | 0.024 s | 1 | 59 | 0 |
| One-file source-only incremental | 0.963 s | 0.266 s | 0.702 s | 0.637 s | 13 | 519 | 2 |

The cold run configured 29,103 targets, including 818 aspect applications, and
loaded 212 packages. Java compilation dominated the executed action mix: 128
`Javac` and 69 `Turbine` actions. The incremental comment rebuilt five `Javac`
and three `Turbine` actions and reran only the two affected tests. This makes
the focused loop a useful sub-second warm/edit cycle after its initial setup,
while preserving all 59 Android application tests.

The guarded APK comparison used `//android/app:swing_capture` and selected
`NodeConfiguration.java` for the temporary one-file edit. This historical run
also predates the bytecode-changing Java marker. Its comment changed the source
input but produced a byte-identical jar, allowing Bazel to reuse downstream
desugar, dex, and APK outputs; it must not be used as the expected latency of a
real APK code change.

| Cache state | Wall | Analysis | Execution | Critical path | Actions executed | Action-cache hits |
|---|---:|---:|---:|---:|---:|---:|
| Cold | 262.278 s | 56.855 s | 203.730 s | 29.375 s | 2,155 | 0 |
| Warm | 0.824 s | 0.323 s | 0.053 s | 0.003 s | 1 | 2 |
| One-file source-only incremental | 1.067 s | 0.039 s | 0.844 s | 0.838 s | 3 | 2 |

The APK cold run configured 38,093 targets and loaded 453 packages. The largest
executed action groups were 406 C++ compilations, 166 compile-jar creations, 153
C++ archives, 121 Java compilations, and 105 actions each for desugaring and dex
building. The source-only edit required one `Javac` and one `Turbine` action
in addition to workspace status. It does not support a conclusion about
ordinary bytecode-changing APK packaging cost.

The trace makes the cold startup cost more specific: resolving the
`rules_android_maven` repository took 36.4 seconds and the Android IDE-common
repository took 12.2 seconds. The longest APK critical-path compile was the
host protobuf descriptor tool at 12.9 seconds; other prominent work included
the rules-android Go builder and host protobuf generators. These are
output-base-cold costs. They do not recur on the measured warm or ordinary
one-file path, so routinely creating new output bases is the wrong iteration
strategy even apart from its memory risk.

The guarded repository comparison predates the explicit pre-commit split. It
used the equivalent of today's `bazel test --config=precommit //...`, retained
all seven lint aspects, excluded manual tests, and selected the same Java source
for its temporary edit. All 180 tests passed in every applicable stage.

| Cache state | Wall | Analysis | Execution | Critical path | Actions executed | Action-cache hits | Tests executed |
|---|---:|---:|---:|---:|---:|---:|---:|
| Cold | 514.217 s | 52.830 s | 511.859 s | 26.598 s | 6,417 | 0 | 180 |
| Warm | 1.250 s | 0.565 s | 0.554 s | 0.008 s | 1 | 184 | 0 |
| One-file source-only incremental | 1.200 s | 0.130 s | 0.943 s | 0.898 s | 2 | 180 | 0 |

The repository cold run configured 72,069 targets, including 3,735
aspect-configured targets, and loaded 797 packages. It executed 1,861 C++
compilations, 369 C++ archives, 198 Java compilations, 180 test runners, 157
clang-format checks, and 86 clang-tidy checks. The incremental Java comment
rebuilt one `Javac` action and did not rerun unaffected tests.

For the cold repository run, `rules_android_maven` resolution took 50.6 seconds
and Android IDE-common resolution took 23.1 seconds. The 24.9-second production
browser test was the longest critical-path action. Native compilation and lint
then dominate aggregate CPU/action volume, including the legacy host, embedded
firmware, MediaPipe/protobuf toolchains, and Android graph. This explains why
the repository-wide cold checkpoint is expensive without implying that the
sub-second warm/edit loops are broken.

Both comparisons ran in stable source windows through the checked resource
guard. Each isolated server was shut down after its incremental stage. The
durable BEP and compressed trace evidence is in
`artifacts/bazel_iteration_profile_safe_20260822`.

## Interpreting `--build_manual_tests`

The previous global `--build_manual_tests` made every wildcard build analyze
and compile manual physical-test targets, although Bazel correctly did not run
them. That is useful coverage but belongs in an explicit check: the normal
software checkpoint now uses `--nobuild_manual_tests`, while
`--config=manual_test_static_check` overrides it for a deliberate wildcard
build. This keeps physical tests non-executing and preserves static coverage
without charging every edit/test loop for HIL-only binaries and toolchains.

An isolated cold `bazel test //... --nobuild` comparison quantified the graph
cost without compiling or executing any target. Both commands end with Bazel's
expected “Unable to run tests” status because `--nobuild` intentionally stops
before execution; the completed analysis metrics are the result being compared.

| Manual tests | Wall analysis run | Targets requested | Targets configured | Aspect-configured targets | Action graph |
|---|---:|---:|---:|---:|---:|
| Disabled | 44.425 s | 387 | 71,855 | 3,567 | 13,674 |
| Enabled | 55.719 s | 733 | 84,979 | 6,904 | 15,109 |
| Added cost | 11.294 s | 346 | 13,124 | 3,337 | 1,435 |

This is analysis-only evidence; compiling the additional 1,435 graph actions
would add further cold cost. It is sufficient to reject global compilation of
manual tests as the default inner-loop policy while retaining the explicit
static check.
