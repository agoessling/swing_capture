import assert from "node:assert/strict";
import { JSDOM, type DOMWindow } from "jsdom";
import { fixtureNodeSetup } from "./fake_node_setup_api.js";
import type {
  NodeSetupApi,
  NodeSetupConfigurationUpdate,
  NodeSetupSnapshot,
} from "./node_setup_api.js";
import { NodeSetupApp } from "./node_setup_app.js";
import type { PairNetworkHealth } from "./pair_network_health.js";
import type {
  CaptureStatus,
  DiagnosticArchive,
  DiagnosticFeedback,
  ReviewApi,
  SessionList,
  SessionSummary,
  SetArmedOptions,
} from "./review_api.js";
import { ReviewApp } from "./review_app.js";

const dom = new JSDOM('<!doctype html><html lang="en"><body></body></html>', {
  url: "http://station.test/#review",
});
installDomGlobals(dom.window);

const direction = {
  schema_version: 1 as const,
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
};

function networkHealth(state: "good" | "degraded" | "unusable", stale = false): PairNetworkHealth {
  return {
    schema_version: 1,
    configured: true,
    state,
    raw_state: state,
    measured: true,
    stale,
    transition_pending: false,
    age_ns: stale ? "31000000000" : "2500000000",
    issues:
      state === "good"
        ? []
        : [
            stale
              ? "Pair network-health evidence is stale."
              : "Some phone-to-phone application requests failed or timed out.",
          ],
    peer: { origin: "http://pixel-5a.test:8088", node_id: "pixel-5a-node" },
    measured_at_elapsed_realtime_ns: "987654321000",
    local_to_peer: direction,
    peer_to_local: direction,
  };
}

async function main() {
  const { cleanup, fireEvent, render, screen, waitFor } = await import("@testing-library/react");

  const degradedApi = new NetworkReviewApi(networkHealth("degraded"), "leader");
  render(<ReviewApp api={degradedApi} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByText("Pair network: Degraded"));
  assert.ok(screen.getAllByText(/4\/5 replies.*1 timeouts.*p95 16\.0 ms.*21\.0 Mbps/).length === 2);
  assert.ok(screen.getByText("Some phone-to-phone application requests failed or timed out."));
  const arm = screen.getByRole("button", { name: "Arm pose capture" });
  assert.equal(arm.hasAttribute("disabled"), true);
  const override = screen.getByRole("checkbox", {
    name: /Arm once using degraded pair network/,
  });
  fireEvent.click(override);
  assert.equal(arm.hasAttribute("disabled"), false);
  fireEvent.click(arm);
  await waitFor(() => assert.equal(degradedApi.armOptions.length, 1));
  assert.deepEqual(degradedApi.armOptions, [{ allowDegradedNetwork: true }]);
  assert.equal(screen.queryByRole("checkbox"), null, "override is consumed and not persisted");
  cleanup();

  const rejectedApi = new NetworkReviewApi(networkHealth("degraded"), "leader", true);
  render(<ReviewApp api={rejectedApi} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByText("Pair network: Degraded"));
  const rejectedOverride = screen.getByRole<HTMLInputElement>("checkbox", {
    name: /Arm once using degraded pair network/,
  });
  const rejectedArm = screen.getByRole("button", { name: "Arm pose capture" });
  fireEvent.click(rejectedOverride);
  fireEvent.click(rejectedArm);
  assert.ok(await screen.findByText("simulated arm rejection"));
  assert.deepEqual(rejectedApi.armOptions, [{ allowDegradedNetwork: true }]);
  assert.equal(rejectedOverride.checked, false, "a rejected attempt consumes the acknowledgement");
  assert.equal(rejectedArm.hasAttribute("disabled"), true);
  cleanup();

  render(
    <ReviewApp
      api={new NetworkReviewApi(networkHealth("unusable", true), "leader")}
      pollIntervalMs={60_000}
    />,
  );
  assert.ok(await screen.findByText("Pair network: Unusable · stale"));
  assert.equal(
    screen.getByRole("button", { name: "Arm pose capture" }).hasAttribute("disabled"),
    true,
  );
  assert.equal(screen.queryByRole("checkbox"), null);
  cleanup();

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
  render(<ReviewApp api={new NetworkReviewApi(unconfigured, "shadow")} pollIntervalMs={60_000} />);
  await screen.findByRole("heading", { name: "Swing review" });
  assert.equal(screen.queryByLabelText("Pair network health"), null);
  assert.equal(
    screen.getByRole("button", { name: "Arm paired capture" }).hasAttribute("disabled"),
    false,
  );
  cleanup();

  render(<ReviewApp api={new NetworkReviewApi(undefined, "leader")} pollIntervalMs={60_000} />);
  assert.ok(await screen.findByText("Pair network: Unknown"));
  assert.equal(
    screen.getByRole("button", { name: "Arm pose capture" }).hasAttribute("disabled"),
    true,
  );
  cleanup();

  render(<NodeSetupApp apis={[new NetworkSetupApi(networkHealth("good"))]} />);
  assert.ok(await screen.findByText("Pair network: Good"));
  assert.ok(screen.getAllByText(/Leader → peer/).length === 1);
  cleanup();
}

