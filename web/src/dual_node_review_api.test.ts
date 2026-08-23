import assert from "node:assert/strict";
import {
  associateDualTriggers,
  type ClockExchangeSample,
  type ClockOffsetEstimate,
  composeDualManifest,
  DualNodeReviewApi,
  estimateClockOffset,
  groupAndroidSessionManifests,
  localizeDiagnosticFeedback,
  type NodeTriggerReport,
} from "./dual_node_review_api.js";
import { FIXTURE_MANIFEST } from "./fake_review_api.js";
import {
  type ClipManifest,
  DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
  type DiagnosticFeedback,
  parseClipManifest,
  type ReviewRole,
} from "./review_api.js";

async function main() {
  estimatesIntersectedClockBounds();
  rejectsInconsistentClockBounds();
  associatesExactlyOneBoundedReportPerRole();
  rejectsUnrelatedTriggerTiming();
  composesTwoOriginPreservingTracks();
  composesLeaderAndPeerAudioTriggerSemantics();
  preservesMissedShotTriggerSemantics();
  localizesCommonRelativeDiagnosticMarks();
  rejectsUnsafeLocalizedDiagnosticMarks();
  parsesAndroidSingleNodeTimingEvidence();
  parsesLegacyUncoordinatedAndroidMetadata();
  isolatesDuplicateHistoricalRolesFromValidPairs();
  await provisionsOneSharedSessionAndRollsBackPartialArm();
  await distinguishesDelayedEncodingFromMissingPeerClip();
  await rejectsMismatchedLiveSharedSessions();
  await persistsAndRecoversAlignmentAcrossCoordinatorInstances();
  await remapsEndpointsAfterLiveRoleSwap();
  await filtersStandbyDiagnosticsBeforeManifestLoading();
  await preservesDualStandbyMissedShotKind();
  await savesExternallyArmedStandbyDiagnosticsWithoutSharedId();
  await rejectsMixedSharedSessionOwnership();
  await rejectsLostBrowserSessionBeforeEitherMissedShotMutation();
  await rejectsStaleBrowserArmBeforeEitherMissedShotMutation();
  await rejectsReplacedBrowserSessionBeforeEitherMissedShotMutation();
  await rejectsStaleManualCaptureBeforeEitherMutation();
  await rejectsReplacedBrowserSessionBeforeEitherManualMutation();
  await rejectsOneStaleCredentialBeforeEitherMissedShotMutation();
  await preservesPeerArmFailureInCombinedStatus();
  await coordinatesAuthenticatedFieldRecording();
  await loadsRichAndroidCatalogWithoutHistoricalManifestRequests();
  await prioritizesFieldControlOverHistoricalManifestHydration();
  await rollsBackAcceptedFieldRecordingAfterAsynchronousPeerFailure();
  await reportsAsynchronousFieldRecordingPublicationFailure();
  await rejectsOneStaleCredentialBeforeFieldRecordingMutations();
  await authenticatesSensitiveGeneralReads();
  await coalescesConcurrentBootstrapReads();
}

async function coalescesConcurrentBootstrapReads() {
  const nodes = fakeDualNodes();
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  await Promise.all([
    api.getCaptureStatus(),
    api.getSessions(),
    api.getFieldRecordingStatus(),
    api.getFieldRecordings(),
  ]);

  const requestCount = (path: string) =>
    nodes.sensitiveReadRequests.filter((request) => request.path === path).length;
  assert.equal(
    requestCount("/api/v1/node"),
    2,
    "simultaneous consumers share one authenticated identity read per phone",
  );
  assert.equal(
    requestCount("/api/v1/capture/status"),
    2,
    "simultaneous live and catalog consumers share one status read per phone",
  );
}

async function authenticatesSensitiveGeneralReads() {
  const nodes = fakeDualNodes();
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  await api.getCaptureStatus();
  await api.getFieldRecordingStatus();
  await api.getSessions();
  await api.getFieldRecordings();

  const expectedByHost = new Map([
    ["dtl.test", "Bearer dtl-token"],
    ["face.test", "Bearer face-token"],
  ]);
  assert.ok(nodes.sensitiveReadRequests.length >= 10);
  assert.ok(
    nodes.sensitiveReadRequests.every(
      (request) => request.authorization === expectedByHost.get(request.host),
    ),
    "every sensitive Android metadata read carries its destination node's credential",
  );
  assert.deepEqual(
    new Set(nodes.sensitiveReadRequests.map((request) => request.path)),
    new Set([
      "/api/v1/node",
      "/api/v1/capture/status",
      "/api/v1/field-recording/status",
      "/api/v1/sessions",
      "/api/v1/field-recordings",
    ]),
  );
}

async function coordinatesAuthenticatedFieldRecording() {
  const nodes = fakeDualNodes();
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  const initial = await api.getFieldRecordingStatus();
  assert.deepEqual(
    initial.nodes.map((node) => [node.role, node.state]),
    [
      ["down_the_line", "idle"],
      ["face_on", "idle"],
    ],
  );

  const started = await api.startFieldRecording();
  assert.ok(started.nodes.every((node) => node.state === "recording"));
  const starts = nodes.fieldMutationRequests.filter((request) => request.path.endsWith("/start"));
  assert.equal(starts.length, 2);
  const sharedIds = new Set(starts.map((request) => request.body.shared_recording_id));
  assert.equal(sharedIds.size, 1);
  assert.match(String([...sharedIds][0]), /^field-[0-9a-f-]{36}$/);
  assert.deepEqual(
    starts.map((request) => request.authorization),
    ["Bearer dtl-token", "Bearer face-token"],
  );
  assert.ok(nodes.armBodies.every((body) => body.armed === false));

  const recordings = await api.getFieldRecordings();
  assert.equal(recordings.recordings.length, 2);
  assert.deepEqual(nodes.fieldCatalogAuthorizations, ["Bearer dtl-token", "Bearer face-token"]);
  assert.equal(
    recordings.recordings[0]?.video_url,
    "http://dtl.test/api/v1/field-recordings/field-dtl/video.mp4",
  );
  assert.equal(
    recordings.recordings[1]?.audio_url,
    "http://face.test/api/v1/field-recordings/field-face/audio.wav",
  );

  const stopped = await api.stopFieldRecording();
  assert.ok(stopped.nodes.every((node) => node.state === "ready"));
  assert.deepEqual(
    nodes.fieldMutationRequests
      .filter((request) => request.path.endsWith("/stop"))
      .map((request) => request.authorization),
    ["Bearer dtl-token", "Bearer face-token"],
  );
}

async function prioritizesFieldControlOverHistoricalManifestHydration() {
  const nodes = contendedHistoricalNodes();
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  const catalog = api.getSessions();
  void catalog.catch(() => undefined);

  await withTestDeadline(
    nodes.manifestCapacityObserved,
    "historical manifests did not fill the bounded background capacity",
  );
  assert.deepEqual(nodes.activeManifestRequests(), [2, 2]);

  const start = api.startFieldRecording();
  void start.catch(() => undefined);
  try {
    await withTestDeadline(
      nodes.fieldStartsObserved,
      "field control was starved behind historical manifest hydration",
    );
    const status = await withTestDeadline(
      start,
      "field recording did not become ready while historical manifests were blocked",
    );
    assert.ok(status.nodes.every((node) => node.state === "recording"));
    assert.deepEqual(nodes.authenticatedIdentityHosts, ["dtl.test", "face.test"]);
    assert.deepEqual(nodes.maximumActiveManifestRequests(), [2, 2]);
  } finally {
    nodes.releaseManifests();
  }

  const sessions = await withTestDeadline(catalog, "historical catalog did not finish hydrating");
  assert.equal(sessions.sessions.length, 8);
}

