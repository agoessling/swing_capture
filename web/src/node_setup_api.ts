import { MutableControlCredential } from "./control_credential.js";
import { parseOperationalHealth, type OperationalHealth } from "./operational_health.js";

export const NODE_SETUP_SCHEMA_VERSION = 1 as const;

export interface SetupChoice {
  value: string;
  label: string;
}

export interface CaptureProfileChoice extends SetupChoice {
  width: number;
  height: number;
  fps: number;
}

export interface HittingRegion {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

export interface NodeSetupSnapshot {
  schema_version: typeof NODE_SETUP_SCHEMA_VERSION;
  revision: number;
  node: {
    node_id: string;
    control_credential_generation: number;
    service_urls: string[];
    device_model: string;
  };
  capabilities: {
    roles: SetupChoice[];
    capture_profiles: CaptureProfileChoice[];
    pose_modes: SetupChoice[];
    inference_delegates: SetupChoice[];
  };
  configuration: {
    role: string;
    capture_profile: string;
    pose: {
      mode: string;
      inference_delegate: string;
      debug_evidence_enabled: boolean;
      hitting_region: HittingRegion;
      peer: null | { origin: string };
    };
  };
  readiness: {
    editable: boolean;
    capture_state: string;
    issues: string[];
  };
  operational_health?: OperationalHealth;
  preview: {
    available: boolean;
    url: string | null;
    state?: "available" | "stale" | "unavailable";
    reason?: string | null;
    frame_age_ms?: number | null;
    image_rotation_degrees?: 0 | 90 | 180 | 270;
  };
  pairing?: null | {
    state: "active" | "revoked";
    peer_node_id: string;
    expected_role: string;
    label: string;
    origin: string;
    credential_generation: number;
    credential_status: "verified" | "re_pair_required" | "unavailable" | "revoked";
    verified_at_epoch_ms: number;
    revoked_at_epoch_ms: number;
  };
}

export interface NodeDiscoveryHint {
  schema_version: 1;
  service_instance: string;
  node_id: string;
  role: string;
  label: string;
  origin: string;
  observed_at_epoch_ms: number;
  expires_at_epoch_ms: number;
  trusted: false;
}

export type PeerSetupUpdate =
  | { operation: "keep" }
  | { operation: "clear" }
  | { operation: "replace"; origin: string; control_token: string };

export interface NodeSetupConfigurationUpdate {
  role: string;
  capture_profile: string;
  pose: {
    mode: string;
    inference_delegate: string;
    debug_evidence_enabled: boolean;
    hitting_region: HittingRegion;
    peer_update: PeerSetupUpdate;
  };
}

export interface NodeSetupApi {
  readonly displayOrigin: string;
  getSetup(): Promise<NodeSetupSnapshot>;
  getSetupPreview?(url: string): Promise<Blob>;
  updateSetup(
    expectedRevision: number,
    configuration: NodeSetupConfigurationUpdate,
  ): Promise<NodeSetupSnapshot>;
  discoverNodes?(): Promise<readonly NodeDiscoveryHint[]>;
  connectDiscovered?(
    observation: NodeDiscoveryHint,
    controlToken: string,
  ): Promise<{ api: NodeSetupApi; setup: NodeSetupSnapshot }>;
  resetPairing?(expectedRevision: number, peerNodeId: string): Promise<NodeSetupSnapshot>;
  rotateControlCredential?(
    expectedRevision: number,
    confirmedNodeId: string,
  ): Promise<ControlCredentialRotationResult>;
}

export interface ControlCredentialRotationResult {
  schema_version: typeof NODE_SETUP_SCHEMA_VERSION;
  node_id: string;
  setup_revision: number;
  control_credential_generation: number;
  control_token: string;
  remote_peer_bindings_require_re_pair: true;
}

type Fetcher = typeof fetch;

export class HttpNodeSetupApi implements NodeSetupApi {
  readonly displayOrigin: string;
  readonly #baseUrl: string;
  readonly #controlCredential: MutableControlCredential;
  readonly #fetcher: Fetcher;

