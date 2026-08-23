import assert from "node:assert/strict";
import { JSDOM, type DOMWindow } from "jsdom";
import {
  App,
  loadPreviewPipelineStall,
  PREVIEW_STALL_STORAGE_KEY,
  PREVIEW_STALL_TTL_MS,
  previewStationIdentity,
  retainPreviewPipelineStall,
  storePreviewPipelineStall,
} from "./app.js";
import {
  HttpStationApi,
  parseStationStatus,
  type CameraStatus,
  type FetchedPreview,
  type PreviewFetchTelemetry,
  type PreviewServerTelemetry,
  type StationApi,
} from "./api.js";
import { MutableControlCredential } from "./control_credential.js";
import { FakeStationApi, FIXTURE_STATUS } from "./fake_api.js";
import { FakeNodeSetupApi, fixtureNodeSetup } from "./fake_node_setup_api.js";
import type { StatusSubscriptionScheduler } from "./live_status.js";
import { HttpNodeSetupApi, parseNodeSetupSnapshot } from "./node_setup_api.js";
import { NodeSetupApp } from "./node_setup_app.js";
import { HttpReviewApi } from "./review_api.js";
import {
  attributePreviewDelay,
  attributePreviewPipelineStall,
  type ObjectUrlFactory,
  PairedPreviewLoader,
  type PreviewImageDecoder,
  type PreviewPair,
} from "./paired_preview.js";

const dom = new JSDOM('<!doctype html><html lang="en"><body></body></html>', {
  url: "http://station.test/setup/cameras",
});

installDomGlobals(dom.window);
dom.window.sessionStorage.clear();

