import { readFile, writeFile } from "node:fs/promises";
import { createHmac } from "node:crypto";
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import path from "node:path";
import axe from "axe-core";
import { expect, test, type Locator, type Page } from "@playwright/test";
import { DualNodeReviewApi } from "./dual_node_review_api.js";
import type { CaptureState, ReviewRole } from "./review_api.js";

// Each worker owns an independent ProductionHarness on ephemeral ports, so the long production
// matrix can use the configured workers without sharing mutable node state between tests.
test.describe.configure({ mode: "parallel" });

interface FixtureRole {
  node_id: string;
  local_session_id: string;
  camera_serial: string;
  media_path: string;
  encoded_bytes: number;
  trigger_timestamp_ns: string;
  clock_offset_ns: string;
}

interface FixtureScenario {
  schema_version: 1;
  shared_session_id: string;
  created_at_utc: string;
  frame_count: number;
  impact_frame_index: number;
  nominal_fps: number;
  encoded_width: number;
  encoded_height: number;
  all_frames_keyframes: boolean;
  keyframe_interval_frames: number;
  roles: Record<ReviewRole, FixtureRole>;
}

interface MutationRequest {
  method: string;
  pathname: string;
  authorization: string | undefined;
  body: string;
}

interface PresentedFrameEvidence {
  role: string;
  media_time_seconds: number;
  pixel_hash: string;
  nonblack_fraction: number;
}

interface NodeState {
  role: ReviewRole;
  advertisedRole: ReviewRole;
  readonly originalToken: string;
  token: string;
  setupRevision: number;
  credentialGeneration: number;
  liveStatusStreamId: string;
  liveStatusRevision: number;
  staleLiveStatus: boolean;
  online: boolean;
  armed: boolean;
  activeSharedSessionId: string | null;
  failNextArm: boolean;
  failNextMissedShot: boolean;
  failNextFieldRecordingStart: boolean;
  failNextFieldRecordingStartAfterAccept: boolean;
  failNextFieldRecordingStop: boolean;
  failNextFieldRecordingStopAfterAccept: boolean;
  dropArmConnections: boolean;
  fieldRecordingState: "idle" | "recording" | "stopping" | "error";
  fieldRecordingError: string;
  fieldRecordingPublicationFailurePending: boolean;
  holdFieldRecordingStop: boolean;
  activeFieldRecordingId: string | null;
  sharedFieldRecordingId: string | null;
  publishFixtureSession: boolean;
  historicalSessionCount: number;
  holdHistoricalManifests: boolean;
  historicalManifestRequestCount: number;
  pendingHistoricalManifestResponses: Array<() => void>;
  holdFieldRecordingCatalog: boolean;
  fieldRecordingCatalogRequestCount: number;
  pendingFieldRecordingCatalogResponses: Array<() => void>;
  publication:
    | "none"
    | "encoding"
    | "encoding_down_the_line"
    | "missing"
    | "missing_down_the_line"
    | "ready"
    | "mismatched";
  mutations: MutationRequest[];
  rangeStatuses: number[];
  statusAuthorizations: Array<string | undefined>;
  previewAuthorizations: Array<string | undefined>;
}

const TOKENS: Record<ReviewRole, string> = {
  down_the_line: "fixture_dtl_control_token_000001",
  face_on: "fixture_face_control_token_00001",
};
const PENDING_SHARED_SESSION_ID = "fixture-pending-shared-001";
const SETUP_PREVIEW_JPEG = Buffer.from(
  "/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////" +
    "2wBDAf//////////////////////////////////////////////////////////////////////////////////////" +
    "wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAX/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/" +
    "9oADAMBAAIQAxAAAAEf/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABBQJ//8QAFBEBAAAAAAAAAAAAAA" +
    "AAAAAP/aAAgBAwEBPwF//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAgEBPwF//8QAFBABAAAAAAAAAAAA" +
    "AAAAAAAAAP/aAAgBAQAGPwJ//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPyF//9oADAMBAAIAAwAA" +
    "ABAf/8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAwEBPxB//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgB" +
    "AgEBPxB//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPxB//9k=",
  "base64",
);
const MUTATING_ROUTES = [
  ["POST", "/api/v1/capture/arm"],
  ["POST", "/api/v1/capture/manual"],
  ["POST", "/api/v1/capture/missed-shot"],
  ["POST", "/api/v1/capture/pose-arm"],
  ["POST", "/api/v1/capture/pose-impact"],
  ["POST", "/api/v1/field-recording/start"],
  ["POST", "/api/v1/field-recording/stop"],
  ["POST", "/api/v1/hil/synthetic-swing"],
  ["POST", "/api/v1/hil/pose-arm"],
  ["POST", "/api/v1/sessions/fixture-local-dtl-001/feedback"],
  ["POST", "/api/v1/coordination/fixture-auth-check"],
  ["POST", "/api/v1/control-credential/rotate"],
  ["PUT", "/api/v1/setup"],
] as const;
const SENSITIVE_GENERAL_READ_ROUTES = [
  "/api/v1/node",
  "/api/v1/setup",
  "/api/v1/setup/preview",
  "/api/v1/pairing/identity",
  "/api/v1/discovery",
  "/api/v1/capture/status",
  "/api/v1/capture/trigger-report",
  "/api/v1/field-recording/status",
  "/api/v1/field-recordings",
  "/api/v1/sessions",
  "/api/v1/sessions/fixture-local-dtl-001/manifest",
  "/api/v1/sessions/fixture-local-dtl-001/diagnostics.zip",
  "/api/v1/coordination/fixture-auth-check",
] as const;

let harness: ProductionHarness;

test.beforeAll(async () => {
  const staticRoot = process.env.SWING_CAPTURE_STATIC_APP;
  const fixtureRoot = process.env.SWING_CAPTURE_PRODUCTION_H264_FIXTURE;
  if (staticRoot === undefined || fixtureRoot === undefined) {
    throw new Error("Production integration fixture paths are not configured");
  }
  harness = await ProductionHarness.start(path.resolve(staticRoot), path.resolve(fixtureRoot));
});

test.afterAll(async () => {
  await harness.close();
});

test.beforeEach(() => {
  harness.reset();
});

test("production H.264 fixtures structurally use the Android one-second GOP", async () => {
  const fixtureRoot = process.env.SWING_CAPTURE_PRODUCTION_H264_FIXTURE;
  if (fixtureRoot === undefined) {
    throw new Error("Production H.264 fixture path is not configured");
  }
  for (const role of ["down_the_line", "face_on"] as const) {
    const fixture = harness.scenario.roles[role];
    const media = await readFile(path.join(fixtureRoot, fixture.media_path));
    expect(media.length).toBe(fixture.encoded_bytes);
    expect(mp4SyncSampleNumbers(media)).toEqual([1, 31, 61]);
  }
});

test("production-shaped nodes reject every unauthenticated mutation", async ({ request }) => {
  for (const origin of harness.nodeOrigins()) {
    for (const [method, pathname] of MUTATING_ROUTES) {
      for (const authorization of [undefined, "Bearer definitely-wrong"]) {
        const response = await request.fetch(`${origin}${pathname}`, {
          method,
          ...(authorization === undefined ? {} : { headers: { Authorization: authorization } }),
          data: {},
        });
        expect(response.status(), `${method} ${pathname} without a valid bearer`).toBe(401);
        expect(response.headers()["www-authenticate"]).toBe("Bearer");
        expect(await response.json()).toEqual({
          error: "a valid bearer control credential is required",
        });
      }
    }
  }
});

test("production-shaped nodes reject anonymous sensitive metadata reads", async ({ request }) => {
  for (const origin of harness.nodeOrigins()) {
    for (const pathname of SENSITIVE_GENERAL_READ_ROUTES) {
      for (const method of ["GET", "HEAD"] as const) {
        for (const authorization of [undefined, "Bearer definitely-wrong"]) {
          const response = await request.fetch(`${origin}${pathname}`, {
            method,
            ...(authorization === undefined ? {} : { headers: { Authorization: authorization } }),
          });
          expect(response.status(), `${method} ${pathname} without a valid bearer`).toBe(401);
          expect(response.headers()["www-authenticate"]).toBe("Bearer");
          if (method === "GET") {
            expect(await response.json()).toEqual({
              error: "a valid bearer control credential is required",
            });
          }
        }
      }
    }
  }
});

test("the public clock hint discloses no control credential", async ({ request }) => {
  const nodes = harness.nodes();
  const origins = harness.nodeOrigins();
  for (const [node, origin] of [
    [nodes[0], origins[0]],
    [nodes[1], origins[1]],
  ] as const) {
    const response = await request.get(`${origin}/api/v1/clock`);
    expect(response.status()).toBe(200);
    const body = await response.text();
    expect(body).not.toContain(node.token);
    expect(body).not.toContain("control_token");
  }
});

test("capability-bearing field catalogs are private and never cacheable", async ({ request }) => {
  const nodes = harness.nodes();
  const origins = harness.nodeOrigins();
  for (const [node, origin] of [
    [nodes[0], origins[0]],
    [nodes[1], origins[1]],
  ] as const) {
    const response = await request.get(`${origin}/api/v1/field-recordings`, {
      headers: { Authorization: `Bearer ${node.token}` },
    });
    expect(response.status()).toBe(200);
    expect(response.headers()["cache-control"]).toBe("private, no-store");
  }
});

test("bootstrap credentials leave the URL and survive a same-tab reload", async ({ page }) => {
  const bootstrapUrl = new URL(harness.applicationUrl());
  expect(bootstrapUrl.searchParams.get("dtl_token")).toBe(harness.downTheLine.token);
  expect(bootstrapUrl.searchParams.get("face_token")).toBe(harness.faceOn.token);
  const assetReferrers: string[] = [];
  page.on("request", (request) => {
    if (["/app.js", "/app.css"].includes(new URL(request.url()).pathname)) {
      assetReferrers.push(request.headers().referer ?? "");
    }
  });

  await page.goto(bootstrapUrl.toString());
  await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();
  expect(assetReferrers.length).toBeGreaterThan(0);
  expect(assetReferrers.every((referrer) => referrer === "")).toBe(true);
  const sanitized = new URL(page.url());
  expect(sanitized.searchParams.has("dtl_token")).toBe(false);
  expect(sanitized.searchParams.has("face_token")).toBe(false);
  expect(sanitized.searchParams.get("dtl_node")).toBe(harness.nodeOrigins()[0]);
  expect(sanitized.searchParams.get("face_node")).toBe(harness.nodeOrigins()[1]);
  expect(sanitized.hash).toBe("#review");

  for (const node of harness.nodes()) {
    node.statusAuthorizations.length = 0;
  }
  await page.reload();
  await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();
  await expect
    .poll(() =>
      harness.nodes().every((node) => node.statusAuthorizations.includes(`Bearer ${node.token}`)),
    )
    .toBe(true);
  expect(page.url()).not.toContain("_token=");
});

test("production-shaped partial arm rolls back, exposes the failure, and retries", async ({
  page,
}) => {
  harness.faceOn.failNextArm = true;
  await page.goto(harness.applicationUrl());
  await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();

  const armButton = page.getByRole("button", { name: "Arm audio capture" });
  await armButton.click();
  await expect(reviewErrorAlert(page)).toContainText("Unable to arm both Android nodes");
  await expect(reviewErrorAlert(page)).toContainText(
    "Face-on Android node request failed (HTTP 409): camera unavailable",
  );
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  await expect(armButton).toBeEnabled();
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-partial-arm-failure.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );
  expect(
    harness.downTheLine.mutations.some(
      (request) =>
        request.pathname === "/api/v1/capture/arm" && JSON.parse(request.body).armed === false,
    ),
    "the successful DTL arm must be rolled back",
  ).toBe(true);

  await armButton.click();
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  await expect(reviewErrorAlert(page)).toHaveCount(0);

  await page.getByRole("button", { name: "Save missed shot" }).click();
  await expect
    .poll(() =>
      harness
        .nodes()
        .map((node) =>
          node.mutations.some((request) => request.pathname === "/api/v1/capture/missed-shot"),
        ),
    )
    .toEqual([true, true]);
  for (const node of harness.nodes()) {
    expect(
      node.mutations
        .filter((request) => request.method !== "GET")
        .every((request) => request.authorization === `Bearer ${node.token}`),
    ).toBe(true);
  }

  const screenshot = await page.screenshot({ animations: "disabled", fullPage: false });
  expect({ width: screenshot.readUInt32BE(16), height: screenshot.readUInt32BE(20) }).toEqual({
    width: 1440,
    height: 1000,
  });
  await saveOutput("production-partial-arm-retry.png", screenshot);
});