  constructor(
    baseUrl = "",
    controlCredential: string | MutableControlCredential = "",
    fetcher: Fetcher = globalThis.fetch.bind(globalThis),
  ) {
    this.#baseUrl = baseUrl.replace(/\/$/, "");
    this.displayOrigin = this.#baseUrl || globalThis.location?.origin || "This phone";
    this.#controlCredential =
      typeof controlCredential === "string"
        ? new MutableControlCredential(controlCredential)
        : controlCredential;
    this.#fetcher = fetcher;
  }

  async getSetup(): Promise<NodeSetupSnapshot> {
    const response = await this.#fetcher(`${this.#baseUrl}/api/v1/setup`, {
      headers: this.#headers(),
    });
    return this.#parseResponse(response);
  }

  async getSetupPreview(url: string): Promise<Blob> {
    const endpoint = this.#authenticatedEndpoint(url);
    const response = await this.#fetcher(endpoint, {
      cache: "no-store",
      headers: { ...this.#headers(), Accept: "image/jpeg" },
      redirect: "error",
    });
    if (!response.ok) {
      throw await setupRequestError(response);
    }
    const preview = await response.blob();
    if (preview.size === 0 || preview.type !== "image/jpeg") {
      const responseKind =
        preview.size === 0 ? "an empty response" : preview.type || "an unknown content type";
      throw new Error(`Setup preview returned ${responseKind}`);
    }
    return preview;
  }

  async updateSetup(
    expectedRevision: number,
    configuration: NodeSetupConfigurationUpdate,
  ): Promise<NodeSetupSnapshot> {
    const response = await this.#fetcher(`${this.#baseUrl}/api/v1/setup`, {
      method: "PUT",
      headers: {
        ...this.#headers(),
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        schema_version: NODE_SETUP_SCHEMA_VERSION,
        expected_revision: expectedRevision,
        configuration,
      }),
    });
    return this.#parseResponse(response);
  }

  async discoverNodes(): Promise<readonly NodeDiscoveryHint[]> {
    const response = await this.#fetcher(`${this.#baseUrl}/api/v1/discovery`, {
      headers: this.#headers(),
    });
    if (!response.ok) {
      throw await setupRequestError(response);
    }
    return parseNodeDiscoveryHints(await response.json());
  }

  async connectDiscovered(
    observation: NodeDiscoveryHint,
    controlToken: string,
  ): Promise<{ api: NodeSetupApi; setup: NodeSetupSnapshot }> {
    const api = new HttpNodeSetupApi(observation.origin, controlToken, this.#fetcher);
    return { api, setup: await api.getSetup() };
  }

  async resetPairing(expectedRevision: number, peerNodeId: string): Promise<NodeSetupSnapshot> {
    const response = await this.#fetcher(`${this.#baseUrl}/api/v1/pairing/reset`, {
      method: "POST",
      headers: { ...this.#headers(), "Content-Type": "application/json" },
      body: JSON.stringify({
        schema_version: NODE_SETUP_SCHEMA_VERSION,
        expected_revision: expectedRevision,
        peer_node_id: peerNodeId,
      }),
    });
    return this.#parseResponse(response);
  }

  async rotateControlCredential(
    expectedRevision: number,
    confirmedNodeId: string,
  ): Promise<ControlCredentialRotationResult> {
    const response = await this.#fetcher(`${this.#baseUrl}/api/v1/control-credential/rotate`, {
      method: "POST",
      headers: { ...this.#headers(), "Content-Type": "application/json" },
      body: JSON.stringify({
        schema_version: NODE_SETUP_SCHEMA_VERSION,
        expected_revision: expectedRevision,
        confirmed_node_id: confirmedNodeId,
      }),
    });
    if (!response.ok) {
      throw await setupRequestError(response);
    }
    const rotation = parseControlCredentialRotation(await response.json());
    this.#controlCredential.replace(rotation.control_token);
    return rotation;
  }

  #headers(): Record<string, string> {
    return {
      Accept: "application/json",
      ...(this.#controlCredential.current().length === 0
        ? {}
        : { Authorization: `Bearer ${this.#controlCredential.current()}` }),
    };
  }

  #authenticatedEndpoint(url: string): string {
    const trustedBase = this.#baseUrl || globalThis.location?.origin;
    if (trustedBase === undefined || trustedBase.length === 0) {
      throw new Error("Cannot resolve an authenticated setup preview without a phone origin");
    }
    const trustedOrigin = new URL(trustedBase).origin;
    const endpoint = new URL(url, `${trustedOrigin}/`);
    if (endpoint.origin !== trustedOrigin) {
      throw new Error("Refusing to send a node control credential to another origin");
    }
    return endpoint.toString();
  }

  async #parseResponse(response: Response): Promise<NodeSetupSnapshot> {
    if (!response.ok) {
      throw await setupRequestError(response);
    }
    const setup = parseNodeSetupSnapshot(await response.json());
    if (setup.preview.url !== null && this.#baseUrl.length > 0) {
      setup.preview.url = new URL(setup.preview.url, `${this.#baseUrl}/`).toString();
    }
    return setup;
  }
}

