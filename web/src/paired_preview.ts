import type { CameraStatus, CameraRole, PreviewFetchTelemetry, StationApi } from "./api.js";

export const PREVIEW_DELAY_THRESHOLD_MS = 500;

export interface PreviewPairRequest {
  down_the_line: number;
  face_on: number;
}

export interface PreviewPair {
  sequences: PreviewPairRequest;
  urls: Record<CameraRole, string>;
  telemetry: PreviewPairTelemetry;
}

export type PreviewDelayStage =
  | "nominal"
  | "capture"
  | "capture_sink"
  | "sampling"
  | "render"
  | "http_server"
  | "http_transport_or_dispatch"
  | "response_body"
  | "browser_decode"
  | "browser_backpressure"
  | "status_poll_or_browser_scheduling";

export interface PreviewRoleTelemetry {
  fetch: PreviewFetchTelemetry;
  decode_ms: number;
}

export interface PreviewPairTelemetry {
  queue_wait_ms: number;
  total_ms: number;
  presented_gap_ms: number | null;
  roles: Record<CameraRole, PreviewRoleTelemetry>;
  attributed_stage: PreviewDelayStage;
  attributed_role: CameraRole | null;
  attributed_ms: number;
}

export interface PreviewPipelineStall {
  schema_version: 1;
  observed_at_epoch_ms: number;
  stage: Exclude<PreviewPipelineStage, "nominal">;
  role: CameraRole;
  preview_sequence: number;
  latest_capture_frame_id: string;
  latest_sink_frame_id: string;
  latest_sampler_frame_id: string;
  sampled_sequence: number;
  latest_capture_age_ms: number;
  latest_sink_completion_age_ms: number;
  latest_sampler_completion_age_ms: number;
  sampled_age_ms: number;
  source_age_ms: number;
  rendered_age_ms: number;
  render_queue_ms: number;
  renderer_stage: CameraStatus["preview_performance"]["renderer_stage"];
  render_pending: boolean;
  attributed_ms: number;
  roles: Record<CameraRole, PreviewPipelineRoleEvidence | null>;
}

export type PreviewPipelineStage = "nominal" | "capture" | "capture_sink" | "sampling" | "render";

export interface PreviewPipelineRoleEvidence {
  stage: PreviewPipelineStage;
  attributed_ms: number;
  preview_sequence: number;
  latest_capture_frame_id: string;
  latest_sink_frame_id: string;
  latest_sampler_frame_id: string;
  sampled_sequence: number;
  latest_capture_age_ms: number;
  latest_sink_completion_age_ms: number;
  latest_sampler_completion_age_ms: number;
  sampled_age_ms: number;
  source_age_ms: number;
  rendered_age_ms: number;
  render_queue_ms: number;
  renderer_stage: CameraStatus["preview_performance"]["renderer_stage"];
  render_pending: boolean;
}

export interface ObjectUrlFactory {
  createObjectURL(blob: Blob): string;
  revokeObjectURL(url: string): void;
}

export type PreviewImageDecoder = (url: string) => Promise<void>;

type PairHandler = (pair: PreviewPair) => void;
type ErrorHandler = (error: Error) => void;
type MonotonicClock = () => number;

interface PendingPairRequest {
  request: PreviewPairRequest;
  enqueued_at_ms: number;
}

function sameRequest(left: PreviewPairRequest | null, right: PreviewPairRequest): boolean {
  return (
    left !== null && left.down_the_line === right.down_the_line && left.face_on === right.face_on
  );
}

function errorMessage(caught: unknown): Error {
  return caught instanceof Error ? caught : new Error("Unknown preview-pair failure");
}

async function decodeImageUrl(url: string): Promise<void> {
  const image = document.createElement("img");
  image.src = url;
  if (typeof image.decode === "function") {
    await image.decode();
  }
}

export class PairedPreviewLoader {
  readonly #api: StationApi;
  readonly #onPair: PairHandler;
  readonly #onError: ErrorHandler;
  readonly #objectUrls: ObjectUrlFactory;
  readonly #decodeImage: PreviewImageDecoder;
  readonly #clock: MonotonicClock;
  #currentRequest: PreviewPairRequest | null = null;
  #inFlightRequest: PendingPairRequest | null = null;
  #pendingRequest: PendingPairRequest | null = null;
  #currentUrls: string[] = [];
  #retiredUrls: string[] = [];
  #decodingUrls: string[] = [];
  #running = false;
  #stopped = false;
  #lastPresentedAtMs: number | null = null;