test("production-shaped missed-shot rejection has no mutations before arm and retry", async ({
  page,
}) => {
  for (const node of harness.nodes()) {
    node.armed = true;
    node.activeSharedSessionId = null;
  }
  await page.goto(harness.applicationUrl());
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  const mutationsBeforeSave = harness.nodes().map((node) => node.mutations.length);

  // Reproduce the field race: the combined browser status is still armed when one phone has
  // already disarmed. saveMissedShot() must re-check both phones before either POST is sent.
  harness.faceOn.armed = false;
  await page.getByRole("button", { name: "Save missed shot" }).click();
  await expect(reviewErrorAlert(page)).toContainText(
    "Both Android nodes must be armed before saving a missed shot",
  );
  expect(harness.nodes().map((node) => node.mutations.length)).toEqual(mutationsBeforeSave);
  expect(
    harness
      .nodes()
      .flatMap((node) => node.mutations)
      .filter((request) => request.pathname === "/api/v1/capture/missed-shot"),
  ).toEqual([]);
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-missed-shot-requires-both-armed.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );

  // Clear the browser's deliberately stale armed status, then prove the operator can arm and
  // immediately retry without reloading the page.
  await page.getByRole("button", { name: "Disarm capture" }).click();
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  await page.getByRole("button", { name: "Arm audio capture" }).click();
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  await page.getByRole("button", { name: "Save missed shot" }).click();
  await expect
    .poll(() =>
      harness
        .nodes()
        .map(
          (node) =>
            node.mutations.filter((request) => request.pathname === "/api/v1/capture/missed-shot")
              .length,
        ),
    )
    .toEqual([1, 1]);
  await expect(reviewErrorAlert(page)).toHaveCount(0);
});

test("stale missed-shot credentials report 401 and recover after correction", async ({ page }) => {
  for (const node of harness.nodes()) {
    node.armed = true;
    node.activeSharedSessionId = null;
  }
  const staleCredentialUrl = harness.applicationUrl();
  await page.goto(staleCredentialUrl);
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();

  harness.faceOn.token = "replacement_face_control_token_01";
  harness.faceOn.credentialGeneration += 1;

  await page.getByRole("button", { name: "Save missed shot" }).click();
  const staleCredentialError = reviewErrorAlert(page);
  await expect(staleCredentialError).toContainText(
    "Face-on Android node request failed (HTTP 401): a valid bearer control credential is required",
  );
  expect(harness.nodes().map((node) => node.armed)).toEqual([true, true]);
  expect(
    harness
      .nodes()
      .flatMap((node) => node.mutations)
      .filter((request) => request.pathname === "/api/v1/capture/missed-shot"),
    "one stale token must fail authenticated read-only preflight before either phone is tagged",
  ).toEqual([]);
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-missed-shot-stale-credential.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );

  // Opening the same station with the corrected per-phone credentials is the explicit recovery
  // action available after an out-of-band credential replacement.
  await page.goto(harness.applicationUrl());
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  await page.getByRole("button", { name: "Save missed shot" }).click();
  await expect(page.getByText("Saving standby diagnostics")).toBeVisible();
  await expect(reviewErrorAlert(page)).toHaveCount(0);
  expect(harness.nodes().map((node) => node.armed)).toEqual([false, false]);
  for (const node of harness.nodes()) {
    const requests = node.mutations.filter(
      (request) => request.pathname === "/api/v1/capture/missed-shot",
    );
    expect(requests).toHaveLength(1);
    expect(requests[0]?.authorization).toBe(`Bearer ${node.token}`);
  }
});

test("partial missed-shot acceptance safely disarms both phones and retries in one tab", async ({
  page,
}) => {
  harness.setFixtureSessionPublished(false);
  await page.goto(harness.applicationUrl());
  const armButton = page.getByRole("button", { name: "Arm audio capture" });
  await armButton.click();
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();

  // Reproduce the unavoidable race after the authenticated status preflight: DTL accepts the
  // diagnostic retention request while face-on rejects its concurrent request. The browser
  // cannot undo the retained DTL ring, but it must stop the still-armed peer and discard its stale
  // shared-session ownership before offering another shot.
  harness.faceOn.failNextMissedShot = true;
  await page.getByRole("button", { name: "Save missed shot" }).click();
  await expect(reviewErrorAlert(page)).toContainText(
    "Unable to save missed shot both Android nodes: Face-on Android node request failed (HTTP 409): diagnostic retention unavailable",
  );
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  expect(harness.nodes().map((node) => node.armed)).toEqual([false, false]);
  expect(
    harness
      .nodes()
      .map(
        (node) =>
          node.mutations.filter((request) => request.pathname === "/api/v1/capture/missed-shot")
            .length,
      ),
    "both diagnostic requests reached their authenticated phone origins",
  ).toEqual([1, 1]);
  for (const node of harness.nodes()) {
    const convergenceDisarms = node.mutations.filter(
      (request) =>
        request.pathname === "/api/v1/capture/arm" &&
        (JSON.parse(request.body) as { armed?: unknown }).armed === false,
    );
    expect(convergenceDisarms, `${node.role} must receive the convergence disarm`).toHaveLength(1);
    expect(convergenceDisarms[0]?.authorization).toBe(`Bearer ${node.token}`);
  }
  expect(await accessibilityViolations(page)).toEqual([]);

  await armButton.click();
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  await expect(reviewErrorAlert(page)).toHaveCount(0);
  await page.getByRole("button", { name: "Save missed shot" }).click();
  await expect
    .poll(() =>
      harness
        .nodes()
        .map(
          (node) =>
            node.mutations.filter((request) => request.pathname === "/api/v1/capture/missed-shot")
              .length,
        ),
    )
    .toEqual([2, 2]);
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  expect(harness.nodes().map((node) => node.armed)).toEqual([false, false]);
  await saveOutput(
    "production-partial-missed-shot-retry.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );
});

test("one stale review credential cannot partially save feedback", async () => {
  const stale = harness.reviewApi();
  await stale.getManifest(harness.scenario.shared_session_id);
  harness.faceOn.token = "replacement_face_control_token_01";
  harness.faceOn.credentialGeneration += 1;

  await expect(
    stale.submitDiagnosticFeedback(harness.scenario.shared_session_id, {
      schema_version: 1,
      classification: "good_capture",
      note: "Credential preflight regression",
    }),
  ).rejects.toThrow(
    "Face-on Android node request failed (HTTP 401): a valid bearer control credential is required Re-enter the face-on control credential for this station.",
  );
  expect(
    harness
      .nodes()
      .flatMap((node) => node.mutations)
      .filter((request) => request.pathname.endsWith("/feedback")),
    "feedback must remain absent on both nodes when either credential fails read-only preflight",
  ).toEqual([]);

  const corrected = harness.reviewApi();
  await corrected.submitDiagnosticFeedback(harness.scenario.shared_session_id, {
    schema_version: 1,
    classification: "good_capture",
    note: "Credential corrected",
  });
  expect(
    harness
      .nodes()
      .flatMap((node) =>
        node.mutations
          .filter((request) => request.pathname.endsWith("/feedback"))
          .map((request) => [node.role, request.authorization]),
      ),
  ).toEqual([
    ["down_the_line", `Bearer ${harness.downTheLine.token}`],
    ["face_on", `Bearer ${harness.faceOn.token}`],
  ]);
});

test("production-shaped UI rejects a wrong face-on credential before either arm mutation", async ({
  page,
}) => {
  harness.setFixtureSessionPublished(false);
  await page.goto(harness.applicationUrl({ face_on: "definitely-wrong" }));
  const armButton = page.getByRole("button", { name: "Arm audio capture" });
  await expect(armButton).toBeDisabled();

  await expect(reviewErrorAlert(page)).toContainText(
    "Face-on Android node request failed (HTTP 401): a valid bearer control credential is required",
  );
  await expect(reviewErrorAlert(page)).toContainText(
    "Re-enter the face-on control credential for this station.",
  );
  expect(
    harness
      .nodes()
      .flatMap((node) =>
        node.mutations.filter((request) => request.pathname === "/api/v1/capture/arm"),
      ),
    "read-only credential preflight must avoid a partial arm that needs rollback",
  ).toEqual([]);
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-wrong-face-credential.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );
});

test("production-shaped UI rejects a missing DTL credential before either arm mutation", async ({
  page,
}) => {
  harness.setFixtureSessionPublished(false);
  await page.goto(harness.applicationUrl({ down_the_line: null }));
  const armButton = page.getByRole("button", { name: "Arm audio capture" });
  await expect(armButton).toBeDisabled();

  await expect(reviewErrorAlert(page)).toContainText(
    "Down-the-line Android node request failed (HTTP 401): a valid bearer control credential is required",
  );
  await expect(reviewErrorAlert(page)).toContainText(
    "Re-enter the down-the-line control credential for this station.",
  );
  expect(
    harness
      .nodes()
      .flatMap((node) =>
        node.mutations.filter((request) => request.pathname === "/api/v1/capture/arm"),
      ),
    "read-only credential preflight must avoid a partial arm that needs rollback",
  ).toEqual([]);
  expect(await accessibilityViolations(page)).toEqual([]);
});

test("credential rotation remains signed in across setup, review, and reload", async ({ page }) => {
  harness.setFixtureSessionPublished(false);
  await page.goto(harness.applicationUrl());
  await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();
  await page.getByRole("button", { name: "Phone setup" }).click();
  await expect(page.getByRole("heading", { name: "Two-phone station configured" })).toBeVisible();
  await expect
    .poll(() =>
      harness
        .nodes()
        .map((node) => node.previewAuthorizations.includes(`Bearer ${node.originalToken}`)),
    )
    .toEqual([true, true]);
  await expect(page.getByRole("img", { name: "Down-the-line phone setup preview" })).toBeVisible();
  await expect(
    page.getByRole("img", { name: "Across-the-line phone setup preview" }),
  ).toBeVisible();
  for (const node of harness.nodes()) {
    expect(new Set(node.previewAuthorizations)).toEqual(new Set([`Bearer ${node.originalToken}`]));
  }

  const downTheLineCard = page
    .getByRole("heading", { name: "Down-the-line phone" })
    .locator("xpath=ancestor::article[1]");
  await expect(downTheLineCard.getByLabel(/Operational health/)).toContainText(
    "light thermal load · 0.18 thermal headroom · power saver off · 20.0 GiB free",
  );
  const nodeId = harness.scenario.roles.down_the_line.node_id;
  await downTheLineCard.getByLabel("Confirm the full local node ID to rotate").fill(nodeId);
  await downTheLineCard
    .getByRole("button", { name: "Rotate this phone's control credential" })
    .click();
  const rotatedToken = "rotated_fixture_dtl_control_0001";
  await expect(downTheLineCard.getByText(rotatedToken, { exact: true })).toBeVisible();
  await expect(downTheLineCard.getByText("Current generation 2", { exact: false })).toBeVisible();
  expect(new URL(page.url()).searchParams.has("dtl_token")).toBe(false);
  expect(page.url()).not.toContain(rotatedToken);
  await expect
    .poll(() => harness.downTheLine.previewAuthorizations.includes(`Bearer ${rotatedToken}`))
    .toBe(true);

  await page.getByRole("button", { name: "Review" }).click();
  const armButton = page.getByRole("button", { name: "Arm audio capture" });
  await expect(armButton).toBeEnabled();
  await armButton.click();
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  await expect
    .poll(() => harness.downTheLine.statusAuthorizations.includes(`Bearer ${rotatedToken}`))
    .toBe(true);
  expect(
    harness.downTheLine.mutations.find(
      (request) => request.pathname === "/api/v1/capture/arm" && JSON.parse(request.body).armed,
    )?.authorization,
  ).toBe(`Bearer ${rotatedToken}`);

  await page.reload();
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  await expect(reviewErrorAlert(page)).toHaveCount(0);
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-credential-rotation-review-reload.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );
});

test("production-shaped mutation connection drop rolls back and recovers without reload", async ({
  page,
}) => {
  harness.faceOn.dropArmConnections = true;
  harness.setFixtureSessionPublished(false);
  await page.goto(harness.applicationUrl());
  const armButton = page.getByRole("button", { name: "Arm audio capture" });
  await expect(armButton).toBeEnabled();
  await armButton.click();

  await expect(reviewErrorAlert(page)).toContainText(
    "Unable to arm both Android nodes: Face-on Android node is unreachable",
  );
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  expect(harness.downTheLine.armed).toBe(false);
  expect(
    harness.downTheLine.mutations.some(
      (request) =>
        request.pathname === "/api/v1/capture/arm" && JSON.parse(request.body).armed === false,
    ),
    "a connection drop after the peer arm mutation must roll back the successful node",
  ).toBe(true);
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-arm-connection-drop.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );

  harness.faceOn.dropArmConnections = false;
  await armButton.click();
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  await expect(reviewErrorAlert(page)).toHaveCount(0);
  expect(harness.nodes().map((node) => node.armed)).toEqual([true, true]);
});