export function parseNodeSetupSnapshot(value: unknown): NodeSetupSnapshot {
  const object = asObject(value, "node setup");
  if (object.schema_version !== NODE_SETUP_SCHEMA_VERSION) {
    throw new Error(`Unsupported node setup schema: ${String(object.schema_version)}`);
  }
  const node = asObject(object.node, "node setup node");
  const capabilities = asObject(object.capabilities, "node setup capabilities");
  const configuration = asObject(object.configuration, "node setup configuration");
  const pose = asObject(configuration.pose, "node setup pose configuration");
  const readiness = asObject(object.readiness, "node setup readiness");
  const preview = asObject(object.preview, "node setup preview");
  const pairing = parseServerPairing(object.pairing);
  const operationalHealth = parseOperationalHealth(object.operational_health);
  const peer = pose.peer;
  const parsedPeer =
    peer === null
      ? null
      : { origin: asNonemptyString(asObject(peer, "node setup peer").origin, "peer origin") };
  const available = asBoolean(preview.available, "preview available");
  const previewUrl = preview.url;
  if (previewUrl !== null && typeof previewUrl !== "string") {
    throw new Error("preview url must be a string or null");
  }
  if (available !== (previewUrl !== null)) {
    throw new Error("preview availability must agree with preview url");
  }
  const previewState = preview.state ?? (available ? "available" : "unavailable");
  if (previewState !== "available" && previewState !== "stale" && previewState !== "unavailable") {
    throw new Error("preview state must be available, stale, or unavailable");
  }
  if (available !== (previewState === "available")) {
    throw new Error("preview availability must agree with preview state");
  }
  const previewReason = nullableString(preview.reason, "preview reason");
  const frameAgeMs = nullableNonnegativeNumber(preview.frame_age_ms, "preview frame age");
  const imageRotationDegrees = preview.image_rotation_degrees ?? 0;
  if (
    imageRotationDegrees !== 0 &&
    imageRotationDegrees !== 90 &&
    imageRotationDegrees !== 180 &&
    imageRotationDegrees !== 270
  ) {
    throw new Error("preview image rotation must be 0, 90, 180, or 270 degrees");
  }
  return {
    schema_version: NODE_SETUP_SCHEMA_VERSION,
    revision: asNonnegativeInteger(object.revision, "setup revision"),
    node: {
      node_id: asNonemptyString(node.node_id, "node id"),
      control_credential_generation: asPositiveInteger(
        node.control_credential_generation,
        "control credential generation",
      ),
      service_urls: parseStringArray(node.service_urls, "node service urls"),
      device_model: asNonemptyString(node.device_model, "device model"),
    },
    capabilities: {
      roles: parseChoices(capabilities.roles, "roles"),
      capture_profiles: parseCaptureProfiles(capabilities.capture_profiles),
      pose_modes: parseChoices(capabilities.pose_modes, "pose modes"),
      inference_delegates: parseChoices(capabilities.inference_delegates, "inference delegates"),
    },
    configuration: {
      role: asNonemptyString(configuration.role, "configured role"),
      capture_profile: asNonemptyString(
        configuration.capture_profile,
        "configured capture profile",
      ),
      pose: {
        mode: asNonemptyString(pose.mode, "configured pose mode"),
        inference_delegate: asNonemptyString(
          pose.inference_delegate,
          "configured inference delegate",
        ),
        debug_evidence_enabled: asBoolean(pose.debug_evidence_enabled, "debug evidence enabled"),
        hitting_region: parseHittingRegion(pose.hitting_region),
        peer: parsedPeer,
      },
    },
    readiness: {
      editable: asBoolean(readiness.editable, "setup editable"),
      capture_state: asNonemptyString(readiness.capture_state, "capture state"),
      issues: parseStringArray(readiness.issues, "readiness issues"),
    },
    preview: {
      available,
      url: previewUrl,
      state: previewState,
      reason: previewReason,
      frame_age_ms: frameAgeMs,
      image_rotation_degrees: imageRotationDegrees,
    },
    pairing,
    ...(operationalHealth === undefined ? {} : { operational_health: operationalHealth }),
  };
}