async function main() {
  const { cleanup, fireEvent, render, screen, waitFor, within } = await import(
    "@testing-library/react"
  );
  const { default: axe } = await import("axe-core");

  const api = new FakeStationApi();
  const rendered = render(<App api={api} pollIntervalMs={60_000} />);

  assert.ok(await screen.findByRole("heading", { name: "Camera setup" }));
  assert.ok(await screen.findByText("FDN22120654", { exact: false }));
  assert.ok(screen.getByText("FDN23010199", { exact: false }));
  assert.equal((await screen.findAllByRole("img")).length, 2);
  assert.ok(screen.getByText("Both configured camera roles are online."));
  assert.ok(screen.getByText("Face-on image needs attention."));
  await flushAsyncWork();
  fireEvent.error(screen.getByRole("img", { name: "Down-the-line live setup preview" }));
  assert.ok(screen.getAllByText("Preview update delayed").length > 0);

  const downTheLineCard = requiredElement(
    screen.getByText("Down-the-line").closest("article"),
    "down-the-line camera card",
  );
  const card = within(downTheLineCard);
  assert.ok(card.getByRole("heading", { name: "Down-the-line" }));
  assert.equal(
    card.getByRole("link", { name: /Open full resolution/ }).getAttribute("href"),
    "fixtures/down-the-line.svg?sequence=18&full=1",
  );
  const exposure = card.getByRole("slider", {
    name: "Down-the-line exposure",
  }) as HTMLInputElement;
  const apply = card.getByRole("button", { name: "Apply settings" });
  const revert = card.getByRole("button", { name: "Revert" });

  assert.equal(exposure.value, "1500");
  assert.equal(apply.hasAttribute("disabled"), true);
  fireEvent.input(exposure, { target: { value: "1770" } });
  assert.equal(exposure.value, "1770");
  assert.equal(apply.hasAttribute("disabled"), false);
  fireEvent.click(revert);
  assert.equal(exposure.value, "1500");

  fireEvent.input(exposure, { target: { value: "1770" } });
  fireEvent.click(apply);
  await waitFor(() => {
    assert.match(card.getByText(/Camera read-back:/).textContent ?? "", /1770 µs/);
  });
  assert.equal(
    api.snapshot().cameras.find((camera) => camera.role === "down_the_line")?.exposure_us.value,
    1770,
  );

  const accessibility = await axe.run(rendered.container, {
    rules: { "color-contrast": { enabled: false } },
  });
  assert.deepEqual(
    accessibility.violations.map((violation) => violation.id),
    [],
    "component fixture should have no automated accessibility violations",
  );
  cleanup();

  const retainedStall = {
    ...fixturePipelineStall("capture"),
    observed_at_epoch_ms: Date.now(),
  };
  const stationIdentity = previewStationIdentity(
    FIXTURE_STATUS.service_instance_id,
    FIXTURE_STATUS.cameras,
  );
  assert.ok(stationIdentity);
  const retainedEnvelope = retainPreviewPipelineStall(stationIdentity, retainedStall);
  storePreviewPipelineStall(retainedEnvelope);
  assert.deepEqual(loadPreviewPipelineStall(stationIdentity), retainedEnvelope);
  render(<App api={new FakeStationApi()} pollIntervalMs={60_000} />);
  fireEvent.click(await screen.findByText("Preview timing"));
  assert.ok(screen.getByText(/Retained server evidence: camera acquisition/));
  assert.ok(
    screen.getByText(
      /Retention is scoped to this station service instance and expires after six hours/,
    ),
  );
  assert.ok(screen.getByText(/stage evidence, not causal proof/));
  cleanup();
  dom.window.sessionStorage.clear();

  storePreviewPipelineStall(retainedEnvelope);
  const restartedStationIdentity = previewStationIdentity(
    "fixture-service-instance-after-restart",
    FIXTURE_STATUS.cameras,
  );
  assert.ok(restartedStationIdentity);
  assert.equal(loadPreviewPipelineStall(restartedStationIdentity), null);
  assert.equal(dom.window.sessionStorage.getItem(PREVIEW_STALL_STORAGE_KEY), null);
  const expiredEnvelope = {
    ...retainedEnvelope,
    expires_at_epoch_ms: Date.now() - 1,
  };
  storePreviewPipelineStall(expiredEnvelope);
  assert.equal(loadPreviewPipelineStall(stationIdentity), null);
  const overlongEnvelope = {
    ...retainedEnvelope,
    expires_at_epoch_ms: Date.now() + PREVIEW_STALL_TTL_MS + 1,
  };
  storePreviewPipelineStall(overlongEnvelope);
  assert.equal(loadPreviewPipelineStall(stationIdentity), null);
  const futureObservedEnvelope = {
    ...retainedEnvelope,
    expires_at_epoch_ms: Date.now() + PREVIEW_STALL_TTL_MS,
    stall: {
      ...retainedEnvelope.stall,
      observed_at_epoch_ms: Date.now() + 120_000,
    },
  };
  storePreviewPipelineStall(futureObservedEnvelope);
  assert.equal(loadPreviewPipelineStall(stationIdentity), null);
  const mismatchedActiveKeyEnvelope = {
    ...retainedEnvelope,
    active_stall_key: "render|face_on|999",
  };
  storePreviewPipelineStall(mismatchedActiveKeyEnvelope);
  assert.equal(loadPreviewPipelineStall(stationIdentity), null);

  const previewFixture = new FakeStationApi();
  let unchangedStatus = fixturePipelineStatus("sampling");
  let unchangedStatusPolls = 0;
  const unchangedStatusApi: StationApi = {
    getStatus: async () => {
      ++unchangedStatusPolls;
      return structuredClone(unchangedStatus);
    },
    getPreview: (role, sequence) => previewFixture.getPreview(role, sequence),
    updateCameraSettings: (role, settings) => previewFixture.updateCameraSettings(role, settings),
    previewUrl: (role, sequence) => previewFixture.previewUrl(role, sequence),
    fullResolutionPreviewUrl: (role, sequence) =>
      previewFixture.fullResolutionPreviewUrl(role, sequence),
  };
  render(<App api={unchangedStatusApi} pollIntervalMs={5} />);
  fireEvent.click(await screen.findByText("Preview timing"));
  assert.ok(await screen.findByText(/Retained server evidence: latest-frame sampling/));
  await waitFor(() => {
    assert.match(
      dom.window.sessionStorage.getItem(PREVIEW_STALL_STORAGE_KEY) ?? "",
      /"stage":"sampling"/,
    );
  });
  const pollsBeforeResume = unchangedStatusPolls;
  unchangedStatus = structuredClone(FIXTURE_STATUS);
  await waitFor(() => assert.ok(unchangedStatusPolls > pollsBeforeResume));
  assert.ok(screen.getByText(/Retained server evidence: latest-frame sampling/));
  await waitFor(() => {
    assert.match(
      dom.window.sessionStorage.getItem(PREVIEW_STALL_STORAGE_KEY) ?? "",
      /"active_stall_key":null/,
    );
  });
  const repeatedSampling = fixturePipelineStatus("sampling");
  const repeatedCamera = repeatedSampling.cameras.find((camera) => camera.role === "down_the_line");
  assert.ok(repeatedCamera);
  repeatedCamera.preview_performance.latest_sink_completion_age_ms = 3_600;
  unchangedStatus = repeatedSampling;
  await waitFor(() => {
    assert.match(
      dom.window.sessionStorage.getItem(PREVIEW_STALL_STORAGE_KEY) ?? "",
      /"attributed_ms":3600/,
    );
  });
  cleanup();
  dom.window.sessionStorage.clear();

  const nodeSetupApi = new FakeNodeSetupApi(
    "http://pixel-6-pro.test:8088",
    "down_the_line",
    "Pixel 6 Pro",
    "leader",
    null,
  );
  const phoneSetup = render(<NodeSetupApp apis={[nodeSetupApi]} />);
  assert.ok(await screen.findByRole("heading", { name: "Phone setup" }));
  assert.ok(await screen.findByRole("heading", { name: "Resolve pose assignment" }));
  assert.ok(screen.getByText("Pixel 6 Pro", { exact: false }));
  assert.equal(
    (screen.getByRole("combobox", { name: "Camera view" }) as HTMLSelectElement).value,
    "down_the_line",
  );
  assert.ok(screen.getByText(/1280×720 · 240 fps · recommended/));
  assert.equal(
    (screen.getByRole("textbox", { name: "Peer phone origin" }) as HTMLInputElement).value,
    "",
  );
  assert.equal(
    (screen.getByLabelText("Peer control token") as HTMLInputElement).value,
    "",
    "stored peer credentials must never be returned to the browser",
  );
  assert.ok(screen.getByText("No peer association stored"));
  assert.ok(screen.getByText("Capture resources ready"));
  assert.ok(screen.getByText(/light thermal load.*0\.18 thermal headroom.*20\.0 GiB free/));
  fireEvent.change(screen.getByRole("textbox", { name: "Peer phone origin" }), {
    target: { value: "http://pixel-5a.test:8088" },
  });
  fireEvent.click(screen.getByRole("button", { name: "Save phone configuration" }));
  assert.ok(
    await screen.findByText(
      "Enter both the peer origin and its control token to replace the association.",
    ),
  );
  assert.equal((await nodeSetupApi.getSetup()).revision, 4);
  fireEvent.change(screen.getByLabelText("Peer control token"), {
    target: { value: "peer-control-token-never-returned" },
  });
  fireEvent.click(screen.getByRole("button", { name: "Save phone configuration" }));
  assert.ok(await screen.findByText("Phone configuration saved."));
  await waitFor(async () => {
    const savedSetup = await nodeSetupApi.getSetup();
    assert.equal(savedSetup.configuration.pose.peer?.origin, "http://pixel-5a.test:8088");
    assert.equal(savedSetup.revision, 5);
  });
  assert.ok(await screen.findByRole("heading", { name: "Phone configuration loaded" }));
  assert.equal(
    (screen.getByLabelText("Peer control token") as HTMLInputElement).value,
    "",
    "a newly written token must be cleared rather than rendered back",
  );
  fireEvent.click(screen.getByLabelText("Clear the existing peer association"));
  fireEvent.click(screen.getByRole("button", { name: "Save phone configuration" }));
  await waitFor(async () => {
    const clearedSetup = await nodeSetupApi.getSetup();
    assert.equal(clearedSetup.configuration.pose.peer, null);
    assert.equal(clearedSetup.revision, 6);
  });

  const externallyUpdated = await nodeSetupApi.getSetup();
  await nodeSetupApi.updateSetup(externallyUpdated.revision, {
    role: externallyUpdated.configuration.role,
    capture_profile: externallyUpdated.configuration.capture_profile,
    pose: {
      mode: externallyUpdated.configuration.pose.mode,
      inference_delegate: "cpu_only",
      debug_evidence_enabled: externallyUpdated.configuration.pose.debug_evidence_enabled,
      hitting_region: externallyUpdated.configuration.pose.hitting_region,
      peer_update: { operation: "keep" },
    },
  });
  fireEvent.change(screen.getByRole("combobox", { name: "Inference hardware" }), {
    target: { value: "gpu_required" },
  });
  fireEvent.click(screen.getByRole("button", { name: "Save phone configuration" }));
  assert.ok(await screen.findByText("setup configuration changed; reload before saving"));
  fireEvent.click(screen.getByRole("button", { name: "Reload" }));
  await waitFor(() => {
    assert.equal(
      (screen.getByRole("combobox", { name: "Inference hardware" }) as HTMLSelectElement).value,
      "cpu_only",
    );
    assert.equal(screen.queryByText("setup configuration changed; reload before saving"), null);
  });
  const phoneAccessibility = await axe.run(phoneSetup.container, {
    rules: { "color-contrast": { enabled: false } },
  });
  assert.deepEqual(
    phoneAccessibility.violations.map((violation) => violation.id),
    [],
    "phone setup fixture should have no automated accessibility violations",
  );
  cleanup();

  const demotedLeaderApi = new FakeNodeSetupApi(
    "http://pixel-6-pro.test:8088",
    "down_the_line",
    "Pixel 6 Pro",
  );
  render(<NodeSetupApp apis={[demotedLeaderApi]} />);
  assert.ok(await screen.findByText("Peer association stored"));
  fireEvent.change(screen.getByRole("combobox", { name: "Pose behavior" }), {
    target: { value: "shadow" },
  });
  assert.equal(screen.queryByRole("textbox", { name: "Peer phone origin" }), null);
  assert.ok(screen.getByText("Peer association will be cleared"));
  fireEvent.click(screen.getByRole("button", { name: "Save phone configuration" }));
  await waitFor(async () => {
    const demoted = await demotedLeaderApi.getSetup();
    assert.equal(demoted.configuration.pose.mode, "shadow");
    assert.equal(demoted.configuration.pose.peer, null);
  });
  cleanup();

  render(
    <NodeSetupApp
      apis={[
        new FakeNodeSetupApi("http://pixel-6-pro.test:8088", "down_the_line", "Pixel 6 Pro"),
        new FakeNodeSetupApi("http://pixel-5a.test:8088", "face_on", "Pixel 5a"),
      ]}
    />,
  );
  assert.ok(await screen.findByRole("heading", { name: "Two-phone station configured" }));
  assert.ok(screen.getByText("Both views assigned"));
  assert.equal(screen.getAllByRole("button", { name: "Save phone configuration" }).length, 2);
  cleanup();

  render(
    <NodeSetupApp
      apis={[
        new FakeNodeSetupApi(
          "http://pixel-6-pro.test:8088",
          "down_the_line",
          "Pixel 6 Pro",
          "leader",
          "http://different-shadow.test:8088",
        ),
        new FakeNodeSetupApi("http://pixel-5a.test:8088", "face_on", "Pixel 5a", "shadow"),
      ]}
    />,
  );
  assert.ok(await screen.findByRole("heading", { name: "Resolve pose assignments" }));
  assert.ok(screen.getByText(/leader associated with one peer-free shadow/));
  assert.equal(screen.queryByText("Both views assigned"), null);
  cleanup();

  render(
    <NodeSetupApp
      apis={[
        new FakeNodeSetupApi("http://pixel-6-pro.test:8088", "down_the_line", "Pixel 6 Pro"),
        new FakeNodeSetupApi(
          "http://pixel-5a.test:8088",
          "face_on",
          "Pixel 5a",
          "shadow",
          "http://pixel-6-pro.test:8088",
        ),
      ]}
    />,
  );
  assert.ok(await screen.findByRole("heading", { name: "Resolve pose assignments" }));
  assert.ok(screen.getByText(/peer-free shadow/));
  assert.equal(screen.queryByText("Both views assigned"), null);
  cleanup();

  await testPairedPreviewBackpressure();
  testPreviewDelayAttribution();
  testPreviewPipelineStallAttribution();

  const disconnected = structuredClone(FIXTURE_STATUS);
  const faceOn = disconnected.cameras.find((camera) => camera.role === "face_on");
  assert.ok(faceOn);
  faceOn.connected = false;
  faceOn.error = "Camera permission denied";
  faceOn.image_quality.assessment = "unavailable";
  render(<App api={new FakeStationApi(disconnected)} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByText("Face-on camera is offline."));
  assert.ok(screen.getByRole("img", { name: "Face-on unavailable" }));
  assert.ok(screen.getByText("Camera permission denied"));
  assert.equal(
    screen.getByRole("slider", { name: "Face-on exposure" }).hasAttribute("disabled"),
    true,
  );
  cleanup();

  const noSignal = structuredClone(FIXTURE_STATUS);
  const downTheLine = noSignal.cameras.find((camera) => camera.role === "down_the_line");
  assert.ok(downTheLine);
  downTheLine.exposure_us.value = 3960;
  downTheLine.exposure_us.max = 4000;
  downTheLine.image_quality = {
    assessment: "too_dark",
    mean: 0.0001,
    p99: 0,
    gradient_energy: 0.001,
  };
  render(<App api={new FakeStationApi(noSignal)} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByText("No optical signal detected"));
  assert.ok(
    screen.getByText(
      "Check the lens cap, lens aperture, and physical obstruction. Exposure is already near its frame-rate-safe maximum.",
    ),
  );
  cleanup();

  await testPollFailureClearsStaleStatus({ cleanup, render, screen });

  await testHttpContract();
  await testNodeSetupHttpContract();
  await testNodeDiscoveryHttpContract();
  assert.throws(
    () => parseStationStatus({ ...FIXTURE_STATUS, schema_version: 2 }),
    /Unsupported station status schema/,
  );
  assert.throws(
    () => parseStationStatus({ ...FIXTURE_STATUS, service_instance_id: "" }),
    /service instance ID must be a non-empty bounded identifier/,
  );
  const missingError = structuredClone(FIXTURE_STATUS) as unknown as {
    cameras: Array<Record<string, unknown>>;
  };
  delete missingError.cameras[0]?.error;
  assert.throws(() => parseStationStatus(missingError), /camera error must be a string/);
  const malformedRenderer = structuredClone(FIXTURE_STATUS) as unknown as {
    cameras: Array<{ preview_performance: Record<string, unknown> }>;
  };
  if (malformedRenderer.cameras[0] !== undefined) {
    malformedRenderer.cameras[0].preview_performance.renderer_stage = "blocked_forever";
  }
  assert.throws(() => parseStationStatus(malformedRenderer), /Unknown preview renderer stage/);
  const fractionalSequence = structuredClone(FIXTURE_STATUS) as unknown as {
    cameras: Array<Record<string, unknown>>;
  };
  if (fractionalSequence.cameras[0] !== undefined) {
    fractionalSequence.cameras[0].preview_sequence = 1.5;
  }
  assert.throws(
    () => parseStationStatus(fractionalSequence),
    /camera preview_sequence must be a nonnegative safe integer/,
  );
  const numericFrameId = structuredClone(FIXTURE_STATUS) as unknown as {
    cameras: Array<{ preview_performance: Record<string, unknown> }>;
  };
  if (numericFrameId.cameras[0] !== undefined) {
    numericFrameId.cameras[0].preview_performance.latest_capture_frame_id = 42;
  }
  assert.throws(
    () => parseStationStatus(numericFrameId),
    /latest_capture_frame_id must be an unsigned decimal string/,
  );
}

