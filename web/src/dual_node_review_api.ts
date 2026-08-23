import { type ControlCredential, controlCredentialValue } from "./control_credential.js";
import {
  LiveStatusCursor,
  parseLiveStatusVersion,
  type StatusSubscriptionOptions,
  subscribeToReconnectableStatus,
} from "./live_status.js";
import {
  CAPTURE_SCHEMA_VERSION,
  type CaptureState,
  type CaptureStatus,
  type ClipManifest,
  type ClipTrack,
  type DiagnosticArchive,
  type DiagnosticFeedback,
  type DiagnosticTimingMarks,
  type DualFieldRecordingStatus,
  FIELD_RECORDING_STATES,
  type FieldRecording,
  type FieldRecordingList,
  type FieldRecordingNodeStatus,
  HttpReviewApi,
  type PeerArmStatus,
  type PoseCaptureStatus,
  parseCaptureStatus,
  parseSessionSummary,
  REVIEW_SCHEMA_VERSION,
  type ReviewApi,
  type ReviewRole,
  type SessionList,
  type SessionSummary,
  validateDiagnosticFeedback,
} from "./review_api.js";

const CLOCK_SAMPLE_COUNT = 3;
const PAIRING_TOLERANCE_NS = 50_000_000n;
const MAXIMUM_REPORT_UNCERTAINTY_NS = 10_000_000n;
const MAXIMUM_PAIR_UNCERTAINTY_NS = 20_000_000n;
const ALIGNMENT_STORAGE_PREFIX = "swing-capture.dual-alignment.";
const FIELD_RECORDING_DISARM_TIMEOUT_MS = 5_000;
const FIELD_RECORDING_DISARM_POLL_MS = 100;
const FIELD_RECORDING_TRANSITION_TIMEOUT_MS = 10_000;
const FIELD_RECORDING_TRANSITION_POLL_MS = 100;
// A phone serves the historical review catalog and live control API from the same small HTTP
// executor. Hydrating every retained manifest at once can occupy every browser connection and
// server worker, preventing an operator's authenticated start/stop request from reaching the
// phone. Keep background catalog work below that shared transport's capacity; control requests do
// not use this gate and therefore retain a path to each node.
const MAXIMUM_CONCURRENT_MANIFEST_REQUESTS_PER_NODE = 2;

export interface DualNodeEndpoint {
  baseUrl: string;
  controlToken: ControlCredential;
  role: ReviewRole;
}

export interface ClockExchangeSample {
  coordinatorSendNs: bigint;
  nodeReceiveNs: bigint;
  nodeSendNs: bigint;
  coordinatorReceiveNs: bigint;
}

export interface ClockOffsetEstimate {
  nodeId: string;
  offsetNs: bigint;
  uncertaintyNs: bigint;
  minimumRoundTripNs: bigint;
  maximumRoundTripNs: bigint;
  sampleCount: number;
}

export interface NodeTriggerReport {
  role: ReviewRole;
  nodeId: string;
  sharedSessionId: string;
  localSessionId: string;
  triggerTimestampNs: bigint;
  timestampUncertaintyNs: bigint;
  source: string;
}

export interface ObservedNodeTrigger {
  report: NodeTriggerReport;
  coordinatorTimestampNs: bigint;
  coordinatorUncertaintyNs: bigint;
}

export interface DualNodeAlignment {
  schema_version: 1;
  shared_session_id: string;
  status: "paired";
  recorded_at_epoch_ms: string;
  down_the_line: SerializedObservedTrigger;
  face_on: SerializedObservedTrigger;
  minimum_trigger_separation_ns: string;
  maximum_trigger_separation_ns: string;
}

interface SerializedObservedTrigger {
  role: ReviewRole;
  node_id: string;
  local_session_id: string;
  trigger_timestamp_ns: string;
  trigger_uncertainty_ns: string;
  mapped_coordinator_timestamp_ns: string;
  mapped_coordinator_uncertainty_ns: string;
  clock_offset_ns: string;
  clock_uncertainty_ns: string;
  minimum_round_trip_ns: string;
  maximum_round_trip_ns: string;
  clock_sample_count: number;
  source: string;
}

interface AlignmentStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}

interface NodeDescriptor {
  nodeId: string;
  role: ReviewRole;
  captureProfile: string;
}

interface DetailedCaptureStatus {
  status: CaptureStatus;
  sharedSessionId: string | null;
}

export interface SessionPair {
  sharedSessionId: string;
  downTheLine?: ClipManifest;
  faceOn?: ClipManifest;
  error?: string;
}

interface LocalSessionReference {
  node: AndroidNodeClient;
  summary: SessionSummary;
  manifest?: ClipManifest;
}

interface SessionReferencePair {
  sharedSessionId: string;
  downTheLine?: LocalSessionReference;
  faceOn?: LocalSessionReference;
  error?: string;
}

type Fetcher = typeof fetch;
type MonotonicNow = () => bigint;
type SessionIdFactory = () => string;

/**
 * Browser-side station coordinator for exactly two explicitly configured Android nodes.
 *
 * Capture remains autonomous on each phone. This class only provisions a shared session ID,
 * measures bounded clock offsets, rejects invalid trigger pairs, and presents one two-view review
 * contract after both independently published clips are ready.
 */
export class DualNodeReviewApi implements ReviewApi {
  #nodes: [AndroidNodeClient, AndroidNodeClient];
  readonly #sessionIdFactory: SessionIdFactory;
  readonly #storage: AlignmentStorage | null;
  readonly #manifestCache = new Map<string, Promise<ClipManifest>>();
  readonly #alignmentCache = new Map<string, DualNodeAlignment>();
  readonly #durablyReplicated = new Set<string>();
  readonly #statusSubscriptionOptions: StatusSubscriptionOptions;
  #descriptorRequest: Promise<readonly [NodeDescriptor, NodeDescriptor]> | null = null;
  #activeSharedSessionId: string | null = null;
  #coordinationError: string | null = null;
  #coordinationAttempt: Promise<void> | null = null;

