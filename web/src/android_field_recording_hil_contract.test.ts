import assert from "node:assert/strict";
import {
  cleanupRestored,
  freshDecodeMediaAbortEvidence,
  primaryStopEvidence,
  restoreStoppedFieldRecorder,
  reviewMediaAbortEvidence,
  scopedMediaCapability,
  stableMediaIdentity,
} from "./android_field_recording_hil_contract.js";

async function main() {
  await acceptsTerminalStateAfterRedundantStopTimeout();
  preservesOriginSpecificPrimaryStopEvidence();
  rejectsNonterminalCleanupDespiteAcceptedRedundantStop();
  identifiesMediaIndependentlyOfCapability();
  rejectsMissingOrAmbiguousMediaCapabilities();
  classifiesOnlyExpectedHistoricalReviewMediaCancellation();
  classifiesOnlyExplicitFreshDecoderRelease();
}

async function acceptsTerminalStateAfterRedundantStopTimeout() {
  let statusAttempts = 0;
  const statusRequests: Array<[string, string]> = [];
  const result = await restoreStoppedFieldRecorder("http://face.test", "face-token", {
    stop: () => Promise.reject(new Error("transport timeout")),
    status: (origin, token) => {
      statusRequests.push([origin, token]);
      return Promise.resolve(++statusAttempts === 1 ? "stopping" : "ready");
    },
    wait: () => Promise.resolve(),
  });
  assert.deepEqual(result, {
    origin: "http://face.test",
    stop_status: null,
    final_state: "ready",
  });
  assert.equal(statusAttempts, 2, "terminal state must be polled after a stop timeout");
  assert.deepEqual(statusRequests, [
    ["http://face.test", "face-token"],
    ["http://face.test", "face-token"],
  ]);
  assert.equal(
    cleanupRestored([{ origin: "http://dtl.test", stop_status: 202, final_state: "idle" }, result]),
    true,
  );
}

function preservesOriginSpecificPrimaryStopEvidence() {
  const evidence = primaryStopEvidence(
    {
      "POST http://dtl.test/api/v1/field-recording/stop": [202],
      "POST http://face.test/api/v1/field-recording/stop": [409, 202],
    },
    ["http://dtl.test", "http://face.test"],
  );
  assert.deepEqual(evidence, [
    { origin: "http://dtl.test", statuses: [202] },
    { origin: "http://face.test", statuses: [409, 202] },
  ]);
}

function rejectsNonterminalCleanupDespiteAcceptedRedundantStop() {
  assert.equal(
    cleanupRestored([
      { origin: "http://dtl.test", stop_status: 202, final_state: "recording" },
      { origin: "http://face.test", stop_status: null, final_state: "ready" },
    ]),
    false,
  );
}

function identifiesMediaIndependentlyOfCapability() {
  const firstCapability = "A".repeat(43);
  const rotatedCapability = "B".repeat(43);
  const first = `http://phone.test/api/v1/field-recordings/field-1/video.mp4?media_access=${firstCapability}`;
  const rotated = `http://phone.test/api/v1/field-recordings/field-1/video.mp4?media_access=${rotatedCapability}`;
  assert.equal(stableMediaIdentity(first), stableMediaIdentity(rotated));
  assert.equal(
    stableMediaIdentity(first),
    "http://phone.test/api/v1/field-recordings/field-1/video.mp4",
  );
  assert.equal(scopedMediaCapability(first), firstCapability);
  assert.equal(scopedMediaCapability(rotated), rotatedCapability);
}

function rejectsMissingOrAmbiguousMediaCapabilities() {
  const capability = "C".repeat(43);
  const base = "http://phone.test/api/v1/field-recordings/field-1/video.mp4";
  assert.equal(scopedMediaCapability(base), null);
  assert.equal(scopedMediaCapability(`${base}?media_access=short`), null);
  assert.equal(scopedMediaCapability(`${base}?other=${capability}`), null);
  assert.equal(scopedMediaCapability(`${base}?media_access=${capability}&other=value`), null);
  assert.equal(
    scopedMediaCapability(`${base}?media_access=${capability}&media_access=${capability}`),
    null,
  );
}

function classifiesOnlyExpectedHistoricalReviewMediaCancellation() {
  const origins = ["http://dtl.test", "http://face.test"];
  assert.deepEqual(
    reviewMediaAbortEvidence(
      "http://dtl.test/api/v1/sessions/session-1/down_the_line.mp4?media_access=secret",
      "net::ERR_ABORTED",
      origins,
    ),
    {
      origin: "http://dtl.test",
      path: "/api/v1/sessions/session-1/down_the_line.mp4",
      error: "net::ERR_ABORTED",
    },
  );
  assert.equal(
    reviewMediaAbortEvidence(
      "http://face.test/api/v1/sessions/session_2/face_on.mp4",
      "net::ERR_FAILED",
      origins,
    ),
    null,
  );
  assert.equal(
    reviewMediaAbortEvidence(
      "http://other.test/api/v1/sessions/session-1/face_on.mp4",
      "net::ERR_ABORTED",
      origins,
    ),
    null,
  );
  assert.equal(
    reviewMediaAbortEvidence(
      "http://dtl.test/api/v1/field-recordings/field-1/video.mp4",
      "net::ERR_ABORTED",
      origins,
    ),
    null,
  );
}

function classifiesOnlyExplicitFreshDecoderRelease() {
  const identity = "http://dtl.test/api/v1/field-recordings/field-1/video.mp4";
  const allowed = new Map([[identity, "down_the_line" as const]]);
  assert.deepEqual(
    freshDecodeMediaAbortEvidence(
      `${identity}?media_access=${"D".repeat(43)}`,
      "net::ERR_ABORTED",
      allowed,
    ),
    {
      role: "down_the_line",
      origin: "http://dtl.test",
      media_identity: identity,
      error: "net::ERR_ABORTED",
    },
  );
  assert.equal(freshDecodeMediaAbortEvidence(identity, "net::ERR_FAILED", allowed), null);
  assert.equal(
    freshDecodeMediaAbortEvidence(
      "http://dtl.test/api/v1/field-recordings/field-2/video.mp4",
      "net::ERR_ABORTED",
      allowed,
    ),
    null,
  );
}

void main().catch((caught: unknown) => {
  console.error(caught);
  process.exit(1);
});