async function testNodeSetupHttpContract() {
  const calls: Array<{ url: string; init: RequestInit | undefined }> = [];
  let responseSetup = fixtureNodeSetup();
  let acceptedToken = "node-secret";
  let liveStatusRevision = 0;
  const fetcher = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = String(input);
    calls.push({ url, init });
    if (new Headers(init?.headers).get("Authorization") !== `Bearer ${acceptedToken}`) {
      return Response.json(
        { error: "a valid bearer control credential is required" },
        { status: 401 },
      );
    }
    if (url.endsWith("/api/v1/capture/missed-shot")) {
      return Response.json({
        session_id: "shared-credential-session",
        state: "waiting_post_roll",
        created_at_utc: "2026-08-22T00:00:00.000Z",
        error: "",
      });
    }
    if (url.endsWith("/api/v1/capture/status")) {
      ++liveStatusRevision;
      return Response.json({
        schema_version: 2,
        state: "setup",
        armed: false,
        active_session_id: null,
        error: "",
        hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
        live_status: {
          schema_version: 1,
          stream_id: "fixture-status-stream",
          revision: String(liveStatusRevision),
          generated_elapsed_realtime_ns: String(liveStatusRevision * 1_000),
        },
      });
    }
    if (url.endsWith("/api/v1/control-credential/rotate")) {
      const request = JSON.parse(String(init?.body)) as {
        expected_revision: number;
        confirmed_node_id: string;
      };
      assert.equal(request.expected_revision, responseSetup.revision);
      assert.equal(request.confirmed_node_id, responseSetup.node.node_id);
      acceptedToken = "rotatedControlCredential00000001";
      responseSetup = {
        ...responseSetup,
        revision: responseSetup.revision + 1,
        node: {
          ...responseSetup.node,
          control_credential_generation: responseSetup.node.control_credential_generation + 1,
        },
      };
      return Response.json({
        schema_version: 1,
        node_id: responseSetup.node.node_id,
        setup_revision: responseSetup.revision,
        control_credential_generation: responseSetup.node.control_credential_generation,
        control_token: acceptedToken,
        remote_peer_bindings_require_re_pair: true,
      });
    }
    if (init?.method === "PUT") {
      const request = JSON.parse(String(init.body)) as {
        configuration: { pose: { peer_update: { operation: string; origin?: string } } };
      };
      const peerUpdate = request.configuration.pose.peer_update;
      responseSetup = {
        ...responseSetup,
        revision: responseSetup.revision + 1,
        configuration: {
          ...responseSetup.configuration,
          pose: {
            ...responseSetup.configuration.pose,
            peer:
              peerUpdate.operation === "keep"
                ? responseSetup.configuration.pose.peer
                : peerUpdate.operation === "clear"
                  ? null
                  : { origin: peerUpdate.origin ?? "" },
          },
        },
      };
    }
    return Response.json(responseSetup);
  }) as typeof fetch;
  const sharedCredential = new MutableControlCredential("node-secret");
  const statusScheduler = new ManualStatusScheduler();
  const api = new HttpNodeSetupApi("http://pixel.test:8088/", sharedCredential, fetcher);
  const reviewApi = new HttpReviewApi(
    "http://pixel.test:8088/",
    fetcher,
    undefined,
    sharedCredential,
    false,
    { intervalMs: 10, maximumBackoffMs: 80, scheduler: statusScheduler },
  );
  const setup = await api.getSetup();
  assert.equal(setup.revision, 4);
  const firstCall = calls[0];
  assert.ok(firstCall);
  assert.equal(firstCall.url, "http://pixel.test:8088/api/v1/setup");
  assert.ok(firstCall.init);
  assert.equal(new Headers(firstCall.init.headers).get("Authorization"), "Bearer node-secret");
  const updated = await api.updateSetup(setup.revision, {
    role: "down_the_line",
    capture_profile: "720p240",
    pose: {
      mode: "leader",
      inference_delegate: "gpu_preferred",
      debug_evidence_enabled: true,
      hitting_region: { left: 0.15, top: 0.3, right: 0.85, bottom: 1 },
      peer_update: { operation: "keep" },
    },
  });
  assert.equal(updated.revision, 5);
  assert.equal(calls[1]?.init?.method, "PUT");
  assert.deepEqual(JSON.parse(String(calls[1]?.init?.body)), {
    schema_version: 1,
    expected_revision: 4,
    configuration: {
      role: "down_the_line",
      capture_profile: "720p240",
      pose: {
        mode: "leader",
        inference_delegate: "gpu_preferred",
        debug_evidence_enabled: true,
        hitting_region: { left: 0.15, top: 0.3, right: 0.85, bottom: 1 },
        peer_update: { operation: "keep" },
      },
    },
  });
  const replaced = await api.updateSetup(updated.revision, {
    role: "down_the_line",
    capture_profile: "720p240",
    pose: {
      mode: "leader",
      inference_delegate: "gpu_preferred",
      debug_evidence_enabled: true,
      hitting_region: { left: 0.15, top: 0.3, right: 0.85, bottom: 1 },
      peer_update: {
        operation: "replace",
        origin: "http://pixel-5a.test:8088",
        control_token: "write-only-peer-token",
      },
    },
  });
  assert.equal(replaced.revision, 6);
  assert.deepEqual(replaced.configuration.pose.peer, { origin: "http://pixel-5a.test:8088" });
  assert.doesNotMatch(JSON.stringify(replaced), /write-only-peer-token/);
  assert.deepEqual(
    (JSON.parse(String(calls[2]?.init?.body)) as { configuration: { pose: object } }).configuration
      .pose,
    {
      mode: "leader",
      inference_delegate: "gpu_preferred",
      debug_evidence_enabled: true,
      hitting_region: { left: 0.15, top: 0.3, right: 0.85, bottom: 1 },
      peer_update: {
        operation: "replace",
        origin: "http://pixel-5a.test:8088",
        control_token: "write-only-peer-token",
      },
    },
  );
  const cleared = await api.updateSetup(replaced.revision, {
    role: "down_the_line",
    capture_profile: "720p240",
    pose: {
      mode: "disabled",
      inference_delegate: "gpu_preferred",
      debug_evidence_enabled: true,
      hitting_region: { left: 0.15, top: 0.3, right: 0.85, bottom: 1 },
      peer_update: { operation: "clear" },
    },
  });
  assert.equal(cleared.revision, 7);
  assert.equal(cleared.configuration.pose.peer, null);
  assert.deepEqual(
    (
      JSON.parse(String(calls[3]?.init?.body)) as {
        configuration: { pose: { peer_update: object } };
      }
    ).configuration.pose.peer_update,
    { operation: "clear" },
  );
  const rotation = await api.rotateControlCredential(cleared.revision, cleared.node.node_id);
  assert.equal(rotation.control_token, "rotatedControlCredential00000001");
  assert.equal(rotation.control_credential_generation, 2);
  assert.equal(sharedCredential.current(), "rotatedControlCredential00000001");
  assert.equal(calls[4]?.init?.method, "POST");
  assert.deepEqual(JSON.parse(String(calls[4]?.init?.body)), {
    schema_version: 1,
    expected_revision: 7,
    confirmed_node_id: cleared.node.node_id,
  });
  const refreshedAfterRotation = await api.getSetup();
  assert.equal(refreshedAfterRotation.revision, 8);
  assert.equal(
    new Headers(calls[5]?.init?.headers).get("Authorization"),
    "Bearer rotatedControlCredential00000001",
  );
  assert.doesNotMatch(JSON.stringify(refreshedAfterRotation), /rotatedControlCredential/);
  assert.equal((await reviewApi.saveMissedShot()).session_id, "shared-credential-session");
  assert.equal(
    new Headers(calls[6]?.init?.headers).get("Authorization"),
    "Bearer rotatedControlCredential00000001",
  );
  let liveStatusChanges = 0;
  const unsubscribe = reviewApi.subscribeToChanges(() => ++liveStatusChanges);
  assert.ok(unsubscribe);
  await statusScheduler.runNext();
  assert.equal(liveStatusChanges, 1);
  const liveStatusCall = [...calls]
    .reverse()
    .find((call) => call.url.endsWith("/api/v1/capture/status"));
  assert.ok(liveStatusCall);
  assert.equal(
    new Headers(liveStatusCall.init?.headers).get("Authorization"),
    "Bearer rotatedControlCredential00000001",
  );
  unsubscribe();
  await assert.rejects(
    () => new HttpNodeSetupApi("http://pixel.test:8088/", "node-secret", fetcher).getSetup(),
    /failed \(401\).*valid bearer control credential/,
  );
  assert.throws(
    () => parseNodeSetupSnapshot({ ...fixtureNodeSetup(), revision: -1 }),
    /setup revision must be a nonnegative integer/,
  );
  assert.throws(
    () =>
      parseNodeSetupSnapshot({
        ...fixtureNodeSetup(),
        preview: { available: true, url: null },
      }),
    /preview availability must agree/,
  );
}