  constructor(
    endpoints: readonly [DualNodeEndpoint, DualNodeEndpoint],
    fetcher: Fetcher = globalThis.fetch.bind(globalThis),
    now: MonotonicNow = browserMonotonicNow,
    sessionIdFactory: SessionIdFactory = newSharedSessionId,
    storage: AlignmentStorage | null = browserStorage(),
    statusSubscriptionOptions: StatusSubscriptionOptions = {},
  ) {
    const ordered = [...endpoints].sort(
      (left, right) => roleOrder(left.role) - roleOrder(right.role),
    );
    if (ordered[0]?.role !== "down_the_line" || ordered[1]?.role !== "face_on") {
      throw new Error("Dual-node capture requires one configured endpoint for each camera role");
    }
    this.#nodes = [
      new AndroidNodeClient(ordered[0], fetcher, now),
      new AndroidNodeClient(ordered[1], fetcher, now),
    ];
    this.#sessionIdFactory = sessionIdFactory;
    this.#storage = storage;
    this.#statusSubscriptionOptions = statusSubscriptionOptions;
  }

  subscribeToChanges(onChange: () => void): () => void {
    const unsubscribers = this.#nodes.map((node) =>
      node.subscribeToStatus(onChange, this.#statusSubscriptionOptions),
    );
    return () => {
      for (const unsubscribe of unsubscribers) {
        unsubscribe();
      }
    };
  }

  async getCaptureStatus(): Promise<CaptureStatus> {
    await this.#ensureDescriptors();
    const details = await Promise.all(this.#nodes.map((node) => node.captureStatus()));
    const reportedSharedId = consistentSharedSessionId(
      details,
      !details.every((detail) => detail.status.armed),
    );
    if (reportedSharedId !== null) {
      this.#activeSharedSessionId = reportedSharedId;
    }
    if (
      this.#activeSharedSessionId !== null &&
      this.#coordinationAttempt === null &&
      (await this.#ensureAlignment(this.#activeSharedSessionId)) === null
    ) {
      const sessionId = this.#activeSharedSessionId;
      this.#coordinationAttempt = this.#coordinateTriggers(sessionId).finally(() => {
        this.#coordinationAttempt = null;
      });
      await this.#coordinationAttempt;
    }
    const combined = combineCaptureStatuses(
      details.map((detail) => detail.status),
      this.#coordinationError,
    );
    return details.some((detail) => detail.status.active_session_id !== null) &&
      reportedSharedId !== null
      ? { ...combined, active_session_id: reportedSharedId }
      : combined;
  }

  async setArmed(armed: boolean): Promise<CaptureStatus> {
    await this.#verifyControlCredentials();
    if (!armed) {
      const results = await Promise.allSettled(
        this.#nodes.map((node) => node.setArmed(false, null)),
      );
      throwRejectedNodeOperation(results, "disarm");
      this.#activeSharedSessionId = null;
      this.#coordinationError = null;
      return combineCaptureStatuses(
        results.map((result) => fulfilled(result)),
        null,
      );
    }

    await Promise.all(this.#nodes.map((node) => node.clockEstimate(CLOCK_SAMPLE_COUNT)));
    const sharedSessionId = this.#sessionIdFactory();
    // A failed two-node arm must not leave browser-local ownership behind. The successful node is
    // rolled back below, and clearing this state before sending either request also makes an
    // immediate operator retry allocate a fresh, internally consistent arm transaction.
    this.#activeSharedSessionId = null;
    this.#coordinationError = null;
    const results = await Promise.allSettled(
      this.#nodes.map((node) => node.setArmed(true, sharedSessionId)),
    );
    if (results.some((result) => result.status === "rejected")) {
      await Promise.allSettled(
        this.#nodes.map((node, index) =>
          results[index]?.status === "fulfilled"
            ? node.setArmed(false, null)
            : Promise.resolve(emptyCaptureStatus()),
        ),
      );
      throwRejectedNodeOperation(results, "arm");
    }
    this.#activeSharedSessionId = sharedSessionId;
    this.#coordinationError = null;
    return combineCaptureStatuses(
      results.map((result) => fulfilled(result)),
      null,
    );
  }

  async triggerManualCapture(): Promise<SessionSummary> {
    if (this.#activeSharedSessionId === null) {
      throw new Error("Both Android nodes must be armed before a dual manual capture");
    }
    await this.#verifyControlCredentials();
    const details = await Promise.all(this.#nodes.map((node) => node.captureStatus()));
    if (!details.every((detail) => detail.status.armed)) {
      throw new Error("Both Android nodes must be armed before a dual manual capture");
    }
    const reportedSharedSessionId = consistentSharedSessionId(details);
    if (reportedSharedSessionId === null) {
      throw new Error("Android nodes no longer report the browser's active shared session");
    }
    if (this.#activeSharedSessionId !== reportedSharedSessionId) {
      throw new Error("Android nodes report a different active shared session than the browser");
    }
    const results = await Promise.allSettled(this.#nodes.map((node) => node.triggerManual()));
    throwRejectedNodeOperation(results, "manual trigger");
    return {
      session_id: this.#activeSharedSessionId,
      state: "waiting_post_roll",
      created_at_utc: new Date().toISOString(),
      error: "",
    };
  }

  async saveMissedShot(): Promise<SessionSummary> {
    await this.#verifyControlCredentials();
    // Always revalidate both live nodes immediately before either diagnostic mutation. Browser
    // ownership can be stale after a local capture error, process restart, or peer transition even
    // when this coordinator originally armed the pair and still retains its shared session ID.
    const details = await Promise.all(this.#nodes.map((node) => node.captureStatus()));
    if (!details.every((detail) => detail.status.armed)) {
      throw new Error("Both Android nodes must be armed before saving a missed shot");
    }
    const reportedSharedSessionId = consistentSharedSessionId(details);
    if (this.#activeSharedSessionId !== null && reportedSharedSessionId === null) {
      throw new Error("Android nodes no longer report the browser's active shared session");
    }
    if (
      this.#activeSharedSessionId !== null &&
      reportedSharedSessionId !== null &&
      this.#activeSharedSessionId !== reportedSharedSessionId
    ) {
      throw new Error("Android nodes report a different active shared session than the browser");
    }
    if (reportedSharedSessionId !== null) {
      this.#activeSharedSessionId = reportedSharedSessionId;
    }
    const results = await Promise.allSettled(this.#nodes.map((node) => node.saveMissedShot()));
    if (results.some((result) => result.status === "rejected")) {
      // A credential can rotate or a phone can leave capture between the read-only preflight and
      // the two concurrent diagnostic mutations. The accepted node has already consumed its ring
      // and disarmed, so the operation cannot be rolled back. Converge both phones to the safe
      // stopped state instead of leaving one armed under stale browser ownership.
      const primaryFailure = rejectedNodeOperation(results, "save missed shot");
      const disarmResults = await Promise.allSettled(
        this.#nodes.map((node) => node.setArmed(false, null)),
      );
      this.#activeSharedSessionId = null;
      this.#coordinationError = null;
      const disarmFailure = rejectedNodeOperation(
        disarmResults,
        "disarm after partial missed shot on",
      );
      throw combinedOperationFailure(primaryFailure, disarmFailure, null);
    }
    const summaries = results.map((result) => fulfilled(result));
    const firstSummary = summaries[0];
    if (firstSummary === undefined) {
      throw new Error("No Android node returned a missed-shot session");
    }
    const sessionKinds = new Set(summaries.map((summary) => summary.session_kind ?? "capture"));
    if (sessionKinds.size !== 1) {
      throw new Error("Android nodes disagreed on the missed-shot session kind");
    }
    const sessionKind = firstSummary.session_kind;
    return {
      session_id: this.#activeSharedSessionId ?? firstSummary.session_id,
      state: "waiting_post_roll",
      created_at_utc: new Date().toISOString(),
      error: "",
      ...(sessionKind === undefined ? {} : { session_kind: sessionKind }),
    };
  }

  async getFieldRecordingStatus(): Promise<DualFieldRecordingStatus> {
    await this.#ensureDescriptors();
    return {
      nodes: await Promise.all(this.#nodes.map((node) => node.fieldRecordingStatus())),
    };
  }

  async startFieldRecording(): Promise<DualFieldRecordingStatus> {
    await this.#verifyControlCredentials();
    const disarmResults = await Promise.allSettled(
      this.#nodes.map((node) => node.setArmed(false, null)),
    );
    throwRejectedNodeOperation(disarmResults, "disarm before field recording");
    this.#activeSharedSessionId = null;
    this.#coordinationError = null;
    await this.#waitUntilCaptureDisarmed();

    const sharedRecordingId = newFieldRecordingId();
    const results = await Promise.allSettled(
      this.#nodes.map((node) => node.startFieldRecording(sharedRecordingId)),
    );
    if (results.some((result) => result.status === "rejected")) {
      const primaryFailure = rejectedNodeOperation(results, "start field recording on");
      const rollbackResults = await Promise.allSettled(
        this.#nodes.map((node, index) =>
          results[index]?.status === "fulfilled"
            ? node.stopFieldRecording()
            : Promise.resolve(null),
        ),
      );
      const rollbackFailure = rejectedNodeOperation(
        rollbackResults,
        "roll back field recording on",
      );
      let convergenceFailure: unknown = null;
      try {
        await this.#waitForFieldRecordingStop(sharedRecordingId);
      } catch (caught) {
        convergenceFailure = caught;
      }
      throw combinedOperationFailure(primaryFailure, rollbackFailure, convergenceFailure);
    }
    try {
      const nodes = await this.#waitForFieldRecordingStart(sharedRecordingId);
      return { nodes };
    } catch (startupFailure) {
      const rollbackResults = await Promise.allSettled(
        this.#nodes.map((node) => node.stopFieldRecording()),
      );
      const rollbackFailure = rejectedNodeOperation(
        rollbackResults,
        "roll back field recording on",
      );
      let convergenceFailure: unknown = null;
      try {
        await this.#waitForFieldRecordingStop(sharedRecordingId);
      } catch (caught) {
        convergenceFailure = caught;
      }
      throw combinedOperationFailure(startupFailure, rollbackFailure, convergenceFailure);
    }
  }

  async stopFieldRecording(): Promise<DualFieldRecordingStatus> {
    await this.#verifyControlCredentials();
    const results = await Promise.allSettled(this.#nodes.map((node) => node.stopFieldRecording()));
    throwRejectedNodeOperation(results, "stop field recording on");
    const accepted = results.map((result) => fulfilled(result));
    const sharedRecordingId = consistentFieldRecordingId(accepted);
    return { nodes: await this.#waitForFieldRecordingStop(sharedRecordingId) };
  }

  async getFieldRecordings(): Promise<FieldRecordingList> {
    await this.#ensureDescriptors();
    const recordings = (
      await Promise.all(this.#nodes.map((node) => node.fieldRecordings()))
    ).flat();
    return {
      recordings: recordings.sort((left, right) =>
        right.created_at_utc.localeCompare(left.created_at_utc),
      ),
    };
  }

  async #waitUntilCaptureDisarmed(): Promise<void> {
    const deadline = Date.now() + FIELD_RECORDING_DISARM_TIMEOUT_MS;
    for (;;) {
      const details = await Promise.all(this.#nodes.map((node) => node.captureStatus()));
      if (details.every((detail) => !detail.status.armed)) {
        return;
      }
      if (Date.now() >= deadline) {
        throw new Error("Both Android nodes did not disarm before field recording");
      }
      await delay(FIELD_RECORDING_DISARM_POLL_MS);
    }
  }

  async #waitForFieldRecordingStart(
    sharedRecordingId: string,
  ): Promise<[FieldRecordingNodeStatus, FieldRecordingNodeStatus]> {
    const deadline = Date.now() + FIELD_RECORDING_TRANSITION_TIMEOUT_MS;
    for (;;) {
      const statuses = await Promise.all(this.#nodes.map((node) => node.fieldRecordingStatus()));
      for (const status of statuses) {
        if (status.state === "error") {
          throw new Error(
            `${nodeRoleLabel(status.role)} field recorder failed after accepting start${
              status.error.length === 0 ? "" : `: ${status.error}`
            }`,
          );
        }
        if (status.state !== "starting" && status.state !== "recording") {
          throw new Error(
            `${nodeRoleLabel(status.role)} field recorder left startup in state ${status.state}`,
          );
        }
        if (status.shared_recording_id !== sharedRecordingId) {
          throw new Error(
            `${nodeRoleLabel(status.role)} field recorder reported a different shared recording ID`,
          );
        }
      }
      if (statuses.every((status) => status.state === "recording")) {
        return fieldRecordingStatusPair(statuses);
      }
      if (Date.now() >= deadline) {
        throw new Error("Both Android field recorders did not become ready within 10 seconds");
      }
      await delay(FIELD_RECORDING_TRANSITION_POLL_MS);
    }
  }

  async #waitForFieldRecordingStop(
    sharedRecordingId: string | null,
  ): Promise<[FieldRecordingNodeStatus, FieldRecordingNodeStatus]> {
    const deadline = Date.now() + FIELD_RECORDING_TRANSITION_TIMEOUT_MS;
    for (;;) {
      const statuses = await Promise.all(this.#nodes.map((node) => node.fieldRecordingStatus()));
      for (const status of statuses) {
        if (status.state === "error") {
          throw new Error(
            `${nodeRoleLabel(status.role)} field recorder failed while stopping or publishing${
              status.error.length === 0 ? "" : `: ${status.error}`
            }`,
          );
        }
        if (
          status.state !== "starting" &&
          status.state !== "recording" &&
          status.state !== "stopping" &&
          status.state !== "ready" &&
          status.state !== "idle"
        ) {
          throw new Error(
            `${nodeRoleLabel(status.role)} field recorder left shutdown in state ${status.state}`,
          );
        }
        if (
          sharedRecordingId !== null &&
          status.state !== "idle" &&
          status.shared_recording_id !== sharedRecordingId
        ) {
          throw new Error(
            `${nodeRoleLabel(status.role)} field recorder published a different shared recording ID`,
          );
        }
      }
      if (statuses.every((status) => status.state === "ready" || status.state === "idle")) {
        return fieldRecordingStatusPair(statuses);
      }
      if (Date.now() >= deadline) {
        throw new Error("Both Android field recorders did not finish publishing within 10 seconds");
      }
      await delay(FIELD_RECORDING_TRANSITION_POLL_MS);
    }
  }

  startSyntheticSwing(): Promise<CaptureStatus> {
    return Promise.reject(
      new Error("The Android dual-node coordinator does not expose the host synthetic HIL action"),
    );
  }

  async getSessions(): Promise<SessionList> {
    await this.#ensureDescriptors();
    const [pairs, captureDetails] = await Promise.all([
      this.#sessionPairs(),
      Promise.all(this.#nodes.map((node) => node.captureStatus())),
    ]);
    const publishingStates = new Map<string, CaptureState>();
    for (const detail of captureDetails) {
      if (
        detail.sharedSessionId !== null &&
        (detail.status.state === "waiting_post_roll" || detail.status.state === "encoding")
      ) {
        const previous = publishingStates.get(detail.sharedSessionId);
        publishingStates.set(
          detail.sharedSessionId,
          previous === "encoding" || detail.status.state === "encoding"
            ? "encoding"
            : "waiting_post_roll",
        );
      }
    }
    // Current phones advertise whether their immutable coordination record exists alongside each
    // compact clip summary. Only legacy nodes need eager record reads to preserve their older
    // catalog semantics; current catalogs defer full record validation until a shot is opened.
    await Promise.all(
      [...pairs.values()]
        .filter(
          (pair) =>
            pair.error === undefined &&
            pair.downTheLine !== undefined &&
            pair.faceOn !== undefined &&
            (pair.downTheLine.summary.android_capture?.coordination_available === undefined ||
              pair.faceOn.summary.android_capture?.coordination_available === undefined),
        )
        .map((pair) => this.#ensureAlignment(pair.sharedSessionId)),
    );
    const sessions = [...pairs.values()].map((pair) =>
      this.#sessionSummary(pair, publishingStates.get(pair.sharedSessionId)),
    );
    // An actively publishing session is the operator's current shot even when only the second
    // configured role has published a manifest. Prefer it over an older ready session whose
    // timestamp happens to compare equal; otherwise DTL-encoding and face-on-encoding select
    // different default rows solely because node manifests are flattened in role order.
    sessions.sort((left, right) => {
      const preferred = (session: SessionSummary) =>
        publishingStates.has(session.session_id) ||
        session.session_id === this.#activeSharedSessionId;
      const activePrecedence = Number(preferred(right)) - Number(preferred(left));
      return activePrecedence !== 0
        ? activePrecedence
        : right.created_at_utc.localeCompare(left.created_at_utc);
    });
    return { schema_version: REVIEW_SCHEMA_VERSION, sessions };
  }

  async getManifest(sessionId: string): Promise<ClipManifest> {
    const pair = await this.#completeSessionPair(sessionId);
    const alignment = await this.#ensureAlignment(sessionId);
    if (alignment === null) {
      throw new Error(
        `Coordinated session ${sessionId} has two clips but no retained clock/trigger evidence`,
      );
    }
    return composeDualManifest(pair.downTheLine, pair.faceOn, alignment);
  }

  async submitDiagnosticFeedback(sessionId: string, feedback: DiagnosticFeedback): Promise<void> {
    validateDiagnosticFeedback(feedback);
    const pair = await this.#completeSessionPair(sessionId);
    const alignment = await this.#ensureAlignment(sessionId);
    if (alignment === null) {
      throw new Error(
        `Coordinated session ${sessionId} has two clips but no retained clock/trigger evidence`,
      );
    }
    if (
      pair.downTheLine.session_id !== alignment.down_the_line.local_session_id ||
      pair.faceOn.session_id !== alignment.face_on.local_session_id
    ) {
      throw new Error("Diagnostic feedback session IDs disagree with the trigger association");
    }
    const shifts = triggerShiftsFromCommon(alignment);
    const nodes = await this.#nodesForPair(pair);
    await Promise.all([
      nodes.downTheLine.verifyControlCredential(
        requiredAndroidNodeId(pair.downTheLine, "down-the-line"),
      ),
      nodes.faceOn.verifyControlCredential(requiredAndroidNodeId(pair.faceOn, "face-on")),
    ]);
    await Promise.all([
      nodes.downTheLine.submitDiagnosticFeedback(
        pair.downTheLine.session_id,
        localizeDiagnosticFeedback(feedback, shifts.downTheLine),
      ),
      nodes.faceOn.submitDiagnosticFeedback(
        pair.faceOn.session_id,
        localizeDiagnosticFeedback(feedback, shifts.faceOn),
      ),
    ]);
  }

  async getDiagnosticArchives(sessionId: string): Promise<readonly DiagnosticArchive[]> {
    const pair = await this.#completeSessionPair(sessionId);
    const nodes = await this.#nodesForPair(pair);
    const [downTheLine, faceOn] = await Promise.all([
      nodes.downTheLine.getDiagnosticArchives(pair.downTheLine.session_id),
      nodes.faceOn.getDiagnosticArchives(pair.faceOn.session_id),
    ]);
    return [
      ...downTheLine.map((archive) => ({
        ...archive,
        filename: `down-the-line-${archive.filename}`,
      })),
      ...faceOn.map((archive) => ({ ...archive, filename: `face-on-${archive.filename}` })),
    ];
  }

  async #completeSessionPair(
    sessionId: string,
  ): Promise<SessionPair & { downTheLine: ClipManifest; faceOn: ClipManifest }> {
    const referencePair = (await this.#sessionPairs()).get(sessionId);
    if (referencePair === undefined) {
      throw new Error(`Coordinated Android session ${sessionId} was not found`);
    }
    if (referencePair.error !== undefined) {
      throw new Error(referencePair.error);
    }
    if (referencePair.downTheLine === undefined || referencePair.faceOn === undefined) {
      throw new Error(missingRoleDiagnostic(referencePair));
    }
    const manifests = await Promise.all([
      this.#referenceManifest(referencePair.downTheLine),
      this.#referenceManifest(referencePair.faceOn),
    ]);
    const verified = groupAndroidSessionManifests(manifests);
    const pair = verified.get(sessionId);
    if (verified.size !== 1 || pair === undefined) {
      throw new Error(`Coordinated session ${sessionId} summary disagrees with its clip manifests`);
    }
    if (pair.error !== undefined) {
      throw new Error(pair.error);
    }
    if (pair.downTheLine === undefined || pair.faceOn === undefined) {
      throw new Error(missingRoleDiagnostic(pair));
    }
    return { ...pair, downTheLine: pair.downTheLine, faceOn: pair.faceOn };
  }

  async #nodesForPair(pair: {
    downTheLine: ClipManifest;
    faceOn: ClipManifest;
  }): Promise<{ downTheLine: AndroidNodeClient; faceOn: AndroidNodeClient }> {
    const described = await Promise.all(
      this.#nodes.map(async (node) => ({ node, descriptor: await node.descriptor() })),
    );
    const downTheLineNodeId = pair.downTheLine.android_capture?.node_id;
    const faceOnNodeId = pair.faceOn.android_capture?.node_id;
    if (downTheLineNodeId === undefined || faceOnNodeId === undefined) {
      throw new Error("Dual Android diagnostic routing requires both retained node IDs");
    }
    const downTheLine = described.find(
      (candidate) => candidate.descriptor.nodeId === downTheLineNodeId,
    )?.node;
    const faceOn = described.find(
      (candidate) => candidate.descriptor.nodeId === faceOnNodeId,
    )?.node;
    if (downTheLine === undefined || faceOn === undefined || downTheLine === faceOn) {
      throw new Error("Dual Android diagnostic routing no longer matches the configured phones");
    }
    return { downTheLine, faceOn };
  }

  async #ensureDescriptors(): Promise<readonly [NodeDescriptor, NodeDescriptor]> {
    if (this.#descriptorRequest !== null) {
      return this.#descriptorRequest;
    }
    const request = this.#loadDescriptors();
    this.#descriptorRequest = request;
    try {
      return await request;
    } finally {
      if (this.#descriptorRequest === request) {
        this.#descriptorRequest = null;
      }
    }
  }

  async #loadDescriptors(): Promise<readonly [NodeDescriptor, NodeDescriptor]> {
    const described = await Promise.all(
      this.#nodes.map(async (node) => ({ node, descriptor: await node.descriptor() })),
    );
    if (described[0]?.descriptor.nodeId === described[1]?.descriptor.nodeId) {
      throw new Error("One Android installation cannot supply both camera roles");
    }
    described.sort(
      (left, right) => roleOrder(left.descriptor.role) - roleOrder(right.descriptor.role),
    );
    const downTheLine = described[0];
    const faceOn = described[1];
    if (downTheLine?.descriptor.role !== "down_the_line" || faceOn?.descriptor.role !== "face_on") {
      throw new Error("Dual-node capture requires one live phone for each camera role");
    }
    downTheLine.node.setCanonicalRole("down_the_line");
    faceOn.node.setCanonicalRole("face_on");
    this.#nodes = [downTheLine.node, faceOn.node];
    return [downTheLine.descriptor, faceOn.descriptor];
  }

  async #verifyControlCredentials(): Promise<readonly [NodeDescriptor, NodeDescriptor]> {
    const descriptors = await this.#ensureDescriptors();
    await Promise.all([
      this.#nodes[0].verifyControlCredential(descriptors[0].nodeId),
      this.#nodes[1].verifyControlCredential(descriptors[1].nodeId),
    ]);
    return descriptors;
  }

  async #coordinateTriggers(sessionId: string): Promise<void> {
    if (this.#alignment(sessionId) !== null) {
      return;
    }
    try {
      const reports = await Promise.all(this.#nodes.map((node) => node.triggerReport()));
      if (reports.some((report) => report === null)) {
        return;
      }
      const estimates = await Promise.all(
        this.#nodes.map((node) => node.clockEstimate(CLOCK_SAMPLE_COUNT)),
      );
      const alignment = associateDualTriggers(
        sessionId,
        reports as [NodeTriggerReport, NodeTriggerReport],
        estimates as [ClockOffsetEstimate, ClockOffsetEstimate],
      );
      this.#alignmentCache.set(sessionId, alignment);
      this.#storage?.setItem(ALIGNMENT_STORAGE_PREFIX + sessionId, JSON.stringify(alignment));
      await this.#replicateAlignment(alignment);
      this.#coordinationError = null;
    } catch (caught) {
      this.#coordinationError = errorMessage(caught);
    }
  }

  async #sessionPairs(): Promise<Map<string, SessionReferencePair>> {
    const lists = await Promise.all(this.#nodes.map((node) => node.sessions()));
    const references = await Promise.all(
      lists.flatMap((list, nodeIndex) => {
        const node = this.#nodes[nodeIndex];
        if (node === undefined) {
          throw new Error("Session list does not belong to a configured Android node");
        }
        return list.sessions
          .filter(
            (session) => session.state === "ready" && session.session_kind !== "standby_diagnostic",
          )
          .map(async (summary): Promise<LocalSessionReference | null> => {
            if (summary.android_capture !== undefined) {
              return summary.android_capture.shared_session_id === null ? null : { node, summary };
            }
            // Backward compatibility for nodes that predate compact Android pairing summaries.
            const manifest = await this.#manifest(node, summary.session_id);
            const androidCapture = manifest.android_capture;
            const track = manifest.views[0];
            if (
              androidCapture?.shared_session_id === null ||
              androidCapture?.shared_session_id === undefined ||
              manifest.views.length !== 1 ||
              track === undefined
            ) {
              return null;
            }
            return {
              node,
              summary: {
                ...summary,
                android_capture: {
                  node_id: androidCapture.node_id,
                  shared_session_id: androidCapture.shared_session_id,
                  role: track.role,
                },
              },
              manifest,
            };
          });
      }),
    );
    return groupAndroidSessionReferences(
      references.filter((reference): reference is LocalSessionReference => reference !== null),
    );
  }

  #referenceManifest(reference: LocalSessionReference): Promise<ClipManifest> {
    return reference.manifest === undefined
      ? this.#manifest(reference.node, reference.summary.session_id)
      : Promise.resolve(reference.manifest);
  }

  #manifest(node: AndroidNodeClient, sessionId: string): Promise<ClipManifest> {
    const key = `${node.baseUrl}\n${sessionId}`;
    let manifest = this.#manifestCache.get(key);
    if (manifest === undefined) {
      manifest = node.manifest(sessionId);
      this.#manifestCache.set(key, manifest);
    }
    return manifest;
  }

  #sessionSummary(pair: SessionReferencePair, publishingState?: CaptureState): SessionSummary {
    const summaries = [pair.downTheLine, pair.faceOn].filter(
      (reference): reference is LocalSessionReference => reference !== undefined,
    );
    const createdAt =
      summaries
        .map((reference) => reference.summary.created_at_utc)
        .sort()
        .at(-1) ?? new Date(0).toISOString();
    if (pair.error !== undefined) {
      return {
        session_id: pair.sharedSessionId,
        state: "error",
        created_at_utc: createdAt,
        error: pair.error,
      };
    }
    if (pair.downTheLine === undefined || pair.faceOn === undefined) {
      if (publishingState === "waiting_post_roll" || publishingState === "encoding") {
        return {
          session_id: pair.sharedSessionId,
          state: publishingState,
          created_at_utc: createdAt,
          error: "",
        };
      }
      return {
        session_id: pair.sharedSessionId,
        state: "error",
        created_at_utc: createdAt,
        error: missingRoleDiagnostic(pair),
      };
    }
    const summarizedCoordination = [
      pair.downTheLine.summary.android_capture?.coordination_available,
      pair.faceOn.summary.android_capture?.coordination_available,
    ];
    if (
      summarizedCoordination.includes(false) ||
      (summarizedCoordination.includes(undefined) && this.#alignment(pair.sharedSessionId) === null)
    ) {
      return {
        session_id: pair.sharedSessionId,
        state: "error",
        created_at_utc: createdAt,
        error: "Both clips exist, but their clock/trigger association evidence is unavailable",
      };
    }
    return {
      session_id: pair.sharedSessionId,
      state: "ready",
      created_at_utc: createdAt,
      error: "",
    };
  }

  #alignment(sessionId: string): DualNodeAlignment | null {
    const cached = this.#alignmentCache.get(sessionId);
    if (cached !== undefined) {
      return cached;
    }
    const serialized = this.#storage?.getItem(ALIGNMENT_STORAGE_PREFIX + sessionId);
    if (serialized === null || serialized === undefined) {
      return null;
    }
    try {
      const parsed = parseStoredAlignment(JSON.parse(serialized));
      this.#alignmentCache.set(sessionId, parsed);
      return parsed;
    } catch {
      return null;
    }
  }

  async #ensureAlignment(sessionId: string): Promise<DualNodeAlignment | null> {
    const local = this.#alignment(sessionId);
    if (local !== null) {
      if (!this.#durablyReplicated.has(sessionId)) {
        await this.#replicateAlignment(local);
      }
      return local;
    }

    const stored = await Promise.all(this.#nodes.map((node) => node.coordinationRecord(sessionId)));
    const available = stored.filter((value): value is DualNodeAlignment => value !== null);
    if (available.length === 0) {
      return null;
    }
    const recovered = available[0];
    if (recovered === undefined) {
      return null;
    }
    if (available.some((candidate) => !sameAlignment(candidate, recovered))) {
      throw new Error(`Android nodes retain conflicting evidence for ${sessionId}`);
    }
    this.#alignmentCache.set(sessionId, recovered);
    this.#storage?.setItem(ALIGNMENT_STORAGE_PREFIX + sessionId, JSON.stringify(recovered));
    await this.#replicateAlignment(recovered);
    return recovered;
  }

  async #replicateAlignment(alignment: DualNodeAlignment): Promise<void> {
    const results = await Promise.allSettled(
      this.#nodes.map((node) => node.storeCoordinationRecord(alignment)),
    );
    throwRejectedNodeOperation(results, "retain coordination evidence on");
    this.#durablyReplicated.add(alignment.shared_session_id);
  }
}