export function parseNodeDiscoveryHints(value: unknown): readonly NodeDiscoveryHint[] {
  const object = asObject(value, "node discovery");
  if (object.schema_version !== NODE_SETUP_SCHEMA_VERSION) {
    throw new Error(`Unsupported node discovery schema: ${String(object.schema_version)}`);
  }
  if (object.authentication_required_for_pairing !== true) {
    throw new Error("Node discovery must require authenticated pairing");
  }
  if (!Array.isArray(object.observations)) {
    throw new Error("node discovery observations must be an array");
  }
  return object.observations.map((value, index) => {
    const observation = asObject(value, `node discovery observation ${index}`);
    if (observation.trusted !== false) {
      throw new Error("LAN discovery metadata must be marked untrusted");
    }
    return {
      schema_version: NODE_SETUP_SCHEMA_VERSION,
      service_instance: asNonemptyString(observation.service_instance, "service instance"),
      node_id: asNonemptyString(observation.node_id, "advertised node ID"),
      role: asNonemptyString(observation.role, "advertised role"),
      label: asNonemptyString(observation.label, "advertised label"),
      origin: asNonemptyString(observation.origin, "discovered origin"),
      observed_at_epoch_ms: asNonnegativeInteger(observation.observed_at_epoch_ms, "observed at"),
      expires_at_epoch_ms: asNonnegativeInteger(observation.expires_at_epoch_ms, "expires at"),
      trusted: false,
    };
  });
}

function parseServerPairing(value: unknown): Exclude<NodeSetupSnapshot["pairing"], undefined> {
  if (value === undefined || value === null) {
    return null;
  }
  const pairing = asObject(value, "node pairing");
  const state = pairing.state;
  if (state !== "active" && state !== "revoked") {
    throw new Error("node pairing state must be active or revoked");
  }
  const credentialStatus = pairing.credential_status;
  if (
    credentialStatus !== "verified" &&
    credentialStatus !== "re_pair_required" &&
    credentialStatus !== "unavailable" &&
    credentialStatus !== "revoked"
  ) {
    throw new Error("node pairing credential status is unsupported");
  }
  return {
    state,
    peer_node_id: asNonemptyString(pairing.peer_node_id, "paired node ID"),
    expected_role: asNonemptyString(pairing.expected_role, "paired role"),
    label: asNonemptyString(pairing.label, "paired label"),
    origin: asNonemptyString(pairing.origin, "paired origin"),
    credential_generation: asPositiveInteger(
      pairing.credential_generation,
      "pairing credential generation",
    ),
    credential_status: credentialStatus,
    verified_at_epoch_ms: asNonnegativeInteger(
      pairing.verified_at_epoch_ms,
      "pairing verification time",
    ),
    revoked_at_epoch_ms: asNonnegativeInteger(
      pairing.revoked_at_epoch_ms,
      "pairing revocation time",
    ),
  };
}