async function testNodeDiscoveryHttpContract() {
  const discoveryOrigin = "http://pixel-6.test:8088";
  const peerOrigin = "http://pixel-5a.test:8088";
  const peerToken = "peer-secret";
  const calls: Array<{ url: string; authorization: string | null }> = [];
  const discoveryResponse = {
    schema_version: 1,
    authentication_required_for_pairing: true,
    observations: [
      {
        service_instance: "swing-capture-pixel-5a",
        node_id: "fixture-pixel-5a-node",
        role: "face_on",
        label: "Hitting bay face-on",
        origin: peerOrigin,
        observed_at_epoch_ms: 1_787_423_999_000,
        expires_at_epoch_ms: 1_787_424_030_000,
        trusted: false,
      },
    ],
  };
  const peerSetup = fixtureNodeSetup(
    peerOrigin,
    "face_on",
    "Pixel 5a",
    "shadow",
    null,
    "fixture-pixel-5a-node",
  );
  const fetcher = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = String(input);
    const authorization = new Headers(init?.headers).get("Authorization");
    calls.push({ url, authorization });
    if (url === `${discoveryOrigin}/api/v1/discovery`) {
      assert.equal(authorization, "Bearer leader-secret");
      return Response.json(discoveryResponse);
    }
    if (url === `${peerOrigin}/api/v1/setup`) {
      assert.equal(authorization, `Bearer ${peerToken}`);
      return Response.json(peerSetup);
    }
    return Response.json({ error: `unexpected fixture URL ${url}` }, { status: 404 });
  }) as typeof fetch;
  const api = new HttpNodeSetupApi(discoveryOrigin, "leader-secret", fetcher);
  const observations = await api.discoverNodes();
  assert.deepEqual(observations, [
    {
      schema_version: 1,
      ...discoveryResponse.observations[0],
    },
  ]);
  const observation = observations[0];
  assert.ok(observation);
  const connected = await api.connectDiscovered(observation, peerToken);
  assert.equal(connected.api.displayOrigin, peerOrigin);
  assert.equal(connected.setup.node.node_id, observation.node_id);
  assert.equal(connected.setup.configuration.role, observation.role);
  assert.deepEqual(calls, [
    {
      url: `${discoveryOrigin}/api/v1/discovery`,
      authorization: "Bearer leader-secret",
    },
    { url: `${peerOrigin}/api/v1/setup`, authorization: `Bearer ${peerToken}` },
  ]);

  for (const [malformed, expectedError] of [
    [
      { ...discoveryResponse, authentication_required_for_pairing: false },
      /must require authenticated pairing/,
    ],
    [
      {
        ...discoveryResponse,
        observations: [{ ...discoveryResponse.observations[0], trusted: true }],
      },
      /metadata must be marked untrusted/,
    ],
    [
      {
        ...discoveryResponse,
        observations: [
          { ...discoveryResponse.observations[0], expires_at_epoch_ms: "not-an-epoch" },
        ],
      },
      /expires at must be a nonnegative integer/,
    ],
  ] as const) {
    const malformedApi = new HttpNodeSetupApi(discoveryOrigin, "leader-secret", (async () =>
      Response.json(malformed)) as typeof fetch);
    await assert.rejects(() => malformedApi.discoverNodes(), expectedError);
  }
}