async function loadsRichAndroidCatalogWithoutHistoricalManifestRequests() {
  const nodes = richSessionSummaryNodes();
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);

  const catalog = await api.getSessions();
  assert.equal(nodes.manifestRequests.length, 0, "catalog loading must not hydrate clip manifests");
  assert.equal(
    nodes.coordinationRequests.length,
    0,
    "catalog loading must not hydrate historical coordination records",
  );
  assert.equal(
    catalog.sessions.filter((session) => session.state === "ready").length,
    35,
    "all complete historical pairs remain reviewable from compact summaries",
  );
  assert.equal(
    catalog.sessions.find((session) => session.session_id === "publishing-one-sided")?.state,
    "encoding",
    "a one-sided current publication stays pending",
  );
  assert.match(
    catalog.sessions.find((session) => session.session_id === "duplicate-dtl")?.error ?? "",
    /duplicate DTL clips/,
    "duplicate-role diagnostics must not require manifests",
  );
  assert.match(
    catalog.sessions.find((session) => session.session_id === "missing-coordination")?.error ?? "",
    /association evidence is unavailable/,
    "missing summarized coordination evidence must remain fail-closed",
  );

  const selected = await api.getManifest("history-17");
  assert.deepEqual(
    selected.views.map((view) => view.role),
    ["down_the_line", "face_on"],
  );
  assert.deepEqual(nodes.manifestRequests.sort(), ["history-17-dtl", "history-17-face"]);
  assert.deepEqual(nodes.coordinationRequests.sort(), [
    "history-17@dtl.test",
    "history-17@face.test",
  ]);

  await api.getSessions();
  assert.equal(
    nodes.manifestRequests.length,
    2,
    "refreshing the catalog must not hydrate unselected history or refetch the selected pair",
  );
  assert.equal(
    nodes.coordinationRequests.length,
    2,
    "refreshing the catalog must not hydrate unselected coordination records",
  );
}

async function rollsBackAcceptedFieldRecordingAfterAsynchronousPeerFailure() {
  const nodes = fakeDualNodes();
  nodes.failNextFieldRecordingStartAsynchronously("face.test");
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);

  await assert.rejects(
    () => api.startFieldRecording(),
    /Face-on field recorder failed after accepting start: camera pipeline unavailable/,
  );
  assert.deepEqual(
    nodes.fieldMutationRequests.map((request) => [request.host, request.path]),
    [
      ["dtl.test", "/api/v1/field-recording/start"],
      ["face.test", "/api/v1/field-recording/start"],
      ["dtl.test", "/api/v1/field-recording/stop"],
      ["face.test", "/api/v1/field-recording/stop"],
    ],
    "accepted startup must be rolled back on both nodes after an asynchronous peer failure",
  );
  const rolledBack = await api.getFieldRecordingStatus();
  assert.ok(rolledBack.nodes.every((node) => node.state === "ready"));

  const retried = await api.startFieldRecording();
  assert.ok(retried.nodes.every((node) => node.state === "recording"));
  const starts = nodes.fieldMutationRequests.filter((request) => request.path.endsWith("/start"));
  assert.equal(starts.length, 4);
  assert.notEqual(starts[0]?.body.shared_recording_id, starts[2]?.body.shared_recording_id);
}

async function reportsAsynchronousFieldRecordingPublicationFailure() {
  const nodes = fakeDualNodes();
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  await api.startFieldRecording();
  nodes.failNextFieldRecordingStopAsynchronously("face.test");

  await assert.rejects(
    () => api.stopFieldRecording(),
    /Face-on field recorder failed while stopping or publishing: manifest publication failed/,
  );
  const status = await api.getFieldRecordingStatus();
  assert.deepEqual(
    status.nodes.map((node) => node.state),
    ["ready", "error"],
    "publication failure must remain visible rather than being reported as a completed stop",
  );
}

async function rejectsOneStaleCredentialBeforeFieldRecordingMutations() {
  const nodes = fakeDualNodes();
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  nodes.rotateControlCredential("face.test", "rotated-face-token");

  await assert.rejects(
    () => api.startFieldRecording(),
    /Face-on Android node request failed \(HTTP 401\)/,
  );
  assert.deepEqual(
    nodes.mutationRequests,
    [],
    "one stale credential must fail read-only preflight before disarm or recording mutations",
  );
}

async function remapsEndpointsAfterLiveRoleSwap() {
  const nodes = fakeDualNodes();
  const api = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-after-role-swap",
    null,
  );
  await api.getCaptureStatus();
  nodes.swapRoles();
  await api.setArmed(true);
  assert.deepEqual(
    nodes.armBodies.filter((body) => body.armed === true).map((body) => body.host),
    ["face.test", "dtl.test"],
    "live node descriptors, not bookmark parameter names, determine canonical role order",
  );
  const status = await api.getCaptureStatus();
  assert.equal(status.active_session_id, "shared-after-role-swap");
  assert.equal(status.error, "");
  assert.equal(nodes.coordinationRecords.size, 2);
}

function localizesCommonRelativeDiagnosticMarks() {
  const feedback: DiagnosticFeedback = {
    schema_version: DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
    classification: "av_sync_wrong",
    note: "Common review timeline",
    timing_marks_us: {
      desired_high_speed_start_us: -1_200_000,
      visual_impact_us: 5_000,
      audio_impact_us: 12_000,
    },
  };
  assert.deepEqual(localizeDiagnosticFeedback(feedback, 3_500_999n), {
    ...feedback,
    timing_marks_us: {
      desired_high_speed_start_us: -1_203_500,
      visual_impact_us: 1_500,
      audio_impact_us: 8_500,
    },
  });
  assert.deepEqual(localizeDiagnosticFeedback(feedback, -3_500_999n), {
    ...feedback,
    timing_marks_us: {
      desired_high_speed_start_us: -1_196_500,
      visual_impact_us: 8_500,
      audio_impact_us: 15_500,
    },
  });
  assert.equal(feedback.timing_marks_us?.visual_impact_us, 5_000);

  const withoutMarks: DiagnosticFeedback = {
    schema_version: DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
    classification: "good_capture",
  };
  assert.deepEqual(localizeDiagnosticFeedback(withoutMarks, 10_000n), withoutMarks);
}

function rejectsUnsafeLocalizedDiagnosticMarks() {
  const maximumMark: DiagnosticFeedback = {
    schema_version: DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
    classification: "av_sync_wrong",
    timing_marks_us: { visual_impact_us: Number.MAX_SAFE_INTEGER },
  };
  assert.throws(
    () => localizeDiagnosticFeedback(maximumMark, -1_000n),
    /outside the safe integer range/,
  );
  assert.throws(
    () => localizeDiagnosticFeedback(maximumMark, (BigInt(Number.MAX_SAFE_INTEGER) + 1n) * 1_000n),
    /cannot be represented in safe microseconds/,
  );
}

async function provisionsOneSharedSessionAndRollsBackPartialArm() {
  const successful = fakeDualNodes();
  const api = new DualNodeReviewApi(
    endpoints(),
    successful.fetcher,
    successful.now,
    () => "shared-browser-session",
    null,
  );
  const status = await api.setArmed(true);
  assert.equal(status.state, "arming");
  assert.deepEqual(
    successful.armBodies.map((body) => body.shared_session_id),
    ["shared-browser-session", "shared-browser-session"],
  );

  const partial = fakeDualNodes("face.test");
  const partialApi = new DualNodeReviewApi(
    endpoints(),
    partial.fetcher,
    partial.now,
    () => "shared-partial-session",
    null,
  );
  await assert.rejects(() => partialApi.setArmed(true), /Unable to arm both Android nodes/);
  assert.ok(
    partial.armBodies.some((body) => body.host === "dtl.test" && body.armed === false),
    "a successfully armed peer must be disarmed when the other node rejects arm",
  );
  const retried = await partialApi.setArmed(true);
  assert.equal(retried.state, "arming");
  assert.deepEqual(
    partial.armBodies.slice(-2).map((body) => [body.host, body.armed]),
    [
      ["dtl.test", true],
      ["face.test", true],
    ],
    "the same coordinator remains retryable after rollback",
  );
  assert.ok(
    partial.mutationRequests.every((request) => request.authorization === request.expected),
    "every dual-node mutation carries the bearer credential for its destination origin",
  );
}

