# Codex Working Agreement

## Default validation loop

- Treat Bazel targets as the canonical build and test interface. Do not add
  shell-script test implementations.
- Target C++23. Preserve existing working code, but prefer clear C++23
  facilities when writing new C++ or materially refactoring existing C++.
- Define first-party host C++ targets through `//tools:strict_cc.bzl`. Its
  wrappers enforce `-Wall -Wextra -Wpedantic -Werror`; bypass them only for a
  documented vendor or toolchain boundary.
- Run `bazel test //...` after software-only changes.
- Before handing off capture, timing, retention, or encoding changes, also run:

  ```bash
  bazel test -c opt //...
  bazel test --config=asan //...
  bazel test --config=ubsan //...
  ```

- Keep physical-device tests tagged `manual`, `local`, and `exclusive` so the
  default suite remains hermetic.

## Hardware-in-the-loop loop

- Use the explicit Bazel target for the primary result:

  ```bash
  bazel test //capture/daheng:dual_camera_smoke_hil_test \
    --test_output=streamed --nocache_test_results
  ```

- Keep agent-initiated HIL stages to roughly 15 seconds or less so the normal
  iteration loop stays fast. Run the five-minute qualification or 30-minute
  soak only when the user explicitly requests that specific longer run; do not
  infer permission from the type or scope of a change.
- Do not run two camera jobs concurrently. The optional
  `//tools:run_unattended_hil` runner invokes the same Bazel tests and adds a
  host lock, watchdog, durable evidence, and failure isolation.
- Always inspect `report.json` and both PNGs. A passing transport test does not
  imply `diagnostic_images_nominal`, exposure synchronization, or calibrated
  audio-to-frame alignment.
- Treat incomplete frames, timeouts, frame-ID gaps, nonmonotonic timestamps,
  pool exhaustion, and sanitizer findings as real failures. Preserve their
  artifacts before changing code.
- Do not configure electrical triggering until the exact camera connector
  pinout, voltage limits, common ground, and pulse source have been verified.

## Reproducible evidence

- Put deterministic software coverage in `cc_test` targets.
- Prefer synthetic clocks, audio, and frames for boundary/fault tests.
- Put physical reports and screenshots in Bazel undeclared test outputs.
- Keep paths inside reports relative when artifacts travel as a directory.
- When fixing a hardware-only failure, first add a software regression test
  where possible, then rerun the shortest HIL that can disprove the fix.

## UI iteration

- Keep the review UI independent of camera SDK objects.
- Build playback and drawing behavior against checked-in fixture metadata and
  encoded fixture clips.
- Add browser interaction, fixed-viewport screenshot, and accessibility tests
  before evaluating visual changes.
- Do not ask for repeated real swings when a recorded fixture can reproduce the
  same trigger, clip, encoding, or UI behavior.