async function testPairedPreviewBackpressure() {
  interface PendingPreview {
    role: "down_the_line" | "face_on";
    sequence: number;
    resolve(preview: FetchedPreview): void;
  }

  const fixture = new FakeStationApi();
  const pending: PendingPreview[] = [];
  const api: StationApi = {
    getStatus: () => fixture.getStatus(),
    getPreview: (role, sequence) =>
      new Promise((resolve) => pending.push({ role, sequence, resolve })),
    updateCameraSettings: (role, settings) => fixture.updateCameraSettings(role, settings),
    previewUrl: (role, sequence) => fixture.previewUrl(role, sequence),
    fullResolutionPreviewUrl: (role, sequence) => fixture.fullResolutionPreviewUrl(role, sequence),
  };
  const activeUrls = new Set<string>();
  let nextUrl = 1;
  const objectUrls: ObjectUrlFactory = {
    createObjectURL: () => {
      const url = `blob:paired-preview-${nextUrl++}`;
      activeUrls.add(url);
      return url;
    },
    revokeObjectURL: (url) => activeUrls.delete(url),
  };
  const pairs: PreviewPair[] = [];
  const errors: Error[] = [];
  const pendingDecodes: Array<{ url: string; resolve(): void }> = [];
  const decodeImage: PreviewImageDecoder = (url) =>
    new Promise((resolve) => pendingDecodes.push({ url, resolve }));
  const loader = new PairedPreviewLoader(
    api,
    (pair) => pairs.push(pair),
    (error) => errors.push(error),
    objectUrls,
    decodeImage,
  );

  loader.enqueue({ down_the_line: 1, face_on: 1 });
  assert.equal(pending.length, 2);
  loader.enqueue({ down_the_line: 2, face_on: 2 });
  loader.enqueue({ down_the_line: 3, face_on: 3 });
  resolvePendingPair(pending.slice(0, 2));
  await flushAsyncWork();
  assert.equal(pairs.length, 0, "a pair must not publish before both images decode");
  assert.equal(pendingDecodes.length, 2);
  resolvePendingDecodes(pendingDecodes.slice(0, 2));
  await flushAsyncWork();
  assert.deepEqual(
    pending.slice(2).map(({ sequence }) => sequence),
    [3, 3],
    "only the newest pending pair should survive backpressure",
  );
  assert.equal(pairs.length, 1);

  resolvePendingPair(pending.slice(2, 4));
  await flushAsyncWork();
  resolvePendingDecodes(pendingDecodes.slice(2, 4));
  await flushAsyncWork();
  assert.deepEqual(
    pairs.map(({ sequences }) => sequences.down_the_line),
    [1, 3],
  );
  assert.equal(pairs[0]?.telemetry.attributed_stage, "nominal");
  assert.equal(activeUrls.size, 4, "at most the current and previous pairs stay retained");

  loader.enqueue({ down_the_line: 4, face_on: 4 });
  assert.equal(pending.length, 6);
  resolvePendingPair(pending.slice(4, 6));
  await flushAsyncWork();
  resolvePendingDecodes(pendingDecodes.slice(4, 6));
  await flushAsyncWork();
  assert.equal(activeUrls.size, 4, "retired object URLs must be reclaimed on the next swap");
  assert.deepEqual(errors, []);
  loader.stop();
  assert.equal(activeUrls.size, 0, "stopping must reclaim every retained object URL");
}

