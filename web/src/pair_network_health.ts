export const PAIR_NETWORK_HEALTH_SCHEMA_VERSION = 1 as const;

export const PAIR_NETWORK_HEALTH_STATES = ["good", "degraded", "unusable"] as const;
export type PairNetworkHealthState = (typeof PAIR_NETWORK_HEALTH_STATES)[number];

export interface PairNetworkDirectionHealth {
  schema_version: typeof PAIR_NETWORK_HEALTH_SCHEMA_VERSION;
  attempts: number;
  successes: number;
  timeouts: number;
  round_trip_ns: string[];
  transfer_bytes: string;
  transfer_duration_ns: string;
  transfer_complete: boolean;
  minimum_round_trip_ns: string;
  median_round_trip_ns: string;
  p95_round_trip_ns: string;
  maximum_round_trip_ns: string;
  jitter_ns: string;
  transfer_bits_per_second: number;
}

export interface PairNetworkHealth {
  schema_version: typeof PAIR_NETWORK_HEALTH_SCHEMA_VERSION;
  configured: boolean;
  state: PairNetworkHealthState;
  raw_state: PairNetworkHealthState;
  measured: boolean;
  stale: boolean;
  transition_pending: boolean;
  age_ns: string;
  issues: string[];
  peer: null | { origin: string; node_id: string };
  measured_at_elapsed_realtime_ns: string | null;
  local_to_peer: PairNetworkDirectionHealth | null;
  peer_to_local: PairNetworkDirectionHealth | null;
}

export function parsePairNetworkHealth(value: unknown): PairNetworkHealth | undefined {
  if (value === undefined) {
    return undefined;
  }
  const object = exactObject(value, "pair network health", [
    "schema_version",
    "configured",
    "state",
    "raw_state",
    "measured",
    "stale",
    "transition_pending",
    "age_ns",
    "issues",
    "peer",
    "measured_at_elapsed_realtime_ns",
    "local_to_peer",
    "peer_to_local",
  ]);
  if (object.schema_version !== PAIR_NETWORK_HEALTH_SCHEMA_VERSION) {
    throw new Error(`Unsupported pair network-health schema: ${String(object.schema_version)}`);
  }
  const configured = strictBoolean(object.configured, "pair network health configured");
  const state = networkState(object.state, "pair network health state");
  const rawState = networkState(object.raw_state, "pair network health raw_state");
  const measured = strictBoolean(object.measured, "pair network health measured");
  const stale = strictBoolean(object.stale, "pair network health stale");
  const transitionPending = strictBoolean(
    object.transition_pending,
    "pair network health transition_pending",
  );
  const age = integerString(object.age_ns, "pair network health age_ns");
  const peer =
    object.peer === null
      ? null
      : (() => {
          const parsed = exactObject(object.peer, "pair network health peer", [
            "origin",
            "node_id",
          ]);
          return {
            origin: nonemptyString(parsed.origin, "pair network health peer origin"),
            node_id: nonemptyString(parsed.node_id, "pair network health peer node_id"),
          };
        })();
  if (configured !== (peer !== null)) {
    throw new Error("pair network health configured must agree with peer");
  }
  const measuredAt = nullableIntegerString(
    object.measured_at_elapsed_realtime_ns,
    "pair network health measured_at_elapsed_realtime_ns",
  );
  const localToPeer = parseDirection(object.local_to_peer, "pair network health local_to_peer");
  const peerToLocal = parseDirection(object.peer_to_local, "pair network health peer_to_local");
  if (measured !== (measuredAt !== null && localToPeer !== null && peerToLocal !== null)) {
    throw new Error("pair network health measured fields disagree");
  }
  if (!measured && (measuredAt !== null || localToPeer !== null || peerToLocal !== null)) {
    throw new Error("unmeasured pair network health must not contain direction evidence");
  }
  if (transitionPending !== (!stale && state !== rawState)) {
    throw new Error("pair network health transition_pending fields disagree");
  }
  if (stale && (!measured || state !== "unusable" || rawState !== "unusable")) {
    throw new Error("stale pair network health must be measured and unusable");
  }
  if (!measured && age !== "0") {
    throw new Error("unmeasured pair network health must have zero age");
  }
  if (!configured && (measured || state !== "unusable" || rawState !== "unusable")) {
    throw new Error("unconfigured pair network health must be unmeasured and unusable");
  }
  return {
    schema_version: PAIR_NETWORK_HEALTH_SCHEMA_VERSION,
    configured,
    state,
    raw_state: rawState,
    measured,
    stale,
    transition_pending: transitionPending,
    age_ns: age,
    issues: stringArray(object.issues, "pair network health issues"),
    peer,
    measured_at_elapsed_realtime_ns: measuredAt,
    local_to_peer: localToPeer,
    peer_to_local: peerToLocal,
  };
}