  constructor(
    api: StationApi,
    onPair: PairHandler,
    onError: ErrorHandler,
    objectUrls: ObjectUrlFactory = URL,
    decodeImage: PreviewImageDecoder = decodeImageUrl,
    clock: MonotonicClock = () => globalThis.performance.now(),
  ) {
    this.#api = api;
    this.#onPair = onPair;
    this.#onError = onError;
    this.#objectUrls = objectUrls;
    this.#decodeImage = decodeImage;
    this.#clock = clock;
  }

  enqueue(request: PreviewPairRequest): void {
    if (
      this.#stopped ||
      request.down_the_line <= 0 ||
      request.face_on <= 0 ||
      sameRequest(this.#currentRequest, request) ||
      sameRequest(this.#inFlightRequest?.request ?? null, request) ||
      sameRequest(this.#pendingRequest?.request ?? null, request)
    ) {
      return;
    }
    this.#pendingRequest = { request, enqueued_at_ms: this.#clock() };
    void this.#pump();
  }

  stop(): void {
    if (this.#stopped) {
      return;
    }
    this.#stopped = true;
    this.#pendingRequest = null;
    this.#revoke(this.#retiredUrls);
    this.#revoke(this.#currentUrls);
    this.#revoke(this.#decodingUrls);
    this.#retiredUrls = [];
    this.#currentUrls = [];
    this.#decodingUrls = [];
  }

  async #pump(): Promise<void> {
    if (this.#running) {
      return;
    }
    this.#running = true;
    try {
      while (!this.#stopped && this.#pendingRequest !== null) {
        const pending = this.#pendingRequest;
        this.#pendingRequest = null;
        this.#inFlightRequest = pending;
        const attemptStartedAt = this.#clock();
        try {
          const [downTheLine, faceOn] = await Promise.all([
            this.#api.getPreview("down_the_line", pending.request.down_the_line),
            this.#api.getPreview("face_on", pending.request.face_on),
          ]);
          if (this.#stopped) {
            return;
          }
          const nextUrls = this.#createUrls(downTheLine.image, faceOn.image);
          this.#decodingUrls = Object.values(nextUrls);
          const decodeStartedAt = this.#clock();
          let downTheLineDecodeMs = 0;
          let faceOnDecodeMs = 0;
          try {
            await Promise.all([
              this.#decodeImage(nextUrls.down_the_line).then(() => {
                downTheLineDecodeMs = Math.max(0, this.#clock() - decodeStartedAt);
              }),
              this.#decodeImage(nextUrls.face_on).then(() => {
                faceOnDecodeMs = Math.max(0, this.#clock() - decodeStartedAt);
              }),
            ]);
          } catch (caught) {
            this.#revoke(this.#decodingUrls);
            this.#decodingUrls = [];
            throw caught;
          }
          if (this.#stopped) {
            return;
          }
          this.#revoke(this.#retiredUrls);
          this.#decodingUrls = [];
          this.#retiredUrls = this.#currentUrls;
          this.#currentUrls = Object.values(nextUrls);
          this.#currentRequest = pending.request;
          const presentedAt = this.#clock();
          const evidence = {
            queue_wait_ms: Math.max(0, attemptStartedAt - pending.enqueued_at_ms),
            total_ms: Math.max(0, presentedAt - pending.enqueued_at_ms),
            presented_gap_ms:
              this.#lastPresentedAtMs === null
                ? null
                : Math.max(0, presentedAt - this.#lastPresentedAtMs),
            roles: {
              down_the_line: {
                fetch: downTheLine.telemetry,
                decode_ms: downTheLineDecodeMs,
              },
              face_on: {
                fetch: faceOn.telemetry,
                decode_ms: faceOnDecodeMs,
              },
            },
          };
          const attribution = attributePreviewDelay(evidence);
          this.#lastPresentedAtMs = presentedAt;
          this.#onPair({
            sequences: pending.request,
            urls: nextUrls,
            telemetry: { ...evidence, ...attribution },
          });
        } catch (caught) {
          if (!this.#stopped) {
            const elapsed = Math.max(0, this.#clock() - attemptStartedAt);
            const error = errorMessage(caught);
            this.#onError(
              new Error(`${error.message} (paired preview attempt ${elapsed.toFixed(1)} ms)`),
            );
          }
        } finally {
          this.#inFlightRequest = null;
        }
      }
    } finally {
      this.#running = false;
      if (!this.#stopped && this.#pendingRequest !== null) {
        void this.#pump();
      }
    }
  }

  #createUrls(downTheLine: Blob, faceOn: Blob): Record<CameraRole, string> {
    const downTheLineUrl = this.#objectUrls.createObjectURL(downTheLine);
    try {
      return {
        down_the_line: downTheLineUrl,
        face_on: this.#objectUrls.createObjectURL(faceOn),
      };
    } catch (caught) {
      this.#objectUrls.revokeObjectURL(downTheLineUrl);
      throw caught;
    }
  }

  #revoke(urls: string[]): void {
    for (const url of urls) {
      this.#objectUrls.revokeObjectURL(url);
    }
  }
}