function resolvePendingDecodes(pending: Array<{ resolve(): void }>) {
  for (const decode of pending) {
    decode.resolve();
  }
}

function resolvePendingPair(
  pending: Array<{ role: string; sequence: number; resolve(preview: FetchedPreview): void }>,
) {
  for (const request of pending) {
    request.resolve(
      fixtureFetchedPreview(
        new Blob([`${request.role}:${request.sequence}`], { type: "image/png" }),
        request.sequence,
      ),
    );
  }
}

function fixtureServerTelemetry(sequence: number): PreviewServerTelemetry {
  return {
    requested_sequence: sequence,
    served_sequence: sequence,
    latest_capture_frame_id: "800",
    latest_capture_age_ms: 3,
    latest_sink_frame_id: "800",
    latest_sink_completion_age_ms: 2,
    latest_sampler_frame_id: "800",
    latest_sampler_completion_age_ms: 1,
    sampled_sequence: sequence,
    sampled_age_ms: 7,
    source_age_ms: 18,
    rendered_age_ms: 12,
    render_queue_ms: 1,
    render_ms: 5,
    renderer_stage: "idle",
    render_pending: false,
    backend_ms: 0.2,
    response_prepare_ms: 0.1,
    handler_ms: 0.3,
  };
}

function fixtureFetchedPreview(image: Blob, sequence: number): FetchedPreview {
  return {
    image,
    telemetry: {
      headers_ms: 2,
      body_ms: 1,
      total_ms: 3,
      server: fixtureServerTelemetry(sequence),
    },
  };
}

function testPreviewDelayAttribution() {
  const roleTelemetry = (
    overrides: Partial<PreviewFetchTelemetry> = {},
    serverOverrides: Partial<PreviewServerTelemetry> = {},
    decodeMs = 2,
  ) => ({
    fetch: {
      headers_ms: 4,
      body_ms: 2,
      total_ms: 6,
      ...overrides,
      server: {
        ...fixtureServerTelemetry(7),
        ...serverOverrides,
      },
    },
    decode_ms: decodeMs,
  });
  const evidence = (
    downTheLine = roleTelemetry(),
    faceOn = roleTelemetry(),
    queueWaitMs = 0,
    totalMs = 3_500,
    presentedGapMs: number | null = 3_500,
  ) => ({
    queue_wait_ms: queueWaitMs,
    total_ms: totalMs,
    presented_gap_ms: presentedGapMs,
    roles: { down_the_line: downTheLine, face_on: faceOn },
  });

  assert.equal(
    attributePreviewDelay(evidence(roleTelemetry({}, { latest_capture_age_ms: 3_200 })))
      .attributed_stage,
    "capture",
  );
  assert.equal(
    attributePreviewDelay(
      evidence(
        roleTelemetry(
          {},
          {
            latest_capture_frame_id: "801",
            latest_sink_frame_id: "800",
            latest_capture_age_ms: 3_200,
          },
        ),
      ),
    ).attributed_stage,
    "capture_sink",
  );
  assert.equal(
    attributePreviewDelay(
      evidence(
        roleTelemetry(
          {},
          {
            latest_capture_frame_id: "801",
            latest_sink_frame_id: "801",
            latest_sampler_frame_id: "800",
            latest_sink_completion_age_ms: 3_100,
          },
        ),
      ),
    ).attributed_stage,
    "sampling",
  );
  assert.equal(
    attributePreviewDelay(evidence(roleTelemetry({}, { sampled_age_ms: 3_100 }))).attributed_stage,
    "sampling",
  );
  assert.equal(
    attributePreviewDelay(evidence(roleTelemetry({}, { source_age_ms: 3_000 }))).attributed_stage,
    "render",
  );
  assert.equal(
    attributePreviewDelay(evidence(roleTelemetry({ headers_ms: 3_400 }, { handler_ms: 3_300 })))
      .attributed_stage,
    "http_server",
  );
  assert.equal(
    attributePreviewDelay(evidence(roleTelemetry({ headers_ms: 3_400 }, { handler_ms: 4 })))
      .attributed_stage,
    "http_transport_or_dispatch",
  );
  assert.equal(
    attributePreviewDelay(evidence(roleTelemetry({ body_ms: 3_200 }))).attributed_stage,
    "response_body",
  );
  assert.equal(
    attributePreviewDelay(
      evidence(roleTelemetry({ body_ms: 3_200 }, { latest_capture_age_ms: 501 })),
    ).attributed_stage,
    "response_body",
  );
  assert.equal(
    attributePreviewDelay(
      evidence(
        roleTelemetry({}, { latest_capture_age_ms: 1_000 }),
        roleTelemetry({}, { latest_capture_age_ms: 3, source_age_ms: 3_000 }),
      ),
    ).attributed_stage,
    "render",
  );
  assert.equal(
    attributePreviewDelay(evidence(roleTelemetry({}, {}, 3_000))).attributed_stage,
    "browser_decode",
  );
  assert.equal(
    attributePreviewDelay(evidence(roleTelemetry(), roleTelemetry(), 2_900)).attributed_stage,
    "browser_backpressure",
  );
  assert.equal(
    attributePreviewDelay(evidence(roleTelemetry(), roleTelemetry(), 0, 20, 3_500))
      .attributed_stage,
    "status_poll_or_browser_scheduling",
  );
  assert.equal(
    attributePreviewDelay(evidence(roleTelemetry(), roleTelemetry(), 0, 20, 20)).attributed_stage,
    "nominal",
  );
}

