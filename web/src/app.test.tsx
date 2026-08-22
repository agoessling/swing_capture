import assert from "node:assert/strict";
import { JSDOM, type DOMWindow } from "jsdom";
import { App } from "./app.js";
import { HttpStationApi, parseStationStatus, type CameraStatus, type StationApi } from "./api.js";
import { FakeStationApi, FIXTURE_STATUS } from "./fake_api.js";
import { FakeNodeSetupApi, fixtureNodeSetup } from "./fake_node_setup_api.js";
import { HttpNodeSetupApi, parseNodeSetupSnapshot } from "./node_setup_api.js";
import { NodeSetupApp } from "./node_setup_app.js";
import {
  type ObjectUrlFactory,
  PairedPreviewLoader,
  type PreviewImageDecoder,
  type PreviewPair,
} from "./paired_preview.js";

const dom = new JSDOM('<!doctype html><html lang="en"><body></body></html>', {
  url: "http://station.test/setup/cameras",
});

installDomGlobals(dom.window);

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
  assert.throws(
    () => parseStationStatus({ ...FIXTURE_STATUS, schema_version: 2 }),
    /Unsupported station status schema/,
  );
  const missingError = structuredClone(FIXTURE_STATUS) as unknown as {
    cameras: Array<Record<string, unknown>>;
  };
  delete missingError.cameras[0]?.error;
  assert.throws(() => parseStationStatus(missingError), /camera error must be a string/);
}

async function testNodeSetupHttpContract() {
  const calls: Array<{ url: string; init: RequestInit | undefined }> = [];
  let responseSetup = fixtureNodeSetup();
  const fetcher = (async (input: RequestInfo | URL, init?: RequestInit) => {
    calls.push({ url: String(input), init });
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
  const api = new HttpNodeSetupApi("http://pixel.test:8088/", "node-secret", fetcher);
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

async function testPairedPreviewBackpressure() {
  interface PendingPreview {
    role: "down_the_line" | "face_on";
    sequence: number;
    resolve(blob: Blob): void;
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
  pending: Array<{ role: string; sequence: number; resolve(blob: Blob): void }>,
) {
  for (const request of pending) {
    request.resolve(new Blob([`${request.role}:${request.sequence}`], { type: "image/png" }));
  }
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
      return new Response("jpeg", { headers: { "Content-Type": "image/jpeg" } });
    }
    if (init?.method === "PATCH") {
      patchCamera = structuredClone(FIXTURE_STATUS.cameras[0]);
      assert.ok(patchCamera);
      patchCamera.exposure_us.value = 1900;
      return Response.json(patchCamera);
    }
    return Response.json(FIXTURE_STATUS);
  }) as typeof fetch;

  const api = new HttpStationApi("http://station.test/", fetcher);
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
  assert.equal(preview.type, "image/jpeg");
  assert.equal(await preview.text(), "jpeg");

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

void main().catch((caught: unknown) => {
  console.error(caught);
  process.exit(1);
});
