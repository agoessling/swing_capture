import type { NodeSetupSnapshot } from "./node_setup_api.js";

export const NODE_DISCOVERY_SCHEMA_VERSION = 1 as const;
export const NODE_PAIRING_SCHEMA_VERSION = 1 as const;

export type PairingRole = "down_the_line" | "face_on" | "unassigned";

/**
 * One untrusted service observation supplied by Android NSD/mDNS or another LAN discovery
 * adapter. Advertised identity fields are hints and must never be treated as authentication.
 */
export interface NodeDiscoveryObservation {
  schema_version: typeof NODE_DISCOVERY_SCHEMA_VERSION;
  service_instance: string;
  origin: string;
  advertised_node_id: string | null;
  advertised_role: PairingRole | null;
  advertised_label: string;
  observed_at_epoch_ms: number;
  expires_at_epoch_ms: number;
}

export interface NodeDiscoverySource {
  snapshot(): Promise<readonly NodeDiscoveryObservation[]>;
  subscribe?(listener: () => void): () => void;
}

export type DiscoveryAssessmentState = "available" | "stale" | "invalid" | "identity_conflict";

export interface DiscoveryAssessment {
  observation: NodeDiscoveryObservation;
  state: DiscoveryAssessmentState;
  detail: string;
}

/** Identity returned through an authenticated node setup connection. */
export interface AuthenticatedNodeIdentity {
  node_id: string;
  role: PairingRole;
  origin: string;
  label: string;
}

export interface VerifiedPairingCandidate extends AuthenticatedNodeIdentity {
  discovery_service_instance: string | null;
  discovery_expires_at_epoch_ms: number | null;
}

export type PairingBindingAction = "pair" | "rotate_credential" | "recover_address" | "re_pair";

export type PairingCredentialState = "active" | "revoked";

/**
 * Persisted pairing metadata. A control credential is deliberately absent: the credential remains
 * write-only and is sent only in the authenticated setup mutation that activates a binding.
 */
export interface PeerPairingBinding {
  schema_version: typeof NODE_PAIRING_SCHEMA_VERSION;
  binding_revision: number;
  leader_node_id: string;
  peer_node_id: string;
  peer_role: Exclude<PairingRole, "unassigned">;
  peer_origin: string;
  peer_label: string;
  credential_generation: number;
  credential_state: PairingCredentialState;
  verified_at_epoch_ms: number;
  revoked_at_epoch_ms: number | null;
}

export interface PairingBindingStore {
  get(leaderNodeId: string): PeerPairingBinding | null;
  set(binding: PeerPairingBinding): void;
  remove(leaderNodeId: string): void;
}

export class MemoryPairingBindingStore implements PairingBindingStore {
  readonly #bindings = new Map<string, PeerPairingBinding>();

  get(leaderNodeId: string): PeerPairingBinding | null {
    const binding = this.#bindings.get(leaderNodeId);
    return binding === undefined ? null : structuredClone(binding);
  }

  set(binding: PeerPairingBinding): void {
    this.#bindings.set(binding.leader_node_id, structuredClone(binding));
  }

  remove(leaderNodeId: string): void {
    this.#bindings.delete(leaderNodeId);
  }
}

export class LocalPairingBindingStore implements PairingBindingStore {
  readonly #storage: Storage;
  readonly #prefix: string;

  constructor(storage: Storage, prefix = "swing-capture.peer-binding.") {
    this.#storage = storage;
    this.#prefix = prefix;
  }

  get(leaderNodeId: string): PeerPairingBinding | null {
    const serialized = this.#storage.getItem(this.#key(leaderNodeId));
    if (serialized === null) {
      return null;
    }
    try {
      return parsePeerPairingBinding(JSON.parse(serialized));
    } catch {
      return null;
    }
  }

  set(binding: PeerPairingBinding): void {
    this.#storage.setItem(this.#key(binding.leader_node_id), JSON.stringify(binding));
  }

  remove(leaderNodeId: string): void {
    this.#storage.removeItem(this.#key(leaderNodeId));
  }

  #key(leaderNodeId: string): string {
    return `${this.#prefix}${leaderNodeId}`;
  }
}

export function browserPairingBindingStore(): PairingBindingStore {
  try {
    const storage = globalThis.localStorage;
    return storage === undefined
      ? new MemoryPairingBindingStore()
      : new LocalPairingBindingStore(storage);
  } catch {
    return new MemoryPairingBindingStore();
  }
}

