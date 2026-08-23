import assert from "node:assert/strict";
import {
  consumeControlCredentials,
  credentialFreeUrl,
  retainSingleNodeAndroidMode,
  type SessionCredentialStorage,
} from "./control_credential_bootstrap.js";

class MemoryStorage implements SessionCredentialStorage {
  readonly values = new Map<string, string>();

  getItem(key: string): string | null {
    return this.values.get(key) ?? null;
  }

  setItem(key: string, value: string): void {
    this.values.set(key, value);
  }
}

const storage = new MemoryStorage();
const parameters = new URLSearchParams(
  "node=http%3A%2F%2Fphone.test%3A8088&node_token=initial-secret&view=latest",
);
const first = consumeControlCredentials(parameters, storage, [
  { parameter: "node_token", scope: "http://phone.test:8088" },
]);
assert.equal(first.removedFromUrl, true);
assert.equal(first.credentials.get("node_token")?.current(), "initial-secret");
assert.equal(parameters.has("node_token"), false);
assert.equal(parameters.get("node"), "http://phone.test:8088");
assert.equal(parameters.get("view"), "latest");
assert.equal(
  credentialFreeUrl("http://phone.test:8088/?node_token=initial-secret#review", parameters),
  "http://phone.test:8088/?node=http%3A%2F%2Fphone.test%3A8088&view=latest#review",
);

first.credentials.get("node_token")?.replace("rotated-secret");
const reload = consumeControlCredentials(new URLSearchParams(), storage, [
  { parameter: "node_token", scope: "http://phone.test:8088" },
]);
assert.equal(reload.removedFromUrl, false);
assert.equal(reload.credentials.get("node_token")?.current(), "rotated-secret");

const phoneHostedParameters = new URLSearchParams("node_token=phone-hosted-secret");
retainSingleNodeAndroidMode(phoneHostedParameters);
consumeControlCredentials(phoneHostedParameters, storage, [
  { parameter: "node_token", scope: "http://phone.test:8088" },
]);
assert.equal(
  phoneHostedParameters.has("node"),
  true,
  "same-origin Android mode survives credential scrubbing and reload",
);
assert.equal(phoneHostedParameters.get("node"), "");
assert.equal(phoneHostedParameters.has("node_token"), false);

const anotherPhone = consumeControlCredentials(new URLSearchParams(), storage, [
  { parameter: "node_token", scope: "http://other-phone.test:8088" },
]);
assert.equal(
  anotherPhone.credentials.get("node_token")?.current(),
  "",
  "a tab-scoped credential must not cross phone origins",
);

const dualParameters = new URLSearchParams("dtl_token=dtl-secret&face_token=face-secret");
const dual = consumeControlCredentials(dualParameters, storage, [
  { parameter: "dtl_token", scope: "http://dtl.test:8088" },
  { parameter: "face_token", scope: "http://face.test:8088" },
]);
assert.equal(dual.credentials.get("dtl_token")?.current(), "dtl-secret");
assert.equal(dual.credentials.get("face_token")?.current(), "face-secret");
assert.equal(dualParameters.toString(), "");

const unavailableStorage: SessionCredentialStorage = {
  getItem() {
    throw new Error("storage denied");
  },
  setItem() {
    throw new Error("storage denied");
  },
};
const unavailable = consumeControlCredentials(
  new URLSearchParams("node_token=one-load-secret"),
  unavailableStorage,
  [{ parameter: "node_token", scope: "http://phone.test:8088" }],
);
assert.equal(unavailable.credentials.get("node_token")?.current(), "one-load-secret");

assert.throws(
  () =>
    consumeControlCredentials(new URLSearchParams(), storage, [
      { parameter: "node_token", scope: "http://phone.test:8088" },
      { parameter: "node_token", scope: "http://phone.test:8088" },
    ]),
  /Duplicate control credential parameter/,
);