export function groupAndroidSessionManifests(
  manifests: readonly ClipManifest[],
): Map<string, SessionPair> {
  const pairs = new Map<string, SessionPair>();
  for (const manifest of manifests) {
    const android = manifest.android_capture;
    if (android?.shared_session_id === null || android?.shared_session_id === undefined) {
      continue;
    }
    const track = manifest.views[0];
    if (manifest.views.length !== 1 || track === undefined) {
      continue;
    }
    const pair = pairs.get(android.shared_session_id) ?? {
      sharedSessionId: android.shared_session_id,
    };
    if (track.role === "down_the_line") {
      if (pair.downTheLine === undefined) {
        pair.downTheLine = manifest;
      } else {
        pair.error = `Coordinated session ${pair.sharedSessionId} has duplicate DTL clips`;
      }
    } else if (pair.faceOn === undefined) {
      pair.faceOn = manifest;
    } else {
      pair.error = `Coordinated session ${pair.sharedSessionId} has duplicate face-on clips`;
    }
    pairs.set(pair.sharedSessionId, pair);
  }
  return pairs;
}

function groupAndroidSessionReferences(
  references: readonly LocalSessionReference[],
): Map<string, SessionReferencePair> {
  const pairs = new Map<string, SessionReferencePair>();
  for (const reference of references) {
    const android = reference.summary.android_capture;
    if (android?.shared_session_id === null || android?.shared_session_id === undefined) {
      continue;
    }
    const pair = pairs.get(android.shared_session_id) ?? {
      sharedSessionId: android.shared_session_id,
    };
    if (android.role === "down_the_line") {
      if (pair.downTheLine === undefined) {
        pair.downTheLine = reference;
      } else {
        pair.error = `Coordinated session ${pair.sharedSessionId} has duplicate DTL clips`;
      }
    } else if (pair.faceOn === undefined) {
      pair.faceOn = reference;
    } else {
      pair.error = `Coordinated session ${pair.sharedSessionId} has duplicate face-on clips`;
    }
    pairs.set(pair.sharedSessionId, pair);
  }
  return pairs;
}

