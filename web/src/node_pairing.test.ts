import assert from "node:assert/strict";
import {
  activatePairingBinding,
  assessNodeDiscoveries,
  directlyVerifiedCandidate,
  MemoryPairingBindingStore,
  parsePeerPairingBinding,
  requiredPairingAction,
  resetPairingBinding,
  revokePairingBinding,
  verifyDiscoveredCandidate,
  type AuthenticatedNodeIdentity,
  type NodeDiscoveryObservation,
} from "./node_pairing.js";

const NOW = 1_787_424_000_000;
const LOCAL: AuthenticatedNodeIdentity = {
  node_id: "pixel-6-installation-id",
  role: "down_the_line",
  origin: "http://10.168.168.111:8088",
  label: "Pixel 6 Pro · Down the line",
};
const PEER: AuthenticatedNodeIdentity = {
  node_id: "pixel-5a-installation-id",
  role: "face_on",
  origin: "http://10.168.168.241:8088",
  label: "Pixel 5a · Face on",
};

function main() {
  rejectsDuplicateAdvertisedIdentities();
  rejectsStaleAndContradictoryDiscovery();
  requiresAuthenticationAndConfirmationForAddressRecovery();
  modelsCredentialRotationRevocationAndExplicitRepair();
  rejectsRoleAndStableIdentityConflicts();
  requiresExactIdentityForDestructiveReset();
}

function rejectsDuplicateAdvertisedIdentities() {
  const first = discovery(PEER.origin);
  const duplicate = discovery("http://10.168.168.199:8088", "swing-capture-copy");
  const assessed = assessNodeDiscoveries([first, duplicate], NOW);
  assert.deepEqual(
    assessed.map(({ state }) => state),
    ["identity_conflict", "identity_conflict"],
  );
  assert.match(assessed[0]?.detail ?? "", /do not auto-select/);
}

function rejectsStaleAndContradictoryDiscovery() {
  const stale = discovery(PEER.origin);
  stale.expires_at_epoch_ms = NOW;
  assert.equal(assessNodeDiscoveries([stale], NOW)[0]?.state, "stale");
  assert.throws(
    () => verifyDiscoveredCandidate(LOCAL, stale, PEER, NOW),
    /expired before the phone was confirmed/,
  );

  const wrongIdentity = discovery(PEER.origin);
  wrongIdentity.advertised_node_id = "spoofed-node-id";
  assert.throws(
    () => verifyDiscoveredCandidate(LOCAL, wrongIdentity, PEER, NOW),
    /disagrees with the authenticated phone identity/,
  );
  const wrongRole = discovery(PEER.origin);
  wrongRole.advertised_role = "down_the_line";
  assert.throws(
    () => verifyDiscoveredCandidate(LOCAL, wrongRole, PEER, NOW),
    /disagrees with the authenticated phone role/,
  );
}

function requiresAuthenticationAndConfirmationForAddressRecovery() {
  const initialCandidate = verifyDiscoveredCandidate(LOCAL, discovery(PEER.origin), PEER, NOW);
  const initial = activatePairingBinding(
    null,
    LOCAL,
    initialCandidate,
    "pair",
    "Hitting bay face-on",
    NOW,
  );
  const movedPeer = { ...PEER, origin: "http://10.168.168.242:8088" };
  const movedDiscovery = discovery(movedPeer.origin, "swing-capture-moved");
  const verifiedMoved = verifyDiscoveredCandidate(LOCAL, movedDiscovery, movedPeer, NOW + 500);
  assert.equal(requiredPairingAction(initial, LOCAL, verifiedMoved), "recover_address");
  assert.throws(
    () =>
      activatePairingBinding(
        initial,
        LOCAL,
        verifiedMoved,
        "rotate_credential",
        initial.peer_label,
        NOW + 500,
      ),
    /requires address recovery confirmation/,
  );
  const recovered = activatePairingBinding(
    initial,
    LOCAL,
    verifiedMoved,
    "recover_address",
    initial.peer_label,
    NOW + 500,
  );
  assert.equal(recovered.peer_node_id, initial.peer_node_id);
  assert.equal(recovered.peer_origin, movedPeer.origin);
  assert.equal(recovered.credential_generation, 2);
  assert.equal(recovered.binding_revision, 2);
}

function modelsCredentialRotationRevocationAndExplicitRepair() {
  const candidate = directlyVerifiedCandidate(LOCAL, PEER);
  const initial = activatePairingBinding(null, LOCAL, candidate, "pair", PEER.label, NOW);
  assert.equal(requiredPairingAction(initial, LOCAL, candidate), "rotate_credential");
  const rotated = activatePairingBinding(
    initial,
    LOCAL,
    candidate,
    "rotate_credential",
    initial.peer_label,
    NOW + 100,
  );
  assert.equal(rotated.credential_generation, 2);
  const revoked = revokePairingBinding(rotated, LOCAL.node_id, NOW + 200);
  assert.equal(revoked.credential_state, "revoked");
  assert.equal(revoked.revoked_at_epoch_ms, NOW + 200);
  assert.equal(requiredPairingAction(revoked, LOCAL, candidate), "re_pair");
  assert.throws(
    () =>
      activatePairingBinding(
        revoked,
        LOCAL,
        candidate,
        "rotate_credential",
        revoked.peer_label,
        NOW + 300,
      ),
    /requires identity re-pair confirmation/,
  );
  const repaired = activatePairingBinding(
    revoked,
    LOCAL,
    candidate,
    "re_pair",
    revoked.peer_label,
    NOW + 300,
  );
  assert.equal(repaired.credential_state, "active");
  assert.equal(repaired.credential_generation, 3);
  assert.doesNotMatch(JSON.stringify(repaired), /token|credential_value|bearer/i);
  assert.deepEqual(parsePeerPairingBinding(repaired), repaired);
}

function rejectsRoleAndStableIdentityConflicts() {
  assert.throws(
    () => directlyVerifiedCandidate(LOCAL, { ...PEER, node_id: LOCAL.node_id }),
    /cannot be paired to itself or appear at two origins/,
  );
  assert.throws(
    () => directlyVerifiedCandidate(LOCAL, { ...PEER, role: "down_the_line" }),
    /distinct camera roles/,
  );
  assert.throws(
    () => directlyVerifiedCandidate(LOCAL, { ...PEER, role: "unassigned" }),
    /Assign a camera role/,
  );
}

function requiresExactIdentityForDestructiveReset() {
  const store = new MemoryPairingBindingStore();
  const binding = activatePairingBinding(
    null,
    LOCAL,
    directlyVerifiedCandidate(LOCAL, PEER),
    "pair",
    PEER.label,
    NOW,
  );
  store.set(binding);
  assert.throws(
    () => resetPairingBinding(store, binding, "pixel-5a"),
    /must match the full peer node ID/,
  );
  assert.deepEqual(store.get(LOCAL.node_id), binding);
  resetPairingBinding(store, binding, PEER.node_id);
  assert.equal(store.get(LOCAL.node_id), null);
}

function discovery(origin: string, instance = "swing-capture-pixel-5a"): NodeDiscoveryObservation {
  return {
    schema_version: 1,
    service_instance: instance,
    origin,
    advertised_node_id: PEER.node_id,
    advertised_role: "face_on",
    advertised_label: "Pixel 5a · Face on",
    observed_at_epoch_ms: NOW - 1_000,
    expires_at_epoch_ms: NOW + 30_000,
  };
}

main();