export function authenticatedIdentity(
  displayOrigin: string,
  setup: NodeSetupSnapshot,
): AuthenticatedNodeIdentity {
  return {
    node_id: nonempty(setup.node.node_id, "node ID"),
    role: pairingRole(setup.configuration.role, "node role"),
    origin: normalizeHttpOrigin(displayOrigin),
    label: humanLabel(setup.node.device_model),
  };
}

export function assessNodeDiscoveries(
  observations: readonly NodeDiscoveryObservation[],
  nowEpochMs: number,
): DiscoveryAssessment[] {
  requireEpoch(nowEpochMs, "current time");
  const normalized = observations.map((observation) => {
    try {
      validateDiscoveryObservation(observation);
      const stale = observation.expires_at_epoch_ms <= nowEpochMs;
      return {
        observation: structuredClone(observation),
        normalizedOrigin: normalizeHttpOrigin(observation.origin),
        state: stale ? ("stale" as const) : ("available" as const),
        detail: stale
          ? "Discovery expired; wait for a fresh advertisement before verifying this phone."
          : "Discovery is only an address hint; authenticate the phone before pairing.",
      };
    } catch (caught) {
      return {
        observation: structuredClone(observation),
        normalizedOrigin: null,
        state: "invalid" as const,
        detail: errorMessage(caught),
      };
    }
  });

  const activeByAdvertisedIdentity = new Map<string, Set<string>>();
  for (const entry of normalized) {
    const advertisedNodeId = entry.observation.advertised_node_id;
    if (
      entry.state !== "available" ||
      advertisedNodeId === null ||
      entry.normalizedOrigin === null
    ) {
      continue;
    }
    const origins = activeByAdvertisedIdentity.get(advertisedNodeId) ?? new Set<string>();
    origins.add(entry.normalizedOrigin);
    activeByAdvertisedIdentity.set(advertisedNodeId, origins);
  }

  return normalized.map(({ observation, state, detail }) => {
    const advertisedNodeId = observation.advertised_node_id;
    if (
      state === "available" &&
      advertisedNodeId !== null &&
      (activeByAdvertisedIdentity.get(advertisedNodeId)?.size ?? 0) > 1
    ) {
      return {
        observation,
        state: "identity_conflict",
        detail:
          "Multiple live addresses advertise the same node ID. Authenticate deliberately; do not auto-select either address.",
      };
    }
    return { observation, state, detail };
  });
}

export function verifyDiscoveredCandidate(
  local: AuthenticatedNodeIdentity,
  observation: NodeDiscoveryObservation,
  authenticated: AuthenticatedNodeIdentity,
  nowEpochMs: number,
): VerifiedPairingCandidate {
  validateDiscoveryObservation(observation);
  requireEpoch(nowEpochMs, "current time");
  if (observation.expires_at_epoch_ms <= nowEpochMs) {
    throw new Error("Discovery expired before the phone was confirmed");
  }
  const observedOrigin = normalizeHttpOrigin(observation.origin);
  const authenticatedOrigin = normalizeHttpOrigin(authenticated.origin);
  if (observedOrigin !== authenticatedOrigin) {
    throw new Error("Authenticated response came from a different origin than the discovery");
  }
  if (
    observation.advertised_node_id !== null &&
    observation.advertised_node_id !== authenticated.node_id
  ) {
    throw new Error("Discovery node ID disagrees with the authenticated phone identity");
  }
  if (observation.advertised_role !== null && observation.advertised_role !== authenticated.role) {
    throw new Error("Discovery role disagrees with the authenticated phone role");
  }
  validatePairingIdentities(local, authenticated);
  return {
    ...authenticated,
    origin: authenticatedOrigin,
    label: humanLabel(authenticated.label),
    discovery_service_instance: observation.service_instance,
    discovery_expires_at_epoch_ms: observation.expires_at_epoch_ms,
  };
}

export function directlyVerifiedCandidate(
  local: AuthenticatedNodeIdentity,
  authenticated: AuthenticatedNodeIdentity,
): VerifiedPairingCandidate {
  validatePairingIdentities(local, authenticated);
  return {
    ...authenticated,
    origin: normalizeHttpOrigin(authenticated.origin),
    label: humanLabel(authenticated.label),
    discovery_service_instance: null,
    discovery_expires_at_epoch_ms: null,
  };
}

