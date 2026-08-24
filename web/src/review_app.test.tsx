import assert from "node:assert/strict";
import { type DOMWindow, JSDOM } from "jsdom";
import { Application } from "./application.js";
import { FakeStationApi } from "./fake_api.js";
import {
  FakeReviewApi,
  FIXTURE_MANIFEST,
  FIXTURE_SYNTHETIC_HIL_EVIDENCE,
} from "./fake_review_api.js";
import {
  CAPTURE_SCHEMA_VERSION,
  type CaptureStatus,
  type ClipManifest,
  DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
  type DiagnosticArchive,
  type DiagnosticFeedback,
  type DualFieldRecordingStatus,
  type FieldRecordingList,
  type FieldRecordingState,
  HttpReviewApi,
  parseCaptureStatus,
  parseClipManifest,
  parseSessionSummary,
  type PeerArmState,
  type PeerArmStatus,
  type PoseCaptureStatus,
  type ReviewApi,
  type SessionList,
  type SessionSummary,
} from "./review_api.js";
import { ReviewApp } from "./review_app.js";
import { isAndroidReviewMode, reviewEventsSupported } from "./review_boot.js";
import { ReviewPlayer } from "./review_player.js";

const dom = new JSDOM('<!doctype html><html lang="en"><body></body></html>', {
  url: "http://station.test/#review",
});

installDomGlobals(dom.window);
installMediaFixture(dom.window);
const downloadedArchiveNames = installDownloadFixture(dom.window);