async function distinguishesDelayedEncodingFromMissingPeerClip() {
  const nodes = publicationNodes("encoding");
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  const delayed = await api.getSessions();
  assert.deepEqual(delayed.sessions, [
    {
      session_id: "shared-publication",
      state: "encoding",
      created_at_utc: "2026-08-17T22:00:00.000Z",
      error: "",
    },
  ]);

  nodes.setFaceState("ready");
  const missing = await api.getSessions();
  assert.equal(missing.sessions[0]?.state, "error");
  assert.match(missing.sessions[0]?.error ?? "", /missing the face-on clip/);
}

async function rejectsMismatchedLiveSharedSessions() {
  const nodes = mismatchedSessionNodes();
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  await assert.rejects(() => api.getCaptureStatus(), /different active shared session IDs/);
}

async function persistsAndRecoversAlignmentAcrossCoordinatorInstances() {
  const nodes = fakeDualNodes();
  const first = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-durable-session",
    null,
  );
  await first.setArmed(true);
  const capturing = await first.getCaptureStatus();
  assert.equal(capturing.active_session_id, "shared-durable-session");
  assert.equal(nodes.coordinationRecords.size, 2);

  const recovered = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  const recoveredStatus = await recovered.getCaptureStatus();
  assert.equal(recoveredStatus.active_session_id, "shared-durable-session");
  assert.equal(nodes.coordinationRecords.size, 2);
  assert.ok(nodes.coordinationReads >= 2);
}

async function filtersStandbyDiagnosticsBeforeManifestLoading() {
  const nodes = fakeDualNodes(null, true);
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  assert.deepEqual(await api.getSessions(), { schema_version: 1, sessions: [] });
  assert.equal(nodes.manifestRequests, 0);
}

async function preservesDualStandbyMissedShotKind() {
  const nodes = fakeDualNodes(null, true, true);
  const api = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-standby-diagnostic",
    null,
  );
  await api.setArmed(true);
  const summary = await api.saveMissedShot();
  assert.equal(summary.session_id, "shared-standby-diagnostic");
  assert.equal(summary.session_kind, "standby_diagnostic");
  assert.deepEqual(await api.getSessions(), { schema_version: 1, sessions: [] });
  assert.equal(nodes.manifestRequests, 0);
}

async function savesExternallyArmedStandbyDiagnosticsWithoutSharedId() {
  const nodes = fakeDualNodes(null, false, true, false, true);
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  const summary = await api.saveMissedShot();
  assert.equal(summary.session_id, "standby-down_the_line");
  assert.equal(summary.session_kind, "standby_diagnostic");
}

async function rejectsMixedSharedSessionOwnership() {
  const nodes = fakeDualNodes();
  const api = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-mixed-ownership",
    null,
  );
  await api.setArmed(true);
  nodes.forceSharedSession("face.test", null);

  await assert.rejects(
    () => api.getCaptureStatus(),
    /Android nodes report different active shared session IDs/,
  );
  await assert.rejects(
    () => api.saveMissedShot(),
    /Android nodes report different active shared session IDs/,
  );
  assert.deepEqual(
    nodes.mutationRequests.filter((request) => request.path === "/api/v1/capture/missed-shot"),
    [],
    "mixed shared-session ownership must be rejected before either diagnostic mutation",
  );
}

async function rejectsLostBrowserSessionBeforeEitherMissedShotMutation() {
  const nodes = fakeDualNodes();
  const api = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-lost-browser-session",
    null,
  );
  await api.setArmed(true);
  nodes.forceSharedSession("dtl.test", null);
  nodes.forceSharedSession("face.test", null);

  await assert.rejects(
    () => api.saveMissedShot(),
    /Android nodes no longer report the browser's active shared session/,
  );
  assert.deepEqual(
    nodes.mutationRequests.filter((request) => request.path === "/api/v1/capture/missed-shot"),
    [],
    "loss of both shared IDs must not turn a browser-owned capture into standby diagnostics",
  );
}

async function rejectsStaleBrowserArmBeforeEitherMissedShotMutation() {
  const nodes = fakeDualNodes(null, false, true);
  const api = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-stale-browser-arm",
    null,
  );
  await api.setArmed(true);
  nodes.forceDisarmed("face.test");

  await assert.rejects(
    () => api.saveMissedShot(),
    /Both Android nodes must be armed before saving a missed shot/,
  );
  assert.deepEqual(
    nodes.mutationRequests.filter((request) => request.path === "/api/v1/capture/missed-shot"),
    [],
    "a stale browser-owned arm must be rejected before either phone saves a diagnostic",
  );
}

async function rejectsReplacedBrowserSessionBeforeEitherMissedShotMutation() {
  const nodes = fakeDualNodes();
  const staleBrowser = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-stale-browser-owner",
    null,
  );
  await staleBrowser.setArmed(true);

  // Reproduce a second browser taking ownership while both phones remain armed. Merely checking
  // the two armed flags is insufficient: the first browser must not tag the replacement session.
  const replacementBrowser = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-replacement-browser-owner",
    null,
  );
  await replacementBrowser.setArmed(true);

  await assert.rejects(
    () => staleBrowser.saveMissedShot(),
    /Android nodes report a different active shared session than the browser/,
  );
  assert.deepEqual(
    nodes.mutationRequests.filter((request) => request.path === "/api/v1/capture/missed-shot"),
    [],
    "a browser superseded on both phones must not tag the replacement owner's session",
  );
}

async function rejectsStaleManualCaptureBeforeEitherMutation() {
  const nodes = fakeDualNodes();
  const api = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-stale-manual",
    null,
  );
  await api.setArmed(true);
  nodes.forceDisarmed("face.test");

  await assert.rejects(
    () => api.triggerManualCapture(),
    /Both Android nodes must be armed before a dual manual capture/,
  );
  assert.deepEqual(
    nodes.mutationRequests.filter((request) => request.path === "/api/v1/capture/manual"),
    [],
    "a stale combined armed state must be rejected before either manual trigger",
  );
}

async function rejectsReplacedBrowserSessionBeforeEitherManualMutation() {
  const nodes = fakeDualNodes();
  const staleBrowser = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-stale-manual-owner",
    null,
  );
  await staleBrowser.setArmed(true);
  const replacementBrowser = new DualNodeReviewApi(
    endpoints(),
    nodes.fetcher,
    nodes.now,
    () => "shared-replacement-manual-owner",
    null,
  );
  await replacementBrowser.setArmed(true);

  await assert.rejects(
    () => staleBrowser.triggerManualCapture(),
    /Android nodes report a different active shared session than the browser/,
  );
  assert.deepEqual(
    nodes.mutationRequests.filter((request) => request.path === "/api/v1/capture/manual"),
    [],
    "a superseded browser must not manually trigger the replacement session",
  );
}

async function rejectsOneStaleCredentialBeforeEitherMissedShotMutation() {
  const nodes = fakeDualNodes(null, false, true, false, true);
  const stale = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  nodes.rotateControlCredential("face.test", "rotated-face-token");

  await assert.rejects(
    () => stale.saveMissedShot(),
    /Face-on Android node request failed \(HTTP 401\)/,
  );
  assert.deepEqual(
    nodes.mutationRequests.filter((request) => request.path === "/api/v1/capture/missed-shot"),
    [],
    "one stale credential must be rejected by read-only preflight before either diagnostic POST",
  );

  const correctedEndpoints = [
    endpoints()[0],
    { ...endpoints()[1], controlToken: "rotated-face-token" },
  ] as const;
  const corrected = new DualNodeReviewApi(
    correctedEndpoints,
    nodes.fetcher,
    nodes.now,
    undefined,
    null,
  );
  const summary = await corrected.saveMissedShot();
  assert.equal(summary.session_kind, "standby_diagnostic");
  assert.deepEqual(
    nodes.mutationRequests
      .filter((request) => request.path === "/api/v1/capture/missed-shot")
      .map((request) => [request.host, request.authorization]),
    [
      ["dtl.test", "Bearer dtl-token"],
      ["face.test", "Bearer rotated-face-token"],
    ],
  );
}