export function activatePairingBinding(
  existing: PeerPairingBinding | null,
  local: AuthenticatedNodeIdentity,
  peer: VerifiedPairingCandidate,
  action: PairingBindingAction,
  peerLabel: string,
  nowEpochMs: number,
): PeerPairingBinding {
  validatePairingIdentities(local, peer);
  requireEpoch(nowEpochMs, "verification time");
  const expectedAction = requiredPairingAction(existing, local, peer);
  if (action !== expectedAction) {
    throw new Error(`Pairing requires ${pairingActionLabel(expectedAction)} confirmation`);
  }
  return {
    schema_version: NODE_PAIRING_SCHEMA_VERSION,
    binding_revision: (existing?.binding_revision ?? 0) + 1,
    leader_node_id: local.node_id,
    peer_node_id: peer.node_id,
    peer_role: assignedRole(peer.role),
    peer_origin: normalizeHttpOrigin(peer.origin),
    peer_label: humanLabel(peerLabel),
    credential_generation: (existing?.credential_generation ?? 0) + 1,
    credential_state: "active",
    verified_at_epoch_ms: nowEpochMs,
    revoked_at_epoch_ms: null,
  };
}

export function requiredPairingAction(
  existing: PeerPairingBinding | null,
  local: AuthenticatedNodeIdentity,
  peer: VerifiedPairingCandidate,
): PairingBindingAction {
  validatePairingIdentities(local, peer);
  if (existing === null) {
    return "pair";
  }
  if (existing.leader_node_id !== local.node_id) {
    throw new Error("Stored pairing belongs to a different leader phone");
  }
  if (existing.credential_state === "revoked" || existing.peer_node_id !== peer.node_id) {
    return "re_pair";
  }
  return normalizeHttpOrigin(existing.peer_origin) === normalizeHttpOrigin(peer.origin)
    ? "rotate_credential"
    : "recover_address";
}

export function revokePairingBinding(
  existing: PeerPairingBinding,
  leaderNodeId: string,
  nowEpochMs: number,
): PeerPairingBinding {
  const binding = parsePeerPairingBinding(existing);
  requireEpoch(nowEpochMs, "revocation time");
  if (binding.leader_node_id !== leaderNodeId) {
    throw new Error("Cannot revoke a pairing owned by another leader phone");
  }
  if (binding.credential_state === "revoked") {
    throw new Error("Peer credential is already revoked");
  }
  return {
    ...binding,
    binding_revision: binding.binding_revision + 1,
    credential_state: "revoked",
    revoked_at_epoch_ms: nowEpochMs,
  };
}

/** Requiring the full peer node ID prevents an accidental reset of the wrong stable identity. */
export function resetPairingBinding(
  store: PairingBindingStore,
  binding: PeerPairingBinding,
  confirmedPeerNodeId: string,
): void {
  if (confirmedPeerNodeId !== binding.peer_node_id) {
    throw new Error("Pairing reset confirmation must match the full peer node ID");
  }
  store.remove(binding.leader_node_id);
}

export function pairingActionLabel(action: PairingBindingAction): string {
  switch (action) {
    case "rotate_credential":
      return "credential rotation";
    case "recover_address":
      return "address recovery";
    case "re_pair":
      return "identity re-pair";
    default:
      return "new pairing";
  }
}

export function parsePeerPairingBinding(value: unknown): PeerPairingBinding {
  const object = asObject(value, "peer pairing binding");
  if (object.schema_version !== NODE_PAIRING_SCHEMA_VERSION) {
    throw new Error(`Unsupported peer pairing schema: ${String(object.schema_version)}`);
  }
  const credentialState = object.credential_state;
  if (credentialState !== "active" && credentialState !== "revoked") {
    throw new Error("Pairing credential state must be active or revoked");
  }
  const revokedAt = object.revoked_at_epoch_ms;
  if (revokedAt !== null) {
    requireEpoch(revokedAt, "revocation time");
  }
  if ((credentialState === "revoked") !== (revokedAt !== null)) {
    throw new Error("Revoked pairing state must include exactly one revocation time");
  }
  return {
    schema_version: NODE_PAIRING_SCHEMA_VERSION,
    binding_revision: positiveInteger(object.binding_revision, "binding revision"),
    leader_node_id: nonempty(object.leader_node_id, "leader node ID"),
    peer_node_id: nonempty(object.peer_node_id, "peer node ID"),
    peer_role: assignedRole(pairingRole(object.peer_role, "peer role")),
    peer_origin: normalizeHttpOrigin(nonempty(object.peer_origin, "peer origin")),
    peer_label: humanLabel(nonempty(object.peer_label, "peer label")),
    credential_generation: positiveInteger(object.credential_generation, "credential generation"),
    credential_state: credentialState,
    verified_at_epoch_ms: epoch(object.verified_at_epoch_ms, "verification time"),
    revoked_at_epoch_ms: revokedAt,
  };
}

