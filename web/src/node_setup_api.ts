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
  preview: {
    available: boolean;
    url: string | null;
  };
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
  updateSetup(
    expectedRevision: number,
    configuration: NodeSetupConfigurationUpdate,
  ): Promise<NodeSetupSnapshot>;
}

type Fetcher = typeof fetch;

export class HttpNodeSetupApi implements NodeSetupApi {
  readonly displayOrigin: string;
  readonly #baseUrl: string;
  readonly #controlToken: string;
  readonly #fetcher: Fetcher;

  constructor(
    baseUrl = "",
    controlToken = "",
    fetcher: Fetcher = globalThis.fetch.bind(globalThis),
  ) {
    this.#baseUrl = baseUrl.replace(/\/$/, "");
    this.displayOrigin = this.#baseUrl || globalThis.location?.origin || "This phone";
    this.#controlToken = controlToken;
    this.#fetcher = fetcher;
  }

  async getSetup(): Promise<NodeSetupSnapshot> {
    const response = await this.#fetcher(`${this.#baseUrl}/api/v1/setup`, {
      headers: this.#headers(),
    });
    return this.#parseResponse(response);
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

  #headers(): Record<string, string> {
    return {
      Accept: "application/json",
      ...(this.#controlToken.length === 0 ? {} : { Authorization: `Bearer ${this.#controlToken}` }),
    };
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
  return {
    schema_version: NODE_SETUP_SCHEMA_VERSION,
    revision: asNonnegativeInteger(object.revision, "setup revision"),
    node: {
      node_id: asNonemptyString(node.node_id, "node id"),
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
    preview: { available, url: previewUrl },
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