class AndroidNodeClient {
  readonly baseUrl: string;
  #canonicalRole: ReviewRole;
  readonly #controlCredential: ControlCredential;
  readonly #fetcher: Fetcher;
  readonly #now: MonotonicNow;
  readonly #reviewApi: HttpReviewApi;
  readonly #manifestRequests = new BoundedRequestGate(
    MAXIMUM_CONCURRENT_MANIFEST_REQUESTS_PER_NODE,
  );
  readonly #liveStatusCursor = new LiveStatusCursor();
  #liveStatusAvailable = true;
  #captureStatusRequest: Promise<DetailedCaptureStatus> | null = null;

  constructor(endpoint: DualNodeEndpoint, fetcher: Fetcher, now: MonotonicNow) {
    this.baseUrl = endpoint.baseUrl.replace(/\/$/, "");
    this.#canonicalRole = endpoint.role;
    this.#controlCredential = endpoint.controlToken;
    this.#fetcher = fetcher;
    this.#now = now;
    this.#reviewApi = new HttpReviewApi(this.baseUrl, fetcher, undefined, endpoint.controlToken);
  }

  async descriptor(): Promise<NodeDescriptor> {
    const value = asObject(await this.#request("/api/v1/node"), "Android node descriptor");
    if (value.schema_version !== 1 || value.control_authentication !== "bearer") {
      throw new Error("Unsupported Android node descriptor");
    }
    return {
      nodeId: asNonemptyString(value.node_id, "node_id"),
      role: asRole(value.role, "node role"),
      captureProfile: asNonemptyString(value.capture_profile, "capture_profile"),
    };
  }

  setCanonicalRole(role: ReviewRole): void {
    this.#canonicalRole = role;
  }

  async captureStatus(): Promise<DetailedCaptureStatus> {
    if (this.#captureStatusRequest !== null) {
      return this.#captureStatusRequest;
    }
    const request = this.#loadCaptureStatus();
    this.#captureStatusRequest = request;
    try {
      return await request;
    } finally {
      if (this.#captureStatusRequest === request) {
        this.#captureStatusRequest = null;
      }
    }
  }

  async #loadCaptureStatus(): Promise<DetailedCaptureStatus> {
    const value = await this.#request("/api/v1/capture/status");
    const object = asObject(value, "Android capture status");
    if (!this.#liveStatusAvailable) {
      throw new Error(
        `${nodeRoleLabel(this.#canonicalRole)} Android live status is disconnected or stale`,
      );
    }
    return {
      status: parseCaptureStatus(value),
      sharedSessionId: asNullableString(object.shared_session_id, "shared_session_id"),
    };
  }

  async verifyControlCredential(expectedNodeId: string): Promise<void> {
    const identity = asObject(
      await this.#request("/api/v1/pairing/identity", { headers: this.#controlHeaders(false) }),
      "Android authenticated identity",
    );
    if (identity.schema_version !== 1) {
      throw new Error(`Unsupported Android identity schema: ${String(identity.schema_version)}`);
    }
    if (asNonemptyString(identity.node_id, "identity node_id") !== expectedNodeId) {
      throw new Error("Authenticated Android identity belongs to another node");
    }
  }

  subscribeToStatus(onChange: () => void, options: StatusSubscriptionOptions): () => void {
    return subscribeToReconnectableStatus(
      async () => {
        try {
          const object = asObject(
            await this.#request("/api/v1/capture/status", {
              headers: this.#controlHeaders(false),
            }),
            "Android capture status",
          );
          const version = parseLiveStatusVersion(object.live_status);
          if (!this.#liveStatusCursor.accept(version)) {
            throw new Error("Android node returned stale live status data");
          }
          this.#liveStatusAvailable = true;
          return version;
        } catch (caught) {
          this.#liveStatusAvailable = false;
          throw caught;
        }
      },
      onChange,
      options,
    );
  }

  async setArmed(armed: boolean, sharedSessionId: string | null): Promise<CaptureStatus> {
    const body: Record<string, unknown> = { armed };
    if (sharedSessionId !== null) {
      body.shared_session_id = sharedSessionId;
    }
    return parseCaptureStatus(
      await this.#request("/api/v1/capture/arm", {
        method: "POST",
        headers: this.#controlHeaders(),
        body: JSON.stringify(body),
      }),
    );
  }

  async triggerManual(): Promise<SessionSummary> {
    const value = asObject(
      await this.#request("/api/v1/capture/manual", {
        method: "POST",
        headers: this.#controlHeaders(),
      }),
      "manual capture response",
    );
    return {
      session_id: asNonemptyString(value.session_id, "session_id"),
      state: asCaptureState(value.state, "session state"),
      created_at_utc: asTimestamp(value.created_at_utc, "created_at_utc"),
      error: asString(value.error, "error"),
    };
  }

  async saveMissedShot(): Promise<SessionSummary> {
    return parseSessionSummary(
      await this.#request("/api/v1/capture/missed-shot", {
        method: "POST",
        headers: this.#controlHeaders(),
        body: "{}",
      }),
    );
  }

  async submitDiagnosticFeedback(sessionId: string, feedback: DiagnosticFeedback): Promise<void> {
    await this.#reviewOperation(() =>
      this.#reviewApi.submitDiagnosticFeedback(sessionId, feedback),
    );
  }

  getDiagnosticArchives(sessionId: string): Promise<readonly DiagnosticArchive[]> {
    return this.#reviewOperation(() => this.#reviewApi.getDiagnosticArchives(sessionId));
  }

  sessions(): Promise<SessionList> {
    return this.#reviewOperation(() => this.#reviewApi.getSessions());
  }

  manifest(sessionId: string): Promise<ClipManifest> {
    return this.#manifestRequests.run(() =>
      this.#reviewOperation(() => this.#reviewApi.getManifest(sessionId)),
    );
  }

  async fieldRecordingStatus(): Promise<FieldRecordingNodeStatus> {
    return this.#parseFieldRecordingStatus(await this.#request("/api/v1/field-recording/status"));
  }

  async startFieldRecording(sharedRecordingId: string): Promise<FieldRecordingNodeStatus> {
    return this.#parseFieldRecordingStatus(
      await this.#request("/api/v1/field-recording/start", {
        method: "POST",
        headers: this.#controlHeaders(),
        body: JSON.stringify({ schema_version: 1, shared_recording_id: sharedRecordingId }),
      }),
    );
  }

  async stopFieldRecording(): Promise<FieldRecordingNodeStatus> {
    return this.#parseFieldRecordingStatus(
      await this.#request("/api/v1/field-recording/stop", {
        method: "POST",
        headers: this.#controlHeaders(),
        body: "{}",
      }),
    );
  }

  async fieldRecordings(): Promise<FieldRecording[]> {
    const object = asObject(
      await this.#request("/api/v1/field-recordings", {
        headers: this.#controlHeaders(false),
      }),
      "field recordings",
    );
    if (object.schema_version !== 1 || !Array.isArray(object.recordings)) {
      throw new Error("Unsupported field recordings response");
    }
    return object.recordings.map((recording) => this.#parseFieldRecording(recording));
  }

  async triggerReport(): Promise<NodeTriggerReport | null> {
    let value: unknown;
    try {
      value = await this.#request("/api/v1/capture/trigger-report");
    } catch (caught) {
      if (caught instanceof NodeRequestError && caught.status === 404) {
        return null;
      }
      throw caught;
    }
    const object = asObject(value, "trigger report");
    if (object.schema_version !== 1) {
      throw new Error(`Unsupported trigger report schema: ${String(object.schema_version)}`);
    }
    const role = asRole(object.role, "trigger role");
    if (role !== this.#canonicalRole) {
      throw new Error(`Configured ${this.#canonicalRole} node reported trigger role ${role}`);
    }
    return {
      role,
      nodeId: asNonemptyString(object.node_id, "trigger node_id"),
      sharedSessionId: asNonemptyString(object.shared_session_id, "shared_session_id"),
      localSessionId: asNonemptyString(object.local_session_id, "local_session_id"),
      triggerTimestampNs: asNonnegativeBigInt(
        object.trigger_elapsed_realtime_ns,
        "trigger_elapsed_realtime_ns",
      ),
      timestampUncertaintyNs: asNonnegativeBigInt(
        object.timestamp_uncertainty_ns,
        "timestamp_uncertainty_ns",
      ),
      source: asNonemptyString(object.source, "trigger source"),
    };
  }

  async coordinationRecord(sharedSessionId: string): Promise<DualNodeAlignment | null> {
    try {
      const record = parseStoredAlignment(
        await this.#request(`/api/v1/coordination/${encodeURIComponent(sharedSessionId)}`, {
          headers: this.#controlHeaders(false),
        }),
      );
      if (record.shared_session_id !== sharedSessionId) {
        throw new Error("Android coordination evidence belongs to another shared session");
      }
      return record;
    } catch (caught) {
      if (caught instanceof NodeRequestError && caught.status === 404) {
        return null;
      }
      throw caught;
    }
  }

  async storeCoordinationRecord(alignment: DualNodeAlignment): Promise<void> {
    const stored = parseStoredAlignment(
      await this.#request(
        `/api/v1/coordination/${encodeURIComponent(alignment.shared_session_id)}`,
        {
          method: "POST",
          headers: this.#controlHeaders(true),
          body: JSON.stringify(alignment),
        },
      ),
    );
    if (!sameAlignment(stored, alignment)) {
      throw new Error(
        `Android node retained conflicting evidence for ${alignment.shared_session_id}`,
      );
    }
  }

  async clockEstimate(sampleCount: number): Promise<ClockOffsetEstimate> {
    const descriptor = await this.descriptor();
    const samples: ClockExchangeSample[] = [];
    for (let index = 0; index < sampleCount; ++index) {
      const coordinatorSendNs = this.#now();
      const value = asObject(await this.#request("/api/v1/clock"), "clock response");
      const coordinatorReceiveNs = this.#now();
      if (value.schema_version !== 1 || value.node_id !== descriptor.nodeId) {
        throw new Error("Clock response belongs to another Android node");
      }
      samples.push({
        coordinatorSendNs,
        nodeReceiveNs: asNonnegativeBigInt(
          value.request_received_elapsed_realtime_ns,
          "clock request receive timestamp",
        ),
        nodeSendNs: asNonnegativeBigInt(
          value.response_prepared_elapsed_realtime_ns,
          "clock response prepare timestamp",
        ),
        coordinatorReceiveNs,
      });
    }
    return estimateClockOffset(descriptor.nodeId, samples);
  }

  async #request(path: string, init: RequestInit = {}): Promise<unknown> {
    let response: Response;
    try {
      const headers = new Headers(init.headers ?? { Accept: "application/json" });
      // Every request stays on the endpoint's already-validated origin. Attach the existing
      // per-node credential by default so newly added read routes cannot accidentally depend on
      // anonymous metadata access. Explicit callers may add content headers, but cannot replace
      // this destination-bound authorization value.
      headers.set("Authorization", `Bearer ${controlCredentialValue(this.#controlCredential)}`);
      response = await this.#fetcher(`${this.baseUrl}${path}`, {
        ...init,
        headers,
      });
    } catch (caught) {
      throw new NodeRequestError(
        0,
        `${nodeRoleLabel(this.#canonicalRole)} Android node is unreachable: ${errorMessage(caught)}`,
      );
    }
    if (!response.ok) {
      const detail = await nodeErrorDetail(response);
      throw new NodeRequestError(
        response.status,
        `${nodeRoleLabel(this.#canonicalRole)} Android node request failed (HTTP ${response.status})${detail === null ? "" : `: ${detail}`}${this.#credentialCorrection(response.status)}`,
      );
    }
    return response.json();
  }

  async #reviewOperation<T>(operation: () => Promise<T>): Promise<T> {
    try {
      return await operation();
    } catch (caught) {
      const message = errorMessage(caught);
      const detail = message.startsWith("Review request failed")
        ? message
            .slice("Review ".length)
            .replace(/^request failed \(([0-9]+)\)/, "request failed (HTTP $1)")
        : `review request failed: ${message}`;
      throw new Error(
        `${nodeRoleLabel(this.#canonicalRole)} Android node ${detail}${this.#credentialCorrection(message.includes("(401) ") || message.includes("(401):") ? 401 : 0)}`,
      );
    }
  }

  #credentialCorrection(status: number): string {
    return status === 401
      ? ` Re-enter the ${nodeRoleLabel(this.#canonicalRole).toLowerCase()} control credential for this station.`
      : "";
  }

  #parseFieldRecordingStatus(value: unknown): FieldRecordingNodeStatus {
    const object = asObject(value, "field recording status");
    if (object.schema_version !== 1) {
      throw new Error(`Unsupported field recording schema: ${String(object.schema_version)}`);
    }
    const state = asString(object.state, "field recording state");
    if (!(FIELD_RECORDING_STATES as readonly string[]).includes(state)) {
      throw new Error(`Unsupported field recording state: ${state}`);
    }
    return {
      schema_version: 1,
      role: this.#canonicalRole,
      origin: this.baseUrl,
      state: state as FieldRecordingNodeStatus["state"],
      active_recording_id: asNullableString(object.active_recording_id, "active_recording_id"),
      shared_recording_id: asNullableString(object.shared_recording_id, "shared_recording_id"),
      started_at_utc: asNullableTimestamp(object.started_at_utc, "started_at_utc"),
      started_elapsed_realtime_ns: asNullableDecimalString(
        object.started_elapsed_realtime_ns,
        "started_elapsed_realtime_ns",
      ),
      elapsed_ms: asNonnegativeNumber(object.elapsed_ms, "elapsed_ms"),
      video_bytes: asDecimalString(object.video_bytes, "video_bytes"),
      audio_frames: asDecimalString(object.audio_frames, "audio_frames"),
      max_duration_seconds: asPositiveInteger(object.max_duration_seconds, "max_duration_seconds"),
      error: asString(object.error, "field recording error"),
    };
  }

  #parseFieldRecording(value: unknown): FieldRecording {
    const object = asObject(value, "field recording");
    const role = asRole(object.role, "field recording role");
    if (role !== this.#canonicalRole) {
      throw new Error(`Configured ${this.#canonicalRole} node returned a ${role} field recording`);
    }
    return {
      recording_id: asNonemptyString(object.recording_id, "recording_id"),
      shared_recording_id: asNonemptyString(object.shared_recording_id, "shared_recording_id"),
      created_at_utc: asTimestamp(object.created_at_utc, "created_at_utc"),
      role,
      origin: this.baseUrl,
      duration_us: asDecimalString(object.duration_us, "duration_us"),
      video_bytes: asDecimalString(object.video_bytes, "video_bytes"),
      audio_frames: asDecimalString(object.audio_frames, "audio_frames"),
      video_url: this.#absoluteUrl(object.video_url, "video_url"),
      audio_url: this.#absoluteUrl(object.audio_url, "audio_url"),
      manifest_url: this.#absoluteUrl(object.manifest_url, "manifest_url"),
    };
  }

  #absoluteUrl(value: unknown, label: string): string {
    const path = asNonemptyString(value, label);
    const url = new URL(path, `${this.baseUrl}/`);
    if (url.origin !== new URL(this.baseUrl).origin) {
      throw new Error(`${label} must remain on its Android node origin`);
    }
    return url.toString();
  }

  #controlHeaders(withJsonBody = true): Headers {
    const headers = new Headers({ Accept: "application/json" });
    if (withJsonBody) {
      headers.set("Content-Type", "application/json");
    }
    headers.set("Authorization", `Bearer ${controlCredentialValue(this.#controlCredential)}`);
    return headers;
  }
}

