import assert from "node:assert/strict";
import {
  DualNodeReviewApi,
  associateDualTriggers,
  composeDualManifest,
  estimateClockOffset,
  groupAndroidSessionManifests,
  localizeDiagnosticFeedback,
  type ClockExchangeSample,
  type ClockOffsetEstimate,
  type NodeTriggerReport,
} from "./dual_node_review_api.js";
import { FIXTURE_MANIFEST } from "./fake_review_api.js";
import {
  DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
  type DiagnosticFeedback,
  parseClipManifest,
  type ClipManifest,
  type ReviewRole,
} from "./review_api.js";

async function main() {
  estimatesIntersectedClockBounds();
  rejectsInconsistentClockBounds();
  associatesExactlyOneBoundedReportPerRole();
  rejectsUnrelatedTriggerTiming();
  composesTwoOriginPreservingTracks();
  preservesMissedShotTriggerSemantics();
  localizesCommonRelativeDiagnosticMarks();
  rejectsUnsafeLocalizedDiagnosticMarks();
  parsesAndroidSingleNodeTimingEvidence();
  parsesLegacyUncoordinatedAndroidMetadata();
  isolatesDuplicateHistoricalRolesFromValidPairs();
  await provisionsOneSharedSessionAndRollsBackPartialArm();
  await persistsAndRecoversAlignmentAcrossCoordinatorInstances();
  await remapsEndpointsAfterLiveRoleSwap();
  await filtersStandbyDiagnosticsBeforeManifestLoading();
  await preservesDualStandbyMissedShotKind();
  await preservesPeerArmFailureInCombinedStatus();
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

function fakeDualNodes(
  failArmHost: string | null = null,
  includeStandbyDiagnostic = false,
  standbyMissedShot = false,
  includePeerFailure = false,
) {
  let coordinatorNow = 0n;
  let activeSharedSessionId: string | null = null;
  let rolesSwapped = false;
  const armBodies: Array<Record<string, unknown> & { host: string }> = [];
  const coordinationRecords = new Map<string, unknown>();
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
      if (body.armed === true && url.hostname === failArmHost) {
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
      return Response.json({
        schema_version: 2,
        state: activeSharedSessionId === null ? "setup" : "waiting_post_roll",
        armed: activeSharedSessionId !== null,
        active_session_id:
          activeSharedSessionId === null
            ? null
            : role === "down_the_line"
              ? "local-dtl"
              : "local-face",
        shared_session_id: activeSharedSessionId,
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
    if (url.pathname === "/api/v1/capture/missed-shot") {
      return Response.json({
        session_id: `standby-${role}`,
        state: standbyMissedShot ? "ready" : "waiting_post_roll",
        created_at_utc: "2026-08-17T22:00:00Z",
        error: "",
        ...(standbyMissedShot ? { session_kind: "standby_diagnostic" } : {}),
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
  };
}

void main().catch((caught: unknown) => {
  console.error(caught);
  process.exit(1);
});