class NetworkReviewApi implements ReviewApi {
  readonly armOptions: Array<SetArmedOptions | undefined> = [];
  #capture: CaptureStatus;

  constructor(
    health: PairNetworkHealth | undefined,
    mode: "leader" | "shadow",
    private readonly rejectArm = false,
  ) {
    this.#capture = {
      schema_version: 2,
      state: "setup",
      armed: false,
      active_session_id: null,
      error: "",
      hil: { enabled: false, busy: false, stage: "idle", error: "", last_run: null },
      pose: {
        mode,
        phase: "idle",
        transition_requested: false,
        peer_arm: {
          state: "not_requested",
          shared_session_id: null,
          http_status: null,
          failure_type: null,
        },
      },
      ...(health === undefined ? {} : { pair_network_health: health }),
    };
  }

  getCaptureStatus() {
    return Promise.resolve(structuredClone(this.#capture));
  }
  setArmed(armed: boolean, options?: SetArmedOptions) {
    this.armOptions.push(options);
    if (armed && this.rejectArm) {
      return Promise.reject(new Error("simulated arm rejection"));
    }
    this.#capture = { ...this.#capture, armed, state: armed ? "armed" : "setup" };
    return Promise.resolve(structuredClone(this.#capture));
  }
  triggerManualCapture(): Promise<SessionSummary> {
    return Promise.reject(new Error("unused"));
  }
  saveMissedShot(): Promise<SessionSummary> {
    return Promise.reject(new Error("unused"));
  }
  startSyntheticSwing(): Promise<CaptureStatus> {
    return Promise.reject(new Error("unused"));
  }
  getSessions(): Promise<SessionList> {
    return Promise.resolve({ schema_version: 1, sessions: [] });
  }
  getManifest() {
    return Promise.reject(new Error("unused"));
  }
  submitDiagnosticFeedback(_sessionId: string, _feedback: DiagnosticFeedback) {
    return Promise.reject(new Error("unused"));
  }
  getDiagnosticArchives(): Promise<readonly DiagnosticArchive[]> {
    return Promise.reject(new Error("unused"));
  }
}

class NetworkSetupApi implements NodeSetupApi {
  readonly displayOrigin = "http://pixel-6.test:8088";
  readonly #setup: NodeSetupSnapshot;
  constructor(health: PairNetworkHealth) {
    this.#setup = { ...fixtureNodeSetup(), pair_network_health: health };
  }
  getSetup() {
    return Promise.resolve(structuredClone(this.#setup));
  }
  updateSetup(_expectedRevision: number, _configuration: NodeSetupConfigurationUpdate) {
    return Promise.resolve(structuredClone(this.#setup));
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

void main().catch((caught: unknown) => {
  console.error(caught);
  process.exit(1);
});