async function preservesPeerArmFailureInCombinedStatus() {
  const nodes = fakeDualNodes(null, false, false, true);
  const api = new DualNodeReviewApi(endpoints(), nodes.fetcher, nodes.now, undefined, null);
  const status = await api.getCaptureStatus();
  assert.deepEqual(status.pose?.peer_arm, {
    state: "rejected",
    shared_session_id: "pose-shared-session",
    http_status: 409,
    failure_type: null,
  });
  assert.equal(status.pose?.mode, "leader");
  assert.equal(status.pose?.phase, "high_speed");
  assert.equal(status.pose?.transition_requested, true);
}

function estimatesIntersectedClockBounds() {
  const samples: ClockExchangeSample[] = [
    sample(1_000n, 1_510n, 1_520n, 1_040n),
    sample(2_000n, 2_505n, 2_510n, 2_030n),
    sample(3_000n, 3_490n, 3_500n, 3_020n),
  ];
  const estimate = estimateClockOffset("node-dtl", samples);
  assert.equal(estimate.offsetNs, 485n);
  assert.equal(estimate.uncertaintyNs, 5n);
  assert.equal(estimate.minimumRoundTripNs, 10n);
  assert.equal(estimate.maximumRoundTripNs, 30n);
  assert.equal(estimate.sampleCount, 3);
}

function rejectsInconsistentClockBounds() {
  assert.throws(
    () =>
      estimateClockOffset("node", [
        sample(1_000n, 1_500n, 1_510n, 1_020n),
        sample(2_000n, 2_700n, 2_710n, 2_020n),
        sample(3_000n, 3_600n, 3_610n, 3_020n),
      ]),
    /do not intersect/,
  );
}

function associatesExactlyOneBoundedReportPerRole() {
  const reports: [NodeTriggerReport, NodeTriggerReport] = [
    report("down_the_line", "node-dtl", 1_001_000n, 100n),
    report("face_on", "node-face", 1_002_500n, 100n),
  ];
  const estimates: [ClockOffsetEstimate, ClockOffsetEstimate] = [
    estimate("node-dtl", 1_000n, 100n),
    estimate("node-face", 2_000n, 100n),
  ];
  const alignment = associateDualTriggers("shared-1", reports, estimates);
  assert.equal(alignment.status, "paired");
  assert.equal(alignment.minimum_trigger_separation_ns, "100");
  assert.equal(alignment.maximum_trigger_separation_ns, "900");
  assert.equal(alignment.down_the_line.mapped_coordinator_timestamp_ns, "1000000");
  assert.equal(alignment.face_on.mapped_coordinator_timestamp_ns, "1000500");
}

function rejectsUnrelatedTriggerTiming() {
  assert.throws(
    () =>
      associateDualTriggers(
        "shared-1",
        [
          report("down_the_line", "node-dtl", 1_001_000n, 100n),
          report("face_on", "node-face", 101_002_000n, 100n),
        ],
        [estimate("node-dtl", 1_000n, 100n), estimate("node-face", 2_000n, 100n)],
      ),
    /cannot prove a match/,
  );
}

function composesTwoOriginPreservingTracks() {
  const down = androidManifest("down_the_line", "node-dtl", "local-dtl");
  const face = androidManifest("face_on", "node-face", "local-face");
  const alignment = associateDualTriggers(
    "shared-1",
    [
      report("down_the_line", "node-dtl", 1_001_000n, 100n),
      report("face_on", "node-face", 1_003_500n, 100n),
    ],
    [estimate("node-dtl", 1_000n, 100n), estimate("node-face", 2_000n, 100n)],
  );
  const composed = composeDualManifest(down, face, alignment);
  assert.equal(composed.session_id, "shared-1");
  assert.equal(composed.views.length, 2);
  assert.equal(composed.views[0]?.role, "down_the_line");
  assert.equal(composed.views[1]?.role, "face_on");
  assert.equal(composed.views[0]?.media.url, "http://dtl.test/local-dtl.mp4");
  assert.equal(composed.views[1]?.media.url, "http://face.test/local-face.mp4");
  assert.equal(composed.trigger.source, "dual_local_audio");
  assert.equal(composed.trigger.host_monotonic_time_ns, "1000750");
  assert.equal(composed.dual_node_alignment, alignment);
}

function preservesMissedShotTriggerSemantics() {
  const down = androidManifest("down_the_line", "node-dtl", "local-dtl");
  const face = androidManifest("face_on", "node-face", "local-face");
  down.trigger.source = "missed_shot";
  face.trigger.source = "missed_shot";
  const alignment = associateDualTriggers(
    "shared-1",
    [
      report("down_the_line", "node-dtl", 1_001_000n, 100n, "missed_shot"),
      report("face_on", "node-face", 1_003_500n, 100n, "missed_shot"),
    ],
    [estimate("node-dtl", 1_000n, 100n), estimate("node-face", 2_000n, 100n)],
  );
  assert.equal(composeDualManifest(down, face, alignment).trigger.source, "missed_shot");

  face.trigger.source = "local_audio";
  assert.throws(() => composeDualManifest(down, face, alignment), /trigger sources disagree/);
}

function composesLeaderAndPeerAudioTriggerSemantics() {
  const down = androidManifest("down_the_line", "node-dtl", "local-dtl");
  const face = androidManifest("face_on", "node-face", "local-face");
  face.trigger.source = "peer_audio_clock_candidate";
  const alignment = associateDualTriggers(
    "shared-1",
    [
      report("down_the_line", "node-dtl", 1_001_000n, 100n, "local_audio"),
      report("face_on", "node-face", 1_003_500n, 100n, "peer_audio_clock_candidate"),
    ],
    [estimate("node-dtl", 1_000n, 100n), estimate("node-face", 2_000n, 100n)],
  );
  assert.equal(composeDualManifest(down, face, alignment).trigger.source, "dual_local_audio");
}

function parsesAndroidSingleNodeTimingEvidence() {
  const manifest = androidManifest("down_the_line", "node-dtl", "local-dtl");
  delete manifest.views[0]?.media.url;
  const parsed = parseClipManifest(structuredClone(manifest));
  assert.deepEqual(parsed.android_capture, {
    node_id: "node-dtl",
    shared_session_id: "shared-1",
    trigger_timestamp_uncertainty_ns: 250_000,
    local_nearest_frame_residual_us: 1_900,
  });
}

function parsesLegacyUncoordinatedAndroidMetadata() {
  const manifest = androidManifest("down_the_line", "node-dtl", "legacy-local-dtl");
  delete manifest.views[0]?.media.url;
  const raw = structuredClone(manifest) as unknown as {
    android_capture: Record<string, unknown>;
  };
  delete raw.android_capture.shared_session_id;
  delete raw.android_capture.trigger_timestamp_uncertainty_ns;
  const parsed = parseClipManifest(raw);
  assert.deepEqual(parsed.android_capture, {
    node_id: "node-dtl",
    shared_session_id: null,
    trigger_timestamp_uncertainty_ns: null,
    local_nearest_frame_residual_us: 1_900,
  });
}

function isolatesDuplicateHistoricalRolesFromValidPairs() {
  const duplicateFirst = androidManifest("down_the_line", "node-dtl", "duplicate-dtl-1");
  const duplicateSecond = androidManifest("down_the_line", "node-dtl", "duplicate-dtl-2");
  const validDown = androidManifest("down_the_line", "node-dtl", "valid-dtl");
  const validFace = androidManifest("face_on", "node-face", "valid-face");
  for (const manifest of [duplicateFirst, duplicateSecond]) {
    assert.ok(manifest.android_capture);
    manifest.android_capture.shared_session_id = "historical-duplicate";
  }
  for (const manifest of [validDown, validFace]) {
    assert.ok(manifest.android_capture);
    manifest.android_capture.shared_session_id = "valid-pair";
  }

  const groups = groupAndroidSessionManifests([
    duplicateFirst,
    duplicateSecond,
    validDown,
    validFace,
  ]);
  assert.match(groups.get("historical-duplicate")?.error ?? "", /duplicate DTL clips/);
  assert.equal(groups.get("valid-pair")?.error, undefined);
  assert.equal(groups.get("valid-pair")?.downTheLine?.session_id, "valid-dtl");
  assert.equal(groups.get("valid-pair")?.faceOn?.session_id, "valid-face");
}

