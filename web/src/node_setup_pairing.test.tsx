import assert from "node:assert/strict";
import { JSDOM, type DOMWindow } from "jsdom";
import { FakeNodeSetupApi, fixtureNodeSetup } from "./fake_node_setup_api.js";
import { MemoryPairingBindingStore } from "./node_pairing.js";
import type {
  NodeDiscoveryHint,
  NodeSetupApi,
  NodeSetupConfigurationUpdate,
  NodeSetupSnapshot,
} from "./node_setup_api.js";
import { NodeSetupApp } from "./node_setup_app.js";

const dom = new JSDOM('<!doctype html><html lang="en"><body></body></html>', {
  url: "http://station.test/#setup",
});
installDomGlobals(dom.window);

async function main() {
  const { cleanup, fireEvent, render, screen, waitFor, within } = await import(
    "@testing-library/react"
  );
  // This target deliberately exercises several complete setup/pairing lifecycles in one JSDOM
  // process. Under the pre-field aggregate, concurrent browser and TypeScript workers can leave a
  // process unscheduled for longer than Testing Library's one-second default even though the fake
  // API resolves immediately. Await semantic DOM/store convergence with a contention-tolerant
  // diagnostic bound; no production timeout or deliberate sleep is involved.
  const waitForUi = (assertion: () => void | Promise<void>) =>
    waitFor(assertion, { timeout: 5_000 });
  const { default: axe } = await import("axe-core");
  const store = new MemoryPairingBindingStore();
  const leader = new FakeNodeSetupApi(
    "http://10.168.168.111:8088",
    "down_the_line",
    "Pixel 6 Pro",
    "leader",
    "http://10.168.168.241:8088",
  );
  const shadow = new FakeNodeSetupApi(
    "http://10.168.168.241:8088",
    "face_on",
    "Pixel 5a",
    "shadow",
  );
  const rendered = render(
    <NodeSetupApp apis={[leader, shadow]} nowMs={() => 1_787_424_000_000} pairingStore={store} />,
  );
  assert.ok(await screen.findByRole("heading", { name: "Two-phone station configured" }));
  const leaderCard = within(
    requiredElement(
      screen.getByRole("heading", { name: "Down-the-line phone" }).closest("article"),
    ),
  );
  assert.ok(leaderCard.getByText("Address-only peer association"));
  const candidate = leaderCard.getByRole("button", { name: /Pixel 5a.*Across-the-line phone/ });
  assert.equal(candidate.hasAttribute("disabled"), false);
  assert.match(candidate.textContent ?? "", /Confirm new pairing/);
  fireEvent.click(candidate);
  fireEvent.change(leaderCard.getByLabelText("Peer label"), {
    target: { value: "Hitting bay face-on" },
  });
  fireEvent.change(leaderCard.getByLabelText("Peer control token"), {
    target: { value: "fresh-write-only-peer-token" },
  });
  fireEvent.click(leaderCard.getByRole("button", { name: "Save phone configuration" }));
  assert.ok(await leaderCard.findByText("Verified peer identity bound"));
  let binding = store.get("fixture-pixel-6-pro-node");
  assert.ok(binding);
  assert.equal(binding.peer_node_id, "fixture-pixel-5a-node");
  assert.equal(binding.peer_label, "Hitting bay face-on");
  assert.equal(binding.credential_generation, 1);
  assert.doesNotMatch(JSON.stringify(binding), /fresh-write-only-peer-token/);
  await waitForUi(() => {
    assert.equal(
      leaderCard.getByRole("button", { name: "Save phone configuration" }).hasAttribute("disabled"),
      false,
    );
  });

  fireEvent.change(leaderCard.getByLabelText("Peer control token"), {
    target: { value: "rotated-write-only-peer-token" },
  });
  fireEvent.click(leaderCard.getByRole("button", { name: "Save phone configuration" }));
  await waitForUi(() => {
    binding = store.get("fixture-pixel-6-pro-node");
    assert.equal(binding?.credential_generation, 2);
    assert.equal(
      leaderCard.getByRole("button", { name: "Save phone configuration" }).hasAttribute("disabled"),
      false,
    );
  });
  assert.match(leaderCard.getByText(/credential generation 2/).textContent ?? "", /generation 2/);

  const rotationButton = leaderCard.getByRole("button", {
    name: "Rotate this phone's control credential",
  });
  const rotationConfirmation = leaderCard.getByLabelText(
    "Confirm the full local node ID to rotate",
  );
  assert.equal(rotationButton.hasAttribute("disabled"), true);
  fireEvent.change(rotationConfirmation, { target: { value: "fixture-pixel-6" } });
  assert.equal(rotationButton.hasAttribute("disabled"), true);
  assert.equal((await leader.getSetup()).node.control_credential_generation, 1);
  fireEvent.change(rotationConfirmation, { target: { value: "fixture-pixel-6-pro-node" } });
  await waitForUi(() => {
    assert.equal(
      leaderCard
        .getByRole("button", { name: "Rotate this phone's control credential" })
        .hasAttribute("disabled"),
      false,
    );
  });
  fireEvent.click(rotationButton);
  assert.ok(await leaderCard.findByText("New control token — copy it now"));
  assert.ok(leaderCard.getByText("rotatedFixtureControlToken00001_"));
  await waitForUi(async () => {
    assert.equal((await leader.getSetup()).node.control_credential_generation, 2);
  });

  const accessibility = await axe.run(rendered.container, {
    rules: { "color-contrast": { enabled: false } },
  });
  assert.deepEqual(
    accessibility.violations.map((violation) => violation.id),
    [],
  );
  cleanup();

  render(
    <NodeSetupApp
      apis={[new StalePeerCredentialApi()]}
      pairingStore={new MemoryPairingBindingStore()}
    />,
  );
  assert.ok(await screen.findByText("Peer credential needs re-pairing"));
  assert.ok(screen.getByText(/peer rejected the stored token/i));
  cleanup();

  const movedLeader = new FakeNodeSetupApi(
    "http://10.168.168.111:8088",
    "down_the_line",
    "Pixel 6 Pro",
    "leader",
    "http://10.168.168.241:8088",
  );
  const movedShadow = new FakeNodeSetupApi(
    "http://10.168.168.242:8088",
    "face_on",
    "Pixel 5a",
    "shadow",
  );
  render(
    <NodeSetupApp
      apis={[movedLeader, movedShadow]}
      nowMs={() => 1_787_424_001_000}
      pairingStore={store}
    />,
  );
  assert.ok(await screen.findByText("Verified peer identity bound"));
  const movedLeaderCard = within(
    requiredElement(
      screen.getByRole("heading", { name: "Down-the-line phone" }).closest("article"),
    ),
  );
  const movedCandidate = movedLeaderCard.getByRole("button", {
    name: /Pixel 5a.*address recovery/,
  });
  fireEvent.click(movedCandidate);
  fireEvent.change(movedLeaderCard.getByLabelText("Peer control token"), {
    target: { value: "credential-for-new-authenticated-address" },
  });
  fireEvent.click(movedLeaderCard.getByRole("button", { name: "Save phone configuration" }));
  await waitForUi(() => {
    binding = store.get("fixture-pixel-6-pro-node");
    assert.equal(binding?.peer_origin, "http://10.168.168.242:8088");
    assert.equal(binding?.credential_generation, 3);
    assert.equal(
      movedLeaderCard
        .getByRole("button", { name: "Save phone configuration" })
        .hasAttribute("disabled"),
      false,
    );
  });

  fireEvent.click(movedLeaderCard.getByLabelText("Clear the existing peer association"));
  await waitForUi(() => {
    assert.equal(
      (movedLeaderCard.getByLabelText("Clear the existing peer association") as HTMLInputElement)
        .checked,
      true,
    );
  });
  fireEvent.click(movedLeaderCard.getByRole("button", { name: "Save phone configuration" }));
  await waitForUi(() => {
    assert.ok(movedLeaderCard.getByText("Peer credential revoked"));
    binding = store.get("fixture-pixel-6-pro-node");
    assert.equal(binding?.credential_state, "revoked");
    assert.equal(
      movedLeaderCard
        .getByRole("button", { name: "Save phone configuration" })
        .hasAttribute("disabled"),
      false,
    );
  });
  const forget = movedLeaderCard.getByRole("button", { name: "Forget revoked identity" });
  assert.equal(forget.hasAttribute("disabled"), true);
  fireEvent.change(
    movedLeaderCard.getByLabelText("Confirm full peer node ID to forget revoked identity metadata"),
    { target: { value: "fixture-pixel-5a-node" } },
  );
  await waitForUi(() => {
    assert.equal(
      movedLeaderCard
        .getByRole("button", { name: "Forget revoked identity" })
        .hasAttribute("disabled"),
      false,
    );
  });
  fireEvent.click(movedLeaderCard.getByRole("button", { name: "Forget revoked identity" }));
  assert.equal(store.get("fixture-pixel-6-pro-node"), null);
  assert.ok(movedLeaderCard.getByText("No stable peer identity bound"));
  cleanup();

  render(
    <NodeSetupApp
      apis={[
        new FakeNodeSetupApi(
          "http://duplicate-one.test:8088",
          "down_the_line",
          "Pixel 6 Pro",
          "leader",
          null,
          "duplicated-installation-id",
        ),
        new FakeNodeSetupApi(
          "http://duplicate-two.test:8088",
          "face_on",
          "Pixel 5a",
          "shadow",
          null,
          "duplicated-installation-id",
        ),
      ]}
      pairingStore={new MemoryPairingBindingStore()}
    />,
  );
  assert.ok(await screen.findByRole("heading", { name: "Duplicate phone identity detected" }));
  assert.ok(screen.getByText(/Do not pair either entry/));
  cleanup();

  const discovering = new DiscoveringNodeSetupApi(
    new FakeNodeSetupApi(
      "http://leader.test:8088",
      "down_the_line",
      "Pixel 6 Pro",
      "disabled",
      null,
    ),
  );
  render(
    <NodeSetupApp
      apis={[discovering]}
      nowMs={() => 1_787_424_000_000}
      pairingStore={new MemoryPairingBindingStore()}
    />,
  );
  assert.ok(await screen.findByRole("heading", { name: "Discovered phones" }));
  const discoveredToken = screen.getByLabelText("Control token for Hitting bay face-on");
  fireEvent.change(discoveredToken, { target: { value: "authenticated-peer-token" } });
  fireEvent.click(screen.getByRole("button", { name: "Authenticate and add" }));
  assert.ok(await screen.findByText("Pixel 5a", { exact: false }));
  assert.equal(discovering.lastToken, "authenticated-peer-token");
  cleanup();

  const reconnecting = new ReconnectingNodeSetupApi(
    new FakeNodeSetupApi("http://reconnected.test:8088", "down_the_line", "Pixel 6 Pro"),
  );
  render(<NodeSetupApp apis={[reconnecting]} pairingStore={new MemoryPairingBindingStore()} />);
  assert.ok(await screen.findByRole("heading", { name: "Phone setup unavailable" }));
  fireEvent.click(screen.getByRole("button", { name: "Retry connection" }));
  assert.ok(await screen.findByText("Pixel 6 Pro", { exact: false }));
  cleanup();
}