class BoundedRequestGate {
  #active = 0;
  readonly #waiting: Array<() => void> = [];

  constructor(readonly maximumActive: number) {
    if (!Number.isSafeInteger(maximumActive) || maximumActive <= 0) {
      throw new Error("A request gate requires a positive safe-integer capacity");
    }
  }

  async run<T>(operation: () => Promise<T>): Promise<T> {
    await this.#acquire();
    try {
      return await operation();
    } finally {
      this.#release();
    }
  }

  #acquire(): Promise<void> {
    if (this.#active < this.maximumActive) {
      ++this.#active;
      return Promise.resolve();
    }
    return new Promise<void>((resolve) => this.#waiting.push(resolve));
  }

  #release(): void {
    const next = this.#waiting.shift();
    if (next !== undefined) {
      next();
      return;
    }
    --this.#active;
  }
}

class NodeRequestError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

async function nodeErrorDetail(response: Response): Promise<string | null> {
  try {
    const value: unknown = await response.clone().json();
    if (typeof value !== "object" || value === null || !("error" in value)) {
      return null;
    }
    const error = value.error;
    return typeof error === "string" && error.length > 0 ? error : null;
  } catch {
    return null;
  }
}

function nodeRoleLabel(role: ReviewRole): string {
  return role === "down_the_line" ? "Down-the-line" : "Face-on";
}