function androidManifest(role: ReviewRole, nodeId: string, sessionId: string): ClipManifest {
  const manifest = structuredClone(FIXTURE_MANIFEST);
  const track = manifest.views.find((candidate) => candidate.role === role);
  assert.ok(track);
  manifest.session_id = sessionId;
  manifest.views = [track];
  manifest.mapped_nearest_frame_skew_us = null;
  manifest.trigger.source = "local_audio";
  delete manifest.pipeline_profile;
  delete manifest.hil_evidence;
  track.camera_serial = nodeId;
  track.media.path = `${role}.mp4`;
  track.media.url = `http://${role === "down_the_line" ? "dtl" : "face"}.test/${sessionId}.mp4`;
  manifest.android_capture = {
    node_id: nodeId,
    shared_session_id: "shared-1",
    trigger_timestamp_uncertainty_ns: 250_000,
    local_nearest_frame_residual_us: 1_900,
  };
  return manifest;
}

function sample(
  coordinatorSendNs: bigint,
  nodeReceiveNs: bigint,
  nodeSendNs: bigint,
  coordinatorReceiveNs: bigint,
): ClockExchangeSample {
  return { coordinatorSendNs, nodeReceiveNs, nodeSendNs, coordinatorReceiveNs };
}

function report(
  role: ReviewRole,
  nodeId: string,
  triggerTimestampNs: bigint,
  timestampUncertaintyNs: bigint,
  source = "local_audio",
): NodeTriggerReport {
  return {
    role,
    nodeId,
    sharedSessionId: "shared-1",
    localSessionId: role === "down_the_line" ? "local-dtl" : "local-face",
    triggerTimestampNs,
    timestampUncertaintyNs,
    source,
  };
}

function estimate(nodeId: string, offsetNs: bigint, uncertaintyNs: bigint): ClockOffsetEstimate {
  return {
    nodeId,
    offsetNs,
    uncertaintyNs,
    minimumRoundTripNs: uncertaintyNs,
    maximumRoundTripNs: uncertaintyNs * 2n,
    sampleCount: 3,
  };
}

function endpoints() {
  return [
    { baseUrl: "http://dtl.test", controlToken: "dtl-token", role: "down_the_line" },
    { baseUrl: "http://face.test", controlToken: "face-token", role: "face_on" },
  ] as const;
}

function richSessionSummaryNodes() {
  let coordinatorNow = 0n;
  const manifestRequests: string[] = [];
  const coordinationRequests: string[] = [];
  const now = () => {
    coordinatorNow += 1_000_000n;
    return coordinatorNow;
  };
  const alignment = (sharedSessionId: string) => {
    const down = report("down_the_line", "node-dtl", 1_001_000n, 100n);
    const face = report("face_on", "node-face", 1_003_500n, 100n);
    down.sharedSessionId = sharedSessionId;
    down.localSessionId = `${sharedSessionId}-dtl`;
    face.sharedSessionId = sharedSessionId;
    face.localSessionId = `${sharedSessionId}-face`;
    return associateDualTriggers(
      sharedSessionId,
      [down, face],
      [estimate("node-dtl", 1_000n, 100n), estimate("node-face", 2_000n, 100n)],
      1_778_000_000_000,
    );
  };
  const summary = (
    sharedSessionId: string,
    role: ReviewRole,
    localSessionId = `${sharedSessionId}-${role === "down_the_line" ? "dtl" : "face"}`,
    coordinationAvailable = true,
  ) => ({
    session_id: localSessionId,
    state: "ready",
    created_at_utc: "2026-08-22T18:00:00.000Z",
    session_kind: "capture",
    error: "",
    android_capture: {
      node_id: role === "down_the_line" ? "node-dtl" : "node-face",
      shared_session_id: sharedSessionId,
      role,
      coordination_available: coordinationAvailable,
    },
  });
  const fetcher = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = new URL(String(input));
    const role: ReviewRole = url.hostname === "dtl.test" ? "down_the_line" : "face_on";
    const nodeId = role === "down_the_line" ? "node-dtl" : "node-face";
    if (url.pathname === "/api/v1/node") {
      return Response.json({
        schema_version: 1,
        node_id: nodeId,
        role,
        capture_profile: "720p240",
        control_authentication: "bearer",
      });
    }
    if (url.pathname === "/api/v1/capture/status") {
      const publishing = role === "down_the_line";
      return Response.json({
        schema_version: 2,
        state: publishing ? "encoding" : "ready",
        armed: false,
        active_session_id: publishing ? "publishing-one-sided-dtl" : null,
        shared_session_id: publishing ? "publishing-one-sided" : null,
        error: "",
        hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
      });
    }
    if (url.pathname === "/api/v1/sessions") {
      const sessions = Array.from({ length: 35 }, (_, index) =>
        summary(`history-${String(index)}`, role),
      );
      sessions.push(summary("missing-coordination", role, undefined, false));
      if (role === "down_the_line") {
        sessions.push(summary("publishing-one-sided", role));
        sessions.push(summary("duplicate-dtl", role, "duplicate-dtl-a"));
        sessions.push(summary("duplicate-dtl", role, "duplicate-dtl-b"));
      }
      return Response.json({ schema_version: 1, sessions });
    }
    const manifestMatch = url.pathname.match(/^\/api\/v1\/sessions\/([^/]+)\/manifest$/);
    if (manifestMatch !== null) {
      const localSessionId = decodeURIComponent(manifestMatch[1] ?? "");
      manifestRequests.push(localSessionId);
      const sharedSessionId = localSessionId.replace(/-(?:dtl|face)$/, "");
      const manifest = androidManifest(role, nodeId, localSessionId);
      assert.ok(manifest.android_capture);
      delete manifest.views[0]?.media.url;
      manifest.android_capture.shared_session_id = sharedSessionId;
      return Response.json(manifest);
    }
    const coordinationMatch = url.pathname.match(/^\/api\/v1\/coordination\/([^/]+)$/);
    if (coordinationMatch !== null) {
      if (init?.method === "POST") {
        return Response.json(JSON.parse(String(init.body)) as unknown);
      }
      const sharedSessionId = decodeURIComponent(coordinationMatch[1] ?? "");
      coordinationRequests.push(`${sharedSessionId}@${url.hostname}`);
      return Response.json(alignment(sharedSessionId));
    }
    return Response.json({ error: `unexpected ${url.pathname}` }, { status: 404 });
  }) as typeof fetch;
  return { coordinationRequests, fetcher, manifestRequests, now };
}