class StalePeerCredentialApi implements NodeSetupApi {
  readonly displayOrigin = "http://leader-with-stale-peer.test:8088";
  readonly #setup: NodeSetupSnapshot;

  constructor() {
    const setup = fixtureNodeSetup(
      this.displayOrigin,
      "down_the_line",
      "Pixel 6 Pro",
      "leader",
      "http://shadow-with-rotated-token.test:8088",
      "stale-leader-node-id",
    );
    this.#setup = {
      ...setup,
      pairing: {
        state: "active",
        peer_node_id: "shadow-node-id",
        expected_role: "face_on",
        label: "Hitting bay face-on",
        origin: "http://shadow-with-rotated-token.test:8088",
        credential_generation: 3,
        credential_status: "re_pair_required",
        verified_at_epoch_ms: 1_787_424_000_000,
        revoked_at_epoch_ms: 0,
      },
    };
  }

  getSetup(): Promise<NodeSetupSnapshot> {
    return Promise.resolve(structuredClone(this.#setup));
  }

  updateSetup(): Promise<NodeSetupSnapshot> {
    return Promise.reject(new Error("not used"));
  }
}

class DiscoveringNodeSetupApi implements NodeSetupApi {
  readonly displayOrigin: string;
  lastToken = "";
  readonly #delegate: FakeNodeSetupApi;
  readonly #hint: NodeDiscoveryHint = {
    schema_version: 1,
    service_instance: "pixel-5a-service",
    node_id: "fixture-pixel-5a-node",
    role: "face_on",
    label: "Hitting bay face-on",
    origin: "http://shadow.test:8088",
    observed_at_epoch_ms: 1_787_423_999_000,
    expires_at_epoch_ms: 1_787_424_030_000,
    trusted: false,
  };

  constructor(delegate: FakeNodeSetupApi) {
    this.#delegate = delegate;
    this.displayOrigin = delegate.displayOrigin;
  }

  getSetup(): Promise<NodeSetupSnapshot> {
    return this.#delegate.getSetup();
  }

  updateSetup(
    expectedRevision: number,
    configuration: NodeSetupConfigurationUpdate,
  ): Promise<NodeSetupSnapshot> {
    return this.#delegate.updateSetup(expectedRevision, configuration);
  }

  discoverNodes(): Promise<readonly NodeDiscoveryHint[]> {
    return Promise.resolve([this.#hint]);
  }

  async connectDiscovered(
    observation: NodeDiscoveryHint,
    controlToken: string,
  ): Promise<{ api: NodeSetupApi; setup: NodeSetupSnapshot }> {
    assert.equal(observation, this.#hint);
    this.lastToken = controlToken;
    const api = new FakeNodeSetupApi(
      observation.origin,
      "face_on",
      "Pixel 5a",
      "shadow",
      null,
      observation.node_id,
    );
    return { api, setup: await api.getSetup() };
  }
}

class ReconnectingNodeSetupApi implements NodeSetupApi {
  readonly displayOrigin: string;
  #failed = false;
  readonly #delegate: FakeNodeSetupApi;

  constructor(delegate: FakeNodeSetupApi) {
    this.#delegate = delegate;
    this.displayOrigin = delegate.displayOrigin;
  }

  getSetup(): Promise<NodeSetupSnapshot> {
    if (!this.#failed) {
      this.#failed = true;
      return Promise.reject(new Error("authenticated phone connection unavailable"));
    }
    return this.#delegate.getSetup();
  }

  updateSetup(
    expectedRevision: number,
    configuration: NodeSetupConfigurationUpdate,
  ): Promise<NodeSetupSnapshot> {
    return this.#delegate.updateSetup(expectedRevision, configuration);
  }
}

function installDomGlobals(window: DOMWindow) {
  const globals = globalThis as unknown as Record<string, unknown>;
  globals.window = window;
  globals.document = window.document;
  Object.defineProperty(globalThis, "navigator", {
    configurable: true,
    value: window.navigator,
  });
  globals.HTMLElement = window.HTMLElement;
  globals.HTMLInputElement = window.HTMLInputElement;
  globals.MutationObserver = window.MutationObserver;
  globals.Node = window.Node;
  globals.getComputedStyle = window.getComputedStyle.bind(window);
  globals.IS_REACT_ACT_ENVIRONMENT = true;
}

function requiredElement<T>(value: T | null): T {
  assert.notEqual(value, null);
  return value as T;
}

void main().catch((caught: unknown) => {
  console.error(caught);
  process.exit(1);
});