export function estimateClockOffset(
  nodeId: string,
  samples: readonly ClockExchangeSample[],
): ClockOffsetEstimate {
  if (nodeId.length === 0 || samples.length < 3 || samples.length > 64) {
    throw new Error("Clock estimation requires a node ID and 3 to 64 samples");
  }
  let lowerBound: bigint | null = null;
  let upperBound: bigint | null = null;
  let minimumRoundTrip: bigint | null = null;
  let maximumRoundTrip = 0n;
  let previousReceive: bigint | null = null;
  for (const sample of samples) {
    if (
      sample.coordinatorSendNs < 0n ||
      sample.nodeReceiveNs < 0n ||
      sample.nodeSendNs < sample.nodeReceiveNs ||
      sample.coordinatorReceiveNs < sample.coordinatorSendNs ||
      (previousReceive !== null && sample.coordinatorReceiveNs <= previousReceive)
    ) {
      throw new Error("Clock exchange timestamps are invalid or not ordered");
    }
    const roundTrip =
      sample.coordinatorReceiveNs -
      sample.coordinatorSendNs -
      (sample.nodeSendNs - sample.nodeReceiveNs);
    if (roundTrip < 0n) {
      throw new Error("Clock exchange node processing exceeds total exchange time");
    }
    const sampleLower = sample.nodeSendNs - sample.coordinatorReceiveNs;
    const sampleUpper = sample.nodeReceiveNs - sample.coordinatorSendNs;
    lowerBound = lowerBound === null || sampleLower > lowerBound ? sampleLower : lowerBound;
    upperBound = upperBound === null || sampleUpper < upperBound ? sampleUpper : upperBound;
    minimumRoundTrip =
      minimumRoundTrip === null || roundTrip < minimumRoundTrip ? roundTrip : minimumRoundTrip;
    maximumRoundTrip = roundTrip > maximumRoundTrip ? roundTrip : maximumRoundTrip;
    previousReceive = sample.coordinatorReceiveNs;
  }
  if (lowerBound === null || upperBound === null || minimumRoundTrip === null) {
    throw new Error("Clock exchange evidence is empty");
  }
  if (lowerBound > upperBound) {
    throw new Error("Repeated clock-exchange offset bounds do not intersect");
  }
  const width = upperBound - lowerBound;
  return {
    nodeId,
    offsetNs: lowerBound + width / 2n,
    uncertaintyNs: width / 2n + (width % 2n),
    minimumRoundTripNs: minimumRoundTrip,
    maximumRoundTripNs: maximumRoundTrip,
    sampleCount: samples.length,
  };
}

export function associateDualTriggers(
  sharedSessionId: string,
  reports: readonly [NodeTriggerReport, NodeTriggerReport],
  estimates: readonly [ClockOffsetEstimate, ClockOffsetEstimate],
  recordedAtEpochMillis: number = Date.now(),
): DualNodeAlignment {
  if (sharedSessionId.length === 0) {
    throw new Error("Shared session ID is required for trigger association");
  }
  if (!Number.isSafeInteger(recordedAtEpochMillis) || recordedAtEpochMillis <= 0) {
    throw new Error("Coordination record time must be a positive epoch millisecond value");
  }
  const observed = reports.map((report) => {
    if (report.sharedSessionId !== sharedSessionId) {
      throw new Error("Trigger report belongs to another shared session");
    }
    const estimate = estimates.find((candidate) => candidate.nodeId === report.nodeId);
    if (estimate === undefined) {
      throw new Error(`No clock estimate belongs to trigger node ${report.nodeId}`);
    }
    const coordinatorUncertaintyNs = report.timestampUncertaintyNs + estimate.uncertaintyNs;
    if (coordinatorUncertaintyNs > MAXIMUM_REPORT_UNCERTAINTY_NS) {
      throw new Error(`Mapped ${report.role} trigger uncertainty exceeds 10 ms`);
    }
    return {
      report,
      estimate,
      coordinatorTimestampNs: report.triggerTimestampNs - estimate.offsetNs,
      coordinatorUncertaintyNs,
    };
  });
  const downTheLine = observed.filter((candidate) => candidate.report.role === "down_the_line");
  const faceOn = observed.filter((candidate) => candidate.report.role === "face_on");
  if (downTheLine.length !== 1 || faceOn.length !== 1) {
    throw new Error("Trigger association requires exactly one report for each camera role");
  }
  const down = downTheLine[0];
  const face = faceOn[0];
  if (down === undefined || face === undefined) {
    throw new Error("Trigger association lost a required camera role");
  }
  if (down.report.nodeId === face.report.nodeId) {
    throw new Error("One Android node cannot supply both trigger roles");
  }
  const pairUncertainty = down.coordinatorUncertaintyNs + face.coordinatorUncertaintyNs;
  if (pairUncertainty > MAXIMUM_PAIR_UNCERTAINTY_NS) {
    throw new Error("Combined dual-node trigger uncertainty exceeds 20 ms");
  }
  const centerSeparation = absolute(down.coordinatorTimestampNs - face.coordinatorTimestampNs);
  const minimumSeparation =
    centerSeparation > pairUncertainty ? centerSeparation - pairUncertainty : 0n;
  const maximumSeparation = centerSeparation + pairUncertainty;
  if (maximumSeparation > PAIRING_TOLERANCE_NS) {
    throw new Error("Dual-node trigger intervals cannot prove a match within 50 ms");
  }
  return {
    schema_version: 1,
    shared_session_id: sharedSessionId,
    status: "paired",
    recorded_at_epoch_ms: recordedAtEpochMillis.toString(),
    down_the_line: serializeObserved(down),
    face_on: serializeObserved(face),
    minimum_trigger_separation_ns: minimumSeparation.toString(),
    maximum_trigger_separation_ns: maximumSeparation.toString(),
  };
}

export function composeDualManifest(
  downTheLine: ClipManifest,
  faceOn: ClipManifest,
  alignment: DualNodeAlignment,
): ClipManifest {
  const downTrack = onlyTrack(downTheLine, "down_the_line");
  const faceTrack = onlyTrack(faceOn, "face_on");
  if (
    downTheLine.android_capture?.node_id !== alignment.down_the_line.node_id ||
    faceOn.android_capture?.node_id !== alignment.face_on.node_id
  ) {
    throw new Error("Dual manifest node identities disagree with the trigger association");
  }
  if (
    downTheLine.session_id !== alignment.down_the_line.local_session_id ||
    faceOn.session_id !== alignment.face_on.local_session_id
  ) {
    throw new Error("Dual manifest local session IDs disagree with the trigger association");
  }
  if (
    downTheLine.trigger.source !== alignment.down_the_line.source ||
    faceOn.trigger.source !== alignment.face_on.source
  ) {
    throw new Error("Dual manifest trigger sources disagree with the trigger association");
  }
  const coordinatedSource = coordinatedTriggerSource(
    downTheLine.trigger.source,
    faceOn.trigger.source,
  );
  const shifts = triggerShiftsFromCommon(alignment);
  const shiftedDown = shiftTrack(downTrack, shifts.downTheLine);
  const shiftedFace = shiftTrack(faceTrack, shifts.faceOn);
  const downImpact = shiftedDown.frames[shiftedDown.impact_frame_index];
  const faceImpact = shiftedFace.frames[shiftedFace.impact_frame_index];
  if (downImpact === undefined || faceImpact === undefined) {
    throw new Error("Dual manifest impact frame index is invalid");
  }
  return {
    schema_version: REVIEW_SCHEMA_VERSION,
    session_id: alignment.shared_session_id,
    created_at_utc:
      downTheLine.created_at_utc < faceOn.created_at_utc
        ? downTheLine.created_at_utc
        : faceOn.created_at_utc,
    trigger: {
      source: coordinatedSource,
      host_monotonic_time_ns: shifts.commonTrigger.toString(),
      confirmation_host_monotonic_time_ns: null,
      sample_rate_hz: 48_000,
      peak_amplitude: null,
      noise_floor: null,
      threshold: null,
    },
    mapped_nearest_frame_skew_us: Math.abs(
      downImpact.time_from_impact_us - faceImpact.time_from_impact_us,
    ),
    views: [shiftedDown, shiftedFace],
    dual_node_alignment: alignment,
  };
}

