import assert from "node:assert/strict";
import { JSDOM, type DOMWindow } from "jsdom";
import { fixtureNodeSetup } from "./fake_node_setup_api.js";
import type { StatusSubscriptionScheduler } from "./live_status.js";
import type {
  NodeSetupApi,
  NodeSetupConfigurationUpdate,
  NodeSetupSnapshot,
} from "./node_setup_api.js";
import { HttpNodeSetupApi, parseNodeSetupSnapshot } from "./node_setup_api.js";
import { NodeSetupApp, SetupPreview } from "./node_setup_app.js";
import { MemoryPairingBindingStore } from "./node_pairing.js";
import type { ObjectUrlFactory } from "./paired_preview.js";

const dom = new JSDOM('<!doctype html><html lang="en"><body></body></html>', {
  url: "http://station.test/#setup",
});
installDomGlobals(dom.window);

async function main() {
  const { act, cleanup, fireEvent, render, screen, waitFor } = await import(
    "@testing-library/react"
  );
  await authenticatedPreviewRequestsStayOnThePhoneOrigin();
  const scheduler = new ManualScheduler();
  const api = new PreviewTransitionApi();
  const objectUrls = new RecordingObjectUrls();

  render(
    <SetupPreview
      label="Down-the-line phone"
      preview={{
        available: false,
        url: null,
        state: "stale",
        reason: "stale",
        frame_age_ms: 3_250.4,
        image_rotation_degrees: 0,
      }}
      scheduler={scheduler}
    />,
  );
  assert.ok(screen.getByText("Preview stale"));
  assert.ok(screen.getByText(/too old \(3250 ms\)/));
  cleanup();

  render(
    <NodeSetupApp
      apis={[api]}
      pairingStore={new MemoryPairingBindingStore()}
      scheduler={scheduler}
      setupPreviewObjectUrls={objectUrls}
      setupPreviewRefreshMs={1_000}
      setupSnapshotRefreshMs={3_000}
    />,
  );
  await act(async () => {
    for (let step = 0; step < 6; ++step) {
      await Promise.resolve();
    }
  });
  assert.equal(api.setupRequests, 1);

  assert.ok(await screen.findByText("Preview unavailable"));
  assert.match(screen.getByText(/Standby may still be starting/).textContent ?? "", /Retrying/);
  const role = screen.getByRole("combobox", { name: "Camera view" }) as HTMLSelectElement;
  fireEvent.change(role, { target: { value: "face_on" } });
  assert.equal(role.value, "face_on");

  await scheduler.runNext(3_000, act);
  assert.equal(api.setupRequests, 2);
  assert.ok(screen.getByText("Pixel 6 Pro", { exact: false }));
  assert.equal(role.value, "face_on", "a transient refresh error must preserve the setup draft");
  assert.equal(screen.queryByText("synthetic preview status failure"), null);

  await scheduler.runNext(3_000, act);
  assert.equal(api.setupRequests, 3);
  assert.equal(api.previewRequests, 1);
  assert.equal(
    scheduler.pendingWithDelay(1_000),
    0,
    "a slow authenticated fetch cannot accumulate another preview request",
  );
  await act(async () => {
    api.resolveFirstPreview();
    for (let step = 0; step < 6; ++step) {
      await Promise.resolve();
    }
  });
  const firstImage = await screen.findByRole("img", {
    name: "Across-the-line phone setup preview",
  });
  const firstSource = firstImage.getAttribute("src");
  assert.ok(firstSource);
  assert.equal(firstSource, "blob:setup-preview-1");
  assert.match(api.previewUrls[0] ?? "", /existing=1&swing_preview_request=\d+-0#framing$/);
  assert.ok(firstImage.parentElement?.classList.contains("rotation-90"));
  assert.equal(role.value, "face_on", "preview readiness must not overwrite the setup draft");
  assert.ok(screen.getByText("Loading low-rate standby preview…"));
  fireEvent.load(firstImage);
  assert.ok(screen.getByText(/Live low-rate standby preview/));

  await scheduler.runNext(1_000, act);
  const secondImage = screen.getByRole("img", {
    name: "Across-the-line phone setup preview",
  });
  assert.notEqual(secondImage.getAttribute("src"), firstSource, "each request bypasses caches");
  fireEvent.error(secondImage);
  assert.ok(screen.getByText(/last frame may be stale.*Retrying automatically/));

  await scheduler.runNext(1_000, act);
  const recoveredImage = screen.getByRole("img", {
    name: "Across-the-line phone setup preview",
  });
  assert.notEqual(recoveredImage.getAttribute("src"), secondImage.getAttribute("src"));
  fireEvent.load(recoveredImage);
  assert.ok(screen.getByText(/Live low-rate standby preview/));

  assert.equal(
    api.setupRequests,
    3,
    "background setup requests stop once a preview URL is available",
  );
  await waitFor(() => assert.equal(scheduler.pendingWithDelay(3_000), 0));
  cleanup();
  assert.equal(scheduler.pending(), 0, "unmount cancels preview and setup refresh timers");
  assert.equal(objectUrls.active.size, 0, "unmount revokes the final preview object URL");
}

async function authenticatedPreviewRequestsStayOnThePhoneOrigin() {
  const calls: Array<{ url: string; init: RequestInit }> = [];
  const fetcher = (async (input: RequestInfo | URL, init?: RequestInit) => {
    calls.push({ url: String(input), init: init ?? {} });
    return new Response(new Uint8Array([0xff, 0xd8, 0xff, 0xd9]), {
      headers: { "Content-Type": "image/jpeg" },
    });
  }) as typeof fetch;
  const api = new HttpNodeSetupApi("http://pixel.test:8088", "node-secret", fetcher);
  const preview = await api.getSetupPreview("/api/v1/setup/preview?swing_preview_request=1-0");
  assert.equal(preview.type, "image/jpeg");
  assert.equal(
    calls[0]?.url,
    "http://pixel.test:8088/api/v1/setup/preview?swing_preview_request=1-0",
  );
  assert.equal(new Headers(calls[0]?.init.headers).get("Authorization"), "Bearer node-secret");
  assert.equal(new Headers(calls[0]?.init.headers).get("Accept"), "image/jpeg");
  assert.equal(calls[0]?.init.cache, "no-store");
  assert.equal(calls[0]?.init.redirect, "error");
  await assert.rejects(
    () => api.getSetupPreview("http://other-phone.test:8088/api/v1/setup/preview"),
    /Refusing to send.*credential.*another origin/,
  );
  assert.equal(calls.length, 1, "cross-origin preview rejection happens before fetch");

  const stale = parseNodeSetupSnapshot({
    ...fixtureNodeSetup(),
    preview: {
      available: false,
      url: null,
      state: "stale",
      reason: "stale",
      frame_age_ms: 3_250.4,
      image_rotation_degrees: 90,
    },
  });
  assert.equal(stale.preview.state, "stale");
  assert.equal(stale.preview.image_rotation_degrees, 90);
  assert.throws(
    () =>
      parseNodeSetupSnapshot({
        ...fixtureNodeSetup(),
        preview: {
          available: true,
          url: "/api/v1/setup/preview",
          state: "stale",
        },
      }),
    /availability must agree with preview state/,
  );
}

class PreviewTransitionApi implements NodeSetupApi {
  readonly displayOrigin = "http://pixel-6-pro.test:8088";
  setupRequests = 0;
  previewRequests = 0;
  readonly previewUrls: string[] = [];
  #resolveFirstPreview: ((blob: Blob) => void) | null = null;
  readonly #unavailable: NodeSetupSnapshot;
  readonly #available: NodeSetupSnapshot;

  constructor() {
    this.#unavailable = fixtureNodeSetup(
      this.displayOrigin,
      "down_the_line",
      "Pixel 6 Pro",
      "disabled",
      null,
    );
    this.#available = {
      ...this.#unavailable,
      revision: this.#unavailable.revision + 1,
      configuration: {
        ...this.#unavailable.configuration,
        role: "unassigned",
      },
      preview: {
        available: true,
        url: `${this.displayOrigin}/api/v1/setup/preview.jpg?existing=1#framing`,
        state: "available",
        reason: null,
        frame_age_ms: 100,
        image_rotation_degrees: 90,
      },
    };
  }

  getSetupPreview(url: string): Promise<Blob> {
    ++this.previewRequests;
    this.previewUrls.push(url);
    if (this.previewRequests === 1) {
      return new Promise((resolve) => {
        this.#resolveFirstPreview = resolve;
      });
    }
    return Promise.resolve(new Blob([`jpeg-${this.previewRequests}`], { type: "image/jpeg" }));
  }

  resolveFirstPreview(): void {
    const resolve = this.#resolveFirstPreview;
    assert.ok(resolve, "first preview request must be pending");
    this.#resolveFirstPreview = null;
    resolve(new Blob(["jpeg-1"], { type: "image/jpeg" }));
  }

  getSetup(): Promise<NodeSetupSnapshot> {
    ++this.setupRequests;
    if (this.setupRequests === 2) {
      return Promise.reject(new Error("synthetic preview status failure"));
    }
    return Promise.resolve(
      structuredClone(this.setupRequests >= 3 ? this.#available : this.#unavailable),
    );
  }

  updateSetup(
    _expectedRevision: number,
    _configuration: NodeSetupConfigurationUpdate,
  ): Promise<NodeSetupSnapshot> {
    return Promise.reject(new Error("not used"));
  }
}