async function main() {
  const { act, cleanup, fireEvent, render, screen, waitFor } = await import(
    "@testing-library/react"
  );
  const { default: axe } = await import("axe-core");

  const diagnosticsApi = new RecordingReviewApi();
  const rendered = render(<ReviewApp api={diagnosticsApi} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByRole("heading", { name: "Swing review" }));
  assert.ok(await screen.findByText("Listening for an audio trigger"));
  assert.ok(screen.getByText("Capture resources ready"));
  assert.ok(screen.getByText(/light thermal load.*0\.18 thermal headroom.*20\.0 GiB free/));
  assert.equal(rendered.container.querySelectorAll("video").length, 2);
  assert.ok(screen.getByText("Frame 46 of 90"));
  assert.ok(screen.getAllByText("Audio-trigger estimate frame").length > 0);

  const videos = Array.from(rendered.container.querySelectorAll("video"));
  for (const video of videos) {
    fireEvent.loadedMetadata(video);
    fireEvent.seeked(video);
  }
  await waitFor(() => {
    assert.ok(screen.getByRole("region", { name: "Pipeline profile" }));
    assert.ok(screen.getByText("Prepublication analysis"));
    assert.ok(screen.getByText("Publisher planning"));
    assert.ok(screen.getByText("-0.40 ms"));
    assert.ok(screen.getByText("-0.20 ms"));
    assert.ok(screen.getByText("Manifest response → both impact frames displayed"));
    const serialized = screen
      .getByRole("region", { name: "Pipeline profile" })
      .getAttribute("data-browser-timing");
    assert.ok(serialized !== null);
    const timing = JSON.parse(serialized) as Record<string, unknown>;
    assert.equal(timing.schema_version, 1);
    assert.equal(timing.presentation_method, "seeked-paint-fallback");
    assert.equal(typeof timing.audio_confirmation_to_both_frames_lower_bound_ms, "number");
  });
  fireEvent.click(screen.getByRole("button", { name: "Next frame" }));
  assert.ok(screen.getByText("Frame 47 of 90"));
  assert.ok(Math.abs((videos[0]?.currentTime ?? 0) - 1.549984) < 0.000_001);
  assert.ok(Math.abs((videos[1]?.currentTime ?? 0) - 1.549984) < 0.000_001);

  const timeline = screen.getByRole("slider", { name: "Review timeline" }) as HTMLInputElement;
  fireEvent.input(timeline, { target: { value: "10" } });
  assert.ok(screen.getByText("Frame 11 of 90"));
  Object.defineProperty(timeline, "getBoundingClientRect", {
    configurable: true,
    value: () => ({
      bottom: 10,
      height: 10,
      left: 0,
      right: 100,
      toJSON: () => ({}),
      top: 0,
      width: 100,
      x: 0,
      y: 0,
    }),
  });
  fireEvent.pointerMove(timeline, { clientX: 50 });
  assert.ok(rendered.container.querySelector("video[data-timeline-thumbnail]"));
  assert.ok(screen.getAllByText("Frame 46 · Trigger estimate").length > 0);
  fireEvent.pointerMove(timeline, { clientX: 0 });
  assert.equal(
    (rendered.container.querySelector(".timeline-thumbnail") as HTMLElement | null)?.style
      .transform,
    "translateX(-0%)",
  );
  fireEvent.pointerMove(timeline, { clientX: 100 });
  assert.equal(
    (rendered.container.querySelector(".timeline-thumbnail") as HTMLElement | null)?.style
      .transform,
    "translateX(-100%)",
  );
  fireEvent.pointerLeave(timeline.parentElement as HTMLElement);
  assert.equal(rendered.container.querySelector("video[data-timeline-thumbnail]"), null);

  screen.getByRole("toolbar", { name: "Playback controls" });
  const playButton = screen.getByRole("button", { name: "Play" });
  fireEvent.keyDown(playButton, { key: "ArrowRight" });
  assert.ok(screen.getByText("Frame 12 of 90"));
  fireEvent.keyDown(playButton, { key: "ArrowLeft" });
  assert.ok(screen.getByText("Frame 11 of 90"));
  fireEvent.keyDown(playButton, { key: "End" });
  assert.ok(screen.getByText("Frame 90 of 90"));
  fireEvent.keyDown(playButton, { key: "Home" });
  assert.ok(screen.getByText("Frame 1 of 90"));
  fireEvent.keyDown(playButton, { key: "k" });
  assert.ok(await screen.findByRole("button", { name: "Pause" }));
  fireEvent.keyDown(screen.getByRole("button", { name: "Pause" }), { key: "K" });
  assert.ok(screen.getByRole("button", { name: "Play" }));
  assert.equal(
    screen.getByRole("button", { name: "Previous frame" }).getAttribute("aria-keyshortcuts"),
    "ArrowLeft ,",
  );
  assert.equal(
    screen.getByRole("button", { name: "Next frame" }).getAttribute("aria-keyshortcuts"),
    "ArrowRight .",
  );
  assert.equal(timeline.getAttribute("aria-keyshortcuts"), "Home End");

  fireEvent.change(screen.getByRole("combobox", { name: "Playback speed" }), {
    target: { value: "0.5" },
  });
  assert.equal(videos[0]?.playbackRate, 0.5);
  assert.equal(videos[1]?.playbackRate, 0.5);

  fireEvent.click(screen.getByRole("button", { name: "Play" }));
  assert.ok(await screen.findByRole("button", { name: "Pause" }));
  fireEvent.click(screen.getByRole("button", { name: "Pause" }));
  assert.ok(screen.getByRole("button", { name: "Play" }));

  assert.ok(screen.getByRole("region", { name: "Capture diagnostics" }));
  fireEvent.change(screen.getByRole("combobox", { name: "Result" }), {
    target: { value: "av_sync_wrong" },
  });
  const note = screen.getByRole("textbox", { name: "Note (optional)" }) as HTMLTextAreaElement;
  assert.equal(note.maxLength, 500);
  fireEvent.change(note, { target: { value: "  Audio trails the club strike  " } });
  fireEvent.click(screen.getByText("Timing marks (optional)"));
  fireEvent.change(screen.getByRole("spinbutton", { name: "Desired high-speed start (ms)" }), {
    target: { value: "-1200" },
  });
  fireEvent.change(screen.getByRole("spinbutton", { name: "Visual impact (ms)" }), {
    target: { value: "4.5" },
  });
  fireEvent.change(screen.getByRole("spinbutton", { name: "Audio impact (ms)" }), {
    target: { value: "12" },
  });
  fireEvent.click(screen.getByRole("button", { name: "Save diagnostic feedback" }));
  assert.ok(await screen.findByText("Diagnostic feedback saved."));
  assert.deepEqual(diagnosticsApi.feedback, [
    {
      sessionId: FIXTURE_MANIFEST.session_id,
      feedback: {
        schema_version: DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
        classification: "av_sync_wrong",
        note: "Audio trails the club strike",
        timing_marks_us: {
          desired_high_speed_start_us: -1_200_000,
          visual_impact_us: 4_500,
          audio_impact_us: 12_000,
        },
      },
    },
  ]);
  fireEvent.click(screen.getByRole("button", { name: "Download diagnostic ZIP" }));
  assert.ok(await screen.findByText("Diagnostic ZIP download started."));
  assert.deepEqual(diagnosticsApi.archiveRequests, [FIXTURE_MANIFEST.session_id]);
  assert.deepEqual(downloadedArchiveNames, ["fixture-diagnostics.zip"]);

  const accessibility = await axe.run(rendered.container, {
    rules: { "color-contrast": { enabled: false } },
  });
  assert.deepEqual(
    accessibility.violations.map((violation) => violation.id),
    [],
    "review fixture should have no automated accessibility violations",
  );
  cleanup();

  const delayedInterFrameManifest = structuredClone(FIXTURE_MANIFEST);
  for (const track of delayedInterFrameManifest.views) {
    track.media.all_frames_keyframes = false;
  }
  const delayedInterFrame = render(<ReviewPlayer manifest={delayedInterFrameManifest} />);
  const delayedTimeline = screen.getByRole("slider", {
    name: "Review timeline",
  }) as HTMLInputElement;
  fireEvent.input(delayedTimeline, { target: { value: "28" } });
  assert.ok(screen.getByText("Frame 29 of 90"));
  for (const video of delayedInterFrame.container.querySelectorAll("video")) {
    fireEvent.loadedMetadata(video);
    fireEvent.canPlay(video);
  }
  assert.ok(
    screen.getByText("Frame 29 of 90"),
    "late inter-frame decoder readiness must not restore the impact frame after an operator seek",
  );
  fireEvent.click(screen.getByRole("button", { name: "Next frame" }));
  assert.ok(screen.getByText("Frame 30 of 90"));
  cleanup();

  const firefoxPresentation = installFirefoxPresentationFixture(dom.window);
  try {
    const firefoxManifest = structuredClone(FIXTURE_MANIFEST);
    firefoxManifest.client_delivery_profile = {
      manifest_fetch_duration_ms: 1,
      manifest_response_received_performance_ms: performance.now(),
      server_response_host_monotonic_ns: "458500000000",
    };
    const exactFirefoxView = render(<ReviewPlayer manifest={firefoxManifest} />);
    const exactFirefoxVideos = Array.from(exactFirefoxView.container.querySelectorAll("video"));
    for (const video of exactFirefoxVideos) {
      fireEvent.loadedMetadata(video);
      fireEvent.seeked(video);
    }
    await act(async () => new Promise((resolve) => setTimeout(resolve, 230)));
    assert.equal(firefoxPresentation.playCount, 2);
    await act(async () => {
      for (const video of exactFirefoxVideos) {
        assert.equal(video.playbackRate, 0.25);
        assert.equal(video.paused, false);
        firefoxPresentation.present(video, 1.499_985);
      }
    });
    await waitFor(() => {
      const serialized = screen
        .getByRole("region", { name: "Pipeline profile" })
        .getAttribute("data-browser-timing");
      assert.ok(serialized !== null);
      assert.equal(
        (JSON.parse(serialized) as Record<string, unknown>).presentation_method,
        "requestVideoFrameCallback",
      );
    });
    assert.equal(firefoxPresentation.pauseCount, 2);
    for (const video of exactFirefoxVideos) {
      assert.equal(video.playbackRate, 1);
      assert.equal(video.paused, true);
    }
    cleanup();

    const rapidSeekView = render(<ReviewPlayer manifest={firefoxManifest} />);
    for (const video of rapidSeekView.container.querySelectorAll("video")) {
      fireEvent.loadedMetadata(video);
      fireEvent.seeked(video);
    }
    fireEvent.click(screen.getByRole("button", { name: "Next frame" }));
    const playCountBeforeRapidSeekDeadline = firefoxPresentation.playCount;
    await act(async () => new Promise((resolve) => setTimeout(resolve, 230)));
    assert.equal(
      firefoxPresentation.playCount,
      playCountBeforeRapidSeekDeadline,
      "a rapid operator seek must cancel the pending impact-frame presentation nudge",
    );
    assert.ok(screen.getByText("Frame 47 of 90"));
    cleanup();

    const sessionReplacement = structuredClone(firefoxManifest);
    sessionReplacement.session_id = "replacement-firefox-session";
    const replacingView = render(<ReviewPlayer manifest={firefoxManifest} />);
    const replacedVideos = Array.from(replacingView.container.querySelectorAll("video"));
    for (const video of replacedVideos) {
      fireEvent.loadedMetadata(video);
      fireEvent.seeked(video);
    }
    await act(async () => new Promise((resolve) => setTimeout(resolve, 230)));
    assert.equal(firefoxPresentation.playCount, playCountBeforeRapidSeekDeadline + 2);
    for (const video of replacedVideos) {
      assert.equal(video.paused, false);
      assert.equal(video.playbackRate, 0.25);
    }
    const pauseCountBeforeReplacement = firefoxPresentation.pauseCount;
    replacingView.rerender(<ReviewPlayer manifest={sessionReplacement} />);
    await waitFor(() => {
      assert.equal(firefoxPresentation.pauseCount, pauseCountBeforeReplacement + 2);
    });
    for (const video of replacedVideos) {
      assert.equal(video.paused, true);
      assert.equal(video.playbackRate, 1);
    }
    const playCountAfterReplacement = firefoxPresentation.playCount;
    await act(async () => new Promise((resolve) => setTimeout(resolve, 230)));
    assert.equal(
      firefoxPresentation.playCount,
      playCountAfterReplacement,
      "session replacement must clear every pending Firefox presentation timer",
    );
    cleanup();
  } finally {
    firefoxPresentation.restore();
  }

  const switchingApi = new DeferredManifestReviewApi();
  switchingApi.resolve("ready-session-a");
  const switchingView = render(<ReviewApp api={switchingApi} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByText("Frame 46 of 90"));
  const firstSessionVideo = switchingView.container.querySelector(
    'video[aria-label="Down-the-line recorded swing"]',
  );
  assert.ok(firstSessionVideo);
  fireEvent.change(screen.getByRole("combobox", { name: "Recorded session" }), {
    target: { value: "ready-session-b" },
  });
  assert.equal(
    screen.queryByLabelText("Synchronized clip player"),
    null,
    "a selected session must not temporarily render the previous session's player or feedback",
  );
  await act(async () => switchingApi.resolve("ready-session-b"));
  await waitFor(() => {
    const source = switchingView.container.querySelector(
      'video[aria-label="Down-the-line recorded swing"] source',
    ) as HTMLSourceElement | null;
    assert.match(source?.src ?? "", /ready-session-b/);
  });
  const secondSessionVideo = switchingView.container.querySelector(
    'video[aria-label="Down-the-line recorded swing"]',
  );
  assert.ok(secondSessionVideo);
  assert.notEqual(
    secondSessionVideo,
    firstSessionVideo,
    "ready-to-ready selection must remount media so the browser performs resource selection",
  );
  cleanup();

  const staleFailureApi = new DeferredManifestReviewApi();
  const staleFailureView = render(<ReviewApp api={staleFailureApi} pollIntervalMs={60_000} />);
  const staleFailureSelector = await screen.findByRole("combobox", {
    name: "Recorded session",
  });
  fireEvent.change(staleFailureSelector, { target: { value: "ready-session-b" } });
  await act(async () => staleFailureApi.resolve("ready-session-b"));
  await waitFor(() => {
    const source = staleFailureView.container.querySelector(
      'video[aria-label="Down-the-line recorded swing"] source',
    ) as HTMLSourceElement | null;
    assert.match(source?.src ?? "", /ready-session-b/);
  });
  await act(async () => staleFailureApi.reject("ready-session-a", "old session failed late"));
  assert.equal(screen.queryByText("old session failed late"), null);
  assert.equal(
    (screen.getByRole("combobox", { name: "Recorded session" }) as HTMLSelectElement).value,
    "ready-session-b",
  );
  assert.ok(screen.getByLabelText("Synchronized clip player"));
  cleanup();

  for (const [state, heading] of [
    ["pending", "Arming paired phone"],
    ["accepted", "Paired phone armed"],
    ["inbound_accepted", "Arm accepted from paired phone"],
  ] as const) {
    render(
      <ReviewApp
        api={new FakeReviewApi({ hilEnabled: false, peerArmState: state })}
        pollIntervalMs={60_000}
      />,
    );
    assert.ok(await screen.findByText(heading));
    await waitFor(() => assert.equal(screen.getAllByRole("status").length, 2));
    cleanup();
  }

  for (const [state, heading, detail] of [
    ["rejected", "Paired phone rejected arm", "HTTP 409"],
    ["failed", "Paired-phone arm failed", "java.io.IOException"],
  ] as const) {
    const peerFailure = render(
      <ReviewApp
        api={new FakeReviewApi({ hilEnabled: false, peerArmState: state })}
        pollIntervalMs={60_000}
      />,
    );
    assert.equal((await screen.findAllByText(heading)).length, 2);
    assert.equal(screen.getAllByRole("alert").length, 3);
    assert.ok(screen.getByText("Pair network: Unknown"));
    assert.equal(screen.getAllByText(new RegExp(detail)).length, 2);
    const peerFailureAccessibility = await axe.run(peerFailure.container, {
      rules: { "color-contrast": { enabled: false } },
    });
    assert.deepEqual(
      peerFailureAccessibility.violations.map((violation) => violation.id),
      [],
      `${state} peer-arm state should have no automated accessibility violations`,
    );
    cleanup();
  }

  const poseLeader = render(
    <ReviewApp
      api={new FakeReviewApi({ hilEnabled: false, poseMode: "leader" })}
      pollIntervalMs={60_000}
    />,
  );
  assert.ok(await screen.findByRole("heading", { name: "Watching for address" }));
  assert.ok(screen.getByText("Address trigger"));
  assert.ok(screen.getByRole("button", { name: "Tag missed shot" }));
  assert.ok(screen.getByText(/no review video is created before high-speed starts/));
  fireEvent.click(screen.getByRole("button", { name: "Disarm capture" }));
  assert.ok(await screen.findByRole("button", { name: "Arm pose capture" }));
  const poseLeaderAccessibility = await axe.run(poseLeader.container, {
    rules: { "color-contrast": { enabled: false } },
  });
  assert.deepEqual(
    poseLeaderAccessibility.violations.map((violation) => violation.id),
    [],
    "pose monitoring status should have no automated accessibility violations",
  );
  cleanup();

  render(
    <ReviewApp
      api={new FakeReviewApi({ hilEnabled: false, poseMode: "shadow" })}
      pollIntervalMs={60_000}
    />,
  );
  assert.ok(await screen.findByRole("heading", { name: "Waiting for the pose leader" }));
  assert.ok(screen.getByText(/waits for the paired leader/));
  cleanup();

  render(
    <ReviewApp
      api={
        new FakeReviewApi({
          hilEnabled: false,
          poseMode: "leader",
          posePhase: "high_speed",
        })
      }
      pollIntervalMs={60_000}
    />,
  );
  assert.ok(
    await screen.findByRole("heading", { name: "High-speed capture is listening for impact" }),
  );
  assert.ok(screen.getByText("Impact trigger"));
  assert.ok(screen.getByText(/240 fps ring and microphone impact detector are active/));
  assert.ok(screen.getByRole("button", { name: "Save missed shot" }));
  cleanup();

  const standbyApi = new StandbyDiagnosticReviewApi();
  render(<ReviewApp api={standbyApi} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByRole("region", { name: "Standby diagnostics" }));
  assert.ok(screen.getByText(/no review video/));
  assert.equal(document.querySelectorAll("video").length, 0);
  assert.equal(standbyApi.manifestRequests, 0);
  assert.match(
    (screen.getByRole("combobox", { name: "Recorded session" }) as HTMLSelectElement).textContent,
    /Diagnostics only/,
  );
  assert.equal(screen.queryByText("Timing marks (optional)"), null);
  assert.equal(
    (screen.getByRole("combobox", { name: "Result" }) as HTMLSelectElement).value,
    "other",
  );
  fireEvent.click(screen.getByRole("button", { name: "Save diagnostic feedback" }));
  assert.ok(await screen.findByText("Diagnostic feedback saved."));
  assert.deepEqual(standbyApi.feedback, [
    {
      sessionId: "standby-diagnostic-001",
      feedback: {
        schema_version: DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
        classification: "other",
      },
    },
  ]);
  fireEvent.click(screen.getByRole("button", { name: "Download diagnostic ZIP" }));
  assert.ok(await screen.findByText("Diagnostic ZIP download started."));
  assert.deepEqual(standbyApi.archiveRequests, ["standby-diagnostic-001"]);
  assert.equal(downloadedArchiveNames.at(-1), "fixture-diagnostics.zip");
  cleanup();

  let pendingNowMs = 0;
  const pendingStandbyApi = new PendingStandbyDiagnosticReviewApi();
  render(<ReviewApp api={pendingStandbyApi} nowMs={() => pendingNowMs} pollIntervalMs={5} />);
  assert.ok(await screen.findByText("Listening for an audio trigger"));
  fireEvent.click(screen.getByRole("button", { name: "Save missed shot" }));
  assert.ok(await screen.findByText("Saving standby diagnostics"));
  assert.ok(screen.getByText(/Retaining audio post-roll/));
  assert.equal(document.querySelectorAll("figure").length, 0);
  const pollsAfterTag = pendingStandbyApi.sessionPolls;
  await waitFor(() => {
    assert.ok(pendingStandbyApi.sessionPolls > pollsAfterTag);
    assert.ok(screen.getByText("Saving standby diagnostics"));
  });
  pendingStandbyApi.published = true;
  assert.ok(await screen.findByRole("region", { name: "Standby diagnostics" }));
  assert.equal(pendingStandbyApi.manifestRequests, 0);
  cleanup();

  pendingNowMs = 0;
  const expiredStandbyApi = new PendingStandbyDiagnosticReviewApi();
  render(<ReviewApp api={expiredStandbyApi} nowMs={() => pendingNowMs} pollIntervalMs={5} />);
  assert.ok(await screen.findByText("Listening for an audio trigger"));
  fireEvent.click(screen.getByRole("button", { name: "Save missed shot" }));
  assert.ok(await screen.findByText("Saving standby diagnostics"));
  await act(async () => {
    pendingNowMs = expiredStandbyApi.standbyDiagnosticPublicationTimeoutMs + 1;
    await new Promise((resolve) => setTimeout(resolve, 20));
  });
  assert.equal(screen.queryByText("Saving standby diagnostics"), null);
  assert.ok(screen.getByText("No recorded swings yet"));
  cleanup();

  const missedShotApi = new MissedShotReviewApi();
  render(<ReviewApp api={missedShotApi} pollIntervalMs={60_000} />);
  const missedShotClassification = (await screen.findByRole("combobox", {
    name: "Result",
  })) as HTMLSelectElement;
  assert.equal(missedShotClassification.value, "missed_shot");
  fireEvent.click(screen.getByText("Timing marks (optional)"));
  const missedShotAudioMark = screen.getByRole("spinbutton", {
    name: "Audio impact (ms)",
  }) as HTMLInputElement;
  assert.equal(missedShotAudioMark.min, "-10000");
  assert.equal(missedShotAudioMark.max, "2000");
  fireEvent.change(missedShotAudioMark, { target: { value: "-9000" } });
  fireEvent.click(screen.getByRole("button", { name: "Save diagnostic feedback" }));
  assert.ok(await screen.findByText("Diagnostic feedback saved."));
  assert.deepEqual(missedShotApi.feedback[0]?.feedback, {
    schema_version: DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
    classification: "missed_shot",
    timing_marks_us: { audio_impact_us: -9_000_000 },
  });
  cleanup();

  const singleNodeManifest = structuredClone(FIXTURE_MANIFEST);
  const singleNodeManifestTrack = singleNodeManifest.views[0];
  assert.ok(singleNodeManifestTrack);
  singleNodeManifestTrack.source.pixel_format = "camera2_private";
  singleNodeManifest.views = [singleNodeManifestTrack];
  delete singleNodeManifest.pipeline_profile;
  render(<ReviewPlayer manifest={singleNodeManifest} />);
  assert.equal(screen.getAllByRole("figure").length, 1);
  assert.equal(document.querySelectorAll("video").length, 1);
  assert.ok(screen.getByText("Down-the-line"));
  cleanup();

  render(<ReviewApp api={new FakeReviewApi({ hilEnabled: false })} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByText("Listening for an audio trigger"));
  assert.equal(screen.queryByRole("button", { name: "Run synthetic swing HIL" }), null);
  cleanup();

  render(<ReviewApp api={new FakeReviewApi()} pollIntervalMs={5} />);
  assert.ok(await screen.findByText("Listening for an audio trigger"));
  assert.equal(
    (screen.getByRole("button", { name: "Run synthetic swing HIL" }) as HTMLButtonElement).disabled,
    true,
  );
  fireEvent.click(screen.getByRole("button", { name: "Disarm capture" }));
  assert.ok(await screen.findByRole("heading", { name: "Not armed" }));
  fireEvent.click(screen.getByRole("button", { name: "Run synthetic swing HIL" }));
  assert.ok(await screen.findByRole("heading", { name: "Calibrating the optical signal" }));
  assert.equal(
    (screen.getByRole("button", { name: "Synthetic swing running…" }) as HTMLButtonElement)
      .disabled,
    true,
  );
  await waitFor(() => {
    assert.ok(screen.getByRole("heading", { name: "Synthetic swing ready" }));
    assert.ok(screen.getAllByText(/Automated white-impact check · passed · frame/).length === 2);
    assert.ok(
      screen.getByText("Audio trigger estimate relative to white · +2.3 ms (uncalibrated)"),
    );
    assert.ok(
      screen.getByText("Audio trigger estimate relative to white · −1.8 ms (uncalibrated)"),
    );
    assert.ok(screen.getByText(/pre-impact and 25 post-impact LED colors/));
    assert.equal(
      (screen.getByRole("combobox", { name: "Recorded session" }) as HTMLSelectElement).value,
      "fixture-synthetic-001",
    );
  });
  cleanup();

  render(
    <Application
      initialView="review"
      pollIntervalMs={60_000}
      reviewApi={new FakeReviewApi()}
      stationApi={new FakeStationApi()}
    />,
  );
  assert.ok(await screen.findByRole("heading", { name: "Swing review" }));
  fireEvent.click(screen.getByRole("button", { name: "Camera setup" }));
  assert.ok(await screen.findByRole("heading", { name: "Camera setup" }));
  fireEvent.click(screen.getByRole("button", { name: "Review" }));
  assert.ok(await screen.findByRole("heading", { name: "Swing review" }));
  cleanup();

  render(
    <Application
      initialView="setup"
      pollIntervalMs={60_000}
      reviewApi={new FakeReviewApi()}
      setupAvailable={false}
      stationApi={new FakeStationApi()}
    />,
  );
  assert.ok(await screen.findByRole("heading", { name: "Swing review" }));
  assert.equal(screen.queryByRole("button", { name: "Camera setup" }), null);
  cleanup();

  render(<ReviewApp api={new FakeReviewApi()} pollIntervalMs={5} />);
  assert.ok(await screen.findByText("Listening for an audio trigger"));
  assert.ok(screen.getByText(/1\.4 seconds of preceding video.*10 seconds before this action/));
  fireEvent.click(screen.getByRole("button", { name: "Save missed shot" }));
  assert.ok(await screen.findByRole("heading", { name: "Audio trigger detected" }));
  assert.equal(
    (screen.getByRole("button", { name: "Arm audio capture" }) as HTMLButtonElement).disabled,
    true,
  );
  assert.equal(
    (screen.getByRole("button", { name: "Save missed shot" }) as HTMLButtonElement).disabled,
    true,
  );
  await waitFor(() => {
    assert.ok(screen.getByRole("heading", { name: "Ready" }));
    assert.ok(screen.getByText("Frame 46 of 90"));
    assert.ok(screen.getByRole("button", { name: "Arm audio capture" }));
    assert.equal(
      (screen.getByRole("button", { name: "Save missed shot" }) as HTMLButtonElement).disabled,
      true,
    );
  });
  fireEvent.click(screen.getByRole("button", { name: "Arm audio capture" }));
  assert.ok(await screen.findByText("Listening for an audio trigger"));
  cleanup();

  const unavailableApi: ReviewApi = {
    getCaptureStatus: () => Promise.reject(new Error("capture service unavailable")),
    setArmed: () => Promise.reject(new Error("capture service unavailable")),
    triggerManualCapture: () => Promise.reject(new Error("capture service unavailable")),
    saveMissedShot: () => Promise.reject(new Error("capture service unavailable")),
    startSyntheticSwing: () => Promise.reject(new Error("capture service unavailable")),
    getSessions: () => Promise.reject(new Error("capture service unavailable")),
    getManifest: () => Promise.reject(new Error("capture service unavailable")),
    submitDiagnosticFeedback: () => Promise.reject(new Error("capture service unavailable")),
    getDiagnosticArchives: () => Promise.reject(new Error("capture service unavailable")),
  };
  render(<ReviewApp api={unavailableApi} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByRole("alert"));
  assert.ok(screen.getByText("capture service unavailable"));
  assert.equal(screen.queryByLabelText("Synchronized clip player"), null);
  cleanup();

  const hilFailureApi: ReviewApi = {
    getCaptureStatus: () =>
      Promise.resolve({
        schema_version: CAPTURE_SCHEMA_VERSION,
        state: "error",
        armed: false,
        active_session_id: null,
        error: "",
        hil: {
          enabled: true,
          busy: false,
          stage: "error",
          error: "Optical calibration did not find the Feather LED",
          last_run: {
            session_id: null,
            stage: "error",
            error: "Optical calibration did not find the Feather LED",
          },
        },
      }),
    setArmed: () => Promise.reject(new Error("not used")),
    triggerManualCapture: () => Promise.reject(new Error("not used")),
    saveMissedShot: () => Promise.reject(new Error("not used")),
    startSyntheticSwing: () => Promise.reject(new Error("not used")),
    getSessions: () => Promise.resolve({ schema_version: 1, sessions: [] }),
    getManifest: () => Promise.reject(new Error("not used")),
    submitDiagnosticFeedback: () => Promise.reject(new Error("not used")),
    getDiagnosticArchives: () => Promise.reject(new Error("not used")),
  };
  render(<ReviewApp api={hilFailureApi} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByRole("heading", { name: "Synthetic swing HIL failed" }));
  assert.ok(screen.getByRole("alert"));
  assert.ok(screen.getByText("Optical calibration did not find the Feather LED"));
  cleanup();

  const delayedHistoryApi = new DelayedHistoryFieldRecordingReviewApi();
  render(<ReviewApp api={delayedHistoryApi} pollIntervalMs={5} />);
  assert.ok(
    await screen.findByText("Listening for an audio trigger"),
    "live capture status must render before historical sessions finish loading",
  );
  const delayedHistoryStart = await screen.findByRole("button", {
    name: "Start field recording",
  });
  const delayedHistoryPanel = delayedHistoryStart.closest("section");
  assert.ok(delayedHistoryPanel);
  assert.equal(delayedHistoryPanel.dataset.recordingCatalogState, "loading");
  assert.equal(delayedHistoryStart.hasAttribute("disabled"), false);
  assert.ok(screen.getByText("Down the line"));
  assert.ok(screen.getByText("Face on"));
  await act(async () => new Promise((resolve) => setTimeout(resolve, 20)));
  assert.ok(
    delayedHistoryApi.statusRequests > 1,
    "live field status polling must continue while the historical catalog is pending",
  );
  assert.equal(
    delayedHistoryApi.catalogRequests,
    1,
    "a pending historical catalog must not be requested on every live-status poll",
  );
  delayedHistoryApi.completed = true;
  await act(async () => delayedHistoryApi.resolveHistory());
  assert.equal(delayedHistoryPanel.dataset.recordingCatalogState, "ready");
  fireEvent.click(await screen.findByText("Completed recordings"));
  assert.equal(screen.getAllByRole("link", { name: "Video" }).length, 4);
  cleanup();

  const fieldRecordingApi = new FieldRecordingReviewApi();
  const fieldRecordingView = render(<ReviewApp api={fieldRecordingApi} pollIntervalMs={60_000} />);
  const fieldHeading = await screen.findByRole("heading", {
    name: "Continuous test recording",
  });
  const fieldPanel = fieldHeading.closest("section");
  assert.ok(fieldPanel);
  assert.match(fieldPanel.textContent ?? "", /Down the line/);
  assert.match(fieldPanel.textContent ?? "", /Face on/);
  fireEvent.click(screen.getByRole("button", { name: "Start field recording" }));
  assert.ok(await screen.findByRole("button", { name: "Stop both phones" }));
  assert.equal(fieldRecordingApi.startRequests, 1);
  assert.match(fieldPanel.textContent ?? "", /Recording/);
  fireEvent.click(screen.getByRole("button", { name: "Stop both phones" }));
  assert.ok(await screen.findByRole("button", { name: "Start field recording" }));
  assert.equal(fieldRecordingApi.stopRequests, 1);
  fireEvent.click(screen.getByText("Completed recordings"));
  assert.equal(screen.getAllByRole("link", { name: "Video" }).length, 4);
  assert.equal(screen.getAllByRole("link", { name: "Audio" }).length, 4);
  assert.equal(screen.getAllByRole("link", { name: "Manifest" }).length, 4);
  const fieldAccessibility = await axe.run(fieldRecordingView.container, {
    rules: { "color-contrast": { enabled: false } },
  });
  assert.deepEqual(
    fieldAccessibility.violations.map((violation) => violation.id),
    [],
    "field recording controls should have no automated accessibility violations",
  );
  cleanup();

  const recoverableFieldRecordingApi = new FieldRecordingReviewApi();
  recoverableFieldRecordingApi.failed = true;
  render(<ReviewApp api={recoverableFieldRecordingApi} pollIntervalMs={60_000} />);
  const retryFieldRecording = await screen.findByRole("button", {
    name: "Start field recording",
  });
  assert.equal(retryFieldRecording.hasAttribute("disabled"), false);
  assert.equal(screen.getAllByText("camera pipeline unavailable").length, 2);
  fireEvent.click(retryFieldRecording);
  assert.ok(await screen.findByRole("button", { name: "Stop both phones" }));
  assert.equal(recoverableFieldRecordingApi.startRequests, 1);
  cleanup();

  testRuntimeSchemaRejection();
  testReviewBootMode();
  await testHttpContract();
}

