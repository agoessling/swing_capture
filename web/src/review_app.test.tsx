import assert from "node:assert/strict";
import { JSDOM, type DOMWindow } from "jsdom";
import { Application } from "./application.js";
import { FakeStationApi } from "./fake_api.js";
import {
  FIXTURE_MANIFEST,
  FIXTURE_SYNTHETIC_HIL_EVIDENCE,
  FakeReviewApi,
} from "./fake_review_api.js";
import {
  CAPTURE_SCHEMA_VERSION,
  HttpReviewApi,
  parseCaptureStatus,
  parseClipManifest,
  type CaptureStatus,
  type ClipManifest,
  type ReviewApi,
  type SessionList,
  type SessionSummary,
} from "./review_api.js";
import { ReviewApp } from "./review_app.js";

const dom = new JSDOM('<!doctype html><html lang="en"><body></body></html>', {
  url: "http://station.test/#review",
});

installDomGlobals(dom.window);
installMediaFixture(dom.window);

async function main() {
  const { cleanup, fireEvent, render, screen, waitFor } = await import("@testing-library/react");
  const { default: axe } = await import("axe-core");

  const rendered = render(<ReviewApp api={new FakeReviewApi()} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByRole("heading", { name: "Swing review" }));
  assert.ok(await screen.findByText("Listening for an audio trigger"));
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
  assert.ok(Math.abs((videos[0]?.currentTime ?? 0) - 1.533318) < 0.000_001);
  assert.ok(Math.abs((videos[1]?.currentTime ?? 0) - 1.533318) < 0.000_001);

  const timeline = screen.getByRole("slider", { name: "Review timeline" }) as HTMLInputElement;
  fireEvent.input(timeline, { target: { value: "10" } });
  assert.ok(screen.getByText("Frame 11 of 90"));

  fireEvent.change(screen.getByRole("combobox", { name: "Playback speed" }), {
    target: { value: "0.5" },
  });
  assert.equal(videos[0]?.playbackRate, 0.5);
  assert.equal(videos[1]?.playbackRate, 0.5);

  fireEvent.click(screen.getByRole("button", { name: "Play" }));
  assert.ok(await screen.findByRole("button", { name: "Pause" }));
  fireEvent.click(screen.getByRole("button", { name: "Pause" }));
  assert.ok(screen.getByRole("button", { name: "Play" }));

  const accessibility = await axe.run(rendered.container, {
    rules: { "color-contrast": { enabled: false } },
  });
  assert.deepEqual(
    accessibility.violations.map((violation) => violation.id),
    [],
    "review fixture should have no automated accessibility violations",
  );
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

  render(<ReviewApp api={new FakeReviewApi()} pollIntervalMs={5} />);
  assert.ok(await screen.findByText("Listening for an audio trigger"));
  fireEvent.click(screen.getByRole("button", { name: "Manual diagnostic capture" }));
  assert.ok(await screen.findByRole("heading", { name: "Audio trigger detected" }));
  assert.equal(
    (screen.getByRole("button", { name: "Arm audio capture" }) as HTMLButtonElement).disabled,
    true,
  );
  assert.equal(
    (screen.getByRole("button", { name: "Manual diagnostic capture" }) as HTMLButtonElement)
      .disabled,
    true,
  );
  await waitFor(() => {
    assert.ok(screen.getByRole("heading", { name: "Ready" }));
    assert.ok(screen.getByText("Frame 46 of 90"));
    assert.ok(screen.getByRole("button", { name: "Arm audio capture" }));
    assert.equal(
      (screen.getByRole("button", { name: "Manual diagnostic capture" }) as HTMLButtonElement)
        .disabled,
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
    startSyntheticSwing: () => Promise.reject(new Error("capture service unavailable")),
    getSessions: () => Promise.reject(new Error("capture service unavailable")),
    getManifest: () => Promise.reject(new Error("capture service unavailable")),
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
    startSyntheticSwing: () => Promise.reject(new Error("not used")),
    getSessions: () => Promise.resolve({ schema_version: 1, sessions: [] }),
    getManifest: () => Promise.reject(new Error("not used")),
  };
  render(<ReviewApp api={hilFailureApi} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByRole("heading", { name: "Synthetic swing HIL failed" }));
  assert.ok(screen.getByRole("alert"));
  assert.ok(screen.getByText("Optical calibration did not find the Feather LED"));
  cleanup();

  testRuntimeSchemaRejection();
  await testHttpContract();
}

function testRuntimeSchemaRejection() {
  const captureStatus = {
    schema_version: CAPTURE_SCHEMA_VERSION,
    state: "ready",
    armed: false,
    active_session_id: FIXTURE_MANIFEST.session_id,
    error: "",
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

  const unsupported = structuredClone(FIXTURE_MANIFEST) as unknown as Record<string, unknown>;
  unsupported.schema_version = 2;
  assert.throws(() => parseClipManifest(unsupported), /Unsupported review schema/);

  const duplicateRole = structuredClone(FIXTURE_MANIFEST);
  const duplicateView = duplicateRole.views[1];
  assert.ok(duplicateView);
  duplicateView.role = "down_the_line";
  assert.throws(() => parseClipManifest(duplicateRole), /one view for each camera role/);

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
  unsupportedPipeline.pipeline_profile.schema_version = 3;
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
    if (url.endsWith("/hil/synthetic-swing")) {
      return Response.json(capture);
    }
    if (url.endsWith("/manifest")) {
      return Response.json(FIXTURE_MANIFEST, {
        headers: { "X-Swing-Capture-Server-Monotonic-Ns": "458500000000" },
      });
    }
    return Response.json(sessions);
  }) as typeof fetch;

  const manifestClock = [100, 112.5];
  const api = new HttpReviewApi("http://station.test/", fetcher, () => {
    const now = manifestClock.shift();
    assert.ok(now !== undefined);
    return now;
  });
  assert.equal((await api.getCaptureStatus()).armed, true);
  await api.setArmed(false);
  assert.deepEqual(JSON.parse(String(calls[1]?.init?.body)), { armed: false });
  assert.equal((await api.triggerManualCapture()).session_id, session.session_id);
  assert.equal((await api.startSyntheticSwing()).hil.enabled, false);
  assert.deepEqual(JSON.parse(String(calls[3]?.init?.body)), {});
  assert.equal((await api.getSessions()).sessions.length, 1);
  const manifest = await api.getManifest(session.session_id);
  assert.equal(
    manifest.views[0]?.media.url,
    `http://station.test/api/v1/sessions/${session.session_id}/down-the-line.webm`,
  );
  assert.deepEqual(manifest.client_delivery_profile, {
    manifest_fetch_duration_ms: 12.5,
    manifest_response_received_performance_ms: 112.5,
    server_response_host_monotonic_ns: "458500000000",
  });
  assert.deepEqual(
    calls.map((call) => call.url),
    [
      "http://station.test/api/v1/capture/status",
      "http://station.test/api/v1/capture/arm",
      "http://station.test/api/v1/capture/manual",
      "http://station.test/api/v1/hil/synthetic-swing",
      "http://station.test/api/v1/sessions",
      `http://station.test/api/v1/sessions/${session.session_id}/manifest`,
    ],
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

void main().catch((caught: unknown) => {
  console.error(caught);
  process.exit(1);
});