function parseDirection(value: unknown, label: string): PairNetworkDirectionHealth | null {
  if (value === null) {
    return null;
  }
  const object = exactObject(value, label, [
    "schema_version",
    "attempts",
    "successes",
    "timeouts",
    "round_trip_ns",
    "transfer_bytes",
    "transfer_duration_ns",
    "transfer_complete",
    "minimum_round_trip_ns",
    "median_round_trip_ns",
    "p95_round_trip_ns",
    "maximum_round_trip_ns",
    "jitter_ns",
    "transfer_bits_per_second",
  ]);
  if (object.schema_version !== PAIR_NETWORK_HEALTH_SCHEMA_VERSION) {
    throw new Error(`${label} has an unsupported schema`);
  }
  const attempts = positiveInteger(object.attempts, `${label} attempts`);
  const successes = nonnegativeInteger(object.successes, `${label} successes`);
  const timeouts = nonnegativeInteger(object.timeouts, `${label} timeouts`);
  if (successes > attempts || timeouts > attempts - successes) {
    throw new Error(`${label} outcome counts disagree`);
  }
  if (!Array.isArray(object.round_trip_ns)) {
    throw new Error(`${label} round_trip_ns must be an array`);
  }
  const roundTrips = object.round_trip_ns.map((sample, index) =>
    integerString(sample, `${label} round_trip_ns[${index}]`),
  );
  if (roundTrips.length !== successes) {
    throw new Error(`${label} round-trip samples must match successes`);
  }
  const sortedRoundTrips = roundTrips.map(BigInt);
  if (
    sortedRoundTrips.some(
      (sample, index) => index > 0 && sample < (sortedRoundTrips[index - 1] ?? sample),
    )
  ) {
    throw new Error(`${label} round-trip samples must be sorted`);
  }
  const transferBytes = integerString(object.transfer_bytes, `${label} transfer_bytes`);
  const transferDuration = integerString(
    object.transfer_duration_ns,
    `${label} transfer_duration_ns`,
  );
  const transferComplete = strictBoolean(object.transfer_complete, `${label} transfer_complete`);
  if (transferComplete && (transferBytes === "0" || transferDuration === "0")) {
    throw new Error(`${label} complete transfer must contain bytes and duration`);
  }
  const minimum = integerString(object.minimum_round_trip_ns, `${label} minimum_round_trip_ns`);
  const median = integerString(object.median_round_trip_ns, `${label} median_round_trip_ns`);
  const p95 = integerString(object.p95_round_trip_ns, `${label} p95_round_trip_ns`);
  const maximum = integerString(object.maximum_round_trip_ns, `${label} maximum_round_trip_ns`);
  const jitter = integerString(object.jitter_ns, `${label} jitter_ns`);
  const expectedMinimum = sortedRoundTrips[0] ?? 0n;
  const expectedMaximum = sortedRoundTrips.at(-1) ?? 0n;
  const percentile = (percent: number) =>
    sortedRoundTrips.length === 0
      ? 0n
      : (sortedRoundTrips[Math.max(0, Math.ceil((percent * sortedRoundTrips.length) / 100) - 1)] ??
        0n);
  if (
    BigInt(minimum) !== expectedMinimum ||
    BigInt(median) !== percentile(50) ||
    BigInt(p95) !== percentile(95) ||
    BigInt(maximum) !== expectedMaximum ||
    BigInt(jitter) !== expectedMaximum - expectedMinimum
  ) {
    throw new Error(`${label} derived latency metrics disagree with round_trip_ns`);
  }
  const transferBitsPerSecond = object.transfer_bits_per_second;
  if (
    typeof transferBitsPerSecond !== "number" ||
    !Number.isFinite(transferBitsPerSecond) ||
    transferBitsPerSecond < 0
  ) {
    throw new Error(`${label} transfer_bits_per_second must be a finite nonnegative number`);
  }
  return {
    schema_version: PAIR_NETWORK_HEALTH_SCHEMA_VERSION,
    attempts,
    successes,
    timeouts,
    round_trip_ns: roundTrips,
    transfer_bytes: transferBytes,
    transfer_duration_ns: transferDuration,
    transfer_complete: transferComplete,
    minimum_round_trip_ns: minimum,
    median_round_trip_ns: median,
    p95_round_trip_ns: p95,
    maximum_round_trip_ns: maximum,
    jitter_ns: jitter,
    transfer_bits_per_second: transferBitsPerSecond,
  };
}

function exactObject(
  value: unknown,
  label: string,
  fields: readonly string[],
): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  const object = value as Record<string, unknown>;
  const actual = Object.keys(object);
  if (actual.length !== fields.length || fields.some((field) => !(field in object))) {
    throw new Error(`${label} fields do not match schema`);
  }
  return object;
}

function networkState(value: unknown, label: string): PairNetworkHealthState {
  if (
    typeof value !== "string" ||
    !(PAIR_NETWORK_HEALTH_STATES as readonly string[]).includes(value)
  ) {
    throw new Error(`${label} is unsupported: ${String(value)}`);
  }
  return value as PairNetworkHealthState;
}

function strictBoolean(value: unknown, label: string): boolean {
  if (typeof value !== "boolean") {
    throw new Error(`${label} must be a boolean`);
  }
  return value;
}

function nonemptyString(value: unknown, label: string): string {
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new Error(`${label} must be a nonempty string`);
  }
  return value;
}

function integerString(value: unknown, label: string): string {
  if (typeof value !== "string" || !/^(?:0|[1-9][0-9]*)$/.test(value)) {
    throw new Error(`${label} must be a canonical nonnegative integer string`);
  }
  if (BigInt(value) > 9_223_372_036_854_775_807n) {
    throw new Error(`${label} is outside the signed 64-bit range`);
  }
  return value;
}

function nullableIntegerString(value: unknown, label: string): string | null {
  return value === null ? null : integerString(value, label);
}

function nonnegativeInteger(value: unknown, label: string): number {
  if (!Number.isSafeInteger(value) || (value as number) < 0) {
    throw new Error(`${label} must be a nonnegative integer`);
  }
  return value as number;
}

function positiveInteger(value: unknown, label: string): number {
  const parsed = nonnegativeInteger(value, label);
  if (parsed === 0) {
    throw new Error(`${label} must be positive`);
  }
  return parsed;
}

function stringArray(value: unknown, label: string): string[] {
  if (!Array.isArray(value) || value.some((entry) => typeof entry !== "string")) {
    throw new Error(`${label} must be an array of strings`);
  }
  return [...value] as string[];
}
