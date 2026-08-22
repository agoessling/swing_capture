import type {
  NodeSetupApi,
  NodeSetupConfigurationUpdate,
  NodeSetupSnapshot,
} from "./node_setup_api.js";

export class FakeNodeSetupApi implements NodeSetupApi {
  readonly displayOrigin: string;
  #setup: NodeSetupSnapshot;

  constructor(
    displayOrigin: string,
    role: "down_the_line" | "face_on" | "unassigned" = "down_the_line",
    model = "Pixel 6 Pro",
    poseMode?: "disabled" | "shadow" | "leader",
    peerOrigin?: string | null,
  ) {
    this.displayOrigin = displayOrigin;
    this.#setup = fixtureNodeSetup(displayOrigin, role, model, poseMode, peerOrigin);
  }

  getSetup(): Promise<NodeSetupSnapshot> {
    return Promise.resolve(structuredClone(this.#setup));
  }

  updateSetup(
    expectedRevision: number,
    configuration: NodeSetupConfigurationUpdate,
  ): Promise<NodeSetupSnapshot> {
    if (expectedRevision !== this.#setup.revision) {
      return Promise.reject(new Error("setup configuration changed; reload before saving"));
    }
    const existingPeer = this.#setup.configuration.pose.peer;
    const peerUpdate = configuration.pose.peer_update;
    this.#setup = {
      ...this.#setup,
      revision: this.#setup.revision + 1,
      configuration: {
        role: configuration.role,
        capture_profile: configuration.capture_profile,
        pose: {
          mode: configuration.pose.mode,
          inference_delegate: configuration.pose.inference_delegate,
          debug_evidence_enabled: configuration.pose.debug_evidence_enabled,
          hitting_region: structuredClone(configuration.pose.hitting_region),
          peer:
            peerUpdate.operation === "keep"
              ? existingPeer
              : peerUpdate.operation === "clear"
                ? null
                : { origin: peerUpdate.origin },
        },
      },
      readiness: {
        ...this.#setup.readiness,
        issues: configuration.role === "unassigned" ? ["Assign a camera view before capture."] : [],
      },
    };
    return Promise.resolve(structuredClone(this.#setup));
  }
}

export function fixtureNodeSetup(
  origin = "http://pixel-6.test:8088",
  role: "down_the_line" | "face_on" | "unassigned" = "down_the_line",
  model = "Pixel 6 Pro",
  poseMode?: "disabled" | "shadow" | "leader",
  peerOrigin?: string | null,
): NodeSetupSnapshot {
  const configuredPoseMode = poseMode ?? (role === "down_the_line" ? "leader" : "shadow");
  return {
    schema_version: 1,
    revision: 4,
    node: {
      node_id: role === "face_on" ? "fixture-pixel-5a-node" : "fixture-pixel-6-pro-node",
      service_urls: [origin],
      device_model: model,
    },
    capabilities: {
      roles: [
        { value: "unassigned", label: "Unassigned" },
        { value: "down_the_line", label: "Down the line" },
        { value: "face_on", label: "Across the line (face-on)" },
      ],
      capture_profiles: [
        {
          value: "720p240",
          label: "Standard: 1280×720 / 240 fps",
          width: 1280,
          height: 720,
          fps: 240,
        },
        {
          value: "1080p240",
          label: "Optional: 1920×1080 / 240 fps",
          width: 1920,
          height: 1080,
          fps: 240,
        },
      ],
      pose_modes: [
        { value: "disabled", label: "Pose monitoring disabled" },
        { value: "shadow", label: "Shadow inference (record decisions only)" },
        { value: "leader", label: "Pose leader (arm both phones)" },
      ],
      inference_delegates: [
        { value: "gpu_preferred", label: "GPU preferred" },
        { value: "gpu_required", label: "GPU required" },
        { value: "cpu_only", label: "CPU only" },
      ],
    },
    configuration: {
      role,
      capture_profile: "720p240",
      pose: {
        mode: configuredPoseMode,
        inference_delegate: "gpu_preferred",
        debug_evidence_enabled: true,
        hitting_region: { left: 0.15, top: 0.3, right: 0.85, bottom: 1 },
        peer:
          peerOrigin === null
            ? null
            : peerOrigin !== undefined
              ? { origin: peerOrigin }
              : configuredPoseMode === "leader"
                ? {
                    origin:
                      role === "face_on"
                        ? "http://pixel-6-pro.test:8088"
                        : "http://pixel-5a.test:8088",
                  }
                : null,
      },
    },
    readiness: {
      editable: true,
      capture_state: "setup",
      issues: role === "unassigned" ? ["Assign a camera view before capture."] : [],
    },
    preview: { available: false, url: null },
  };
}