function contendedHistoricalNodes() {
  const hosts = ["dtl.test", "face.test"] as const;
  const activeTransport = new Map<string, number>();
  const waitingTransport = new Map<string, Array<() => void>>();
  const activeManifests = new Map(hosts.map((host) => [host, 0]));
  const maximumManifests = new Map(hosts.map((host) => [host, 0]));
  const authenticatedIdentityHosts: string[] = [];
  const fieldStates = new Map(hosts.map((host) => [host, "idle" as "idle" | "recording"]));
  const fieldSharedIds = new Map<string, string>();
  let coordinatorNow = 0n;
  let releaseManifestRequests: (() => void) | undefined;
  const manifestBlocker = new Promise<void>((resolve) => {
    releaseManifestRequests = resolve;
  });
  let manifestCapacityResolve: (() => void) | undefined;
  const manifestCapacityObserved = new Promise<void>((resolve) => {
    manifestCapacityResolve = resolve;
  });
  let fieldStartsResolve: (() => void) | undefined;
  const fieldStartsObserved = new Promise<void>((resolve) => {
    fieldStartsResolve = resolve;
  });

  const acquireTransport = async (host: string) => {
    const active = activeTransport.get(host) ?? 0;
    if (active < 4) {
      activeTransport.set(host, active + 1);
      return;
    }
    await new Promise<void>((resolve) => {
      const waiting = waitingTransport.get(host) ?? [];
      waiting.push(resolve);
      waitingTransport.set(host, waiting);
    });
  };
  const releaseTransport = (host: string) => {
    const next = waitingTransport.get(host)?.shift();
    if (next !== undefined) {
      next();
      return;
    }
    activeTransport.set(host, (activeTransport.get(host) ?? 1) - 1);
  };
  const throughTransport = async <T>(host: string, operation: () => Promise<T>) => {
    await acquireTransport(host);
    try {
      return await operation();
    } finally {
      releaseTransport(host);
    }
  };

  const fetcher = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = new URL(String(input));
    const host = url.hostname;
    const downTheLine = host === "dtl.test";
    const role: ReviewRole = downTheLine ? "down_the_line" : "face_on";
    const nodeId = downTheLine ? "node-dtl" : "node-face";
    return throughTransport(host, async () => {
      if (url.pathname === "/api/v1/node") {
        return Response.json({
          schema_version: 1,
          node_id: nodeId,
          role,
          capture_profile: "720p240",
          control_authentication: "bearer",
        });
      }
      if (url.pathname === "/api/v1/sessions") {
        return Response.json({
          schema_version: 1,
          sessions: Array.from({ length: 8 }, (_, index) => ({
            session_id: `historical-${index}-${downTheLine ? "dtl" : "face"}`,
            state: "ready",
            created_at_utc: `2026-08-22T18:00:${String(index).padStart(2, "0")}.000Z`,
            error: "",
          })),
        });
      }
      const manifestMatch = url.pathname.match(/^\/api\/v1\/sessions\/([^/]+)\/manifest$/);
      if (manifestMatch !== null) {
        const active = (activeManifests.get(host) ?? 0) + 1;
        activeManifests.set(host, active);
        maximumManifests.set(host, Math.max(maximumManifests.get(host) ?? 0, active));
        if (hosts.every((candidate) => (activeManifests.get(candidate) ?? 0) >= 2)) {
          manifestCapacityResolve?.();
        }
        await manifestBlocker;
        activeManifests.set(host, (activeManifests.get(host) ?? 1) - 1);
        const localSessionId = decodeURIComponent(manifestMatch[1] ?? "");
        const index = Number(localSessionId.split("-")[1]);
        const manifest = androidManifest(role, nodeId, localSessionId);
        assert.ok(manifest.android_capture);
        delete manifest.views[0]?.media.url;
        manifest.android_capture.shared_session_id = `historical-${String(index)}`;
        return Response.json(manifest);
      }
      if (url.pathname === "/api/v1/pairing/identity") {
        assert.equal(
          new Headers(init?.headers).get("Authorization"),
          `Bearer ${downTheLine ? "dtl-token" : "face-token"}`,
        );
        authenticatedIdentityHosts.push(host);
        authenticatedIdentityHosts.sort();
        return Response.json({ schema_version: 1, node_id: nodeId });
      }
      if (url.pathname === "/api/v1/capture/arm") {
        assert.equal(init?.method, "POST");
        return Response.json({
          schema_version: 2,
          state: "setup",
          armed: false,
          active_session_id: null,
          error: "",
          hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
        });
      }
      if (url.pathname === "/api/v1/capture/status") {
        return Response.json({
          schema_version: 2,
          state: "setup",
          armed: false,
          active_session_id: null,
          shared_session_id: null,
          error: "",
          hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
        });
      }
      if (url.pathname === "/api/v1/field-recording/start") {
        assert.equal(init?.method, "POST");
        fieldStates.set(host, "recording");
        if (hosts.every((candidate) => fieldStates.get(candidate) === "recording")) {
          fieldStartsResolve?.();
        }
        const body = JSON.parse(String(init?.body)) as Record<string, unknown>;
        fieldSharedIds.set(host, String(body.shared_recording_id));
        return Response.json(
          {
            ...fieldRecordingStatus("starting"),
            active_recording_id: `active-${host}`,
            shared_recording_id: body.shared_recording_id,
            started_at_utc: "2026-08-22T18:01:00.000Z",
            started_elapsed_realtime_ns: "123456789",
          },
          { status: 202 },
        );
      }
      if (url.pathname === "/api/v1/field-recording/status") {
        const state = fieldStates.get(host) ?? "idle";
        return Response.json({
          ...fieldRecordingStatus(state),
          active_recording_id: state === "recording" ? `active-${host}` : null,
          shared_recording_id: state === "recording" ? (fieldSharedIds.get(host) ?? null) : null,
          started_at_utc: state === "recording" ? "2026-08-22T18:01:00.000Z" : null,
          started_elapsed_realtime_ns: state === "recording" ? "123456789" : null,
        });
      }
      if (url.pathname === "/api/v1/field-recording/stop") {
        fieldStates.set(host, "idle");
        return Response.json(fieldRecordingStatus("idle"), { status: 202 });
      }
      if (url.pathname.startsWith("/api/v1/coordination/")) {
        return Response.json({ error: "missing" }, { status: 404 });
      }
      return Response.json({ error: `unexpected ${url.pathname}` }, { status: 404 });
    });
  }) as typeof fetch;

  return {
    fetcher,
    now: () => {
      coordinatorNow += 1_000_000n;
      return coordinatorNow;
    },
    manifestCapacityObserved,
    fieldStartsObserved,
    authenticatedIdentityHosts,
    activeManifestRequests: () => hosts.map((host) => activeManifests.get(host) ?? 0),
    maximumActiveManifestRequests: () => hosts.map((host) => maximumManifests.get(host) ?? 0),
    releaseManifests: () => releaseManifestRequests?.(),
  };
}

function withTestDeadline<T>(promise: Promise<T>, failure: string): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error(failure)), 500);
    promise.then(
      (value) => {
        clearTimeout(timeout);
        resolve(value);
      },
      (caught: unknown) => {
        clearTimeout(timeout);
        reject(caught);
      },
    );
  });
}

