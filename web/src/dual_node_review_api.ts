import {
  CAPTURE_SCHEMA_VERSION,
  type CaptureState,
  type CaptureStatus,
  type ClipManifest,
  type ClipTrack,
  type DiagnosticArchive,
  type DiagnosticFeedback,
  type DiagnosticTimingMarks,
  HttpReviewApi,
  parseCaptureStatus,
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

export interface DualNodeEndpoint {
  baseUrl: string;
  controlToken: string;
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
  readonly #nodes: readonly [AndroidNodeClient, AndroidNodeClient];
  readonly #sessionIdFactory: SessionIdFactory;
  readonly #storage: AlignmentStorage | null;
  readonly #manifestCache = new Map<string, Promise<ClipManifest>>();
  readonly #alignmentCache = new Map<string, DualNodeAlignment>();
  readonly #durablyReplicated = new Set<string>();
  #descriptors: Promise<readonly [NodeDescriptor, NodeDescriptor]> | null = null;
  #activeSharedSessionId: string | null = null;
  #coordinationError: string | null = null;
  #coordinationAttempt: Promise<void> | null = null;

  constructor(
    endpoints: readonly [DualNodeEndpoint, DualNodeEndpoint],
    fetcher: Fetcher = globalThis.fetch.bind(globalThis),
    now: MonotonicNow = browserMonotonicNow,
    sessionIdFactory: SessionIdFactory = newSharedSessionId,
    storage: AlignmentStorage | null = browserStorage(),
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
  }

  async getCaptureStatus(): Promise<CaptureStatus> {
    await this.#ensureDescriptors();
    const details = await Promise.all(this.#nodes.map((node) => node.captureStatus()));
    const sharedIds = new Set(
      details
        .map((detail) => detail.sharedSessionId)
        .filter((value): value is string => value !== null),
    );
    if (sharedIds.size > 1) {
      throw new Error("Android nodes report different active shared session IDs");
    }
    const reportedSharedId = [...sharedIds][0] ?? null;
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
    await this.#ensureDescriptors();
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
    if (this.#activeSharedSessionId === null) {
      throw new Error("Both Android nodes must be armed before saving a missed shot");
    }
    const results = await Promise.allSettled(this.#nodes.map((node) => node.saveMissedShot()));
    throwRejectedNodeOperation(results, "save missed shot");
    return {
      session_id: this.#activeSharedSessionId,
      state: "waiting_post_roll",
      created_at_utc: new Date().toISOString(),
      error: "",
    };
  }

  startSyntheticSwing(): Promise<CaptureStatus> {
    return Promise.reject(
      new Error("The Android dual-node coordinator does not expose the host synthetic HIL action"),
    );
  }

  async getSessions(): Promise<SessionList> {
    await this.#ensureDescriptors();
    const pairs = await this.#sessionPairs();
    await Promise.all(
      [...pairs.values()]
        .filter((pair) => pair.error === undefined)
        .map((pair) => this.#ensureAlignment(pair.sharedSessionId)),
    );
    const sessions = [...pairs.values()].map((pair) => this.#sessionSummary(pair));
    sessions.sort((left, right) => right.created_at_utc.localeCompare(left.created_at_utc));
    return { schema_version: REVIEW_SCHEMA_VERSION, sessions };
  }

  async getManifest(sessionId: string): Promise<ClipManifest> {
    const pairs = await this.#sessionPairs();
    const pair = pairs.get(sessionId);
    if (pair === undefined) {
      throw new Error(`Coordinated Android session ${sessionId} was not found`);
    }
    if (pair.error !== undefined) {
      throw new Error(pair.error);
    }
    if (pair.downTheLine === undefined || pair.faceOn === undefined) {
      throw new Error(missingRoleDiagnostic(pair));
    }
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
    await Promise.all([
      this.#nodes[0].submitDiagnosticFeedback(
        pair.downTheLine.session_id,
        localizeDiagnosticFeedback(feedback, shifts.downTheLine),
      ),
      this.#nodes[1].submitDiagnosticFeedback(
        pair.faceOn.session_id,
        localizeDiagnosticFeedback(feedback, shifts.faceOn),
      ),
    ]);
  }

  async getDiagnosticArchives(sessionId: string): Promise<readonly DiagnosticArchive[]> {
    const pair = await this.#completeSessionPair(sessionId);
    const [downTheLine, faceOn] = await Promise.all([
      this.#nodes[0].getDiagnosticArchives(pair.downTheLine.session_id),
      this.#nodes[1].getDiagnosticArchives(pair.faceOn.session_id),
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
    const pair = (await this.#sessionPairs()).get(sessionId);
    if (pair === undefined) {
      throw new Error(`Coordinated Android session ${sessionId} was not found`);
    }
    if (pair.error !== undefined) {
      throw new Error(pair.error);
    }
    if (pair.downTheLine === undefined || pair.faceOn === undefined) {
      throw new Error(missingRoleDiagnostic(pair));
    }
    return { ...pair, downTheLine: pair.downTheLine, faceOn: pair.faceOn };
  }

  async #ensureDescriptors(): Promise<readonly [NodeDescriptor, NodeDescriptor]> {
    const descriptors =
      this.#descriptors ??
      Promise.all([this.#nodes[0].descriptor(), this.#nodes[1].descriptor()]).then(
        ([downTheLine, faceOn]) => {
          if (downTheLine.role !== "down_the_line" || faceOn.role !== "face_on") {
            throw new Error(
              "Phone role assignments do not match the configured down-the-line/face-on endpoints",
            );
          }
          if (downTheLine.nodeId === faceOn.nodeId) {
            throw new Error("One Android installation cannot supply both camera roles");
          }
          return [downTheLine, faceOn] as const;
        },
      );
    this.#descriptors = descriptors;
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

  async #sessionPairs(): Promise<Map<string, SessionPair>> {
    const lists = await Promise.all(this.#nodes.map((node) => node.sessions()));
    const manifests = await Promise.all(
      lists.flatMap((list, nodeIndex) => {
        const node = this.#nodes[nodeIndex];
        if (node === undefined) {
          throw new Error("Session list does not belong to a configured Android node");
        }
        return list.sessions
          .filter((session) => session.state === "ready")
          .map((session) => this.#manifest(node, session.session_id));
      }),
    );
    return groupAndroidSessionManifests(manifests);
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

  #sessionSummary(pair: SessionPair): SessionSummary {
    const manifests = [pair.downTheLine, pair.faceOn].filter(
      (manifest): manifest is ClipManifest => manifest !== undefined,
    );
    const createdAt =
      manifests
        .map((manifest) => manifest.created_at_utc)
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
      return {
        session_id: pair.sharedSessionId,
        state: "error",
        created_at_utc: createdAt,
        error: missingRoleDiagnostic(pair),
      };
    }
    if (this.#alignment(pair.sharedSessionId) === null) {
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

class AndroidNodeClient {
  readonly baseUrl: string;
  readonly #role: ReviewRole;
  readonly #controlToken: string;
  readonly #fetcher: Fetcher;
  readonly #now: MonotonicNow;
  readonly #reviewApi: HttpReviewApi;

  constructor(endpoint: DualNodeEndpoint, fetcher: Fetcher, now: MonotonicNow) {
    this.baseUrl = endpoint.baseUrl.replace(/\/$/, "");
    this.#role = endpoint.role;
    this.#controlToken = endpoint.controlToken;
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

  async captureStatus(): Promise<DetailedCaptureStatus> {
    const value = await this.#request("/api/v1/capture/status");
    const object = asObject(value, "Android capture status");
    return {
      status: parseCaptureStatus(value),
      sharedSessionId: asNullableString(object.shared_session_id, "shared_session_id"),
    };
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

  saveMissedShot(): Promise<SessionSummary> {
    return this.#reviewApi.saveMissedShot();
  }

  submitDiagnosticFeedback(sessionId: string, feedback: DiagnosticFeedback): Promise<void> {
    return this.#reviewApi.submitDiagnosticFeedback(sessionId, feedback);
  }

  getDiagnosticArchives(sessionId: string): Promise<readonly DiagnosticArchive[]> {
    return this.#reviewApi.getDiagnosticArchives(sessionId);
  }

  sessions(): Promise<SessionList> {
    return this.#reviewApi.getSessions();
  }

  manifest(sessionId: string): Promise<ClipManifest> {
    return this.#reviewApi.getManifest(sessionId);
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
    if (role !== this.#role) {
      throw new Error(`Configured ${this.#role} node reported trigger role ${role}`);
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
    const response = await this.#fetcher(`${this.baseUrl}${path}`, {
      ...init,
      headers: new Headers(init.headers ?? { Accept: "application/json" }),
    });
    if (!response.ok) {
      throw new NodeRequestError(
        response.status,
        `${this.#role} node request failed: ${response.status}`,
      );
    }
    return response.json();
  }

  #controlHeaders(withJsonBody = true): Headers {
    const headers = new Headers({ Accept: "application/json" });
    if (withJsonBody) {
      headers.set("Content-Type", "application/json");
    }
    headers.set("Authorization", `Bearer ${this.#controlToken}`);
    return headers;
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
  if (downTheLine.trigger.source !== faceOn.trigger.source) {
    throw new Error("Dual manifest trigger sources disagree");
  }
  if (
    downTheLine.trigger.source !== alignment.down_the_line.source ||
    faceOn.trigger.source !== alignment.face_on.source
  ) {
    throw new Error("Dual manifest trigger sources disagree with the trigger association");
  }
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
      source: downTheLine.trigger.source === "missed_shot" ? "missed_shot" : "dual_local_audio",
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
  };
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

function missingRoleDiagnostic(pair: SessionPair): string {
  if (pair.downTheLine === undefined && pair.faceOn === undefined) {
    return `Coordinated session ${pair.sharedSessionId} has no published clips`;
  }
  return pair.downTheLine === undefined
    ? `Coordinated session ${pair.sharedSessionId} is missing the down-the-line clip`
    : `Coordinated session ${pair.sharedSessionId} is missing the face-on clip`;
}

function throwRejectedNodeOperation<T>(
  results: readonly PromiseSettledResult<T>[],
  operation: string,
): void {
  const failures = results.filter(
    (result): result is PromiseRejectedResult => result.status === "rejected",
  );
  if (failures.length > 0) {
    throw new Error(
      `Unable to ${operation} both Android nodes: ${failures.map((failure) => errorMessage(failure.reason)).join("; ")}`,
    );
  }
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