function parseControlCredentialRotation(value: unknown): ControlCredentialRotationResult {
  const object = asObject(value, "control credential rotation");
  if (object.schema_version !== NODE_SETUP_SCHEMA_VERSION) {
    throw new Error(
      `Unsupported control credential rotation schema: ${String(object.schema_version)}`,
    );
  }
  const controlToken = asNonemptyString(object.control_token, "new control token");
  if (!/^[A-Za-z0-9_-]{32}$/.test(controlToken)) {
    throw new Error("new control token has an invalid format");
  }
  if (object.remote_peer_bindings_require_re_pair !== true) {
    throw new Error("control credential rotation must require remote peer re-pairing");
  }
  return {
    schema_version: NODE_SETUP_SCHEMA_VERSION,
    node_id: asNonemptyString(object.node_id, "rotated node ID"),
    setup_revision: asNonnegativeInteger(object.setup_revision, "rotated setup revision"),
    control_credential_generation: asPositiveInteger(
      object.control_credential_generation,
      "rotated control credential generation",
    ),
    control_token: controlToken,
    remote_peer_bindings_require_re_pair: true,
  };
}

function parseChoices(value: unknown, label: string): SetupChoice[] {
  if (!Array.isArray(value) || value.length === 0) {
    throw new Error(`${label} must be a non-empty array`);
  }
  return value.map((entry, index) => {
    const choice = asObject(entry, `${label}[${index}]`);
    return {
      value: asNonemptyString(choice.value, `${label} value`),
      label: asNonemptyString(choice.label, `${label} label`),
    };
  });
}

function parseCaptureProfiles(value: unknown): CaptureProfileChoice[] {
  return parseChoices(value, "capture profiles").map((choice, index) => {
    const source = asObject((value as unknown[])[index], `capture profiles[${index}]`);
    return {
      ...choice,
      width: asPositiveInteger(source.width, "capture profile width"),
      height: asPositiveInteger(source.height, "capture profile height"),
      fps: asPositiveInteger(source.fps, "capture profile fps"),
    };
  });
}

function parseHittingRegion(value: unknown): HittingRegion {
  const region = asObject(value, "hitting region");
  const parsed = {
    left: asFiniteNumber(region.left, "hitting region left"),
    top: asFiniteNumber(region.top, "hitting region top"),
    right: asFiniteNumber(region.right, "hitting region right"),
    bottom: asFiniteNumber(region.bottom, "hitting region bottom"),
  };
  if (
    parsed.left < 0 ||
    parsed.top < 0 ||
    parsed.right > 1 ||
    parsed.bottom > 1 ||
    parsed.left >= parsed.right ||
    parsed.top >= parsed.bottom
  ) {
    throw new Error("hitting region must be an ordered rectangle within normalized image bounds");
  }
  return parsed;
}

async function setupRequestError(response: Response): Promise<Error> {
  let detail = "";
  try {
    const body = asObject(await response.json(), "node setup error");
    detail = typeof body.error === "string" ? body.error : "";
  } catch {
    // Preserve status-only errors returned by a proxy.
  }
  return new Error(
    `Node setup request failed (${response.status})${detail.length === 0 ? "" : `: ${detail}`}`,
  );
}

function parseStringArray(value: unknown, label: string): string[] {
  if (!Array.isArray(value) || value.some((entry) => typeof entry !== "string")) {
    throw new Error(`${label} must be an array of strings`);
  }
  return value as string[];
}

function asObject(value: unknown, label: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

function asNonemptyString(value: unknown, label: string): string {
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new Error(`${label} must be a non-empty string`);
  }
  return value;
}

function nullableString(value: unknown, label: string): string | null {
  return value === undefined || value === null ? null : asNonemptyString(value, label);
}

function nullableNonnegativeNumber(value: unknown, label: string): number | null {
  if (value === undefined || value === null) {
    return null;
  }
  const number = asFiniteNumber(value, label);
  if (number < 0) {
    throw new Error(`${label} must be nonnegative`);
  }
  return number;
}

function asBoolean(value: unknown, label: string): boolean {
  if (typeof value !== "boolean") {
    throw new Error(`${label} must be a boolean`);
  }
  return value;
}

function asFiniteNumber(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new Error(`${label} must be a finite number`);
  }
  return value;
}

function asNonnegativeInteger(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) {
    throw new Error(`${label} must be a nonnegative integer`);
  }
  return value;
}

function asPositiveInteger(value: unknown, label: string): number {
  const integer = asNonnegativeInteger(value, label);
  if (integer === 0) {
    throw new Error(`${label} must be positive`);
  }
  return integer;
}