function fakeDualNodes(
  failArmHost: string | null = null,
  includeStandbyDiagnostic = false,
  standbyMissedShot = false,
  includePeerFailure = false,
  externallyArmed = false,
) {
  let coordinatorNow = 0n;
  let activeSharedSessionId: string | null = null;
  let remainingArmFailures = failArmHost === null ? 0 : 1;
  let rolesSwapped = false;
  let forcedDisarmedHost: string | null = null;
  const forcedSharedSessions = new Map<string, string | null>();
  const controlTokens = new Map([
    ["dtl.test", "dtl-token"],
    ["face.test", "face-token"],
  ]);
  const fieldStates = new Map<string, "idle" | "recording" | "ready" | "error">([
    ["dtl.test", "idle"],
    ["face.test", "idle"],
  ]);
  const fieldSharedRecordingIds = new Map<string, string | null>([
    ["dtl.test", null],
    ["face.test", null],
  ]);
  const fieldErrors = new Map<string, string>([
    ["dtl.test", ""],
    ["face.test", ""],
  ]);
  const asynchronousStartFailures = new Set<string>();
  const asynchronousStopFailures = new Set<string>();
  const armBodies: Array<Record<string, unknown> & { host: string }> = [];
  const fieldMutationRequests: Array<{
    host: string;
    path: string;
    authorization: string | null;
    body: Record<string, unknown>;
  }> = [];
  const fieldCatalogAuthorizations: Array<string | null> = [];
  const sensitiveReadRequests: Array<{
    host: string;
    path: string;
    authorization: string | null;
  }> = [];
  const coordinationRecords = new Map<string, unknown>();
  const mutationRequests: Array<{
    host: string;
    path: string;
    authorization: string | null;
    expected: string;
  }> = [];
  let coordinationReads = 0;
  let manifestRequests = 0;
  const now = () => {
    coordinatorNow += 1_000_000n;
    return coordinatorNow;
  };
  const fetcher = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = new URL(String(input));
    const originallyDownTheLine = url.hostname === "dtl.test";
    const role = originallyDownTheLine !== rolesSwapped ? "down_the_line" : "face_on";
    const nodeId = originallyDownTheLine ? "node-dtl" : "node-face";
    const expectedAuthorization = `Bearer ${controlTokens.get(url.hostname) ?? ""}`;
    const method = init?.method ?? "GET";
    if (
      (method === "GET" || method === "HEAD") &&
      [
        "/api/v1/node",
        "/api/v1/capture/status",
        "/api/v1/capture/trigger-report",
        "/api/v1/field-recording/status",
        "/api/v1/field-recordings",
        "/api/v1/sessions",
      ].includes(url.pathname)
    ) {
      const authorization = new Headers(init?.headers).get("Authorization");
      sensitiveReadRequests.push({ host: url.hostname, path: url.pathname, authorization });
      if (authorization !== expectedAuthorization) {
        return Response.json(
          { error: "a valid bearer control credential is required" },
          { status: 401, headers: { "WWW-Authenticate": "Bearer" } },
        );
      }
    }
    if (method !== "GET" && method !== "HEAD") {
      const authorization = new Headers(init?.headers).get("Authorization");
      mutationRequests.push({
        host: url.hostname,
        path: url.pathname,
        authorization,
        expected: expectedAuthorization,
      });
      if (authorization !== expectedAuthorization) {
        return Response.json(
          { error: "a valid bearer control credential is required" },
          { status: 401, headers: { "WWW-Authenticate": "Bearer" } },
        );
      }
    }
    if (url.pathname === "/api/v1/pairing/identity") {
      const authorization = new Headers(init?.headers).get("Authorization");
      if (authorization !== expectedAuthorization) {
        return Response.json(
          { error: "a valid bearer control credential is required" },
          { status: 401, headers: { "WWW-Authenticate": "Bearer" } },
        );
      }
      return Response.json({ schema_version: 1, node_id: nodeId });
    }
    if (url.pathname === "/api/v1/node") {
      return Response.json({
        schema_version: 1,
        node_id: nodeId,
        role,
        capture_profile: role === "down_the_line" ? "1080p240" : "720p240",
        control_authentication: "bearer",
      });
    }
    if (url.pathname === "/api/v1/clock") {
      const nodeOffset = role === "down_the_line" ? 10_000_000_000n : 20_000_000_000n;
      return Response.json({
        schema_version: 1,
        node_id: nodeId,
        request_received_elapsed_realtime_ns: (coordinatorNow + nodeOffset).toString(),
        response_prepared_elapsed_realtime_ns: (coordinatorNow + nodeOffset + 1_000n).toString(),
      });
    }
    if (url.pathname === "/api/v1/capture/arm") {
      const body = JSON.parse(String(init?.body)) as Record<string, unknown>;
      armBodies.push({ ...body, host: url.hostname });
      if (body.armed === true && url.hostname === failArmHost && remainingArmFailures > 0) {
        --remainingArmFailures;
        return Response.json({ error: "camera unavailable" }, { status: 409 });
      }
      activeSharedSessionId = body.armed === true ? String(body.shared_session_id) : null;
      return Response.json({
        schema_version: 2,
        state: body.armed === true ? "arming" : "setup",
        armed: false,
        active_session_id: null,
        error: "",
        hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
      });
    }
    if (url.pathname === "/api/v1/capture/status") {
      const armed =
        (externallyArmed || activeSharedSessionId !== null) && url.hostname !== forcedDisarmedHost;
      const reportedSharedSessionId = forcedSharedSessions.has(url.hostname)
        ? (forcedSharedSessions.get(url.hostname) ?? null)
        : activeSharedSessionId;
      return Response.json({
        schema_version: 2,
        state: armed ? "waiting_post_roll" : "setup",
        armed,
        active_session_id:
          activeSharedSessionId === null || !armed
            ? null
            : role === "down_the_line"
              ? "local-dtl"
              : "local-face",
        shared_session_id: armed ? reportedSharedSessionId : null,
        error: "",
        hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
        ...(includePeerFailure
          ? {
              pose: {
                mode: role === "down_the_line" ? "leader" : "shadow",
                phase: role === "down_the_line" ? "high_speed" : "monitoring",
                transition_requested: role === "down_the_line",
                peer_arm:
                  role === "down_the_line"
                    ? {
                        state: "rejected",
                        shared_session_id: "pose-shared-session",
                        http_status: 409,
                        failure_type: null,
                      }
                    : {
                        state: "inbound_accepted",
                        shared_session_id: "pose-shared-session",
                        http_status: null,
                        failure_type: null,
                      },
              },
            }
          : {}),
      });
    }
    if (url.pathname === "/api/v1/field-recording/status") {
      return Response.json({
        ...fieldRecordingStatus(fieldStates.get(url.hostname) ?? "idle"),
        active_recording_id: fieldStates.get(url.hostname) === "idle" ? null : `field-${role}`,
        shared_recording_id: fieldSharedRecordingIds.get(url.hostname) ?? null,
        started_at_utc: fieldStates.get(url.hostname) === "idle" ? null : "2026-08-22T18:00:00Z",
        started_elapsed_realtime_ns: fieldStates.get(url.hostname) === "idle" ? null : "123456789",
        error: fieldErrors.get(url.hostname) ?? "",
      });
    }
    if (url.pathname === "/api/v1/field-recording/start") {
      const body = JSON.parse(String(init?.body)) as Record<string, unknown>;
      fieldMutationRequests.push({
        host: url.hostname,
        path: url.pathname,
        authorization: new Headers(init?.headers).get("Authorization"),
        body,
      });
      fieldSharedRecordingIds.set(url.hostname, String(body.shared_recording_id));
      if (asynchronousStartFailures.delete(url.hostname)) {
        fieldStates.set(url.hostname, "error");
        fieldErrors.set(url.hostname, "camera pipeline unavailable");
      } else {
        fieldStates.set(url.hostname, "recording");
        fieldErrors.set(url.hostname, "");
      }
      return Response.json(
        {
          ...fieldRecordingStatus("starting"),
          active_recording_id: `field-${role}`,
          shared_recording_id: body.shared_recording_id,
          started_at_utc: "2026-08-22T18:00:00Z",
          started_elapsed_realtime_ns: "123456789",
        },
        { status: 202 },
      );
    }
    if (url.pathname === "/api/v1/field-recording/stop") {
      const body = JSON.parse(String(init?.body)) as Record<string, unknown>;
      fieldMutationRequests.push({
        host: url.hostname,
        path: url.pathname,
        authorization: new Headers(init?.headers).get("Authorization"),
        body,
      });
      if (asynchronousStopFailures.delete(url.hostname)) {
        fieldStates.set(url.hostname, "error");
        fieldErrors.set(url.hostname, "manifest publication failed");
      } else {
        fieldStates.set(url.hostname, "ready");
        fieldErrors.set(url.hostname, "");
      }
      return Response.json(
        {
          ...fieldRecordingStatus("stopping"),
          active_recording_id: `field-${role}`,
          shared_recording_id: fieldSharedRecordingIds.get(url.hostname) ?? null,
          started_at_utc: "2026-08-22T18:00:00Z",
          started_elapsed_realtime_ns: "123456789",
        },
        { status: 202 },
      );
    }
    if (url.pathname === "/api/v1/field-recordings") {
      const authorization = new Headers(init?.headers).get("Authorization");
      fieldCatalogAuthorizations.push(authorization);
      if (authorization !== expectedAuthorization) {
        return Response.json(
          { error: "a valid bearer control credential is required" },
          { status: 401, headers: { "WWW-Authenticate": "Bearer" } },
        );
      }
      const shortRole = role === "down_the_line" ? "dtl" : "face";
      return Response.json({
        schema_version: 1,
        recordings: [
          {
            recording_id: `field-${shortRole}`,
            shared_recording_id: "field-shared",
            created_at_utc: "2026-08-22T18:00:00Z",
            role,
            duration_us: "12000000",
            video_bytes: "1200000",
            audio_frames: "576000",
            video_url: `/api/v1/field-recordings/field-${shortRole}/video.mp4`,
            audio_url: `/api/v1/field-recordings/field-${shortRole}/audio.wav`,
            manifest_url: `/api/v1/field-recordings/field-${shortRole}/manifest`,
          },
        ],
      });
    }
    if (url.pathname === "/api/v1/capture/missed-shot") {
      return Response.json({
        session_id: `standby-${role}`,
        state: standbyMissedShot ? "ready" : "waiting_post_roll",
        created_at_utc: "2026-08-17T22:00:00Z",
        error: "",
        ...(standbyMissedShot ? { session_kind: "standby_diagnostic" } : {}),
      });
    }
    if (url.pathname === "/api/v1/capture/manual") {
      return Response.json({
        session_id: `manual-${role}`,
        state: "waiting_post_roll",
        created_at_utc: "2026-08-17T22:00:00Z",
        error: "",
      });
    }
    if (url.pathname === "/api/v1/sessions") {
      return Response.json({
        schema_version: 1,
        sessions: includeStandbyDiagnostic
          ? [
              {
                session_id: `standby-${role}`,
                state: "ready",
                created_at_utc: "2026-08-17T22:00:00Z",
                error: "",
                session_kind: "standby_diagnostic",
              },
            ]
          : [],
      });
    }
    if (url.pathname.endsWith("/manifest")) {
      manifestRequests += 1;
      return Response.json({ error: "standby diagnostic has no clip manifest" }, { status: 404 });
    }
    if (url.pathname === "/api/v1/capture/trigger-report") {
      if (activeSharedSessionId === null) {
        return Response.json({ error: "no trigger" }, { status: 404 });
      }
      const nodeOffset = role === "down_the_line" ? 10_000_000_000n : 20_000_000_000n;
      return Response.json({
        schema_version: 1,
        role,
        node_id: nodeId,
        shared_session_id: activeSharedSessionId,
        local_session_id: role === "down_the_line" ? "local-dtl" : "local-face",
        trigger_elapsed_realtime_ns: (nodeOffset + 100_000_000n).toString(),
        timestamp_uncertainty_ns: "250000",
        source: "local_audio",
      });
    }
    if (url.pathname.startsWith("/api/v1/coordination/")) {
      const key = url.hostname;
      if (init?.method === "POST") {
        const record = JSON.parse(String(init.body)) as unknown;
        const existing = coordinationRecords.get(key);
        if (existing !== undefined && JSON.stringify(existing) !== JSON.stringify(record)) {
          return Response.json(existing, { status: 409 });
        }
        coordinationRecords.set(key, record);
        return Response.json(record, { status: existing === undefined ? 201 : 200 });
      }
      ++coordinationReads;
      const record = coordinationRecords.get(key);
      return record === undefined
        ? Response.json({ error: "missing" }, { status: 404 })
        : Response.json(record);
    }
    return Response.json({ error: `unexpected ${url.pathname}` }, { status: 404 });
  }) as typeof fetch;
  return {
    armBodies,
    sensitiveReadRequests,
    fieldCatalogAuthorizations,
    fieldMutationRequests,
    mutationRequests,
    coordinationRecords,
    get coordinationReads() {
      return coordinationReads;
    },
    get manifestRequests() {
      return manifestRequests;
    },
    fetcher,
    now,
    swapRoles() {
      rolesSwapped = !rolesSwapped;
    },
    forceDisarmed(host: string) {
      forcedDisarmedHost = host;
    },
    forceSharedSession(host: string, sharedSessionId: string | null) {
      forcedSharedSessions.set(host, sharedSessionId);
    },
    rotateControlCredential(host: string, token: string) {
      assert.ok(controlTokens.has(host));
      controlTokens.set(host, token);
    },
    failNextFieldRecordingStartAsynchronously(host: string) {
      assert.ok(fieldStates.has(host));
      asynchronousStartFailures.add(host);
    },
    failNextFieldRecordingStopAsynchronously(host: string) {
      assert.ok(fieldStates.has(host));
      asynchronousStopFailures.add(host);
    },
  };
}