function testReviewBootMode() {
  const host = new URLSearchParams();
  assert.equal(isAndroidReviewMode(host), false);
  assert.equal(reviewEventsSupported(host), true);

  const phoneHosted = new URLSearchParams("node_token=phone-control-token");
  assert.equal(isAndroidReviewMode(phoneHosted), true);
  assert.equal(reviewEventsSupported(phoneHosted), false);

  const dual = new URLSearchParams(
    "dtl_node=http%3A%2F%2Fdtl.test&face_node=http%3A%2F%2Fface.test",
  );
  assert.equal(isAndroidReviewMode(dual), true);
  assert.equal(reviewEventsSupported(dual), false);
}

function testRuntimeSchemaRejection() {
  const captureStatus = {
    schema_version: CAPTURE_SCHEMA_VERSION,
    state: "ready",
    armed: false,
    active_session_id: FIXTURE_MANIFEST.session_id,
    error: "",
    operational_health: {
      ready_for_capture: false,
      thermal: { status: 3, headroom: 0.95, ready: false, power_save_mode: true },
      storage: {
        usable_bytes: 1_073_741_824,
        minimum_free_bytes: 2_147_483_648,
        ready: false,
      },
      issues: [
        "Let this phone cool below Android thermal status SEVERE before capture.",
        "Free at least 2 GiB of app storage before capture.",
      ],
    },
    hil: {
      enabled: true,
      busy: false,
      stage: "ready",
      error: "",
      last_run: {
        session_id: FIXTURE_MANIFEST.session_id,
        stage: "ready",
        error: "",
      },
    },
  } as const;
  assert.deepEqual(parseCaptureStatus(captureStatus), captureStatus);
  assert.throws(
    () =>
      parseCaptureStatus({
        ...captureStatus,
        operational_health: {
          ...captureStatus.operational_health,
          ready_for_capture: true,
        },
      }),
    /readiness disagrees/,
  );
  for (const state of [
    "not_requested",
    "pending",
    "accepted",
    "rejected",
    "failed",
    "inbound_accepted",
  ] as const) {
    const peerArm = peerArmFixture(state);
    const pose = poseStatusFixture(peerArm);
    assert.deepEqual(parseCaptureStatus({ ...captureStatus, pose }).pose, pose);
  }
  assert.throws(
    () =>
      parseCaptureStatus({
        ...captureStatus,
        pose: poseStatusFixture({
          ...peerArmFixture("pending"),
          state: "timed_out",
        } as unknown as PeerArmStatus),
      }),
    /state is unsupported/,
  );
  assert.throws(
    () =>
      parseCaptureStatus({
        ...captureStatus,
        pose: poseStatusFixture({ ...peerArmFixture("rejected"), http_status: null }),
      }),
    /rejected requires http_status/,
  );
  assert.throws(
    () =>
      parseCaptureStatus({
        ...captureStatus,
        pose: poseStatusFixture({ ...peerArmFixture("failed"), failure_type: null }),
      }),
    /failed requires failure_type/,
  );
  assert.throws(
    () =>
      parseCaptureStatus({
        ...captureStatus,
        pose: { ...poseStatusFixture(peerArmFixture("not_requested")), mode: "automatic" },
      }),
    /pose status mode is unsupported/,
  );
  assert.throws(
    () =>
      parseCaptureStatus({
        ...captureStatus,
        pose: { ...poseStatusFixture(peerArmFixture("not_requested")), phase: "warming" },
      }),
    /pose status phase is unsupported/,
  );
  const sessionSummary = {
    session_id: "standby-diagnostic-001",
    state: "ready",
    created_at_utc: "2026-08-17T22:00:00Z",
    error: "",
  } as const;
  assert.deepEqual(parseSessionSummary(sessionSummary), sessionSummary);
  assert.deepEqual(parseSessionSummary({ ...sessionSummary, session_kind: "standby_diagnostic" }), {
    ...sessionSummary,
    session_kind: "standby_diagnostic",
  });
  assert.throws(
    () => parseSessionSummary({ ...sessionSummary, session_kind: "unknown" }),
    /Unsupported session_kind/,
  );
  assert.throws(
    () =>
      parseCaptureStatus({
        ...captureStatus,
        state: "ready",
        armed: undefined,
      }),
    /capture armed must be a boolean/,
  );
  assert.throws(
    () => parseCaptureStatus({ ...captureStatus, hil: undefined }),
    /capture hil status must be an object/,
  );
  assert.deepEqual(parseClipManifest(FIXTURE_MANIFEST), FIXTURE_MANIFEST);
  const androidManifest = structuredClone(FIXTURE_MANIFEST);
  androidManifest.android_capture = {
    node_id: "pixel-node",
    shared_session_id: "shared-session-001",
    trigger_timestamp_uncertainty_ns: 250_000,
    peer_arm: peerArmFixture("accepted"),
  };
  assert.deepEqual(
    parseClipManifest(androidManifest).android_capture?.peer_arm,
    peerArmFixture("accepted"),
  );
  assert.equal(
    parseClipManifest(FIXTURE_MANIFEST).pipeline_profile?.capture.confirmation_to_acceptance_ms,
    -0.4,
  );
  assert.equal(
    parseClipManifest(FIXTURE_MANIFEST).pipeline_profile?.capture.freeze_schedule_lateness_ms,
    -0.2,
  );
  assert.equal(
    parseClipManifest(FIXTURE_MANIFEST).pipeline_profile?.session.prepublication_analysis_ms,
    4.6,
  );
  const legacySoftwareProfile = structuredClone(FIXTURE_MANIFEST) as unknown as {
    pipeline_profile: {
      schema_version: number;
      views: Array<Record<string, unknown>>;
    };
  };
  legacySoftwareProfile.pipeline_profile.schema_version = 1;
  for (const view of legacySoftwareProfile.pipeline_profile.views) {
    view.rgb_to_i420_ms = view.rgb_to_yuv420_ms;
    view.vp8_encode_ms = view.codec_encode_ms;
    delete view.rgb_to_yuv420_ms;
    delete view.codec_encode_ms;
  }
  const parsedLegacySoftwareProfile = parseClipManifest(legacySoftwareProfile).pipeline_profile;
  assert.equal(parsedLegacySoftwareProfile?.schema_version, 1);
  assert.equal(parsedLegacySoftwareProfile?.views[0]?.rgb_to_yuv420_ms, 9.1);
  assert.equal(parsedLegacySoftwareProfile?.views[0]?.codec_encode_ms, 43.8);
  const manifestWithoutProfile = structuredClone(FIXTURE_MANIFEST);
  delete manifestWithoutProfile.pipeline_profile;
  assert.deepEqual(parseClipManifest(manifestWithoutProfile), manifestWithoutProfile);

  const singleNode = structuredClone(manifestWithoutProfile);
  const singleNodeTrack = singleNode.views[0];
  assert.ok(singleNodeTrack);
  singleNodeTrack.source.pixel_format = "camera2_private";
  singleNode.views = [singleNodeTrack];
  assert.deepEqual(parseClipManifest(singleNode), singleNode);

  const emptyManifest = structuredClone(manifestWithoutProfile);
  emptyManifest.views = [];
  assert.throws(() => parseClipManifest(emptyManifest), /one or two unique camera roles/);

  const unsupported = structuredClone(FIXTURE_MANIFEST) as unknown as Record<string, unknown>;
  unsupported.schema_version = 2;
  assert.throws(() => parseClipManifest(unsupported), /Unsupported review schema/);

  const duplicateRole = structuredClone(FIXTURE_MANIFEST);
  const duplicateView = duplicateRole.views[1];
  assert.ok(duplicateView);
  duplicateView.role = "down_the_line";
  assert.throws(() => parseClipManifest(duplicateRole), /one or two unique camera roles/);

  const nonmonotonic = structuredClone(FIXTURE_MANIFEST);
  const firstView = nonmonotonic.views[0];
  assert.ok(firstView);
  const thirdFrame = firstView.frames[2];
  assert.ok(thirdFrame);
  thirdFrame.media_time_us = 0;
  assert.throws(() => parseClipManifest(nonmonotonic), /strictly increasing/);

  const unsafeMedia = structuredClone(FIXTURE_MANIFEST);
  const unsafeView = unsafeMedia.views[0];
  assert.ok(unsafeView);
  unsafeView.media.path = "../outside.webm";
  assert.throws(() => parseClipManifest(unsafeMedia), /safe relative artifact path/);

  const incompleteTrigger = structuredClone(FIXTURE_MANIFEST) as unknown as {
    trigger: Record<string, unknown>;
  };
  delete incompleteTrigger.trigger.confirmation_host_monotonic_time_ns;
  assert.throws(() => parseClipManifest(incompleteTrigger), /confirmation_host_monotonic_time_ns/);

  const hilManifest = structuredClone(FIXTURE_MANIFEST);
  hilManifest.hil_evidence = structuredClone(FIXTURE_SYNTHETIC_HIL_EVIDENCE);
  assert.deepEqual(parseClipManifest(hilManifest), hilManifest);
  const extendedEvidence = structuredClone(hilManifest) as unknown as {
    hil_evidence: Record<string, unknown> & { tone: Record<string, unknown> };
  };
  extendedEvidence.hil_evidence.brightness = { baseline: 18, peak: 241 };
  extendedEvidence.hil_evidence.tone.waveform = "sine";
  assert.doesNotThrow(() => parseClipManifest(extendedEvidence));
  hilManifest.hil_evidence.optical_white_impact_frame_index.face_on = 90;
  assert.throws(() => parseClipManifest(hilManifest), /optical frame is outside/);

  const inconsistentWhiteEvidence = structuredClone(FIXTURE_MANIFEST);
  inconsistentWhiteEvidence.hil_evidence = structuredClone(FIXTURE_SYNTHETIC_HIL_EVIDENCE);
  inconsistentWhiteEvidence.hil_evidence.optical_white_impact.down_the_line.matching_fraction = 0.5;
  assert.throws(
    () => parseClipManifest(inconsistentWhiteEvidence),
    /disagrees with its frame counts/,
  );

  const unsupportedPipeline = structuredClone(FIXTURE_MANIFEST) as unknown as {
    pipeline_profile: { schema_version: number };
  };
  unsupportedPipeline.pipeline_profile.schema_version = 2;
  assert.throws(() => parseClipManifest(unsupportedPipeline), /pipeline profile schema/);

  const mismatchedPipelineFrames = structuredClone(FIXTURE_MANIFEST);
  const firstProfileView = mismatchedPipelineFrames.pipeline_profile?.views[0];
  assert.ok(firstProfileView !== undefined);
  firstProfileView.frame_count = 89;
  assert.throws(() => parseClipManifest(mismatchedPipelineFrames), /frame_count disagrees/);

  const duplicatePipelineRole = structuredClone(FIXTURE_MANIFEST);
  const secondProfileView = duplicatePipelineRole.pipeline_profile?.views[1];
  assert.ok(secondProfileView !== undefined);
  secondProfileView.role = "down_the_line";
  assert.throws(() => parseClipManifest(duplicatePipelineRole), /each camera role/);

  const negativePublisherPlanning = structuredClone(FIXTURE_MANIFEST);
  assert.ok(negativePublisherPlanning.pipeline_profile !== undefined);
  negativePublisherPlanning.pipeline_profile.session.publisher_planning_ms = -0.1;
  assert.throws(() => parseClipManifest(negativePublisherPlanning), /publisher_planning_ms/);

  const negativeImpactPreview = structuredClone(FIXTURE_MANIFEST);
  assert.ok(negativeImpactPreview.pipeline_profile !== undefined);
  negativeImpactPreview.pipeline_profile.session.impact_preview_render_ms = -0.1;
  assert.throws(() => parseClipManifest(negativeImpactPreview), /impact_preview_render_ms/);

  const negativePrepublicationAnalysis = structuredClone(FIXTURE_MANIFEST);
  assert.ok(negativePrepublicationAnalysis.pipeline_profile !== undefined);
  negativePrepublicationAnalysis.pipeline_profile.session.prepublication_analysis_ms = -0.1;
  assert.throws(
    () => parseClipManifest(negativePrepublicationAnalysis),
    /prepublication_analysis_ms/,
  );

  const invalidProfileSnapshot = structuredClone(FIXTURE_MANIFEST);
  assert.ok(invalidProfileSnapshot.pipeline_profile !== undefined);
  invalidProfileSnapshot.pipeline_profile.session.profile_snapshot_host_monotonic_ns = "12ms";
  assert.throws(() => parseClipManifest(invalidProfileSnapshot), /unsigned decimal string/);

  const persistedDelivery = structuredClone(FIXTURE_MANIFEST) as ClipManifest;
  persistedDelivery.client_delivery_profile = {
    manifest_fetch_duration_ms: 1,
    manifest_response_received_performance_ms: 2,
    server_response_host_monotonic_ns: "3",
  };
  assert.throws(() => parseClipManifest(persistedDelivery), /transport-derived/);

  const persistedExternalUrl = structuredClone(FIXTURE_MANIFEST) as unknown as {
    views: Array<{ media: Record<string, unknown> }>;
  };
  const externalView = persistedExternalUrl.views[0];
  assert.ok(externalView !== undefined);
  externalView.media.url = "https://external.invalid/clip.webm";
  assert.throws(() => parseClipManifest(persistedExternalUrl), /must not be persisted/);
}