interface PreviewDelayEvidence {
  queue_wait_ms: number;
  total_ms: number;
  presented_gap_ms: number | null;
  roles: Record<CameraRole, PreviewRoleTelemetry>;
}

interface PreviewDelayAttribution {
  attributed_stage: PreviewDelayStage;
  attributed_role: CameraRole | null;
  attributed_ms: number;
}

export function attributePreviewDelay(
  evidence: PreviewDelayEvidence,
  thresholdMs = PREVIEW_DELAY_THRESHOLD_MS,
): PreviewDelayAttribution {
  const presentedGap = evidence.presented_gap_ms ?? 0;
  if (evidence.total_ms < thresholdMs && presentedGap < thresholdMs) {
    return { attributed_stage: "nominal", attributed_role: null, attributed_ms: 0 };
  }

  const roles: CameraRole[] = ["down_the_line", "face_on"];
  const candidates: Array<PreviewDelayAttribution> = [
    {
      attributed_stage: "browser_backpressure",
      attributed_role: null,
      attributed_ms: evidence.queue_wait_ms,
    },
  ];
  for (const role of roles) {
    const roleTelemetry = evidence.roles[role];
    const server = roleTelemetry.fetch.server;
    const serverHandler = server.handler_ms ?? 0;
    let pipelineCandidate: PreviewDelayAttribution | null = null;
    if (
      server.latest_capture_frame_id !== null &&
      server.latest_sink_frame_id !== null &&
      server.latest_capture_frame_id !== server.latest_sink_frame_id &&
      (server.latest_capture_age_ms ?? 0) >= thresholdMs
    ) {
      pipelineCandidate = {
        attributed_stage: "capture_sink",
        attributed_role: role,
        attributed_ms: server.latest_capture_age_ms ?? 0,
      };
    } else if (
      server.latest_sink_frame_id !== null &&
      server.latest_sampler_frame_id !== null &&
      server.latest_sink_frame_id !== server.latest_sampler_frame_id &&
      (server.latest_sink_completion_age_ms ?? 0) >= thresholdMs
    ) {
      pipelineCandidate = {
        attributed_stage: "sampling",
        attributed_role: role,
        attributed_ms: server.latest_sink_completion_age_ms ?? 0,
      };
    } else if ((server.latest_capture_age_ms ?? 0) >= thresholdMs) {
      pipelineCandidate = {
        attributed_stage: "capture",
        attributed_role: role,
        attributed_ms: server.latest_capture_age_ms ?? 0,
      };
    } else if ((server.sampled_age_ms ?? 0) >= thresholdMs) {
      pipelineCandidate = {
        attributed_stage: "sampling",
        attributed_role: role,
        attributed_ms: server.sampled_age_ms ?? 0,
      };
    } else if ((server.source_age_ms ?? 0) >= thresholdMs) {
      pipelineCandidate = {
        attributed_stage: "render",
        attributed_role: role,
        attributed_ms: server.source_age_ms ?? 0,
      };
    }
    if (pipelineCandidate !== null) {
      candidates.push(pipelineCandidate);
    }
    candidates.push(
      {
        attributed_stage: "http_server",
        attributed_role: role,
        attributed_ms: serverHandler,
      },
      {
        attributed_stage: "http_transport_or_dispatch",
        attributed_role: role,
        attributed_ms: Math.max(0, roleTelemetry.fetch.headers_ms - serverHandler),
      },
      {
        attributed_stage: "response_body",
        attributed_role: role,
        attributed_ms: roleTelemetry.fetch.body_ms,
      },
      {
        attributed_stage: "browser_decode",
        attributed_role: role,
        attributed_ms: roleTelemetry.decode_ms,
      },
    );
  }
  const maximum = candidates.reduce((left, right) =>
    right.attributed_ms > left.attributed_ms ? right : left,
  );
  // A stage label is a best-fit heuristic. Choose the strongest concurrent
  // threshold crossing rather than letting a barely-stale upstream counter
  // hide a substantially larger HTTP, decode, or second-camera delay.
  if (maximum.attributed_ms >= thresholdMs) {
    return maximum;
  }
  return {
    attributed_stage: "status_poll_or_browser_scheduling",
    attributed_role: null,
    attributed_ms: presentedGap,
  };
}