test("partial field-recording start rolls back and retries without reload", async ({ page }) => {
  harness.faceOn.failNextFieldRecordingStart = true;
  await page.goto(harness.applicationUrl());
  await openReviewInspector(page, "Capture");

  const panel = page.getByRole("region", { name: "Continuous test recording" });
  const start = panel.getByRole("button", { name: "Start field recording" });
  await expect(start).toBeEnabled();
  await start.click();

  const failure = panel.getByRole("alert");
  await expect(failure).toContainText(
    "Unable to start field recording on both Android nodes: Face-on Android node request failed (HTTP 409): recorder unavailable",
  );
  await expect(start).toBeEnabled();
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual(["idle", "idle"]);

  const firstStarts = harness
    .nodes()
    .flatMap((node) =>
      node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/start"),
    );
  expect(firstStarts).toHaveLength(2);
  const firstSharedRecordingIds = new Set(
    firstStarts.map(
      (request) =>
        (JSON.parse(request.body) as { shared_recording_id: string }).shared_recording_id,
    ),
  );
  expect(firstSharedRecordingIds.size).toBe(1);
  expect(
    harness.downTheLine.mutations.filter(
      (request) => request.pathname === "/api/v1/field-recording/stop",
    ),
    "the successfully started DTL recording must be stopped after its peer rejects start",
  ).toHaveLength(1);
  expect(
    harness.faceOn.mutations.filter(
      (request) => request.pathname === "/api/v1/field-recording/stop",
    ),
  ).toHaveLength(0);
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-field-recording-partial-start.png",
    await panel.screenshot({ animations: "disabled" }),
  );

  await start.click();
  await expect(panel.getByRole("button", { name: "Stop both phones" })).toBeVisible();
  await expect(failure).toHaveCount(0);
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual([
    "recording",
    "recording",
  ]);
  for (const node of harness.nodes()) {
    const starts = node.mutations.filter(
      (request) => request.pathname === "/api/v1/field-recording/start",
    );
    expect(starts).toHaveLength(2);
    expect(starts.every((request) => request.authorization === `Bearer ${node.token}`)).toBe(true);
  }
  const retrySharedRecordingIds = new Set(
    harness
      .nodes()
      .map((node) =>
        node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/start"),
      )
      .map(
        (requests) =>
          (JSON.parse(requests[1]?.body ?? "{}") as { shared_recording_id?: string })
            .shared_recording_id,
      ),
  );
  expect(retrySharedRecordingIds.size).toBe(1);
  expect(retrySharedRecordingIds).not.toEqual(firstSharedRecordingIds);

  await panel.getByRole("button", { name: "Stop both phones" }).click();
  await expect(start).toBeVisible();
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual(["idle", "idle"]);
  expect(
    harness
      .nodes()
      .map(
        (node) =>
          node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/stop")
            .length,
      ),
  ).toEqual([2, 1]);
});

test("accepted field-recording start failure rolls back both phones and retries", async ({
  page,
}) => {
  harness.faceOn.failNextFieldRecordingStartAfterAccept = true;
  await page.goto(harness.applicationUrl());
  await openReviewInspector(page, "Capture");

  const panel = page.getByRole("region", { name: "Continuous test recording" });
  const start = panel.getByRole("button", { name: "Start field recording" });
  await expect(start).toBeEnabled();
  await start.click();

  await expect(panel.getByRole("alert")).toContainText(
    "Face-on field recorder failed after accepting start: camera pipeline unavailable",
  );
  await expect(start).toBeEnabled();
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual(["idle", "idle"]);
  for (const node of harness.nodes()) {
    expect(
      node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/start"),
    ).toHaveLength(1);
    expect(
      node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/stop"),
      "both accepted starts need rollback even when only one asynchronous pipeline fails",
    ).toHaveLength(1);
  }
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-field-recording-accepted-failure.png",
    await panel.screenshot({ animations: "disabled" }),
  );

  await start.click();
  await expect(panel.getByRole("button", { name: "Stop both phones" })).toBeVisible();
  await expect(panel.getByRole("alert")).toHaveCount(0);
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual([
    "recording",
    "recording",
  ]);
});

test("stale field-recording credential blocks before either phone is disarmed", async ({
  page,
}) => {
  const staleCredentialUrl = harness.applicationUrl();
  harness.faceOn.token = "replacement_face_control_token_01";
  harness.faceOn.credentialGeneration += 1;
  await page.goto(staleCredentialUrl);
  await openReviewInspector(page, "Capture");

  const panel = page.getByRole("region", { name: "Continuous test recording" });
  await expect(panel.getByRole("alert")).toContainText(
    "Face-on Android node request failed (HTTP 401): a valid bearer control credential is required",
  );
  await expect(panel.getByRole("button", { name: "Start field recording" })).toBeDisabled();
  expect(
    harness
      .nodes()
      .flatMap((node) => node.mutations)
      .filter(
        (request) =>
          request.pathname === "/api/v1/capture/arm" ||
          request.pathname.startsWith("/api/v1/field-recording/"),
      ),
    "read-only credential preflight must run before disarm or recording mutations",
  ).toEqual([]);

  await page.goto(harness.applicationUrl());
  await openReviewInspector(page, "Capture");
  await panel.getByRole("button", { name: "Start field recording" }).click();
  await expect(panel.getByRole("button", { name: "Stop both phones" })).toBeVisible();
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual([
    "recording",
    "recording",
  ]);
});

test("historical catalog hydration cannot starve field-recording controls", async ({ page }) => {
  harness.holdHistoricalCatalog(8);
  harness.setFixtureSessionPublished(false);
  await page.goto(harness.applicationUrl());
  await openReviewInspector(page, "Capture");

  await expect
    .poll(() => harness.nodes().map((node) => node.historicalManifestRequestCount))
    .toEqual([2, 2]);

  const panel = page.getByRole("region", { name: "Continuous test recording" });
  const start = panel.getByRole("button", { name: "Start field recording" });
  await expect(start).toBeEnabled();
  await start.click();
  await expect(panel.getByRole("button", { name: "Stop both phones" })).toBeVisible();
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual([
    "recording",
    "recording",
  ]);
  expect(
    harness.nodes().map((node) => node.historicalManifestRequestCount),
    "only two background manifests per phone may consume browser/server connections",
  ).toEqual([2, 2]);
  expect(
    harness
      .nodes()
      .map(
        (node) =>
          node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/start")
            .length,
      ),
  ).toEqual([1, 1]);

  harness.releaseHistoricalManifests();
  await expect
    .poll(() => harness.nodes().map((node) => node.historicalManifestRequestCount))
    .toEqual([8, 8]);
  await panel.getByRole("button", { name: "Stop both phones" }).click();
  await expect(start).toBeVisible();
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual(["idle", "idle"]);
});

test("slow field-recording history cannot block live controls or stop completion", async ({
  page,
}) => {
  harness.holdFieldRecordingCatalog();
  await page.goto(harness.applicationUrl());
  await openReviewInspector(page, "Capture");

  await expect
    .poll(() => harness.nodes().map((node) => node.fieldRecordingCatalogRequestCount))
    .toEqual([1, 1]);
  const panel = page.getByRole("region", { name: "Continuous test recording" });
  await expect(panel).toContainText("Down the line");
  await expect(panel).toContainText("Face on");
  const start = panel.getByRole("button", { name: "Start field recording" });
  await expect(start).toBeEnabled();

  await start.click();
  const stop = panel.getByRole("button", { name: "Stop both phones" });
  await expect(stop).toBeVisible();
  await stop.click();
  await expect(start).toBeEnabled();
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual(["idle", "idle"]);
  expect(
    harness.nodes().map((node) => node.fieldRecordingCatalogRequestCount),
    "Stop queues history refresh without adding a second contending request",
  ).toEqual([1, 1]);

  harness.releaseFieldRecordingCatalog();
  await expect
    .poll(() => harness.nodes().map((node) => node.fieldRecordingCatalogRequestCount))
    .toEqual([2, 2]);
  await expect(panel).toHaveAttribute("data-recording-catalog-state", "ready");
});

test("field recording recovers a disconnected phone during stop without reload", async ({
  page,
}) => {
  await page.goto(harness.applicationUrl());
  await openReviewInspector(page, "Capture");
  const panel = page.getByRole("region", { name: "Continuous test recording" });
  const start = panel.getByRole("button", { name: "Start field recording" });
  await expect(start).toBeEnabled();
  await start.click();
  const stop = panel.getByRole("button", { name: "Stop both phones" });
  await expect(stop).toBeVisible();

  harness.faceOn.online = false;
  await stop.click();
  await expect(panel.getByRole("alert")).toContainText(
    "Face-on Android node request failed (HTTP 503): phone temporarily offline",
  );
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-field-recording-stop-disconnected.png",
    await panel.screenshot({ animations: "disabled" }),
  );
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual([
    "recording",
    "recording",
  ]);
  expect(
    harness.downTheLine.mutations.filter(
      (request) => request.pathname === "/api/v1/field-recording/stop",
    ),
  ).toHaveLength(0);

  harness.faceOn.online = true;
  await stop.click();
  await expect(start).toBeVisible();
  await expect(panel.getByRole("alert")).toHaveCount(0);
  expect(await accessibilityViolations(page)).toEqual([]);
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual(["idle", "idle"]);
  expect(
    harness
      .nodes()
      .map(
        (node) =>
          node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/stop")
            .length,
      ),
  ).toEqual([1, 1]);
});

test("field recording retries an after-preflight partial stop without reload", async ({ page }) => {
  await page.goto(harness.applicationUrl());
  await openReviewInspector(page, "Capture");
  const panel = page.getByRole("region", { name: "Continuous test recording" });
  const start = panel.getByRole("button", { name: "Start field recording" });
  await start.click();
  const stop = panel.getByRole("button", { name: "Stop both phones" });
  await expect(stop).toBeVisible();

  harness.faceOn.failNextFieldRecordingStop = true;
  await stop.click();
  await expect(panel.getByRole("alert")).toContainText(
    "Face-on Android node request failed (HTTP 503): stop transport interrupted",
  );
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual(["idle", "recording"]);
  await expect(stop).toBeVisible();

  await stop.click();
  await expect(start).toBeVisible();
  await expect(panel.getByRole("alert")).toHaveCount(0);
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual(["idle", "idle"]);
  expect(
    harness
      .nodes()
      .map(
        (node) =>
          node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/stop")
            .length,
      ),
  ).toEqual([2, 2]);
});

test("field recording preserves one stop action while both phones publish asynchronously", async ({
  page,
}) => {
  await page.goto(harness.applicationUrl());
  await openReviewInspector(page, "Capture");
  const panel = page.getByRole("region", { name: "Continuous test recording" });
  const start = panel.getByRole("button", { name: "Start field recording" });
  await start.click();
  const stop = panel.getByRole("button", { name: "Stop both phones" });
  await expect(stop).toBeVisible();

  harness.holdFieldRecordingStops();
  await stop.click();
  await expect(panel.getByRole("button", { name: "Stopping both phones…" })).toBeDisabled();
  await expect(panel.getByText("Stopping", { exact: true })).toHaveCount(2);
  expect(
    harness
      .nodes()
      .map(
        (node) =>
          node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/stop")
            .length,
      ),
  ).toEqual([1, 1]);

  harness.releaseFieldRecordingStops();
  await expect(start).toBeVisible();
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual(["idle", "idle"]);
  expect(
    harness
      .nodes()
      .map(
        (node) =>
          node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/stop")
            .length,
      ),
  ).toEqual([1, 1]);
});

test("accepted field-recording stop surfaces a terminal publication failure and retries", async ({
  page,
}) => {
  await page.goto(harness.applicationUrl());
  await openReviewInspector(page, "Capture");
  const panel = page.getByRole("region", { name: "Continuous test recording" });
  const start = panel.getByRole("button", { name: "Start field recording" });
  await start.click();
  const stop = panel.getByRole("button", { name: "Stop both phones" });
  await expect(stop).toBeVisible();

  harness.faceOn.failNextFieldRecordingStopAfterAccept = true;
  await stop.click();
  await expect(panel.getByRole("alert")).toContainText(
    "Face-on field recorder failed while stopping or publishing: manifest publication failed",
  );
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual(["idle", "error"]);
  expect(
    harness
      .nodes()
      .map(
        (node) =>
          node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/stop")
            .length,
      ),
    "both stop requests were accepted exactly once before terminal encoder state was observed",
  ).toEqual([1, 1]);
  await expect(start).toBeEnabled();
  expect(await accessibilityViolations(page)).toEqual([]);

  await start.click();
  await expect(stop).toBeVisible();
  await expect(panel.getByRole("alert")).toHaveCount(0);
  expect(harness.nodes().map((node) => node.fieldRecordingState)).toEqual([
    "recording",
    "recording",
  ]);
  expect(
    harness
      .nodes()
      .map(
        (node) =>
          node.mutations.filter((request) => request.pathname === "/api/v1/field-recording/start")
            .length,
      ),
    "the terminal publication failure must remain retryable without reloading the page",
  ).toEqual([2, 2]);
});

test("production-shaped publication distinguishes encoding, missing, and stale sessions", async () => {
  harness.setPublication("encoding");
  const api = harness.reviewApi();
  const encoding = (await api.getSessions()).sessions.find(
    (session) => session.session_id === PENDING_SHARED_SESSION_ID,
  );
  expect(encoding).toMatchObject({ state: "encoding", error: "" });

  harness.setPublication("missing");
  const missing = (await api.getSessions()).sessions.find(
    (session) => session.session_id === PENDING_SHARED_SESSION_ID,
  );
  expect(missing?.state).toBe("error");
  expect(missing?.error).toContain("missing the face-on clip");

  harness.setPublication("mismatched");
  await expect(api.getCaptureStatus()).rejects.toThrow("different active shared session IDs");
});