function peerArmFixture(state: PeerArmState): PeerArmStatus {
  return {
    state,
    shared_session_id: state === "not_requested" ? null : "shared-session-001",
    http_status: state === "accepted" ? 202 : state === "rejected" ? 409 : null,
    failure_type: state === "failed" ? "java.io.IOException" : null,
  };
}

function poseStatusFixture(peerArm: PeerArmStatus): PoseCaptureStatus {
  return {
    mode: "leader",
    phase: "monitoring",
    transition_requested: false,
    peer_arm: peerArm,
  };
}

async function testHttpContract() {
  const calls: Array<{ url: string; init: RequestInit | undefined }> = [];
  const capture: CaptureStatus = {
    schema_version: CAPTURE_SCHEMA_VERSION,
    state: "armed",
    armed: true,
    active_session_id: null,
    error: "",
    hil: {
      enabled: false,
      busy: false,
      stage: "idle",
      error: "",
      last_run: null,
    },
  };
  const session: SessionSummary = {
    session_id: FIXTURE_MANIFEST.session_id,
    state: "ready",
    created_at_utc: FIXTURE_MANIFEST.created_at_utc,
    error: "",
  };
  const missedShotSession: SessionSummary = {
    ...session,
    session_id: "standby-missed-shot-001",
    session_kind: "standby_diagnostic",
  };
  const sessions: SessionList = { schema_version: 1, sessions: [session] };
  const fetcher = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = String(input);
    calls.push({ url, init });
    if (url.endsWith("/capture/status") || url.endsWith("/capture/arm")) {
      return Response.json(capture);
    }
    if (url.endsWith("/capture/manual")) {
      return Response.json(session);
    }
    if (url.endsWith("/capture/missed-shot")) {
      return Response.json(missedShotSession);
    }
    if (url.endsWith("/hil/synthetic-swing")) {
      return Response.json(capture);
    }
    if (url.endsWith("/manifest")) {
      return Response.json(FIXTURE_MANIFEST, {
        headers: {
          "X-Swing-Capture-Server-Monotonic-Ns": "458500000000",
          "X-Swing-Capture-Media-Access":
            "media_access=abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ",
        },
      });
    }
    if (url.endsWith("/feedback")) {
      return new Response(null, { status: 204 });
    }
    if (url.endsWith("/diagnostics.zip")) {
      return new Response(new Blob(["diagnostic evidence"], { type: "application/zip" }), {
        headers: { "Content-Disposition": 'attachment; filename="station-evidence.zip"' },
      });
    }
    return Response.json(sessions);
  }) as typeof fetch;

  const manifestClock = [100, 112.5];
  const api = new HttpReviewApi(
    "http://station.test/",
    fetcher,
    () => {
      const now = manifestClock.shift();
      assert.ok(now !== undefined);
      return now;
    },
    "test-control-token",
  );
  const unsubscribe = api.subscribeToChanges(() => undefined);
  assert.ok(unsubscribe);
  unsubscribe();
  assert.equal((await api.getCaptureStatus()).armed, true);
  await api.setArmed(false);
  assert.deepEqual(JSON.parse(String(calls[1]?.init?.body)), { armed: false });
  assert.equal(
    new Headers(calls[1]?.init?.headers).get("Authorization"),
    "Bearer test-control-token",
  );
  assert.equal((await api.triggerManualCapture()).session_id, session.session_id);
  assert.deepEqual(await api.saveMissedShot(), missedShotSession);
  assert.equal((await api.startSyntheticSwing()).hil.enabled, false);
  assert.deepEqual(JSON.parse(String(calls[4]?.init?.body)), {});
  assert.equal((await api.getSessions()).sessions.length, 1);
  const manifest = await api.getManifest(session.session_id);
  assert.equal(
    manifest.views[0]?.media.url,
    `http://station.test/api/v1/sessions/${session.session_id}/down-the-line.webm?media_access=abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ`,
  );
  assert.deepEqual(manifest.client_delivery_profile, {
    manifest_fetch_duration_ms: 12.5,
    manifest_response_received_performance_ms: 112.5,
    server_response_host_monotonic_ns: "458500000000",
  });
  assert.equal(
    new Headers(calls[6]?.init?.headers).get("Authorization"),
    "Bearer test-control-token",
    "manifest metadata must be ready for read authorization",
  );
  await assert.rejects(
    () =>
      api.submitDiagnosticFeedback(session.session_id, {
        schema_version: DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
        classification: "other",
        note: "x".repeat(501),
      }),
    /1 to 500 characters/,
  );
  const feedback = {
    schema_version: DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
    classification: "av_sync_wrong",
    note: "Audio follows visual impact",
    timing_marks_us: { visual_impact_us: 4_500, audio_impact_us: 12_000 },
  } as const;
  await api.submitDiagnosticFeedback(session.session_id, feedback);
  assert.deepEqual(JSON.parse(String(calls[7]?.init?.body)), feedback);
  assert.equal(
    new Headers(calls[7]?.init?.headers).get("Authorization"),
    "Bearer test-control-token",
  );
  const archives = await api.getDiagnosticArchives(session.session_id);
  assert.equal(archives.length, 1);
  assert.equal(archives[0]?.filename, "station-evidence.zip");
  assert.equal(await archives[0]?.data.text(), "diagnostic evidence");
  assert.equal(
    new Headers(calls[8]?.init?.headers).get("Authorization"),
    "Bearer test-control-token",
  );
  assert.deepEqual(
    calls.map((call) => call.url),
    [
      "http://station.test/api/v1/capture/status",
      "http://station.test/api/v1/capture/arm",
      "http://station.test/api/v1/capture/manual",
      "http://station.test/api/v1/capture/missed-shot",
      "http://station.test/api/v1/hil/synthetic-swing",
      "http://station.test/api/v1/sessions",
      `http://station.test/api/v1/sessions/${session.session_id}/manifest`,
      `http://station.test/api/v1/sessions/${session.session_id}/feedback`,
      `http://station.test/api/v1/sessions/${session.session_id}/diagnostics.zip`,
    ],
  );
  await api.setArmed(true, { allowDegradedNetwork: true });
  assert.deepEqual(JSON.parse(String(calls.at(-1)?.init?.body)), {
    armed: true,
    allow_degraded_network: true,
  });
  await api.setArmed(true, { allowDegradedNetwork: false });
  assert.deepEqual(
    JSON.parse(String(calls.at(-1)?.init?.body)),
    { armed: true },
    "the optional degraded acknowledgement is omitted unless explicitly true",
  );

  const malformedCapabilityApi = new HttpReviewApi(
    "http://station.test",
    async () =>
      Response.json(FIXTURE_MANIFEST, {
        headers: { "X-Swing-Capture-Media-Access": "media_access=not-a-valid-capability" },
      }),
    () => 0,
    "test-control-token",
  );
  await assert.rejects(
    () => malformedCapabilityApi.getManifest(session.session_id),
    /invalid scoped media capability/,
  );
}

