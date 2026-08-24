import assert from "node:assert/strict";
import { parsePairNetworkHealth, type PairNetworkHealth } from "./pair_network_health.js";

const direction = {
  schema_version: 1,
  attempts: 5,
  successes: 4,
  timeouts: 1,
  round_trip_ns: ["8000000", "10000000", "12000000", "16000000"],
  transfer_bytes: "262144",
  transfer_duration_ns: "100000000",
  transfer_complete: true,
  minimum_round_trip_ns: "8000000",
  median_round_trip_ns: "10000000",
  p95_round_trip_ns: "16000000",
  maximum_round_trip_ns: "16000000",
  jitter_ns: "8000000",
  transfer_bits_per_second: 20_971_520,
} as const;

const degraded = {
  schema_version: 1,
  configured: true,
  state: "degraded",
  raw_state: "degraded",
  measured: true,
  stale: false,
  transition_pending: false,
  age_ns: "2500000000",
  issues: ["Some phone-to-phone application requests failed or timed out."],
  peer: { origin: "http://pixel-5a.test:8088", node_id: "pixel-5a-node" },
  measured_at_elapsed_realtime_ns: "987654321000",
  local_to_peer: direction,
  peer_to_local: direction,
} as const;

assert.deepEqual(parsePairNetworkHealth(degraded), degraded);
assert.equal(parsePairNetworkHealth(undefined), undefined);

const unconfigured: PairNetworkHealth = {
  schema_version: 1,
  configured: false,
  state: "unusable",
  raw_state: "unusable",
  measured: false,
  stale: false,
  transition_pending: false,
  age_ns: "0",
  issues: ["A peer is not configured for network-health measurement."],
  peer: null,
  measured_at_elapsed_realtime_ns: null,
  local_to_peer: null,
  peer_to_local: null,
};
assert.deepEqual(parsePairNetworkHealth(unconfigured), unconfigured);

assert.throws(
  () => parsePairNetworkHealth({ ...degraded, unexpected: true }),
  /fields do not match schema/,
);
assert.throws(
  () => parsePairNetworkHealth({ ...degraded, configured: false }),
  /configured must agree with peer/,
);
assert.throws(
  () => parsePairNetworkHealth({ ...degraded, stale: true, measured: false }),
  /measured fields disagree/,
);
assert.throws(
  () =>
    parsePairNetworkHealth({
      ...degraded,
      local_to_peer: { ...direction, p95_round_trip_ns: "12000000" },
    }),
  /derived latency metrics disagree/,
);
assert.throws(
  () =>
    parsePairNetworkHealth({
      ...degraded,
      peer_to_local: { ...direction, round_trip_ns: ["10000000"] },
    }),
  /samples must match successes/,
);
assert.throws(
  () => parsePairNetworkHealth({ ...degraded, age_ns: "01" }),
  /canonical nonnegative integer string/,
);
assert.throws(
  () =>
    parsePairNetworkHealth({
      ...degraded,
      state: "good",
      raw_state: "degraded",
      transition_pending: false,
    }),
  /transition_pending fields disagree/,
);
assert.throws(
  () => parsePairNetworkHealth({ ...degraded, stale: true }),
  /stale pair network health must be measured and unusable/,
);
assert.throws(
  () => parsePairNetworkHealth({ ...unconfigured, age_ns: "1" }),
  /unmeasured pair network health must have zero age/,
);
assert.throws(
  () =>
    parsePairNetworkHealth({
      ...degraded,
      configured: false,
      state: "good",
      raw_state: "good",
      peer: null,
    }),
  /unconfigured pair network health must be unmeasured and unusable/,
);