test("production-shaped UI follows a delayed peer clip through encoding, error, and recovery", async ({
  page,
}) => {
  harness.setPublication("encoding");
  await page.goto(harness.applicationUrl());

  await expect(page.getByRole("combobox", { name: "Recorded session" })).toHaveValue(
    PENDING_SHARED_SESSION_ID,
  );
  await expect(page.getByRole("status").filter({ hasText: "Encoding review clip" })).toContainText(
    "exact video playback is encoding",
  );
  await expect(page.locator('video[aria-label$="recorded swing"]')).toHaveCount(0);

  harness.setPublication("missing");
  const missingAlert = page.getByRole("alert").filter({ hasText: "missing the face-on clip" });
  await expect(missingAlert).toContainText(
    `Coordinated session ${PENDING_SHARED_SESSION_ID} is missing the face-on clip`,
  );
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-delayed-peer-missing.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );

  harness.setPublication("ready");
  await expect(missingAlert).toHaveCount(0);
  await expect(page.locator('video[aria-label$="recorded swing"]')).toHaveCount(2);
  await expect(page.getByText("Frame 46 of 90")).toBeVisible();
  const recoveredSources = await page
    .locator('video[aria-label$="recorded swing"] source')
    .evaluateAll((sources) =>
      sources.map((source) => new URL((source as HTMLSourceElement).src).origin),
    );
  expect(recoveredSources).toEqual(harness.nodeOrigins());
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-delayed-peer-recovered.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );
});

test("production-shaped UI recovers the historical missing down-the-line clip", async ({
  page,
}) => {
  harness.setPublication("encoding_down_the_line");
  await page.goto(harness.applicationUrl());

  await expect(page.getByRole("combobox", { name: "Recorded session" })).toHaveValue(
    PENDING_SHARED_SESSION_ID,
  );
  await expect(page.getByRole("status").filter({ hasText: "Encoding review clip" })).toContainText(
    "exact video playback is encoding",
  );
  await expect(page.locator('video[aria-label$="recorded swing"]')).toHaveCount(0);

  harness.setPublication("missing_down_the_line");
  const missingAlert = page
    .getByRole("alert")
    .filter({ hasText: "missing the down-the-line clip" });
  await expect(missingAlert).toContainText(
    `Coordinated session ${PENDING_SHARED_SESSION_ID} is missing the down-the-line clip`,
  );
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-delayed-dtl-missing.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );

  harness.setPublication("ready");
  await expect(missingAlert).toHaveCount(0);
  await expect(page.locator('video[aria-label$="recorded swing"]')).toHaveCount(2);
  await expect(page.getByText("Frame 46 of 90")).toBeVisible();
  expect(await accessibilityViolations(page)).toEqual([]);
});