function publicationNodes(initialFaceState: "encoding" | "ready") {
  let coordinatorNow = 0n;
  let faceState: "encoding" | "ready" = initialFaceState;
  const now = () => {
    coordinatorNow += 1_000_000n;
    return coordinatorNow;
  };
  const fetcher = (async (input: RequestInfo | URL) => {
    const url = new URL(String(input));
    const downTheLine = url.hostname === "dtl.test";
    const role: ReviewRole = downTheLine ? "down_the_line" : "face_on";
    const nodeId = downTheLine ? "node-dtl" : "node-face";
    if (url.pathname === "/api/v1/node") {
      return Response.json({
        schema_version: 1,
        node_id: nodeId,
        role,
        capture_profile: downTheLine ? "1080p240" : "720p240",
        control_authentication: "bearer",
      });
    }
    if (url.pathname === "/api/v1/capture/status") {
      const state = downTheLine ? "ready" : faceState;
      return Response.json({
        schema_version: 2,
        state,
        armed: false,
        active_session_id: state === "encoding" ? "local-face" : null,
        shared_session_id: state === "encoding" ? "shared-publication" : null,
        error: "",
        hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
      });
    }
    if (url.pathname === "/api/v1/sessions") {
      return Response.json({
        schema_version: 1,
        sessions: downTheLine
          ? [
              {
                session_id: "local-dtl",
                state: "ready",
                created_at_utc: "2026-08-17T22:00:00.000Z",
                error: "",
              },
            ]
          : [],
      });
    }
    if (url.pathname === "/api/v1/sessions/local-dtl/manifest" && downTheLine) {
      const manifest = androidManifest("down_the_line", "node-dtl", "local-dtl");
      assert.ok(manifest.android_capture);
      delete manifest.views[0]?.media.url;
      manifest.created_at_utc = "2026-08-17T22:00:00.000Z";
      manifest.android_capture.shared_session_id = "shared-publication";
      return Response.json(manifest);
    }
    if (url.pathname === "/api/v1/coordination/shared-publication") {
      return Response.json({ error: "missing" }, { status: 404 });
    }
    return Response.json({ error: `unexpected ${url.pathname}` }, { status: 404 });
  }) as typeof fetch;
  return {
    fetcher,
    now,
    setFaceState(state: "encoding" | "ready") {
      faceState = state;
    },
  };
}

function mismatchedSessionNodes() {
  let coordinatorNow = 0n;
  const now = () => {
    coordinatorNow += 1_000_000n;
    return coordinatorNow;
  };
  const fetcher = (async (input: RequestInfo | URL) => {
    const url = new URL(String(input));
    const downTheLine = url.hostname === "dtl.test";
    const role: ReviewRole = downTheLine ? "down_the_line" : "face_on";
    if (url.pathname === "/api/v1/node") {
      return Response.json({
        schema_version: 1,
        node_id: downTheLine ? "node-dtl" : "node-face",
        role,
        capture_profile: downTheLine ? "1080p240" : "720p240",
        control_authentication: "bearer",
      });
    }
    if (url.pathname === "/api/v1/capture/status") {
      return Response.json({
        schema_version: 2,
        state: "armed",
        armed: true,
        active_session_id: downTheLine ? "local-dtl" : "local-face",
        shared_session_id: downTheLine ? "stale-dtl" : "stale-face",
        error: "",
        hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
      });
    }
    return Response.json({ error: `unexpected ${url.pathname}` }, { status: 404 });
  }) as typeof fetch;
  return { fetcher, now };
}

function fieldRecordingStatus(
  state: "idle" | "starting" | "recording" | "stopping" | "ready" | "error",
) {
  return {
    schema_version: 1,
    state,
    active_recording_id: null,
    shared_recording_id: null,
    started_at_utc: null,
    started_elapsed_realtime_ns: null,
    elapsed_ms: 0,
    video_bytes: "0",
    audio_frames: "0",
    max_duration_seconds: 600,
    error: "",
  };
}

void main().catch((caught: unknown) => {
  console.error(caught);
  process.exit(1);
});