export function attributePreviewPipelineStall(
  cameras: CameraStatus[],
  observedAtEpochMs = Date.now(),
  thresholdMs = PREVIEW_DELAY_THRESHOLD_MS,
): PreviewPipelineStall | null {
  const roleEvidence: Record<CameraRole, PreviewPipelineRoleEvidence | null> = {
    down_the_line: null,
    face_on: null,
  };
  const candidates: Array<{ role: CameraRole; evidence: PreviewPipelineRoleEvidence }> = [];
  for (const camera of cameras) {
    if (!camera.connected || camera.preview_sequence <= 0) {
      continue;
    }
    const performance = camera.preview_performance;
    let stage: PreviewPipelineStage = "nominal";
    let attributedMs = 0;
    if (
      performance.latest_capture_frame_id !== "0" &&
      performance.latest_capture_frame_id !== performance.latest_sink_frame_id &&
      performance.latest_capture_age_ms >= thresholdMs
    ) {
      stage = "capture_sink";
      attributedMs = performance.latest_capture_age_ms;
    } else if (
      performance.latest_sink_frame_id !== "0" &&
      performance.latest_sink_frame_id !== performance.latest_sampler_frame_id &&
      performance.latest_sink_completion_age_ms >= thresholdMs
    ) {
      stage = "sampling";
      attributedMs = performance.latest_sink_completion_age_ms;
    } else if (
      performance.latest_capture_frame_id !== "0" &&
      performance.latest_capture_age_ms >= thresholdMs
    ) {
      stage = "capture";
      attributedMs = performance.latest_capture_age_ms;
    } else if (performance.sampled_sequence > 0 && performance.sampled_age_ms >= thresholdMs) {
      stage = "sampling";
      attributedMs = performance.sampled_age_ms;
    } else if (performance.source_age_ms >= thresholdMs) {
      stage = "render";
      attributedMs = performance.source_age_ms;
    }
    const roleSnapshot: PreviewPipelineRoleEvidence = {
      stage,
      attributed_ms: attributedMs,
      preview_sequence: camera.preview_sequence,
      latest_capture_frame_id: performance.latest_capture_frame_id,
      latest_sink_frame_id: performance.latest_sink_frame_id,
      latest_sampler_frame_id: performance.latest_sampler_frame_id,
      sampled_sequence: performance.sampled_sequence,
      latest_capture_age_ms: performance.latest_capture_age_ms,
      latest_sink_completion_age_ms: performance.latest_sink_completion_age_ms,
      latest_sampler_completion_age_ms: performance.latest_sampler_completion_age_ms,
      sampled_age_ms: performance.sampled_age_ms,
      source_age_ms: performance.source_age_ms,
      rendered_age_ms: performance.rendered_age_ms,
      render_queue_ms: performance.render_queue_ms,
      renderer_stage: performance.renderer_stage,
      render_pending: performance.render_pending,
    };
    roleEvidence[camera.role] = roleSnapshot;
    if (stage !== "nominal") {
      candidates.push({ role: camera.role, evidence: roleSnapshot });
    }
  }
  const maximum = candidates.reduce<(typeof candidates)[number] | null>(
    (current, candidate) =>
      current === null || candidate.evidence.attributed_ms > current.evidence.attributed_ms
        ? candidate
        : current,
    null,
  );
  if (maximum === null) {
    return null;
  }
  const selectedStage = maximum.evidence.stage;
  if (selectedStage === "nominal") {
    return null;
  }
  return {
    schema_version: 1,
    observed_at_epoch_ms: observedAtEpochMs,
    role: maximum.role,
    ...maximum.evidence,
    stage: selectedStage,
    roles: roleEvidence,
  };
}

export function samePreviewPipelineStall(
  left: PreviewPipelineStall | null,
  right: PreviewPipelineStall,
): boolean {
  // A sampling or render stall can keep receiving upstream frames. The fixed
  // rendered sequence identifies one threshold crossing and prevents a 30 Hz
  // status poll from rewriting session storage throughout the same stall.
  return (
    left !== null &&
    left.stage === right.stage &&
    left.role === right.role &&
    left.preview_sequence === right.preview_sequence
  );
}