test("production-shaped UI keeps review usable while one phone disconnects and auto-recovers", async ({
  page,
}) => {
  await page.goto(harness.applicationUrl());
  await expect(page.getByText("Frame 46 of 90")).toBeVisible();
  const timeline = page.getByRole("slider", { name: "Review timeline" });
  await timeline.fill("11");
  await expect(page.locator(".timeline-summary")).toContainText("Frame 12 of 90");

  harness.faceOn.online = false;
  const unavailable = reviewErrorAlert(page).filter({
    hasText: "Face-on Android node request failed",
  });
  await expect(unavailable).toContainText("HTTP 503");
  await expect(unavailable).toContainText("phone temporarily offline");
  await expect(page.getByRole("heading", { name: "Connecting to capture engine" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Arm audio capture" })).toBeDisabled();
  await expect(page.locator(".timeline-summary")).toContainText("Frame 12 of 90");
  await expect(page.getByRole("button", { name: "Next frame" })).toBeEnabled();
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-phone-disconnected.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );

  harness.faceOn.online = true;
  await expect(unavailable).toHaveCount(0);
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  await expect(page.getByText("Frame 12 of 90")).toBeVisible();
  await page.getByRole("button", { name: "Next frame" }).click();
  await expect(page.getByText("Frame 13 of 90")).toBeVisible();
});

test("production-shaped UI rejects stale status data and accepts a fresh reconnect", async ({
  page,
}) => {
  await page.goto(harness.applicationUrl());
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  await expect(page.getByText("Frame 46 of 90")).toBeVisible();

  harness.faceOn.staleLiveStatus = true;
  const stale = reviewErrorAlert(page).filter({
    hasText: "Face-on Android live status is disconnected or stale",
  });
  await expect(stale).toBeVisible();
  await expect(page.getByRole("heading", { name: "Connecting to capture engine" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Arm audio capture" })).toBeDisabled();
  await expect(page.getByText("Frame 46 of 90")).toBeVisible();

  harness.faceOn.staleLiveStatus = false;
  await expect(stale).toHaveCount(0);
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Arm audio capture" })).toBeEnabled();
});

test("production-shaped UI blocks stale cross-phone session ownership and then recovers", async ({
  page,
}) => {
  harness.setPublication("mismatched");
  await page.goto(harness.applicationUrl());

  const mismatch = reviewErrorAlert(page).filter({
    hasText: "Android nodes report different active shared session IDs",
  });
  await expect(mismatch).toBeVisible();
  await expect(page.getByRole("heading", { name: "Connecting to capture engine" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Arm audio capture" })).toBeDisabled();
  expect(await accessibilityViolations(page)).toEqual([]);

  harness.setPublication("none");
  await expect(mismatch).toHaveCount(0);
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Arm audio capture" })).toBeEnabled();
});

test("production-shaped UI diagnoses duplicate live roles and recovers after setup correction", async ({
  page,
}) => {
  harness.setFixtureSessionPublished(false);
  harness.faceOn.advertisedRole = "down_the_line";
  await page.goto(harness.applicationUrl());

  const roleConflict = reviewErrorAlert(page).filter({
    hasText: "Dual-node capture requires one live phone for each camera role",
  });
  await expect(roleConflict).toBeVisible();
  await expect(page.getByRole("heading", { name: "Connecting to capture engine" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Arm audio capture" })).toBeDisabled();
  await expect(page.getByRole("button", { name: "Phone setup" })).toBeVisible();
  expect(await accessibilityViolations(page)).toEqual([]);
  await saveOutput(
    "production-duplicate-phone-roles.png",
    await page.screenshot({ animations: "disabled", fullPage: false }),
  );

  // This mutates the descriptor returned by the running fixture node, matching an operator fixing
  // the role through phone setup while the review tab remains open.
  harness.faceOn.advertisedRole = "face_on";
  await expect(roleConflict).toHaveCount(0);
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  const armButton = page.getByRole("button", { name: "Arm audio capture" });
  await expect(armButton).toBeEnabled();
  await armButton.click();
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  expect(harness.nodes().map((node) => node.armed)).toEqual([true, true]);
});

test("production-shaped UI follows live roles after bookmark role assignments become stale", async ({
  page,
}) => {
  harness.setFixtureSessionPublished(false);
  // The bookmark still calls the first origin DTL and the second face-on, but the operator has
  // since swapped the roles in phone setup. Runtime descriptors must be authoritative without
  // ever moving one origin's credential to the other origin.
  harness.downTheLine.advertisedRole = "face_on";
  harness.faceOn.advertisedRole = "down_the_line";
  harness.downTheLine.failNextArm = true;
  await page.goto(harness.applicationUrl());

  const armButton = page.getByRole("button", { name: "Arm audio capture" });
  await expect(armButton).toBeEnabled();
  await armButton.click();
  await expect(reviewErrorAlert(page)).toContainText(
    "Face-on Android node request failed (HTTP 409): camera unavailable",
  );
  expect(harness.faceOn.armed).toBe(false);
  expect(
    harness.faceOn.mutations.some(
      (request) =>
        request.pathname === "/api/v1/capture/arm" && JSON.parse(request.body).armed === false,
    ),
    "the live DTL node must be rolled back even though the bookmark labels its origin face-on",
  ).toBe(true);
  for (const node of harness.nodes()) {
    expect(
      node.mutations.every((request) => request.authorization === `Bearer ${node.token}`),
      "a stale role hint must not swap credentials between origins",
    ).toBe(true);
  }

  await armButton.click();
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  await expect(reviewErrorAlert(page)).toHaveCount(0);
  expect(harness.nodes().map((node) => node.armed)).toEqual([true, true]);
  expect(await accessibilityViolations(page)).toEqual([]);
});

test("scoped media capabilities reject missing, wrong, and rotated grants", async ({ request }) => {
  const manifest = await harness.reviewApi().getManifest(harness.scenario.shared_session_id);
  expect(manifest.views).toHaveLength(2);
  for (const [index, view] of manifest.views.entries()) {
    const mediaUrl = new URL(view.media.url ?? "http://invalid.test/");
    expect(mediaUrl.searchParams.get("media_access")).toMatch(/^[A-Za-z0-9_-]{43}$/);

    const valid = await request.get(mediaUrl.toString(), { headers: { Range: "bytes=0-0" } });
    expect(valid.status()).toBe(206);

    const missing = new URL(mediaUrl);
    missing.search = "";
    expect((await request.get(missing.toString())).status()).toBe(401);

    const wrong = new URL(mediaUrl);
    wrong.searchParams.set("media_access", "W".repeat(43));
    expect((await request.get(wrong.toString())).status()).toBe(401);

    const node = harness.nodes()[index];
    if (node === undefined) throw new Error("Fixture manifest returned an unexpected third view");
    node.token = index === 0 ? "R".repeat(32) : "S".repeat(32);
    node.credentialGeneration += 1;
    expect((await request.get(mediaUrl.toString())).status()).toBe(401);

    const bearerOverride = await request.get(mediaUrl.toString(), {
      headers: { Authorization: `Bearer ${node.token}`, Range: "bytes=0-0" },
    });
    expect(bearerOverride.status()).toBe(206);
  }
});

test("production-shaped H.264 uses two origins, byte ranges, and exact player controls", async ({
  page,
}) => {
  const mediaResponses = new Set<string>();
  page.on("response", (response) => {
    const url = new URL(response.url());
    // Chromium and Firefox request a byte range immediately. WebKit may fetch
    // the complete small clip first; the explicit requests below independently
    // require and validate 206/416 range behavior for both node origins.
    if ([200, 206].includes(response.status()) && url.pathname.endsWith(".mp4")) {
      mediaResponses.add(url.origin);
    }
  });
  await page.goto(harness.applicationUrl());
  await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();
  await expect(page.getByRole("combobox", { name: "Recorded session" })).toHaveValue(
    harness.scenario.shared_session_id,
  );

  const videos = page.locator('video[aria-label$="recorded swing"]');
  await expect(videos).toHaveCount(2);
  const sources = await videos.evaluateAll((elements) =>
    elements.map((element) => {
      const source = element.querySelector("source");
      return { src: source?.src ?? "", type: source?.type ?? "" };
    }),
  );
  expect(sources.map(({ src }) => new URL(src).origin)).toEqual(harness.nodeOrigins());
  expect(sources.map(({ type }) => type)).toEqual([
    'video/mp4; codecs="avc1.42C01E"',
    'video/mp4; codecs="avc1.42C01E"',
  ]);
  const inspector = await openReviewInspector(page, "Session");
  await expect(inspector.getByText("AVC1.42C01E · inter-frame · full resolution")).toBeVisible();
  expect(harness.scenario).toMatchObject({
    all_frames_keyframes: false,
    keyframe_interval_frames: 30,
  });

  for (const { src } of sources) {
    expect(new URL(src).searchParams.get("media_access")).toMatch(/^[A-Za-z0-9_-]{43}$/);
    const partial = await page.request.get(src, { headers: { Range: "bytes=7-31" } });
    expect(partial.status()).toBe(206);
    expect(partial.headers()["accept-ranges"]).toBe("bytes");
    expect(partial.headers()["content-range"]).toMatch(/^bytes 7-31\/\d+$/);
    expect((await partial.body()).length).toBe(25);
    const invalid = await page.request.get(src, { headers: { Range: "bytes=9999999-" } });
    expect(invalid.status()).toBe(416);
    expect(invalid.headers()["content-range"]).toMatch(/^bytes \*\/\d+$/);
  }
  const uncredentialedMedia = new URL(sources[0]?.src ?? "http://invalid.test/");
  uncredentialedMedia.search = "";
  expect((await page.request.get(uncredentialedMedia.toString())).status()).toBe(401);
  expect(harness.nodes().map((node) => node.rangeStatuses.includes(206))).toEqual([true, true]);
  expect(harness.nodes().map((node) => node.rangeStatuses.includes(416))).toEqual([true, true]);
  const h264Support = await page.evaluate(() =>
    document.createElement("video").canPlayType('video/mp4; codecs="avc1.42C01E"'),
  );
  if (process.env.SWING_CAPTURE_REQUIRE_H264_DECODE === "1") {
    expect(h264Support, "the system-browser target requires AVC/H.264 decoding").not.toBe("");
  }
  test.info().annotations.push({
    type: "h264-codec",
    description:
      h264Support === ""
        ? "Bundled Chromium has no proprietary H.264 decoder; run //web:system_h264_browser_test"
        : `Browser reports ${h264Support}`,
  });
  if (h264Support !== "") {
    // WebKit's headless MiniBrowser may defer even preload="auto" media until
    // the element receives an explicit load request. Exercise the same source
    // elements while making the cross-engine test deterministic.
    await videos.evaluateAll((elements) => {
      for (const element of elements) {
        (element as HTMLVideoElement).load();
      }
    });
    await expect.poll(() => [...mediaResponses].sort()).toEqual([...harness.nodeOrigins()].sort());
    await expect
      .poll(async () =>
        videos.evaluateAll((elements) =>
          elements.map((element) => {
            const video = element as HTMLVideoElement;
            return {
              readyState: video.readyState,
              duration: video.duration,
              width: video.videoWidth,
              height: video.videoHeight,
            };
          }),
        ),
      )
      .toEqual([
        { readyState: 4, duration: 3, width: 320, height: 180 },
        { readyState: 4, duration: 3, width: 320, height: 180 },
      ]);
    for (const { src } of sources) {
      expect(await nonblackPixelFraction(page, src)).toBeGreaterThan(0.2);
    }

    const timeline = page.getByRole("slider", { name: "Review timeline" });
    const presentationEvidence: Array<{
      label: string;
      frame_index: number;
      frames: PresentedFrameEvidence[];
    }> = [];
    const captureAction = async (
      label: string,
      frameIndex: number,
      action: () => Promise<void>,
    ) => {
      const expectedMediaTimeSeconds = fixtureMediaTimeSeconds(harness.scenario, frameIndex);
      const frames = await capturePresentedFrameEvidence(page, expectedMediaTimeSeconds, action);
      for (const frame of frames) {
        expect(
          frame.media_time_seconds,
          `${frame.role} must present the requested ${label} frame`,
        ).toBeCloseTo(expectedMediaTimeSeconds, 4);
        expect(
          frame.nonblack_fraction,
          `${frame.role} ${label} frame must contain decoded pixels`,
        ).toBeGreaterThan(0.2);
        expect(frame.pixel_hash).toMatch(/^[0-9a-f]{8}$/);
      }
      presentationEvidence.push({ label, frame_index: frameIndex, frames });
      return frames;
    };

    await captureAction("forward GOP seek", 61, () => timeline.fill("61"));
    await expect(page.getByText("Frame 62 of 90")).toBeVisible();
    await expectVideoTimesWithinFixtureFrame(videos, harness.scenario, 61);
    await captureAction("backward GOP seek", 28, () => timeline.fill("28"));
    await expect(page.getByText("Frame 29 of 90")).toBeVisible();
    await expectVideoTimesWithinFixtureFrame(videos, harness.scenario, 28);
    const firstFrame29 = await captureAction("single-frame step", 29, () =>
      page.getByRole("button", { name: "Next frame" }).click(),
    );
    await expect(page.locator(".timeline-summary")).toContainText("Frame 30 of 90");
    await expectVideoTimesWithinFixtureFrame(videos, harness.scenario, 29);
    await captureAction("IDR-boundary step", 30, () =>
      page.getByRole("button", { name: "Next frame" }).click(),
    );
    await expect(page.getByText("Frame 31 of 90")).toBeVisible();
    const repeatedFrame29 = await captureAction("reverse single-frame step", 29, () =>
      page.getByRole("button", { name: "Previous frame" }).click(),
    );
    await expect(page.getByText("Frame 30 of 90")).toBeVisible();
    expect(
      Object.fromEntries(repeatedFrame29.map((frame) => [frame.role, frame.pixel_hash])),
    ).toEqual(Object.fromEntries(firstFrame29.map((frame) => [frame.role, frame.pixel_hash])));
    for (const role of ["Down-the-line recorded swing", "Face-on recorded swing"]) {
      const hashes = presentationEvidence.flatMap((capture) =>
        capture.frames.filter((frame) => frame.role === role).map((frame) => frame.pixel_hash),
      );
      expect(
        new Set(hashes).size,
        `${role} fixture must visibly distinguish requested frames`,
      ).toBeGreaterThanOrEqual(3);
    }
    await test.info().attach("presented-frame-evidence.json", {
      body: Buffer.from(JSON.stringify(presentationEvidence, null, 2)),
      contentType: "application/json",
    });
    await page.getByRole("button", { name: "Play" }).click();
    await expect(page.getByRole("button", { name: "Pause" })).toBeVisible();
    await expect
      .poll(async () => videos.nth(0).evaluate((video) => (video as HTMLVideoElement).currentTime))
      .toBeGreaterThan(0.98);
    await page.getByRole("button", { name: "Pause" }).click();
  }

  expect(await accessibilityViolations(page)).toEqual([]);

  const screenshot = await page.screenshot({ animations: "disabled", fullPage: false });
  expect({ width: screenshot.readUInt32BE(16), height: screenshot.readUInt32BE(20) }).toEqual({
    width: 1440,
    height: 1000,
  });
  await saveOutput("production-two-origin-h264.png", screenshot);
});

class ProductionHarness {
  readonly downTheLine: NodeState;
  readonly faceOn: NodeState;

  private constructor(
    readonly scenario: FixtureScenario,
    private readonly staticServer: Server,
    private readonly nodeServers: readonly [Server, Server],
    private readonly staticOrigin: string,
    private readonly origins: readonly [string, string],
    states: readonly [NodeState, NodeState],
  ) {
    [this.downTheLine, this.faceOn] = states;
  }

  static async start(staticRoot: string, fixtureRoot: string): Promise<ProductionHarness> {
    const scenario = JSON.parse(
      await readFile(path.join(fixtureRoot, "scenario.json"), "utf8"),
    ) as FixtureScenario;
    if (scenario.schema_version !== 1 || scenario.frame_count !== 90) {
      throw new Error("Unsupported production H.264 fixture metadata");
    }
    const states = [
      nodeState("down_the_line", TOKENS.down_the_line),
      nodeState("face_on", TOKENS.face_on),
    ] as const;
    const nodeServers = states.map((state) =>
      createServer((request, response) => {
        void serveNode(scenario, fixtureRoot, state, request, response);
      }),
    ) as unknown as [Server, Server];
    const [downOrigin, faceOrigin] = await Promise.all(nodeServers.map(listen));
    const staticServer = createServer((request, response) => {
      void serveStatic(staticRoot, request, response);
    });
    const staticOrigin = await listen(staticServer);
    const harness = new ProductionHarness(
      scenario,
      staticServer,
      nodeServers,
      staticOrigin,
      [downOrigin, faceOrigin],
      states,
    );
    return harness;
  }

  reset(): void {
    for (const node of this.nodes()) {
      for (const release of node.pendingHistoricalManifestResponses.splice(0)) {
        release();
      }
      for (const release of node.pendingFieldRecordingCatalogResponses.splice(0)) {
        release();
      }
      node.armed = false;
      node.advertisedRole = node.role;
      node.token = node.originalToken;
      node.setupRevision = 4;
      node.credentialGeneration = 1;
      node.liveStatusRevision = 0;
      node.staleLiveStatus = false;
      node.online = true;
      node.activeSharedSessionId = null;
      node.failNextArm = false;
      node.failNextMissedShot = false;
      node.failNextFieldRecordingStart = false;
      node.failNextFieldRecordingStartAfterAccept = false;
      node.failNextFieldRecordingStop = false;
      node.failNextFieldRecordingStopAfterAccept = false;
      node.dropArmConnections = false;
      node.fieldRecordingState = "idle";
      node.fieldRecordingError = "";
      node.fieldRecordingPublicationFailurePending = false;
      node.holdFieldRecordingStop = false;
      node.activeFieldRecordingId = null;
      node.sharedFieldRecordingId = null;
      node.publishFixtureSession = true;
      node.historicalSessionCount = 0;
      node.holdHistoricalManifests = false;
      node.historicalManifestRequestCount = 0;
      node.holdFieldRecordingCatalog = false;
      node.fieldRecordingCatalogRequestCount = 0;
      node.publication = "none";
      node.mutations.length = 0;
      node.rangeStatuses.length = 0;
      node.statusAuthorizations.length = 0;
      node.previewAuthorizations.length = 0;
    }
  }

  nodes(): readonly [NodeState, NodeState] {
    return [this.downTheLine, this.faceOn];
  }

  nodeOrigins(): [string, string] {
    return [...this.origins];
  }

  applicationUrl(credentials: Partial<Record<ReviewRole, string | null>> = {}): string {
    const url = new URL("/index.html", this.staticOrigin);
    url.searchParams.set("dtl_node", this.origins[0]);
    const downTheLineToken = credentials.down_the_line ?? this.downTheLine.token;
    if (credentials.down_the_line !== null) {
      url.searchParams.set("dtl_token", downTheLineToken);
    }
    url.searchParams.set("face_node", this.origins[1]);
    const faceOnToken = credentials.face_on ?? this.faceOn.token;
    if (credentials.face_on !== null) {
      url.searchParams.set("face_token", faceOnToken);
    }
    url.hash = "review";
    return url.toString();
  }

  reviewApi(): DualNodeReviewApi {
    let now = 0n;
    return new DualNodeReviewApi(
      [
        { baseUrl: this.origins[0], controlToken: this.downTheLine.token, role: "down_the_line" },
        { baseUrl: this.origins[1], controlToken: this.faceOn.token, role: "face_on" },
      ],
      globalThis.fetch.bind(globalThis),
      () => (now += 1_000_000n),
      undefined,
      null,
    );
  }

  setPublication(publication: NodeState["publication"]): void {
    this.downTheLine.publication = publication;
    this.faceOn.publication = publication;
  }

  setFixtureSessionPublished(published: boolean): void {
    this.downTheLine.publishFixtureSession = published;
    this.faceOn.publishFixtureSession = published;
  }

  holdHistoricalCatalog(sessionCount: number): void {
    for (const node of this.nodes()) {
      node.historicalSessionCount = sessionCount;
      node.holdHistoricalManifests = true;
      node.historicalManifestRequestCount = 0;
    }
  }

  releaseHistoricalManifests(): void {
    for (const node of this.nodes()) {
      node.holdHistoricalManifests = false;
      for (const release of node.pendingHistoricalManifestResponses.splice(0)) {
        release();
      }
    }
  }

  holdFieldRecordingCatalog(): void {
    for (const node of this.nodes()) {
      node.holdFieldRecordingCatalog = true;
      node.fieldRecordingCatalogRequestCount = 0;
    }
  }

  releaseFieldRecordingCatalog(): void {
    for (const node of this.nodes()) {
      node.holdFieldRecordingCatalog = false;
      for (const release of node.pendingFieldRecordingCatalogResponses.splice(0)) {
        release();
      }
    }
  }

  holdFieldRecordingStops(): void {
    for (const node of this.nodes()) node.holdFieldRecordingStop = true;
  }

  releaseFieldRecordingStops(): void {
    for (const node of this.nodes()) {
      node.holdFieldRecordingStop = false;
      if (node.fieldRecordingState === "stopping") {
        node.fieldRecordingState = "idle";
        node.activeFieldRecordingId = null;
        node.sharedFieldRecordingId = null;
      }
    }
  }

  async close(): Promise<void> {
    this.releaseHistoricalManifests();
    this.releaseFieldRecordingCatalog();
    await Promise.all([close(this.staticServer), ...this.nodeServers.map(close)]);
  }
}

function nodeState(role: ReviewRole, token: string): NodeState {
  return {
    role,
    advertisedRole: role,
    originalToken: token,
    token,
    setupRevision: 4,
    credentialGeneration: 1,
    liveStatusStreamId: `fixture-${role}-live-status`,
    liveStatusRevision: 0,
    staleLiveStatus: false,
    online: true,
    armed: false,
    activeSharedSessionId: null,
    failNextArm: false,
    failNextMissedShot: false,
    failNextFieldRecordingStart: false,
    failNextFieldRecordingStartAfterAccept: false,
    failNextFieldRecordingStop: false,
    failNextFieldRecordingStopAfterAccept: false,
    dropArmConnections: false,
    fieldRecordingState: "idle",
    fieldRecordingError: "",
    fieldRecordingPublicationFailurePending: false,
    holdFieldRecordingStop: false,
    activeFieldRecordingId: null,
    sharedFieldRecordingId: null,
    publishFixtureSession: true,
    historicalSessionCount: 0,
    holdHistoricalManifests: false,
    historicalManifestRequestCount: 0,
    pendingHistoricalManifestResponses: [],
    holdFieldRecordingCatalog: false,
    fieldRecordingCatalogRequestCount: 0,
    pendingFieldRecordingCatalogResponses: [],
    publication: "none",
    mutations: [],
    rangeStatuses: [],
    statusAuthorizations: [],
    previewAuthorizations: [],
  };
}

function setupSnapshot(
  state: NodeState,
  fixtureRole: FixtureRole,
  request: IncomingMessage,
): Record<string, unknown> {
  const origin = `http://${request.headers.host ?? "node.invalid"}`;
  return {
    schema_version: 1,
    revision: state.setupRevision,
    node: {
      node_id: fixtureRole.node_id,
      control_credential_generation: state.credentialGeneration,
      service_urls: [origin],
      device_model: state.role === "down_the_line" ? "Pixel 6 Pro" : "Pixel 5a",
    },
    capabilities: {
      roles: [
        { value: "unassigned", label: "Unassigned" },
        { value: "down_the_line", label: "Down the line" },
        { value: "face_on", label: "Across the line (face-on)" },
      ],
      capture_profiles: [
        {
          value: "720p240",
          label: "Standard: 1280×720 / 240 fps",
          width: 1280,
          height: 720,
          fps: 240,
        },
      ],
      pose_modes: [{ value: "disabled", label: "Pose monitoring disabled" }],
      inference_delegates: [{ value: "gpu_required", label: "GPU required" }],
    },
    configuration: {
      role: state.advertisedRole,
      capture_profile: "720p240",
      pose: {
        mode: "disabled",
        inference_delegate: "gpu_required",
        debug_evidence_enabled: true,
        hitting_region: { left: 0, top: 0, right: 1, bottom: 1 },
        peer: null,
      },
    },
    readiness: {
      editable: true,
      capture_state: state.armed ? "armed" : "setup",
      issues: [],
    },
    operational_health: {
      ready_for_capture: true,
      thermal: { status: 1, headroom: 0.18, ready: true, power_save_mode: false },
      storage: {
        usable_bytes: 21_474_836_480,
        minimum_free_bytes: 2_147_483_648,
        ready: true,
      },
      issues: [],
    },
    preview: {
      available: true,
      url: "/api/v1/setup/preview",
      state: "available",
      reason: null,
      frame_age_ms: 80,
      image_rotation_degrees: state.role === "down_the_line" ? 90 : 270,
    },
    pairing: null,
  };
}

async function serveNode(
  scenario: FixtureScenario,
  fixtureRoot: string,
  state: NodeState,
  request: IncomingMessage,
  response: ServerResponse,
): Promise<void> {
  const method = request.method ?? "GET";
  const requestUrl = new URL(request.url ?? "/", "http://node.invalid");
  const pathname = requestUrl.pathname;
  if (method === "OPTIONS") {
    respond(response, 204, Buffer.alloc(0), "text/plain");
    return;
  }
  const body = await requestBody(request);
  if (!state.online) {
    respondJson(response, 503, { error: "phone temporarily offline" });
    return;
  }
  if (pathname === "/api/v1/setup/preview") {
    state.previewAuthorizations.push(request.headers.authorization);
  }
  const authenticatedRead =
    pathname === "/api/v1/node" ||
    pathname === "/api/v1/setup" ||
    pathname === "/api/v1/setup/preview" ||
    pathname === "/api/v1/pairing/identity" ||
    pathname === "/api/v1/discovery" ||
    pathname === "/api/v1/capture/status" ||
    pathname === "/api/v1/capture/trigger-report" ||
    pathname === "/api/v1/field-recording/status" ||
    pathname === "/api/v1/field-recordings" ||
    pathname === "/api/v1/sessions" ||
    /^\/api\/v1\/sessions\/[^/]+\/(manifest|diagnostics\.zip)$/.test(pathname) ||
    /^\/api\/v1\/coordination\/[^/]+$/.test(pathname);
  if (method !== "GET" && method !== "HEAD") {
    state.mutations.push({ method, pathname, authorization: request.headers.authorization, body });
  }
  if ((method !== "GET" && method !== "HEAD") || authenticatedRead) {
    if (request.headers.authorization !== `Bearer ${state.token}`) {
      respondJson(
        response,
        401,
        { error: "a valid bearer control credential is required" },
        { "WWW-Authenticate": "Bearer" },
      );
      return;
    }
  }

  const fixtureRole = scenario.roles[state.role];
  if (method === "GET" && pathname === "/api/v1/setup") {
    respondJson(response, 200, setupSnapshot(state, fixtureRole, request));
    return;
  }
  if ((method === "GET" || method === "HEAD") && pathname === "/api/v1/setup/preview") {
    respond(response, 200, method === "HEAD" ? Buffer.alloc(0) : SETUP_PREVIEW_JPEG, "image/jpeg", {
      "Cache-Control": "no-store, max-age=0",
      "X-Content-Type-Options": "nosniff",
    });
    return;
  }
  if (method === "GET" && pathname === "/api/v1/discovery") {
    respondJson(response, 200, {
      schema_version: 1,
      authentication_required_for_pairing: true,
      observations: [],
    });
    return;
  }
  if (method === "GET" && pathname === "/api/v1/pairing/identity") {
    respondJson(response, 200, {
      schema_version: 1,
      node_id: fixtureRole.node_id,
      role: state.advertisedRole,
      pose_mode: "disabled",
      control_credential_generation: state.credentialGeneration,
      label: state.role === "down_the_line" ? "Pixel 6 Pro" : "Pixel 5a",
      service_urls: [`http://${request.headers.host ?? "node.invalid"}`],
    });
    return;
  }
  if (method === "POST" && pathname === "/api/v1/control-credential/rotate") {
    const payload = JSON.parse(body) as {
      expected_revision?: unknown;
      confirmed_node_id?: unknown;
    };
    if (
      payload.expected_revision !== state.setupRevision ||
      payload.confirmed_node_id !== fixtureRole.node_id
    ) {
      respondJson(response, 409, { error: "setup configuration changed" });
      return;
    }
    state.setupRevision += 1;
    state.credentialGeneration += 1;
    state.token =
      state.role === "down_the_line"
        ? "rotated_fixture_dtl_control_0001"
        : "rotated_fixture_face_control_001";
    respondJson(response, 200, {
      schema_version: 1,
      node_id: fixtureRole.node_id,
      setup_revision: state.setupRevision,
      control_credential_generation: state.credentialGeneration,
      control_token: state.token,
      remote_peer_bindings_require_re_pair: true,
    });
    return;
  }
  if (method === "GET" && pathname === "/api/v1/node") {
    respondJson(response, 200, {
      schema_version: 1,
      node_id: fixtureRole.node_id,
      role: state.advertisedRole,
      capture_profile: state.advertisedRole === "down_the_line" ? "1080p240" : "720p240",
      control_authentication: "bearer",
    });
    return;
  }
  if (method === "GET" && pathname === "/api/v1/clock") {
    const monotonic = process.hrtime.bigint() + BigInt(fixtureRole.clock_offset_ns);
    respondJson(response, 200, {
      schema_version: 1,
      node_id: fixtureRole.node_id,
      request_received_elapsed_realtime_ns: monotonic.toString(),
      response_prepared_elapsed_realtime_ns: (monotonic + 1_000n).toString(),
    });
    return;
  }
  if (method === "GET" && pathname === "/api/v1/capture/status") {
    state.statusAuthorizations.push(request.headers.authorization);
    respondJson(response, 200, captureStatus(state, fixtureRole), {
      "Cache-Control": "no-store, max-age=0",
    });
    return;
  }
  if (method === "POST" && pathname === "/api/v1/capture/arm") {
    if (state.dropArmConnections) {
      request.socket.destroy();
      return;
    }
    const payload = JSON.parse(body) as { armed?: unknown; shared_session_id?: unknown };
    if (payload.armed === true && state.failNextArm) {
      state.failNextArm = false;
      respondJson(response, 409, { error: "camera unavailable" });
      return;
    }
    state.armed = payload.armed === true;
    state.activeSharedSessionId =
      state.armed && typeof payload.shared_session_id === "string"
        ? payload.shared_session_id
        : null;
    respondJson(response, 200, captureStatus(state, fixtureRole));
    return;
  }
  if (method === "POST" && pathname === "/api/v1/capture/missed-shot") {
    if (state.failNextMissedShot) {
      state.failNextMissedShot = false;
      respondJson(response, 409, { error: "diagnostic retention unavailable" });
      return;
    }
    state.armed = false;
    state.activeSharedSessionId = null;
    respondJson(response, 202, {
      session_id: `fixture-tag-${state.role}`,
      state: "waiting_post_roll",
      created_at_utc: scenario.created_at_utc,
      error: "",
      session_kind: "standby_diagnostic",
    });
    return;
  }
  if (method === "GET" && pathname === "/api/v1/capture/trigger-report") {
    respondJson(response, 404, { error: "no coordinated trigger report" });
    return;
  }
  if (method === "GET" && pathname === "/api/v1/field-recording/status") {
    if (state.fieldRecordingPublicationFailurePending) {
      state.fieldRecordingPublicationFailurePending = false;
      state.fieldRecordingState = "error";
      state.fieldRecordingError = "manifest publication failed";
    }
    respondJson(response, 200, fieldRecordingStatus(state));
    return;
  }
  if (method === "POST" && pathname === "/api/v1/field-recording/start") {
    if (state.failNextFieldRecordingStart) {
      state.failNextFieldRecordingStart = false;
      respondJson(response, 409, { error: "recorder unavailable" });
      return;
    }
    const payload = JSON.parse(body) as { shared_recording_id?: unknown };
    if (typeof payload.shared_recording_id !== "string") {
      respondJson(response, 400, { error: "shared_recording_id is required" });
      return;
    }
    state.activeFieldRecordingId = `fixture-field-${state.role}`;
    state.sharedFieldRecordingId = payload.shared_recording_id;
    state.fieldRecordingPublicationFailurePending = false;
    if (state.failNextFieldRecordingStartAfterAccept) {
      state.failNextFieldRecordingStartAfterAccept = false;
      state.fieldRecordingState = "error";
      state.fieldRecordingError = "camera pipeline unavailable";
    } else {
      state.fieldRecordingState = "recording";
      state.fieldRecordingError = "";
    }
    respondJson(response, 202, fieldRecordingStatus(state));
    return;
  }
  if (method === "POST" && pathname === "/api/v1/field-recording/stop") {
    if (state.failNextFieldRecordingStop) {
      state.failNextFieldRecordingStop = false;
      respondJson(response, 503, { error: "stop transport interrupted" });
      return;
    }
    if (state.failNextFieldRecordingStopAfterAccept) {
      state.failNextFieldRecordingStopAfterAccept = false;
      state.fieldRecordingState = "stopping";
      state.fieldRecordingError = "";
      state.fieldRecordingPublicationFailurePending = true;
    } else {
      state.fieldRecordingState = state.holdFieldRecordingStop ? "stopping" : "idle";
      state.fieldRecordingError = "";
    }
    if (state.fieldRecordingState === "idle") {
      state.activeFieldRecordingId = null;
      state.sharedFieldRecordingId = null;
    }
    respondJson(response, 202, fieldRecordingStatus(state));
    return;
  }
  if (method === "GET" && pathname === "/api/v1/field-recordings") {
    const send = () =>
      respondJson(
        response,
        200,
        { schema_version: 1, recordings: [] },
        { "Cache-Control": "private, no-store" },
      );
    state.fieldRecordingCatalogRequestCount += 1;
    if (state.holdFieldRecordingCatalog) {
      state.pendingFieldRecordingCatalogResponses.push(send);
    } else {
      send();
    }
    return;
  }
  if (method === "GET" && pathname === "/api/v1/sessions") {
    const sessions = state.publishFixtureSession
      ? [sessionSummary(fixtureRole.local_session_id, scenario.created_at_utc)]
      : [];
    sessions.push(
      ...Array.from({ length: state.historicalSessionCount }, (_, index) =>
        sessionSummary(
          historicalLocalSessionId(state.role, index),
          `2026-08-21T18:00:${String(index).padStart(2, "0")}Z`,
        ),
      ),
    );
    const publishesPendingClip =
      state.publication === "ready" ||
      (state.role === "down_the_line" &&
        (state.publication === "encoding" || state.publication === "missing")) ||
      (state.role === "face_on" &&
        (state.publication === "encoding_down_the_line" ||
          state.publication === "missing_down_the_line"));
    if (publishesPendingClip) {
      sessions.unshift(sessionSummary(pendingLocalSessionId(state.role), scenario.created_at_utc));
    }
    respondJson(response, 200, { schema_version: 1, sessions });
    return;
  }

  const manifestMatch = /^\/api\/v1\/sessions\/([^/]+)\/manifest$/.exec(pathname);
  if (method === "GET" && manifestMatch !== null) {
    const localSessionId = decodeURIComponent(manifestMatch[1] ?? "");
    const historicalIndex = historicalSessionIndex(state.role, localSessionId);
    if (historicalIndex !== null && historicalIndex < state.historicalSessionCount) {
      const send = () =>
        respondJson(
          response,
          200,
          manifest(
            scenario,
            state.role,
            localSessionId,
            historicalSharedSessionId(historicalIndex),
          ),
          manifestAccessHeaders(state, localSessionId),
        );
      state.historicalManifestRequestCount += 1;
      if (state.holdHistoricalManifests) {
        state.pendingHistoricalManifestResponses.push(send);
      } else {
        send();
      }
      return;
    }
    if (
      localSessionId === fixtureRole.local_session_id ||
      localSessionId === pendingLocalSessionId(state.role)
    ) {
      const sharedSessionId =
        localSessionId === pendingLocalSessionId(state.role)
          ? PENDING_SHARED_SESSION_ID
          : scenario.shared_session_id;
      respondJson(
        response,
        200,
        manifest(scenario, state.role, localSessionId, sharedSessionId),
        manifestAccessHeaders(state, localSessionId),
      );
      return;
    }
  }

  const coordinationMatch = /^\/api\/v1\/coordination\/([^/]+)$/.exec(pathname);
  if (method === "GET" && coordinationMatch !== null) {
    if (request.headers.authorization !== `Bearer ${state.token}`) {
      respondJson(response, 401, { error: "a valid bearer control credential is required" });
      return;
    }
    const sharedSessionId = decodeURIComponent(coordinationMatch[1] ?? "");
    const historicalIndex = historicalSharedSessionIndex(sharedSessionId);
    if (
      sharedSessionId === scenario.shared_session_id ||
      (sharedSessionId === PENDING_SHARED_SESSION_ID && state.publication === "ready") ||
      (historicalIndex !== null && historicalIndex < state.historicalSessionCount)
    ) {
      respondJson(response, 200, alignment(scenario, sharedSessionId));
    } else {
      respondJson(response, 404, { error: "coordination evidence not found" });
    }
    return;
  }

  const mediaMatch = /^\/api\/v1\/sessions\/([^/]+)\/([^/]+\.mp4)$/.exec(pathname);
  if ((method === "GET" || method === "HEAD") && mediaMatch !== null) {
    const localSessionId = decodeURIComponent(mediaMatch[1] ?? "");
    if (
      localSessionId === fixtureRole.local_session_id ||
      localSessionId === pendingLocalSessionId(state.role) ||
      historicalSessionIndex(state.role, localSessionId) !== null
    ) {
      const expectedQuery = mediaAccessQuery("sessions", localSessionId, state.token);
      if (
        request.headers.authorization !== `Bearer ${state.token}` &&
        requestUrl.search.slice(1) !== expectedQuery
      ) {
        respondJson(response, 401, {
          error: "a valid bearer credential or scoped media capability is required",
        });
        return;
      }
      await serveRangeFile(
        path.join(fixtureRoot, fixtureRole.media_path),
        request,
        response,
        state.rangeStatuses,
      );
      return;
    }
  }

  if (method === "POST" && pathname.endsWith("/feedback")) {
    respond(response, 204, Buffer.alloc(0), "application/json");
    return;
  }
  if (method === "POST" && pathname.startsWith("/api/v1/coordination/")) {
    respondJson(response, 201, JSON.parse(body));
    return;
  }
  if (method === "PUT" && pathname === "/api/v1/setup") {
    respondJson(response, 200, { schema_version: 1, saved: true });
    return;
  }
  respondJson(response, 404, { error: `no fixture route for ${method} ${pathname}` });
}

function captureStatus(state: NodeState, role: FixtureRole): Record<string, unknown> {
  if (!state.staleLiveStatus) {
    state.liveStatusRevision += 1;
  }
  const liveStatusRevision = Math.max(1, state.liveStatusRevision);
  let captureState: CaptureState = state.armed ? "armed" : "setup";
  let sharedSessionId = state.activeSharedSessionId;
  if (
    (state.publication === "encoding" && state.role === "face_on") ||
    (state.publication === "encoding_down_the_line" && state.role === "down_the_line")
  ) {
    captureState = "encoding";
    sharedSessionId = PENDING_SHARED_SESSION_ID;
  } else if (state.publication === "mismatched") {
    captureState = "armed";
    sharedSessionId = state.role === "down_the_line" ? "stale-dtl" : "stale-face";
  }
  return {
    schema_version: 2,
    state: captureState,
    armed: captureState === "armed",
    active_session_id: sharedSessionId === null ? null : role.local_session_id,
    shared_session_id: sharedSessionId,
    error: "",
    hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
    live_status: {
      schema_version: 1,
      stream_id: state.liveStatusStreamId,
      revision: String(liveStatusRevision),
      generated_elapsed_realtime_ns: String(liveStatusRevision * 1_000_000),
    },
  };
}

function sessionSummary(sessionId: string, createdAtUtc: string) {
  return { session_id: sessionId, state: "ready", created_at_utc: createdAtUtc, error: "" };
}

function fieldRecordingStatus(state: NodeState): Record<string, unknown> {
  const recording = state.fieldRecordingState === "recording";
  return {
    schema_version: 1,
    state: state.fieldRecordingState,
    active_recording_id: state.activeFieldRecordingId,
    shared_recording_id: state.sharedFieldRecordingId,
    started_at_utc: recording ? "2026-08-22T18:00:00Z" : null,
    started_elapsed_realtime_ns: recording ? "123456789" : null,
    elapsed_ms: recording ? 12_000 : 0,
    video_bytes: recording ? "1200000" : "0",
    audio_frames: recording ? "576000" : "0",
    max_duration_seconds: 600,
    error: state.fieldRecordingError,
  };
}

function manifest(
  scenario: FixtureScenario,
  role: ReviewRole,
  localSessionId: string,
  sharedSessionId: string,
) {
  const fixtureRole = scenario.roles[role];
  return {
    schema_version: 1,
    session_id: localSessionId,
    created_at_utc: scenario.created_at_utc,
    trigger: {
      source: "local_audio",
      host_monotonic_time_ns: fixtureRole.trigger_timestamp_ns,
      confirmation_host_monotonic_time_ns: fixtureRole.trigger_timestamp_ns,
      sample_rate_hz: 48_000,
      peak_amplitude: 0.72,
      noise_floor: 0.01,
      threshold: 0.08,
    },
    mapped_nearest_frame_skew_us: null,
    views: [
      {
        role,
        camera_serial: fixtureRole.camera_serial,
        source: {
          pixel_format: "camera2_private",
          width: scenario.encoded_width,
          height: scenario.encoded_height,
        },
        encoded: { width: scenario.encoded_width, height: scenario.encoded_height },
        frame_count: scenario.frame_count,
        nominal_fps: scenario.nominal_fps,
        impact_frame_index: scenario.impact_frame_index,
        media: {
          path: fixtureRole.media_path,
          mime_type: "video/mp4",
          codec: "avc1.42C01E",
          all_frames_keyframes: scenario.all_frames_keyframes,
          encoded_bytes: fixtureRole.encoded_bytes,
        },
        frames: Array.from({ length: scenario.frame_count }, (_, frameIndex) => {
          const mediaTimeUs = Math.round((frameIndex * 1_000_000) / scenario.nominal_fps);
          const impactMediaTimeUs = Math.round(
            (scenario.impact_frame_index * 1_000_000) / scenario.nominal_fps,
          );
          return {
            frame_index: frameIndex,
            frame_id: String(10_000 + frameIndex),
            device_timestamp: String(
              2_000_000_000 + Math.round((frameIndex * 1_000_000_000) / scenario.nominal_fps),
            ),
            time_from_impact_us: mediaTimeUs - impactMediaTimeUs,
            media_time_us: mediaTimeUs,
          };
        }),
      },
    ],
    android_capture: {
      node_id: fixtureRole.node_id,
      shared_session_id: sharedSessionId,
      trigger_timestamp_uncertainty_ns: 250_000,
      local_nearest_frame_residual_us: 1_000,
    },
  };
}

function manifestAccessHeaders(
  state: NodeState,
  localSessionId: string,
): Readonly<Record<string, string>> {
  return {
    "Cache-Control": "private, no-store",
    "X-Swing-Capture-Media-Access": mediaAccessQuery("sessions", localSessionId, state.token),
  };
}

function mediaAccessQuery(collection: string, identifier: string, controlToken: string): string {
  const capability = createHmac("sha256", controlToken)
    .update(`swing-capture-media-v1\n${collection}/${identifier}`, "ascii")
    .digest("base64url");
  return `media_access=${capability}`;
}

function alignment(scenario: FixtureScenario, sharedSessionId = scenario.shared_session_id) {
  const observed = (role: ReviewRole) => {
    const fixtureRole = scenario.roles[role];
    const historicalIndex = historicalSharedSessionIndex(sharedSessionId);
    return {
      role,
      node_id: fixtureRole.node_id,
      local_session_id:
        historicalIndex !== null
          ? historicalLocalSessionId(role, historicalIndex)
          : sharedSessionId === PENDING_SHARED_SESSION_ID
            ? pendingLocalSessionId(role)
            : fixtureRole.local_session_id,
      trigger_timestamp_ns: fixtureRole.trigger_timestamp_ns,
      trigger_uncertainty_ns: "250000",
      mapped_coordinator_timestamp_ns: (
        BigInt(fixtureRole.trigger_timestamp_ns) - BigInt(fixtureRole.clock_offset_ns)
      ).toString(),
      mapped_coordinator_uncertainty_ns: "500000",
      clock_offset_ns: fixtureRole.clock_offset_ns,
      clock_uncertainty_ns: "250000",
      minimum_round_trip_ns: "250000",
      maximum_round_trip_ns: "500000",
      clock_sample_count: 3,
      source: "local_audio",
    };
  };
  return {
    schema_version: 1,
    shared_session_id: sharedSessionId,
    status: "paired",
    recorded_at_epoch_ms: "1787428800000",
    down_the_line: observed("down_the_line"),
    face_on: observed("face_on"),
    minimum_trigger_separation_ns: "0",
    maximum_trigger_separation_ns: "1200000",
  };
}

function pendingLocalSessionId(role: ReviewRole): string {
  return role === "down_the_line" ? "fixture-pending-dtl-001" : "fixture-pending-face-001";
}

function historicalLocalSessionId(role: ReviewRole, index: number): string {
  return `fixture-history-${String(index)}-${role}`;
}

function historicalSharedSessionId(index: number): string {
  return `fixture-history-shared-${String(index)}`;
}

function historicalSessionIndex(role: ReviewRole, localSessionId: string): number | null {
  const match = /^fixture-history-(\d+)-(down_the_line|face_on)$/.exec(localSessionId);
  if (match?.[2] !== role) {
    return null;
  }
  const index = Number(match[1]);
  return Number.isSafeInteger(index) && index >= 0 ? index : null;
}

function historicalSharedSessionIndex(sharedSessionId: string): number | null {
  const match = /^fixture-history-shared-(\d+)$/.exec(sharedSessionId);
  if (match === null) {
    return null;
  }
  const index = Number(match[1]);
  return Number.isSafeInteger(index) && index >= 0 ? index : null;
}

async function serveStatic(
  root: string,
  request: IncomingMessage,
  response: ServerResponse,
): Promise<void> {
  try {
    const pathname = new URL(request.url ?? "/", "http://static.invalid").pathname;
    const relativePath = pathname === "/" ? "index.html" : pathname.slice(1);
    const filename = path.resolve(root, relativePath);
    if (filename !== root && !filename.startsWith(`${root}${path.sep}`)) {
      respond(response, 403, Buffer.from("Forbidden"), "text/plain");
      return;
    }
    const content = await readFile(filename);
    respond(response, 200, content, contentType(filename));
  } catch {
    respond(response, 404, Buffer.from("Not found"), "text/plain");
  }
}

async function serveRangeFile(
  filename: string,
  request: IncomingMessage,
  response: ServerResponse,
  statuses: number[],
): Promise<void> {
  const content = await readFile(filename);
  const range = parseByteRange(request.headers.range, content.length);
  if (range === "invalid") {
    statuses.push(416);
    respond(response, 416, Buffer.alloc(0), "video/mp4", {
      "Accept-Ranges": "bytes",
      "Content-Range": `bytes */${content.length}`,
    });
    return;
  }
  if (range !== null) {
    statuses.push(206);
    const selected = content.subarray(range.start, range.end + 1);
    respond(response, 206, selected, "video/mp4", {
      "Accept-Ranges": "bytes",
      "Content-Range": `bytes ${range.start}-${range.end}/${content.length}`,
    });
    return;
  }
  statuses.push(200);
  respond(response, 200, content, "video/mp4", { "Accept-Ranges": "bytes" });
}

function parseByteRange(
  header: string | undefined,
  size: number,
): { start: number; end: number } | "invalid" | null {
  if (header === undefined) {
    return null;
  }
  const match = /^bytes=(\d*)-(\d*)$/.exec(header);
  if (match === null || (match[1] === "" && match[2] === "")) {
    return "invalid";
  }
  if (match[1] === "") {
    const suffixLength = Number(match[2]);
    if (!Number.isSafeInteger(suffixLength) || suffixLength <= 0) {
      return "invalid";
    }
    return { start: Math.max(0, size - suffixLength), end: size - 1 };
  }
  const start = Number(match[1]);
  const requestedEnd = match[2] === "" ? size - 1 : Number(match[2]);
  if (
    !Number.isSafeInteger(start) ||
    !Number.isSafeInteger(requestedEnd) ||
    start < 0 ||
    start >= size ||
    requestedEnd < start
  ) {
    return "invalid";
  }
  return { start, end: Math.min(requestedEnd, size - 1) };
}

function mp4SyncSampleNumbers(media: Buffer): number[] {
  const marker = Buffer.from("stss", "ascii");
  const candidates: number[][] = [];
  let searchOffset = 0;
  for (;;) {
    const typeOffset = media.indexOf(marker, searchOffset);
    if (typeOffset < 0) {
      break;
    }
    searchOffset = typeOffset + marker.length;
    const boxOffset = typeOffset - 4;
    if (boxOffset < 0 || boxOffset + 16 > media.length) {
      continue;
    }
    const boxSize = media.readUInt32BE(boxOffset);
    const entryCount = media.readUInt32BE(typeOffset + 8);
    const entriesOffset = typeOffset + 12;
    const entriesEnd = entriesOffset + entryCount * 4;
    if (
      boxSize !== 16 + entryCount * 4 ||
      boxOffset + boxSize > media.length ||
      entriesEnd !== boxOffset + boxSize
    ) {
      continue;
    }
    candidates.push(
      Array.from({ length: entryCount }, (_, index) =>
        media.readUInt32BE(entriesOffset + index * 4),
      ),
    );
  }
  if (candidates.length !== 1) {
    throw new Error(`Expected one valid MP4 stss box, found ${String(candidates.length)}`);
  }
  return candidates[0] ?? [];
}

function respondJson(
  response: ServerResponse,
  status: number,
  value: unknown,
  headers: Readonly<Record<string, string>> = {},
): void {
  respond(
    response,
    status,
    Buffer.from(JSON.stringify(value)),
    "application/json; charset=utf-8",
    headers,
  );
}

function respond(
  response: ServerResponse,
  status: number,
  body: Buffer,
  type: string,
  extraHeaders: Readonly<Record<string, string>> = {},
): void {
  response.writeHead(status, {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, HEAD, POST, PUT, OPTIONS",
    "Access-Control-Allow-Headers": "Authorization, Range, Content-Type",
    "Access-Control-Expose-Headers":
      "Accept-Ranges, Content-Length, Content-Range, WWW-Authenticate, " +
      "X-Swing-Capture-Media-Access",
    "Content-Length": body.length,
    "Content-Type": type,
    ...extraHeaders,
  });
  response.end(body);
}

async function requestBody(request: IncomingMessage): Promise<string> {
  const chunks: Buffer[] = [];
  for await (const chunk of request) {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk as Uint8Array));
  }
  return Buffer.concat(chunks).toString("utf8");
}

function fixtureMediaTimeSeconds(scenario: FixtureScenario, frameIndex: number): number {
  return Math.round((frameIndex * 1_000_000) / scenario.nominal_fps) / 1_000_000;
}

function currentVideoTimes(videos: Locator): Promise<number[]> {
  return videos.evaluateAll((elements) =>
    elements.map((element) => (element as HTMLVideoElement).currentTime),
  );
}

async function expectVideoTimesWithinFixtureFrame(
  videos: Locator,
  scenario: FixtureScenario,
  frameIndex: number,
): Promise<void> {
  const startSeconds = fixtureMediaTimeSeconds(scenario, frameIndex);
  const endSeconds = fixtureMediaTimeSeconds(scenario, frameIndex + 1);
  // Browsers are allowed to expose either the requested point inside a sample
  // or the decoded sample timestamp through currentTime. The RVFC/pixel checks
  // above prove the exact presented frame; this assertion independently keeps
  // both video clocks inside that frame's presentation interval. Two
  // microseconds cover MP4 timescale conversion at a nominal 30 fps.
  const timescaleToleranceSeconds = 0.000_002;
  for (const currentTime of await currentVideoTimes(videos)) {
    expect(currentTime).toBeGreaterThanOrEqual(startSeconds - timescaleToleranceSeconds);
    expect(currentTime).toBeLessThan(endSeconds + timescaleToleranceSeconds);
  }
}

async function capturePresentedFrameEvidence(
  page: Page,
  expectedMediaTimeSeconds: number,
  action: () => Promise<void>,
): Promise<PresentedFrameEvidence[]> {
  const videos = page.locator('video[aria-label$="recorded swing"]');
  if (process.env.SWING_CAPTURE_BROWSER_NAME === "webkit") {
    await action();
    return videos.evaluateAll(async (elements, mediaTimeSeconds) => {
      return Promise.all(
        elements.map(async (element) => {
          const video = element as HTMLVideoElement;
          const applicationTimeSeconds = video.currentTime;
          const seek = async (targetSeconds: number) => {
            await new Promise<void>((resolve, reject) => {
              const timeout = setTimeout(
                () => reject(new Error(`WebKit seek to ${String(targetSeconds)} seconds stalled`)),
                1_000,
              );
              video.addEventListener(
                "seeked",
                () => {
                  clearTimeout(timeout);
                  resolve();
                },
                { once: true },
              );
              video.currentTime = targetSeconds;
            });
          };
          video.pause();
          await seek(Math.max(0, mediaTimeSeconds - 1 / 30));
          await seek(mediaTimeSeconds);
          await new Promise<void>((resolve) =>
            requestAnimationFrame(() => requestAnimationFrame(() => resolve())),
          );
          const canvas = document.createElement("canvas");
          canvas.width = 80;
          canvas.height = 45;
          const context = canvas.getContext("2d", { willReadFrequently: true });
          if (context === null) {
            throw new Error("2D canvas is unavailable for WebKit presented-frame evidence");
          }
          context.drawImage(video, 0, 0, canvas.width, canvas.height);
          const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data;
          let hash = 2_166_136_261;
          let nonblack = 0;
          for (let index = 0; index < pixels.length; index += 4) {
            const red = pixels[index] ?? 0;
            const green = pixels[index + 1] ?? 0;
            const blue = pixels[index + 2] ?? 0;
            if (red + green + blue > 24) {
              ++nonblack;
            }
            hash = Math.imul(hash ^ red, 16_777_619);
            hash = Math.imul(hash ^ green, 16_777_619);
            hash = Math.imul(hash ^ blue, 16_777_619);
          }
          const evidence = {
            role: video.getAttribute("aria-label") ?? "unknown role",
            media_time_seconds: video.currentTime,
            pixel_hash: (hash >>> 0).toString(16).padStart(8, "0"),
            nonblack_fraction: nonblack / (pixels.length / 4),
          };
          video.currentTime = applicationTimeSeconds;
          return evidence;
        }),
      );
    }, expectedMediaTimeSeconds);
  }
  const token = `${String(Date.now())}-${Math.random().toString(16).slice(2)}`;
  await videos.evaluateAll(
    (elements, request) => {
      interface EvidenceState {
        token: string;
        frames: PresentedFrameEvidence[];
        errors: string[];
      }
      const evidenceWindow = window as typeof window & {
        __swingCapturePresentedFrameEvidence?: EvidenceState;
      };
      const state: EvidenceState = { token: request.token, frames: [], errors: [] };
      evidenceWindow.__swingCapturePresentedFrameEvidence = state;
      for (const element of elements) {
        const video = element as HTMLVideoElement;
        if (typeof video.requestVideoFrameCallback !== "function") {
          state.errors.push("requestVideoFrameCallback is unavailable");
          continue;
        }
        let presentedFrames = 0;
        const inspect: VideoFrameRequestCallback = (_now, metadata) => {
          const current = evidenceWindow.__swingCapturePresentedFrameEvidence;
          if (current?.token !== request.token) {
            return;
          }
          ++presentedFrames;
          if (
            Math.abs(metadata.mediaTime - request.expectedMediaTimeSeconds) > 0.000_1 &&
            presentedFrames < 8
          ) {
            video.requestVideoFrameCallback(inspect);
            return;
          }
          const canvas = document.createElement("canvas");
          canvas.width = 80;
          canvas.height = 45;
          const context = canvas.getContext("2d", { willReadFrequently: true });
          if (context === null) {
            current.errors.push("2D canvas is unavailable for presented-frame evidence");
            return;
          }
          try {
            video.pause();
            context.drawImage(video, 0, 0, canvas.width, canvas.height);
            const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data;
            let hash = 2_166_136_261;
            let nonblack = 0;
            for (let index = 0; index < pixels.length; index += 4) {
              const red = pixels[index] ?? 0;
              const green = pixels[index + 1] ?? 0;
              const blue = pixels[index + 2] ?? 0;
              if (red + green + blue > 24) {
                ++nonblack;
              }
              hash = Math.imul(hash ^ red, 16_777_619);
              hash = Math.imul(hash ^ green, 16_777_619);
              hash = Math.imul(hash ^ blue, 16_777_619);
            }
            current.frames.push({
              role: video.getAttribute("aria-label") ?? "unknown role",
              media_time_seconds: metadata.mediaTime,
              pixel_hash: (hash >>> 0).toString(16).padStart(8, "0"),
              nonblack_fraction: nonblack / (pixels.length / 4),
            });
          } catch (caught) {
            current.errors.push(
              `Unable to inspect presented video pixels: ${caught instanceof Error ? caught.message : String(caught)}`,
            );
          }
        };
        video.requestVideoFrameCallback(inspect);
      }
    },
    { expectedMediaTimeSeconds, token },
  );
  await action();
  await page.waitForTimeout(150);
  const stalled = await page.evaluate((expectedToken) => {
    const evidenceWindow = window as typeof window & {
      __swingCapturePresentedFrameEvidence?: {
        token: string;
        frames: PresentedFrameEvidence[];
      };
    };
    const state = evidenceWindow.__swingCapturePresentedFrameEvidence;
    return state?.token === expectedToken && state.frames.length === 0;
  }, token);
  if (stalled) {
    // WPE WebKit exposes requestVideoFrameCallback but does not dispatch a
    // callback for a paused seek until the decode pipeline advances. Nudge one
    // preceding fixture frame at low speed; the callback pauses at the exact
    // requested presentation timestamp before pixel evidence is sampled.
    await videos.evaluateAll(async (elements, mediaTimeSeconds) => {
      await Promise.all(
        elements.map(async (element) => {
          const video = element as HTMLVideoElement;
          video.playbackRate = 0.25;
          video.currentTime = Math.max(0, mediaTimeSeconds - 1 / 30);
          await video.play();
        }),
      );
    }, expectedMediaTimeSeconds);
  }
  await expect
    .poll(() =>
      page.evaluate((expectedToken) => {
        const evidenceWindow = window as typeof window & {
          __swingCapturePresentedFrameEvidence?: {
            token: string;
            frames: PresentedFrameEvidence[];
            errors: string[];
          };
        };
        const state = evidenceWindow.__swingCapturePresentedFrameEvidence;
        if (state?.token !== expectedToken) {
          return { errors: [], frameCount: 0 };
        }
        return { errors: state.errors, frameCount: state.frames.length };
      }, token),
    )
    .toEqual({ errors: [], frameCount: 2 });
  await videos.evaluateAll((elements, mediaTimeSeconds) => {
    for (const element of elements) {
      const video = element as HTMLVideoElement;
      video.pause();
      video.playbackRate = 1;
      video.currentTime = mediaTimeSeconds;
    }
  }, expectedMediaTimeSeconds);
  return page.evaluate((expectedToken) => {
    const evidenceWindow = window as typeof window & {
      __swingCapturePresentedFrameEvidence?: {
        token: string;
        frames: PresentedFrameEvidence[];
      };
    };
    const state = evidenceWindow.__swingCapturePresentedFrameEvidence;
    return state?.token === expectedToken ? state.frames : [];
  }, token);
}

async function nonblackPixelFraction(page: Page, source: string): Promise<number> {
  return page.evaluate(async (url) => {
    const video = document.createElement("video");
    video.crossOrigin = "anonymous";
    video.muted = true;
    video.src = url;
    await new Promise<void>((resolve, reject) => {
      video.addEventListener("loadeddata", () => resolve(), { once: true });
      video.addEventListener("error", () => reject(new Error("fixture video decode failed")), {
        once: true,
      });
      video.load();
    });
    video.currentTime = 1.5;
    await new Promise<void>((resolve) => {
      video.addEventListener("seeked", () => resolve(), { once: true });
    });
    const canvas = document.createElement("canvas");
    canvas.width = video.videoWidth;
    canvas.height = video.videoHeight;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    if (context === null) {
      throw new Error("2D canvas is unavailable");
    }
    context.drawImage(video, 0, 0);
    const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data;
    let nonblack = 0;
    for (let index = 0; index < pixels.length; index += 4) {
      if ((pixels[index] ?? 0) + (pixels[index + 1] ?? 0) + (pixels[index + 2] ?? 0) > 24) {
        ++nonblack;
      }
    }
    return nonblack / (pixels.length / 4);
  }, source);
}

type ReviewInspectorTab = "Capture" | "Diagnostics" | "Session";

async function openReviewInspector(page: Page, tab: ReviewInspectorTab): Promise<Locator> {
  const inspector = page.getByRole("dialog", { name: "Review inspector" });
  if (!(await inspector.isVisible())) {
    await page.getByRole("button", { name: "Inspector", exact: true }).click();
  }
  await expect(inspector).toBeVisible();
  const tabButton = inspector.getByRole("tab", { name: tab, exact: true });
  if ((await tabButton.getAttribute("aria-selected")) !== "true") {
    await tabButton.click();
  }
  await expect(tabButton).toHaveAttribute("aria-selected", "true");
  return inspector;
}

function reviewErrorAlert(page: Page): Locator {
  return page.locator(
    '.review-error-toast[role="alert"]:visible, .capture-error[role="alert"]:visible',
  );
}

async function accessibilityViolations(page: Page): Promise<string[]> {
  await page.addScriptTag({ content: axe.source });
  return page.evaluate(async () => {
    const axeApi = (window as typeof window & { axe: typeof axe }).axe;
    const result = await axeApi.run(document, { rules: { "color-contrast": { enabled: false } } });
    return result.violations.map((violation) => violation.id);
  });
}

function contentType(filename: string): string {
  switch (path.extname(filename)) {
    case ".css":
      return "text/css; charset=utf-8";
    case ".html":
      return "text/html; charset=utf-8";
    case ".js":
      return "text/javascript; charset=utf-8";
    default:
      return "application/octet-stream";
  }
}

async function listen(server: Server): Promise<string> {
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address();
  if (address === null || typeof address === "string") {
    throw new Error("Fixture server did not bind a TCP address");
  }
  return `http://127.0.0.1:${address.port}`;
}

async function close(server: Server): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    server.close((error) => (error === undefined ? resolve() : reject(error)));
  });
}

async function saveOutput(filename: string, data: Buffer): Promise<void> {
  const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  if (outputDirectory !== undefined) {
    await writeFile(path.join(outputDirectory, filename), data);
  }
}
