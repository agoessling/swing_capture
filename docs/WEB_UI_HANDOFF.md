# Swing review UI handoff

Last updated 2026-09-28 America/Los_Angeles. Read this file together with
[`web/README.md`](../web/README.md) before changing the review workspace. The web README is the
runbook; this file preserves the product intent and the decisions behind the current component
layout.

## Product direction

The review interface should feel like a native golf-analysis application, not a vertically stacked
web page or stock Bootstrap dashboard. The review workspace is media-first and bounded to the
viewport:

- The review route must not use document scrolling. The command bar, camera canvas, timeline, and
  transport remain visible as one application workspace.
- The two synchronized camera views are the primary content. They should consume the available
  canvas without stretching or disguising their source geometry.
- Secondary information belongs in the session drawer, tabbed inspector, modal dialogs, or another
  explicit application mode. It should not push the player down the page.
- Compact controls, dark neutral chrome, restrained separators, and the lime action color establish
  hierarchy. Avoid generic cards, large marketing headings, and repeated blocks of explanatory
  text in the main workspace.
- Desktop iteration is the current product priority. Preserve inexpensive keyboard, focus,
  semantic-dialog, and screen-reader guardrails, but do not expand accessibility scope at the
  expense of the core visual and interaction work unless the user requests it.

The fixed-viewport contract applies to the review application only. Camera-setup and other form-heavy
surfaces may continue to scroll when their content requires it.

## Visual references

The current direction was distilled from these five references. They are inspiration for
information density, media emphasis, and application chrome; the UI is not intended to copy any one
product exactly.

1. [Swing Catalyst analysis workspace](https://swingcatalyst.com/_next/image?url=https%3A%2F%2Fcdn.swingcatalyst.com%2Fcms%2Fmedia%2F53ae5iak%2Fhero-drawing.png&w=3840&q=75)
2. [Swing Profile desktop synchronization UI](https://www.swingprofile.com/wp-content/uploads/2024/08/home_autoSyncWithPro_v12_resized.png)
3. [Swing Catalyst laptop software view](https://swingcatalyst.com/_next/image?url=https%3A%2F%2Fcms.swingcatalyst.com%2Fmedia%2Ffy4bheid%2Fsoftware-laptop-golf.png&w=828&q=75)
4. [Golf AI motion-analysis workspace](https://a.storyblok.com/f/117513/2160x1156/14134f2ac7/post_image_4_ai_motion_analysis_v2_blog.jpg)
5. [Tablet golf-analysis simulator](https://cdn.prod.website-files.com/6789bc64ac387ec5c5d0dc74/69798049e3aa6b06d576110a_iPad%20-%20Simulator%20-%20Plain.png)

The useful common traits were a persistent application frame, dominant video, dense transport
controls, narrow tool navigation, and details that appear on demand rather than in a long document.

## Current workspace contract

The implemented review workspace has these stable regions:

- A compact top command bar contains the session selector, capture state, arm/disarm action, and
  missed-shot action.
- A narrow left tool rail opens the recorded-session library, capture inspector, and diagnostics.
- The center stage owns synchronized video, per-view identity, the shared timeline, exact-frame
  stepping, playback speed, and the fixed transport.
- The session library is an overlay drawer. It does not resize or unmount the media player.
- The inspector is docked on wide desktops, overlays the stage below 1080 px, and becomes a
  full-workspace surface on phones. Its Capture, Diagnostics, and Session panels remain mounted
  while hidden so polling, pending requests, and form state survive tab changes.
- Pipeline timing, HIL evidence, and other dense playback metadata live in the **Review details**
  modal rather than below the video.
- Capture/service failures remain visible as a compact toast when the Capture inspector is closed.

The inspector is closed by default to maximize the media canvas. Library and modal dismissal must
restore focus to the control that opened them, and Escape must close the active transient surface.

## Media and responsive behavior

Video must use `object-fit: contain`; never stretch it to fill a box. Letterboxing is correct when
the media and available canvas have different aspect ratios. The collection-backed fixture is
retained normal-speed portrait footage encoded at 360x640 and described as 720x1280 source media.
Its provenance and reproduction commands are in
[`web/fixtures/ui_lab/README.md`](../web/fixtures/ui_lab/README.md).

On desktop, both views are shown side by side and the transport is docked at the bottom of the
stage. At phone widths, one complete view is visible at a time and the second view is reached by
horizontal snap scrolling. The timeline, transport, and bottom Review/Camera setup application
switcher remain onscreen; the page itself must not scroll.

The deterministic visual contracts currently cover 1440x1000 desktop and 390x844 phone viewports:

- `review-desktop-linux.png` and `review-phone-linux.png` exercise the ordinary fixture player.
- `ui-lab-desktop-linux.png` and `ui-lab-phone-linux.png` exercise the collection-backed workspace.
- The existing phone-setup baselines ensure that review-only fixed-viewport CSS does not break the
  setup application.

Inspect changed screenshots before accepting new baselines. A passing pixel comparison is not a
substitute for checking video proportions, clipped controls, unused stage space, and fixed-viewport
containment.

## Fixture-backed iteration

Run the lab locally without phones or camera SDKs:

```bash
bazel run //web:ui_lab
```

Use `-- --lan` to print a reachable address and bind on the trusted LAN. Do not document or depend
on the printed numeric address because it is host- and network-specific.

The `?ui_lab=1` surface includes the floating scenario switcher for **Review ready**,
**Armed / monitoring**, and **Processing**. The `?collection=1` surface uses the same collection
fixture without the switcher and is the stable screenshot-test surface. Both use the same
`ReviewApi` boundary as production. Do not request new physical swings for behavior reproducible by
these checked-in fixtures.

## Validation checkpoint

At this checkpoint, both of the repository-wide software validations pass:

```bash
bazel test //...
bazel test --config=precommit -c opt //...
```

The suite contains 247 passing Bazel tests. The browser target covers interaction, production-shaped
two-node behavior, fixed desktop and phone viewports, the collection-backed lab, media seeking and
frame stepping, drawer/inspector/modal behavior, and golden screenshots. No physical HIL was run or
needed for this UI-only checkpoint.

## Likely next iteration areas

There is no known correctness blocker in the fixture-backed workspace. Future visual discussion is
expected to focus on toolbar density, typography, icon treatment, camera-stage composition, and the
eventual drawing/annotation workflow. Preserve the media-first application structure and validated
state behavior while iterating on those details.