function fixturePipelineStatus(stage: "capture" | "capture_sink" | "sampling" | "render") {
  const status = structuredClone(FIXTURE_STATUS);
  const camera = status.cameras.find((candidate) => candidate.role === "down_the_line");
  assert.ok(camera);
  camera.preview_sequence = 17;
  camera.preview_performance.latest_capture_age_ms =
    stage === "capture" || stage === "capture_sink" ? 3_500 : 3;
  if (stage === "capture_sink") {
    camera.preview_performance.latest_capture_frame_id = "801";
  }
  if (stage === "sampling") {
    camera.preview_performance.latest_capture_frame_id = "801";
    camera.preview_performance.latest_sink_frame_id = "801";
    camera.preview_performance.latest_sink_completion_age_ms = 3_400;
  }
  camera.preview_performance.sampled_age_ms =
    stage === "capture" || stage === "sampling" ? 3_400 : 7;
  camera.preview_performance.source_age_ms = 3_600;
  camera.preview_performance.renderer_stage = stage === "render" ? "routine" : "idle";
  camera.preview_performance.render_pending = stage === "render";
  return status;
}

function fixturePipelineStall(stage: "capture" | "capture_sink" | "sampling" | "render") {
  const stall = attributePreviewPipelineStall(
    fixturePipelineStatus(stage).cameras,
    1_787_500_000_000,
  );
  assert.ok(stall);
  return stall;
}

function testPreviewPipelineStallAttribution() {
  for (const stage of ["capture", "capture_sink", "sampling", "render"] as const) {
    const stall = fixturePipelineStall(stage);
    assert.equal(stall.stage, stage);
    assert.equal(
      stall.preview_sequence,
      17,
      `${stage} attribution must survive an unchanged rendered sequence`,
    );
  }

  const fresh = structuredClone(FIXTURE_STATUS);
  assert.equal(attributePreviewPipelineStall(fresh.cameras, 1_787_500_000_000), null);

  const concurrent = fixturePipelineStatus("capture_sink");
  const faceOn = concurrent.cameras.find((camera) => camera.role === "face_on");
  assert.ok(faceOn);
  faceOn.preview_performance.source_age_ms = 5_000;
  const concurrentStall = attributePreviewPipelineStall(concurrent.cameras, 1_787_500_000_000);
  assert.ok(concurrentStall);
  assert.equal(concurrentStall.stage, "render");
  assert.equal(concurrentStall.roles.down_the_line?.stage, "capture_sink");
  assert.equal(concurrentStall.roles.face_on?.stage, "render");
}

async function flushAsyncWork() {
  await new Promise((resolve) => setTimeout(resolve, 0));
}

async function testPollFailureClearsStaleStatus(testing: {
  cleanup(): void;
  render(element: React.ReactNode): unknown;
  screen: typeof import("@testing-library/react").screen;
}) {
  const fixture = new FakeStationApi();
  let failRequests = false;
  const unstableApi: StationApi = {
    getStatus: async () => {
      if (failRequests) {
        throw new Error("station connection lost");
      }
      return fixture.getStatus();
    },
    getPreview: (role, sequence) => fixture.getPreview(role, sequence),
    updateCameraSettings: (role, settings) => fixture.updateCameraSettings(role, settings),
    previewUrl: (role, sequence) => fixture.previewUrl(role, sequence),
    fullResolutionPreviewUrl: (role, sequence) => fixture.fullResolutionPreviewUrl(role, sequence),
  };

  testing.render(<App api={unstableApi} pollIntervalMs={5} />);
  assert.ok(await testing.screen.findByText("Both configured camera roles are online."));
  failRequests = true;
  assert.ok(await testing.screen.findByRole("heading", { name: "Station unavailable" }));
  assert.equal(testing.screen.queryByText("Both configured camera roles are online."), null);
  assert.equal(testing.screen.queryByLabelText("Camera previews"), null);
  assert.ok(testing.screen.getByText("Unavailable"));
  assert.ok(testing.screen.getByText("station connection lost"));
  testing.cleanup();
}