class RecordingObjectUrls implements ObjectUrlFactory {
  readonly active = new Set<string>();
  #next = 1;

  createObjectURL(_blob: Blob): string {
    const url = `blob:setup-preview-${this.#next++}`;
    this.active.add(url);
    return url;
  }

  revokeObjectURL(url: string): void {
    assert.equal(this.active.delete(url), true, `object URL ${url} must be owned exactly once`);
  }
}

class ManualScheduler implements StatusSubscriptionScheduler {
  readonly #tasks: Array<{ callback: () => void; delayMs: number; cancelled: boolean }> = [];

  setTimeout(callback: () => void, delayMs: number): unknown {
    const task = { callback, delayMs, cancelled: false };
    this.#tasks.push(task);
    return task;
  }

  clearTimeout(handle: unknown): void {
    (handle as { cancelled: boolean }).cancelled = true;
  }

  async runNext(
    delayMs: number,
    act: (callback: () => Promise<void>) => Promise<void>,
  ): Promise<void> {
    const index = this.#tasks.findIndex(
      (candidate) => !candidate.cancelled && candidate.delayMs === delayMs,
    );
    assert.notEqual(index, -1, `expected a ${delayMs} ms scheduled task`);
    const [task] = this.#tasks.splice(index, 1);
    assert.ok(task);
    await act(async () => {
      task.callback();
      for (let step = 0; step < 6; ++step) {
        await Promise.resolve();
      }
    });
  }

  pending(): number {
    return this.#tasks.filter((task) => !task.cancelled).length;
  }

  pendingWithDelay(delayMs: number): number {
    return this.#tasks.filter((task) => !task.cancelled && task.delayMs === delayMs).length;
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