function coordinatedTriggerSource(downTheLine: string, faceOn: string): string {
  const audioSources = new Set([
    "local_audio",
    "peer_audio_arrival",
    "peer_audio_local_candidate",
    "peer_audio_clock_candidate",
  ]);
  if (audioSources.has(downTheLine) && audioSources.has(faceOn)) {
    return "dual_local_audio";
  }
  if (downTheLine === faceOn) {
    return downTheLine;
  }
  throw new Error("Dual manifest trigger sources disagree");
}

export function localizeDiagnosticFeedback(
  feedback: DiagnosticFeedback,
  localTriggerShiftFromCommonNs: bigint,
): DiagnosticFeedback {
  if (feedback.timing_marks_us === undefined) {
    return { ...feedback };
  }
  const triggerShiftUs = triggerShiftMicros(localTriggerShiftFromCommonNs);
  const localized: DiagnosticTimingMarks = {};
  const keys = ["desired_high_speed_start_us", "visual_impact_us", "audio_impact_us"] as const;
  for (const key of keys) {
    const commonRelativeUs = feedback.timing_marks_us[key];
    if (commonRelativeUs === undefined) {
      continue;
    }
    if (!Number.isSafeInteger(commonRelativeUs)) {
      throw new Error(`Diagnostic timing mark ${key} must be a safe integer`);
    }
    const localRelativeUs = commonRelativeUs - triggerShiftUs;
    if (!Number.isSafeInteger(localRelativeUs)) {
      throw new Error(`Localized diagnostic timing mark ${key} is outside the safe integer range`);
    }
    localized[key] = localRelativeUs;
  }
  return { ...feedback, timing_marks_us: localized };
}

function triggerShiftsFromCommon(alignment: DualNodeAlignment): {
  commonTrigger: bigint;
  downTheLine: bigint;
  faceOn: bigint;
} {
  const downTrigger = BigInt(alignment.down_the_line.mapped_coordinator_timestamp_ns);
  const faceTrigger = BigInt(alignment.face_on.mapped_coordinator_timestamp_ns);
  const commonTrigger = downTrigger + (faceTrigger - downTrigger) / 2n;
  return {
    commonTrigger,
    downTheLine: downTrigger - commonTrigger,
    faceOn: faceTrigger - commonTrigger,
  };
}

function serializeObserved(
  observed: ObservedNodeTrigger & { estimate: ClockOffsetEstimate },
): SerializedObservedTrigger {
  return {
    role: observed.report.role,
    node_id: observed.report.nodeId,
    local_session_id: observed.report.localSessionId,
    trigger_timestamp_ns: observed.report.triggerTimestampNs.toString(),
    trigger_uncertainty_ns: observed.report.timestampUncertaintyNs.toString(),
    mapped_coordinator_timestamp_ns: observed.coordinatorTimestampNs.toString(),
    mapped_coordinator_uncertainty_ns: observed.coordinatorUncertaintyNs.toString(),
    clock_offset_ns: observed.estimate.offsetNs.toString(),
    clock_uncertainty_ns: observed.estimate.uncertaintyNs.toString(),
    minimum_round_trip_ns: observed.estimate.minimumRoundTripNs.toString(),
    maximum_round_trip_ns: observed.estimate.maximumRoundTripNs.toString(),
    clock_sample_count: observed.estimate.sampleCount,
    source: observed.report.source,
  };
}

function shiftTrack(track: ClipTrack, triggerShiftNs: bigint): ClipTrack {
  const shiftUs = triggerShiftMicros(triggerShiftNs);
  const frames = track.frames.map((frame) => ({
    ...frame,
    time_from_impact_us: frame.time_from_impact_us + shiftUs,
  }));
  let impactFrameIndex = 0;
  for (let index = 1; index < frames.length; ++index) {
    const candidate = frames[index];
    const selected = frames[impactFrameIndex];
    if (candidate === undefined || selected === undefined) {
      throw new Error("Track frame metadata is incomplete");
    }
    if (Math.abs(candidate.time_from_impact_us) < Math.abs(selected.time_from_impact_us)) {
      impactFrameIndex = index;
    }
  }
  return { ...track, impact_frame_index: impactFrameIndex, frames };
}

function triggerShiftMicros(triggerShiftNs: bigint): number {
  const shiftUs = Number(triggerShiftNs / 1_000n);
  if (!Number.isSafeInteger(shiftUs)) {
    throw new Error("Dual-node trigger shift cannot be represented in safe microseconds");
  }
  return shiftUs;
}

function onlyTrack(manifest: ClipManifest, role: ReviewRole): ClipTrack {
  const track = manifest.views[0];
  if (manifest.views.length !== 1 || track?.role !== role) {
    throw new Error(`${role} node manifest must contain exactly its configured role`);
  }
  return track;
}

function combineCaptureStatuses(
  statuses: readonly CaptureStatus[],
  error: string | null,
): CaptureStatus {
  if (statuses.length !== 2) {
    throw new Error("Dual-node capture requires exactly two status responses");
  }
  const state = error === null ? combinedState(statuses.map((status) => status.state)) : "error";
  const activeIds = statuses
    .map((status) => status.active_session_id)
    .filter((value): value is string => value !== null);
  const pose = combinedPoseCaptureStatus(
    statuses.flatMap((status) => (status.pose === undefined ? [] : [status.pose])),
  );
  return {
    schema_version: CAPTURE_SCHEMA_VERSION,
    state,
    armed: statuses.every((status) => status.armed),
    active_session_id: activeIds[0] ?? null,
    error:
      error ?? statuses.map((status) => status.error).find((message) => message.length > 0) ?? "",
    hil: {
      enabled: false,
      busy: false,
      stage: "idle",
      error: "",
      last_run: null,
    },
    ...(pose === undefined ? {} : { pose }),
  };
}

function consistentSharedSessionId(
  details: readonly DetailedCaptureStatus[],
  allowOneMissingDuringPublication = false,
): string | null {
  if (details.length !== 2) {
    throw new Error("Dual-node capture requires exactly two status responses");
  }
  const first = details[0]?.sharedSessionId;
  const second = details[1]?.sharedSessionId;
  if (first === undefined || second === undefined) {
    throw new Error("Dual-node capture status is incomplete");
  }
  if (
    first !== second &&
    (!allowOneMissingDuringPublication || (first !== null && second !== null))
  ) {
    throw new Error("Android nodes report different active shared session IDs");
  }
  return first ?? second;
}

function combinedPoseCaptureStatus(
  statuses: readonly PoseCaptureStatus[],
): PoseCaptureStatus | undefined {
  if (statuses.length === 0) {
    return undefined;
  }
  const peerArm = combinedPeerArmStatus(statuses.map((status) => status.peer_arm));
  if (peerArm === undefined) {
    return undefined;
  }
  const modes = statuses.map((status) => status.mode);
  const phases = statuses.map((status) => status.phase);
  return {
    mode: modes.includes("leader") ? "leader" : modes.includes("shadow") ? "shadow" : "disabled",
    phase: phases.includes("high_speed")
      ? "high_speed"
      : phases.includes("monitoring")
        ? "monitoring"
        : "idle",
    transition_requested: statuses.some((status) => status.transition_requested),
    peer_arm: peerArm,
  };
}

function combinedPeerArmStatus(statuses: readonly PeerArmStatus[]): PeerArmStatus | undefined {
  const precedence: readonly PeerArmStatus["state"][] = [
    "failed",
    "rejected",
    "pending",
    "accepted",
    "inbound_accepted",
    "not_requested",
  ];
  for (const state of precedence) {
    const status = statuses.find((candidate) => candidate.state === state);
    if (status !== undefined) {
      return structuredClone(status);
    }
  }
  return undefined;
}

function combinedState(states: readonly CaptureState[]): CaptureState {
  const precedence: readonly CaptureState[] = [
    "error",
    "encoding",
    "waiting_post_roll",
    "arming",
    "armed",
    "ready",
    "setup",
  ];
  return precedence.find((candidate) => states.includes(candidate)) ?? "error";
}

function missingRoleDiagnostic(pair: {
  sharedSessionId: string;
  downTheLine?: unknown;
  faceOn?: unknown;
}): string {
  if (pair.downTheLine === undefined && pair.faceOn === undefined) {
    return `Coordinated session ${pair.sharedSessionId} has no published clips`;
  }
  return pair.downTheLine === undefined
    ? `Coordinated session ${pair.sharedSessionId} is missing the down-the-line clip`
    : `Coordinated session ${pair.sharedSessionId} is missing the face-on clip`;
}

function requiredAndroidNodeId(manifest: ClipManifest, roleLabel: string): string {
  const nodeId = manifest.android_capture?.node_id;
  if (nodeId === undefined) {
    throw new Error(`Dual Android ${roleLabel} diagnostics require a retained node ID`);
  }
  return nodeId;
}

function throwRejectedNodeOperation<T>(
  results: readonly PromiseSettledResult<T>[],
  operation: string,
): void {
  const failure = rejectedNodeOperation(results, operation);
  if (failure !== null) {
    throw failure;
  }
}

function rejectedNodeOperation<T>(
  results: readonly PromiseSettledResult<T>[],
  operation: string,
): Error | null {
  const failures = results.filter(
    (result): result is PromiseRejectedResult => result.status === "rejected",
  );
  if (failures.length === 0) {
    return null;
  }
  return new Error(
    `Unable to ${operation} both Android nodes: ${failures.map((failure) => errorMessage(failure.reason)).join("; ")}`,
  );
}