function validateDiscoveryObservation(observation: NodeDiscoveryObservation): void {
  if (observation.schema_version !== NODE_DISCOVERY_SCHEMA_VERSION) {
    throw new Error(`Unsupported node discovery schema: ${String(observation.schema_version)}`);
  }
  nonempty(observation.service_instance, "discovery service instance");
  normalizeHttpOrigin(observation.origin);
  if (observation.advertised_node_id !== null) {
    nonempty(observation.advertised_node_id, "advertised node ID");
  }
  if (observation.advertised_role !== null) {
    pairingRole(observation.advertised_role, "advertised role");
  }
  humanLabel(observation.advertised_label);
  requireEpoch(observation.observed_at_epoch_ms, "discovery observation time");
  requireEpoch(observation.expires_at_epoch_ms, "discovery expiration time");
  if (observation.expires_at_epoch_ms <= observation.observed_at_epoch_ms) {
    throw new Error("Discovery expiration must follow its observation time");
  }
}

function validatePairingIdentities(
  local: AuthenticatedNodeIdentity,
  peer: AuthenticatedNodeIdentity,
): void {
  const localNodeId = nonempty(local.node_id, "local node ID");
  const peerNodeId = nonempty(peer.node_id, "peer node ID");
  if (localNodeId === peerNodeId) {
    throw new Error("One stable node identity cannot be paired to itself or appear at two origins");
  }
  const localRole = assignedRole(pairingRole(local.role, "local role"));
  const peerRole = assignedRole(pairingRole(peer.role, "peer role"));
  if (localRole === peerRole) {
    throw new Error("Paired phones must have distinct camera roles");
  }
  normalizeHttpOrigin(local.origin);
  normalizeHttpOrigin(peer.origin);
  if (normalizeHttpOrigin(local.origin) === normalizeHttpOrigin(peer.origin)) {
    throw new Error("Two stable node identities cannot share one service origin");
  }
}

function normalizeHttpOrigin(value: string): string {
  let url: URL;
  try {
    url = new URL(value);
  } catch {
    throw new Error("Phone origin must be an absolute HTTP(S) origin");
  }
  if (
    (url.protocol !== "http:" && url.protocol !== "https:") ||
    url.username.length > 0 ||
    url.password.length > 0 ||
    url.pathname !== "/" ||
    url.search.length > 0 ||
    url.hash.length > 0
  ) {
    throw new Error("Phone origin must be an absolute HTTP(S) origin without credentials or path");
  }
  return url.origin;
}

function pairingRole(value: unknown, label: string): PairingRole {
  if (value !== "down_the_line" && value !== "face_on" && value !== "unassigned") {
    throw new Error(`${label} is unsupported`);
  }
  return value;
}

function assignedRole(role: PairingRole): Exclude<PairingRole, "unassigned"> {
  if (role === "unassigned") {
    throw new Error("Assign a camera role before pairing this phone");
  }
  return role;
}

function humanLabel(value: string): string {
  const label = nonempty(value, "phone label").trim();
  if (label.length > 64) {
    throw new Error("Phone label must be 64 characters or fewer");
  }
  return label;
}

function nonempty(value: unknown, label: string): string {
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new Error(`${label} must be a non-empty string`);
  }
  return value;
}

function requireEpoch(value: unknown, label: string): asserts value is number {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`${label} must be a positive epoch millisecond value`);
  }
}

function epoch(value: unknown, label: string): number {
  requireEpoch(value, label);
  return value;
}

function positiveInteger(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`${label} must be a positive integer`);
  }
  return value;
}

function asObject(value: unknown, label: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

function errorMessage(caught: unknown): string {
  return caught instanceof Error ? caught.message : "Invalid node discovery";
}