function installDomGlobals(window: DOMWindow) {
  const globals = globalThis as unknown as Record<string, unknown>;
  globals.window = window;
  globals.document = window.document;
  globals.location = window.location;
  globals.history = window.history;
  Object.defineProperty(globalThis, "navigator", {
    configurable: true,
    value: window.navigator,
  });
  globals.HTMLElement = window.HTMLElement;
  globals.HTMLInputElement = window.HTMLInputElement;
  globals.HTMLMediaElement = window.HTMLMediaElement;
  globals.MutationObserver = window.MutationObserver;
  globals.Node = window.Node;
  globals.getComputedStyle = window.getComputedStyle.bind(window);
  globals.IS_REACT_ACT_ENVIRONMENT = true;
}

interface FirefoxPresentationFixture {
  readonly playCount: number;
  readonly pauseCount: number;
  present(video: HTMLVideoElement, mediaTime: number): void;
  restore(): void;
}

function installFirefoxPresentationFixture(window: DOMWindow): FirefoxPresentationFixture {
  const mediaPrototype = window.HTMLMediaElement.prototype;
  const videoPrototype = window.HTMLVideoElement.prototype;
  const originalPlay = Object.getOwnPropertyDescriptor(mediaPrototype, "play");
  const originalPause = Object.getOwnPropertyDescriptor(mediaPrototype, "pause");
  const originalPaused = Object.getOwnPropertyDescriptor(mediaPrototype, "paused");
  const originalRequest = Object.getOwnPropertyDescriptor(
    videoPrototype,
    "requestVideoFrameCallback",
  );
  const originalCancel = Object.getOwnPropertyDescriptor(
    videoPrototype,
    "cancelVideoFrameCallback",
  );
  const originalUserAgent = Object.getOwnPropertyDescriptor(window.navigator, "userAgent");
  const playing = new WeakSet<HTMLMediaElement>();
  const callbacks = new WeakMap<HTMLVideoElement, Map<number, VideoFrameRequestCallback>>();
  let nextCallbackId = 1;
  let playCount = 0;
  let pauseCount = 0;

  Object.defineProperty(window.navigator, "userAgent", {
    configurable: true,
    value: "Mozilla/5.0 Firefox/153.0",
  });
  Object.defineProperty(mediaPrototype, "paused", {
    configurable: true,
    get(this: HTMLMediaElement) {
      return !playing.has(this);
    },
  });
  Object.defineProperty(mediaPrototype, "play", {
    configurable: true,
    value(this: HTMLMediaElement) {
      playCount += 1;
      playing.add(this);
      this.dispatchEvent(new window.Event("play"));
      return Promise.resolve();
    },
  });
  Object.defineProperty(mediaPrototype, "pause", {
    configurable: true,
    value(this: HTMLMediaElement) {
      pauseCount += 1;
      playing.delete(this);
      this.dispatchEvent(new window.Event("pause"));
    },
  });
  Object.defineProperty(videoPrototype, "requestVideoFrameCallback", {
    configurable: true,
    value(this: HTMLVideoElement, callback: VideoFrameRequestCallback) {
      const callbackId = nextCallbackId;
      nextCallbackId += 1;
      const pending = callbacks.get(this) ?? new Map<number, VideoFrameRequestCallback>();
      pending.set(callbackId, callback);
      callbacks.set(this, pending);
      return callbackId;
    },
  });
  Object.defineProperty(videoPrototype, "cancelVideoFrameCallback", {
    configurable: true,
    value(this: HTMLVideoElement, callbackId: number) {
      callbacks.get(this)?.delete(callbackId);
    },
  });

  const restoreDescriptor = (
    owner: object,
    name: string,
    descriptor: PropertyDescriptor | undefined,
  ) => {
    if (descriptor === undefined) {
      Reflect.deleteProperty(owner, name);
    } else {
      Object.defineProperty(owner, name, descriptor);
    }
  };
  return {
    get playCount() {
      return playCount;
    },
    get pauseCount() {
      return pauseCount;
    },
    present(video, mediaTime) {
      const pending = callbacks.get(video);
      assert.ok(pending !== undefined, "video has no pending presentation callback");
      const next = pending.entries().next();
      assert.ok(!next.done, "video has no pending presentation callback");
      const [callbackId, callback] = next.value;
      pending.delete(callbackId);
      const now = performance.now();
      callback(now, {
        expectedDisplayTime: now,
        height: 180,
        mediaTime,
        presentationTime: now,
        presentedFrames: 1,
        processingDuration: 0,
        width: 320,
      });
    },
    restore() {
      restoreDescriptor(mediaPrototype, "play", originalPlay);
      restoreDescriptor(mediaPrototype, "pause", originalPause);
      restoreDescriptor(mediaPrototype, "paused", originalPaused);
      restoreDescriptor(videoPrototype, "requestVideoFrameCallback", originalRequest);
      restoreDescriptor(videoPrototype, "cancelVideoFrameCallback", originalCancel);
      restoreDescriptor(window.navigator, "userAgent", originalUserAgent);
    },
  };
}