function combinedOperationFailure(
  primary: unknown,
  rollback: Error | null,
  convergence: unknown | null,
): Error {
  const diagnostics = [
    errorMessage(primary),
    ...(rollback === null ? [] : [`rollback request failed: ${rollback.message}`]),
    ...(convergence === null ? [] : [`rollback did not converge: ${errorMessage(convergence)}`]),
  ];
  return new Error(diagnostics.join("; "));
}

function fieldRecordingStatusPair(
  statuses: readonly FieldRecordingNodeStatus[],
): [FieldRecordingNodeStatus, FieldRecordingNodeStatus] {
  const first = statuses[0];
  const second = statuses[1];
  if (statuses.length !== 2 || first === undefined || second === undefined) {
    throw new Error("Dual-node field recording requires exactly two status responses");
  }
  return [first, second];
}

function consistentFieldRecordingId(statuses: readonly FieldRecordingNodeStatus[]): string | null {
  fieldRecordingStatusPair(statuses);
  const identifiers = new Set(
    statuses.flatMap((status) =>
      status.shared_recording_id === null ? [] : [status.shared_recording_id],
    ),
  );
  if (identifiers.size > 1) {
    throw new Error("Android nodes report different active field-recording IDs");
  }
  return identifiers.values().next().value ?? null;
}

function fulfilled<T>(result: PromiseSettledResult<T>): T {
  if (result.status === "rejected") {
    throw result.reason;
  }
  return result.value;
}

function parseStoredAlignment(value: unknown): DualNodeAlignment {
  const object = asObject(value, "stored dual-node alignment");
  if (object.schema_version !== 1 || object.status !== "paired") {
    throw new Error("Unsupported stored dual-node alignment");
  }
  const down = parseSerializedObserved(object.down_the_line, "down_the_line");
  const face = parseSerializedObserved(object.face_on, "face_on");
  const alignment: DualNodeAlignment = {
    schema_version: 1,
    shared_session_id: asNonemptyString(object.shared_session_id, "shared_session_id"),
    status: "paired",
    recorded_at_epoch_ms: asDecimalString(object.recorded_at_epoch_ms, "recorded_at_epoch_ms"),
    down_the_line: down,
    face_on: face,
    minimum_trigger_separation_ns: asDecimalString(
      object.minimum_trigger_separation_ns,
      "minimum_trigger_separation_ns",
    ),
    maximum_trigger_separation_ns: asDecimalString(
      object.maximum_trigger_separation_ns,
      "maximum_trigger_separation_ns",
    ),
  };
  validateStoredAlignment(alignment);
  return alignment;
}

function sameAlignment(left: DualNodeAlignment, right: DualNodeAlignment): boolean {
  return JSON.stringify(left) === JSON.stringify(right);
}

function parseSerializedObserved(value: unknown, role: ReviewRole): SerializedObservedTrigger {
  const object = asObject(value, `stored ${role} trigger`);
  if (object.role !== role) {
    throw new Error(`Stored trigger role is not ${role}`);
  }
  return {
    role,
    node_id: asNonemptyString(object.node_id, "node_id"),
    local_session_id: asNonemptyString(object.local_session_id, "local_session_id"),
    trigger_timestamp_ns: asDecimalString(object.trigger_timestamp_ns, "trigger_timestamp_ns"),
    trigger_uncertainty_ns: asDecimalString(
      object.trigger_uncertainty_ns,
      "trigger_uncertainty_ns",
    ),
    mapped_coordinator_timestamp_ns: asDecimalString(
      object.mapped_coordinator_timestamp_ns,
      "mapped_coordinator_timestamp_ns",
    ),
    mapped_coordinator_uncertainty_ns: asDecimalString(
      object.mapped_coordinator_uncertainty_ns,
      "mapped_coordinator_uncertainty_ns",
    ),
    clock_offset_ns: asSignedDecimalString(object.clock_offset_ns, "clock_offset_ns"),
    clock_uncertainty_ns: asDecimalString(object.clock_uncertainty_ns, "clock_uncertainty_ns"),
    minimum_round_trip_ns: asDecimalString(object.minimum_round_trip_ns, "minimum_round_trip_ns"),
    maximum_round_trip_ns: asDecimalString(object.maximum_round_trip_ns, "maximum_round_trip_ns"),
    clock_sample_count: asPositiveInteger(object.clock_sample_count, "clock_sample_count"),
    source: asNonemptyString(object.source, "source"),
  };
}

function validateStoredAlignment(alignment: DualNodeAlignment): void {
  const recordedAt = BigInt(alignment.recorded_at_epoch_ms);
  if (recordedAt <= 0n || alignment.shared_session_id.length > 128) {
    throw new Error("Stored coordination identity or record time is invalid");
  }
  if (alignment.down_the_line.node_id === alignment.face_on.node_id) {
    throw new Error("Stored coordination evidence reuses one node for both roles");
  }
  for (const observed of [alignment.down_the_line, alignment.face_on]) {
    const trigger = BigInt(observed.trigger_timestamp_ns);
    const triggerUncertainty = BigInt(observed.trigger_uncertainty_ns);
    const offset = BigInt(observed.clock_offset_ns);
    const clockUncertainty = BigInt(observed.clock_uncertainty_ns);
    const mapped = BigInt(observed.mapped_coordinator_timestamp_ns);
    const mappedUncertainty = BigInt(observed.mapped_coordinator_uncertainty_ns);
    const minimumRoundTrip = BigInt(observed.minimum_round_trip_ns);
    const maximumRoundTrip = BigInt(observed.maximum_round_trip_ns);
    if (
      trigger <= 0n ||
      mapped < 0n ||
      mapped !== trigger - offset ||
      mappedUncertainty !== triggerUncertainty + clockUncertainty ||
      maximumRoundTrip < minimumRoundTrip ||
      observed.clock_sample_count > 64
    ) {
      throw new Error(`Stored ${observed.role} timing evidence is inconsistent`);
    }
  }
  const down = alignment.down_the_line;
  const face = alignment.face_on;
  const center = absolute(
    BigInt(down.mapped_coordinator_timestamp_ns) - BigInt(face.mapped_coordinator_timestamp_ns),
  );
  const uncertainty =
    BigInt(down.mapped_coordinator_uncertainty_ns) + BigInt(face.mapped_coordinator_uncertainty_ns);
  const expectedMinimum = center > uncertainty ? center - uncertainty : 0n;
  if (
    BigInt(alignment.minimum_trigger_separation_ns) !== expectedMinimum ||
    BigInt(alignment.maximum_trigger_separation_ns) !== center + uncertainty
  ) {
    throw new Error("Stored trigger-separation bounds are inconsistent");
  }
}

function emptyCaptureStatus(): CaptureStatus {
  return {
    schema_version: CAPTURE_SCHEMA_VERSION,
    state: "setup",
    armed: false,
    active_session_id: null,
    error: "",
    hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
  };
}

function browserMonotonicNow(): bigint {
  return BigInt(Math.floor(performance.now() * 1_000_000));
}

function newSharedSessionId(): string {
  const random = new Uint8Array(12);
  globalThis.crypto.getRandomValues(random);
  return `dual-${Date.now()}-${[...random].map((value) => value.toString(16).padStart(2, "0")).join("")}`;
}

function newFieldRecordingId(): string {
  const random = new Uint8Array(16);
  globalThis.crypto.getRandomValues(random);
  random[6] = ((random[6] ?? 0) & 0x0f) | 0x40;
  random[8] = ((random[8] ?? 0) & 0x3f) | 0x80;
  const hex = [...random].map((value) => value.toString(16).padStart(2, "0")).join("");
  return `field-${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function browserStorage(): AlignmentStorage | null {
  try {
    return globalThis.localStorage;
  } catch {
    return null;
  }
}

function roleOrder(role: ReviewRole): number {
  return role === "down_the_line" ? 0 : 1;
}

function absolute(value: bigint): bigint {
  return value < 0n ? -value : value;
}

function asObject(value: unknown, label: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

function asString(value: unknown, label: string): string {
  if (typeof value !== "string") {
    throw new Error(`${label} must be a string`);
  }
  return value;
}

function asNonemptyString(value: unknown, label: string): string {
  const result = asString(value, label);
  if (result.length === 0) {
    throw new Error(`${label} cannot be empty`);
  }
  return result;
}

function asNullableString(value: unknown, label: string): string | null {
  return value === null || value === undefined ? null : asNonemptyString(value, label);
}

function asNullableTimestamp(value: unknown, label: string): string | null {
  return value === null ? null : asTimestamp(value, label);
}

function asRole(value: unknown, label: string): ReviewRole {
  if (value !== "down_the_line" && value !== "face_on") {
    throw new Error(`${label} must be down_the_line or face_on`);
  }
  return value;
}

function asCaptureState(value: unknown, label: string): CaptureState {
  if (
    value !== "setup" &&
    value !== "arming" &&
    value !== "armed" &&
    value !== "waiting_post_roll" &&
    value !== "encoding" &&
    value !== "ready" &&
    value !== "error"
  ) {
    throw new Error(`${label} is invalid`);
  }
  return value;
}

function asTimestamp(value: unknown, label: string): string {
  const result = asNonemptyString(value, label);
  if (!Number.isFinite(Date.parse(result))) {
    throw new Error(`${label} must be an ISO timestamp`);
  }
  return result;
}

function asNonnegativeBigInt(value: unknown, label: string): bigint {
  const result = BigInt(asDecimalString(value, label));
  if (result < 0n) {
    throw new Error(`${label} cannot be negative`);
  }
  return result;
}

function asDecimalString(value: unknown, label: string): string {
  const result = asString(value, label);
  if (!/^\d+$/.test(result)) {
    throw new Error(`${label} must be a nonnegative decimal string`);
  }
  return result;
}

function asNullableDecimalString(value: unknown, label: string): string | null {
  return value === null ? null : asDecimalString(value, label);
}

function asNonnegativeNumber(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) {
    throw new Error(`${label} must be a nonnegative number`);
  }
  return value;
}

function asSignedDecimalString(value: unknown, label: string): string {
  const result = asString(value, label);
  if (!/^-?\d+$/.test(result)) {
    throw new Error(`${label} must be a decimal string`);
  }
  return result;
}

function asPositiveInteger(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`${label} must be a positive integer`);
  }
  return value;
}

function errorMessage(caught: unknown): string {
  return caught instanceof Error ? caught.message : String(caught);
}