async function testHttpContract() {
  const calls: Array<{ url: string; init: RequestInit | undefined }> = [];
  let patchCamera: CameraStatus | undefined;
  const fetcher = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = String(input);
    calls.push({ url, init });
    if (url.includes("/preview?")) {
      return new Response("jpeg", {
        headers: {
          "Content-Type": "image/jpeg",
          "X-Preview-Sequence": "43",
          "X-Preview-Requested-Sequence": "41",
          "X-Preview-Latest-Capture-Frame-Id": "9007199254740993",
          "X-Preview-Latest-Capture-Age-Ms": "3.5",
          "X-Preview-Latest-Sink-Frame-Id": "9007199254740993",
          "X-Preview-Latest-Sink-Completion-Age-Ms": "2.5",
          "X-Preview-Latest-Sampler-Frame-Id": "9007199254740993",
          "X-Preview-Latest-Sampler-Completion-Age-Ms": "1.5",
          "X-Preview-Sampled-Sequence": "44",
          "X-Preview-Sampled-Age-Ms": "7.5",
          "X-Preview-Source-Age-Ms": "18.5",
          "X-Preview-Rendered-Age-Ms": "12.5",
          "X-Preview-Render-Queue-Ms": "1.5",
          "X-Preview-Render-Ms": "5.5",
          "X-Preview-Renderer-Stage": "idle",
          "X-Preview-Render-Pending": "false",
          "X-Preview-Server-Backend-Ms": "0.2",
          "X-Preview-Server-Response-Prepare-Ms": "0.1",
          "X-Preview-Server-Handler-Ms": "0.3",
        },
      });
    }
    if (init?.method === "PATCH") {
      patchCamera = structuredClone(FIXTURE_STATUS.cameras[0]);
      assert.ok(patchCamera);
      patchCamera.exposure_us.value = 1900;
      return Response.json(patchCamera);
    }
    return Response.json(FIXTURE_STATUS);
  }) as typeof fetch;

  const previewClockValues = [100, 140, 155];
  const api = new HttpStationApi("http://station.test/", fetcher, () => {
    const value = previewClockValues.shift();
    assert.notEqual(value, undefined);
    return value as number;
  });
  const status = await api.getStatus();
  assert.equal(status.schema_version, 1);
  assert.equal(calls[0]?.url, "http://station.test/api/v1/status");

  const updated = await api.updateCameraSettings("down_the_line", {
    exposure_us: 1900,
    gain_db: 3,
  });
  assert.equal(updated.exposure_us.value, 1900);
  assert.equal(calls[1]?.url, "http://station.test/api/v1/cameras/down_the_line/settings");
  assert.equal(calls[1]?.init?.method, "PATCH");
  assert.deepEqual(JSON.parse(String(calls[1]?.init?.body)), {
    exposure_us: 1900,
    gain_db: 3,
  });
  assert.equal(
    api.previewUrl("face_on", 41),
    "http://station.test/api/v1/cameras/face_on/preview?sequence=41",
  );
  assert.equal(
    api.fullResolutionPreviewUrl("face_on", 41),
    "http://station.test/api/v1/cameras/face_on/preview?sequence=41&full=1",
  );
  const preview = await api.getPreview("face_on", 41);
  assert.equal(preview.image.type, "image/jpeg");
  assert.equal(await preview.image.text(), "jpeg");
  assert.equal(preview.telemetry.headers_ms, 40);
  assert.equal(preview.telemetry.body_ms, 15);
  assert.equal(preview.telemetry.total_ms, 55);
  assert.equal(preview.telemetry.server.requested_sequence, 41);
  assert.equal(preview.telemetry.server.served_sequence, 43);
  assert.equal(preview.telemetry.server.latest_capture_frame_id, "9007199254740993");
  assert.equal(preview.telemetry.server.latest_sink_frame_id, "9007199254740993");
  assert.equal(preview.telemetry.server.latest_sampler_frame_id, "9007199254740993");
  assert.equal(preview.telemetry.server.sampled_sequence, 44);
  assert.equal(preview.telemetry.server.render_queue_ms, 1.5);
  assert.equal(preview.telemetry.server.handler_ms, 0.3);

  const regressedApi = new HttpStationApi(
    "http://station.test",
    (async () =>
      new Response("jpeg", {
        headers: { "Content-Type": "image/jpeg", "X-Preview-Sequence": "40" },
      })) as typeof fetch,
  );
  await assert.rejects(
    () => regressedApi.getPreview("face_on", 41),
    /preview regressed from requested sequence 41 to 40/,
  );

  const malformedPreviewApi = (headers: Record<string, string>) =>
    new HttpStationApi(
      "http://station.test",
      (async () =>
        new Response("jpeg", {
          headers: { "Content-Type": "image/jpeg", "X-Preview-Sequence": "41", ...headers },
        })) as typeof fetch,
    );
  await assert.rejects(
    () => malformedPreviewApi({ "X-Preview-Latest-Capture-Age-Ms": "" }).getPreview("face_on", 41),
    /invalid X-Preview-Latest-Capture-Age-Ms/,
  );
  await assert.rejects(
    () => malformedPreviewApi({ "X-Preview-Renderer-Stage": "wedged" }).getPreview("face_on", 41),
    /Unknown preview renderer stage/,
  );
  await assert.rejects(
    () => malformedPreviewApi({ "X-Preview-Sampled-Sequence": "1.5" }).getPreview("face_on", 41),
    /invalid X-Preview-Sampled-Sequence/,
  );
  assert.throws(
    () => malformedPreviewApi({}).previewUrl("face_on", Number.MAX_SAFE_INTEGER + 1),
    /preview sequence must be a nonnegative safe integer/,
  );

  const failingApi = new HttpStationApi("http://station.test", (async () =>
    Response.json(
      { error: "a camera settings update is already pending" },
      { status: 409 },
    )) as typeof fetch);
  await assert.rejects(
    () => failingApi.getStatus(),
    /Station request failed \(409\): a camera settings update is already pending/,
  );
}

function installDomGlobals(window: DOMWindow) {
  const globals = globalThis as unknown as Record<string, unknown>;
  globals.window = window;
  globals.document = window.document;
  Object.defineProperty(globalThis, "navigator", {
    configurable: true,
    value: window.navigator,
  });
  globals.HTMLElement = window.HTMLElement;
  globals.HTMLInputElement = window.HTMLInputElement;
  globals.MutationObserver = window.MutationObserver;
  globals.Node = window.Node;
  globals.getComputedStyle = window.getComputedStyle.bind(window);
  globals.IS_REACT_ACT_ENVIRONMENT = true;
}

function requiredElement<T>(value: T | null, label: string): T {
  assert.notEqual(value, null, `expected ${label}`);
  return value as T;
}

class ManualStatusScheduler implements StatusSubscriptionScheduler {
  #task: { callback: () => void; cancelled: boolean } | null = null;

  setTimeout(callback: () => void): unknown {
    const task = { callback, cancelled: false };
    this.#task = task;
    return task;
  }

  clearTimeout(handle: unknown): void {
    (handle as { cancelled: boolean }).cancelled = true;
  }

  async runNext(): Promise<void> {
    const task = this.#task;
    assert.ok(task);
    this.#task = null;
    task.callback();
    await flushAsyncWork();
  }
}

void main().catch((caught: unknown) => {
  console.error(caught);
  process.exit(1);
});