function installMediaFixture(window: DOMWindow) {
  Object.defineProperty(window.HTMLMediaElement.prototype, "play", {
    configurable: true,
    value(this: HTMLMediaElement) {
      this.dispatchEvent(new window.Event("play"));
      return Promise.resolve();
    },
  });
  Object.defineProperty(window.HTMLMediaElement.prototype, "pause", {
    configurable: true,
    value(this: HTMLMediaElement) {
      this.dispatchEvent(new window.Event("pause"));
    },
  });
}

function installDownloadFixture(window: DOMWindow): string[] {
  const names: string[] = [];
  Object.defineProperty(URL, "createObjectURL", {
    configurable: true,
    value: () => "blob:fixture-diagnostics",
  });
  Object.defineProperty(URL, "revokeObjectURL", {
    configurable: true,
    value: () => undefined,
  });
  Object.defineProperty(window.HTMLAnchorElement.prototype, "click", {
    configurable: true,
    value(this: HTMLAnchorElement) {
      names.push(this.download);
    },
  });
  return names;
}

class RecordingReviewApi extends FakeReviewApi {
  readonly feedback: Array<{ sessionId: string; feedback: DiagnosticFeedback }> = [];
  readonly archiveRequests: string[] = [];

  override async submitDiagnosticFeedback(
    sessionId: string,
    feedback: DiagnosticFeedback,
  ): Promise<void> {
    this.feedback.push({ sessionId, feedback: structuredClone(feedback) });
  }

  override async getDiagnosticArchives(sessionId: string): Promise<readonly DiagnosticArchive[]> {
    this.archiveRequests.push(sessionId);
    return [
      {
        filename: "fixture-diagnostics.zip",
        data: new Blob(["diagnostics"], { type: "application/zip" }),
      },
    ];
  }
}

class DeferredManifestReviewApi extends FakeReviewApi {
  readonly #sessions: SessionSummary[] = [
    {
      session_id: "ready-session-a",
      state: "ready",
      created_at_utc: "2026-08-22T20:01:00Z",
      error: "",
    },
    {
      session_id: "ready-session-b",
      state: "ready",
      created_at_utc: "2026-08-22T20:00:00Z",
      error: "",
    },
  ];
  readonly #manifests = new Map(
    this.#sessions.map((session) => [session.session_id, new DeferredValue<ClipManifest>()]),
  );

  override getSessions(): Promise<SessionList> {
    return Promise.resolve({ schema_version: 1, sessions: structuredClone(this.#sessions) });
  }

  override getManifest(sessionId: string): Promise<ClipManifest> {
    const deferred = this.#manifests.get(sessionId);
    if (deferred === undefined) {
      return Promise.reject(new Error(`Unknown deferred session ${sessionId}`));
    }
    return deferred.promise;
  }

  resolve(sessionId: string): void {
    const deferred = this.#manifests.get(sessionId);
    assert.ok(deferred);
    const session = this.#sessions.find((candidate) => candidate.session_id === sessionId);
    assert.ok(session);
    const manifest = structuredClone(FIXTURE_MANIFEST);
    manifest.session_id = sessionId;
    manifest.created_at_utc = session.created_at_utc;
    for (const view of manifest.views) {
      view.media.path = `${sessionId}-${view.role}.webm`;
      delete view.media.url;
    }
    deferred.resolve(manifest);
  }

  reject(sessionId: string, message: string): void {
    const deferred = this.#manifests.get(sessionId);
    assert.ok(deferred);
    deferred.reject(new Error(message));
  }
}

class DeferredValue<T> {
  readonly promise: Promise<T>;
  readonly resolve: (value: T) => void;
  readonly reject: (reason: unknown) => void;

  constructor() {
    let resolveValue: ((value: T) => void) | undefined;
    let rejectValue: ((reason: unknown) => void) | undefined;
    this.promise = new Promise<T>((resolve, reject) => {
      resolveValue = resolve;
      rejectValue = reject;
    });
    this.resolve = (value) => resolveValue?.(value);
    this.reject = (reason) => rejectValue?.(reason);
  }
}

class FieldRecordingReviewApi extends FakeReviewApi {
  startRequests = 0;
  stopRequests = 0;
  recording = false;
  completed = false;
  failed = false;

  getFieldRecordingStatus(): Promise<DualFieldRecordingStatus> {
    return Promise.resolve(this.#status());
  }

  startFieldRecording(): Promise<DualFieldRecordingStatus> {
    this.startRequests += 1;
    this.failed = false;
    this.recording = true;
    return Promise.resolve(this.#status());
  }

  stopFieldRecording(): Promise<DualFieldRecordingStatus> {
    this.stopRequests += 1;
    this.recording = false;
    this.completed = true;
    return Promise.resolve(this.#status("ready"));
  }

  getFieldRecordings(): Promise<FieldRecordingList> {
    return Promise.resolve({
      recordings: this.completed
        ? (["down_the_line", "face_on", "down_the_line", "face_on"] as const).map(
            (role, index) => ({
              recording_id: `field-${role}-${String(index)}`,
              shared_recording_id: "field-shared",
              created_at_utc: `2026-08-22T18:00:0${String(index)}Z`,
              role,
              origin: role === "down_the_line" ? "http://dtl.test" : "http://face.test",
              duration_us: "12000000",
              video_bytes: "1200000",
              audio_frames: "576000",
              video_url: `http://${role}.test/${String(index)}/video.mp4`,
              audio_url: `http://${role}.test/${String(index)}/audio.wav`,
              manifest_url: `http://${role}.test/${String(index)}/manifest`,
            }),
          )
        : [],
    });
  }

  #status(
    state: FieldRecordingState = this.failed ? "error" : this.recording ? "recording" : "idle",
  ): DualFieldRecordingStatus {
    return {
      nodes: (["down_the_line", "face_on"] as const).map((role) => ({
        schema_version: 1,
        role,
        origin: role === "down_the_line" ? "http://dtl.test" : "http://face.test",
        state,
        active_recording_id: this.recording ? `field-${role}` : null,
        shared_recording_id: this.recording ? "field-shared" : null,
        started_at_utc: this.recording ? "2026-08-22T18:00:00Z" : null,
        started_elapsed_realtime_ns: this.recording ? "123456789" : null,
        elapsed_ms: this.recording ? 12_000 : 0,
        video_bytes: this.recording ? "1200000" : "0",
        audio_frames: this.recording ? "576000" : "0",
        max_duration_seconds: 600,
        error: this.failed ? "camera pipeline unavailable" : "",
      })),
    };
  }
}

class DelayedHistoryFieldRecordingReviewApi extends FieldRecordingReviewApi {
  readonly #sessions = new DeferredValue<SessionList>();
  readonly #catalog = new DeferredValue<FieldRecordingList>();
  statusRequests = 0;
  catalogRequests = 0;

  override getSessions(): Promise<SessionList> {
    return this.#sessions.promise;
  }

  override getFieldRecordingStatus(): Promise<DualFieldRecordingStatus> {
    this.statusRequests += 1;
    return super.getFieldRecordingStatus();
  }

  override getFieldRecordings(): Promise<FieldRecordingList> {
    this.catalogRequests += 1;
    return this.#catalog.promise;
  }

  async resolveHistory(): Promise<void> {
    this.#sessions.resolve({ schema_version: 1, sessions: [] });
    this.#catalog.resolve(await super.getFieldRecordings());
  }
}

class MissedShotReviewApi extends RecordingReviewApi {
  override async getManifest(sessionId: string): Promise<ClipManifest> {
    const manifest = await super.getManifest(sessionId);
    manifest.trigger.source = "missed_shot";
    return manifest;
  }
}

class StandbyDiagnosticReviewApi extends RecordingReviewApi {
  manifestRequests = 0;

  override getSessions(): Promise<SessionList> {
    return Promise.resolve({
      schema_version: 1,
      sessions: [
        {
          session_id: "standby-diagnostic-001",
          state: "ready",
          created_at_utc: "2026-08-17T22:00:00Z",
          error: "",
          session_kind: "standby_diagnostic",
        },
      ],
    });
  }

  override getManifest(): Promise<ClipManifest> {
    this.manifestRequests += 1;
    return Promise.reject(new Error("standby diagnostics must not request a clip manifest"));
  }
}

class PendingStandbyDiagnosticReviewApi extends RecordingReviewApi {
  readonly standbyDiagnosticPublicationTimeoutMs = 5_000;
  manifestRequests = 0;
  published = false;
  sessionPolls = 0;

  override getSessions(): Promise<SessionList> {
    this.sessionPolls += 1;
    return Promise.resolve({
      schema_version: 1,
      sessions: this.published
        ? [
            {
              ...pendingStandbySession(),
              state: "ready",
            },
          ]
        : [],
    });
  }

  override saveMissedShot(): Promise<SessionSummary> {
    return Promise.resolve(pendingStandbySession());
  }

  override getManifest(): Promise<ClipManifest> {
    this.manifestRequests += 1;
    return Promise.reject(new Error("standby diagnostics must not request a clip manifest"));
  }
}

function pendingStandbySession(): SessionSummary {
  return {
    session_id: "standby-pending-001",
    state: "waiting_post_roll",
    created_at_utc: "2026-08-17T22:01:00Z",
    error: "",
    session_kind: "standby_diagnostic",
  };
}

void main().catch((caught: unknown) => {
  console.error(caught);
  process.exit(1);
});
