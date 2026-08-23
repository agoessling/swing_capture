import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type {
  DualFieldRecordingStatus,
  FieldRecording,
  FieldRecordingList,
  FieldRecordingNodeStatus,
  ReviewApi,
  ReviewRole,
} from "./review_api.js";

export type FieldRecordingApi = Required<
  Pick<
    ReviewApi,
    "getFieldRecordingStatus" | "startFieldRecording" | "stopFieldRecording" | "getFieldRecordings"
  >
>;

export function supportsFieldRecording(api: ReviewApi): api is ReviewApi & FieldRecordingApi {
  return (
    api.getFieldRecordingStatus !== undefined &&
    api.startFieldRecording !== undefined &&
    api.stopFieldRecording !== undefined &&
    api.getFieldRecordings !== undefined
  );
}

export function FieldRecordingPanel({
  api,
  pollIntervalMs = 1_000,
}: {
  api: FieldRecordingApi;
  pollIntervalMs?: number;
}) {
  const [status, setStatus] = useState<DualFieldRecordingStatus | null>(null);
  const [catalog, setCatalog] = useState<FieldRecordingList>({ recordings: [] });
  const [catalogState, setCatalogState] = useState<"loading" | "ready" | "error">("loading");
  const [statusLoadError, setStatusLoadError] = useState<string | null>(null);
  const [catalogLoadError, setCatalogLoadError] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [action, setAction] = useState<"start" | "stop" | null>(null);
  const catalogRefreshRequest = useRef<Promise<void> | null>(null);
  const catalogRefreshAgain = useRef(false);

  const refreshStatus = useCallback(async () => {
    try {
      setStatus(await api.getFieldRecordingStatus());
      setStatusLoadError(null);
    } catch (caught) {
      setStatusLoadError(errorMessage(caught));
    }
  }, [api]);

  const refreshCatalog = useCallback((): Promise<void> => {
    if (catalogRefreshRequest.current !== null) {
      // A completed recording needs a fresh catalog even if an older background read is still in
      // flight. Queue one trailing read without adding another contending request immediately.
      catalogRefreshAgain.current = true;
      return catalogRefreshRequest.current;
    }
    const request = (async () => {
      do {
        catalogRefreshAgain.current = false;
        try {
          setCatalog(await api.getFieldRecordings());
          setCatalogState("ready");
          setCatalogLoadError(null);
        } catch (caught) {
          setCatalogState("error");
          setCatalogLoadError(errorMessage(caught));
        }
      } while (catalogRefreshAgain.current);
    })();
    catalogRefreshRequest.current = request;
    void request.finally(() => {
      if (catalogRefreshRequest.current === request) {
        catalogRefreshRequest.current = null;
      }
    });
    return request;
  }, [api]);

  useEffect(() => {
    let active = true;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const poll = async () => {
      await refreshStatus();
      if (active) {
        timer = setTimeout(() => void poll(), pollIntervalMs);
      }
    };
    void poll();
    return () => {
      active = false;
      if (timer !== undefined) {
        clearTimeout(timer);
      }
    };
  }, [pollIntervalMs, refreshStatus]);

  const liveStatusReady = status !== null;
  useEffect(() => {
    if (!liveStatusReady) {
      return;
    }
    let active = true;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const poll = async () => {
      await refreshCatalog();
      if (active) {
        // The catalog is immutable history, not live control state. Keep it fresh for another
        // browser without rereading every retained manifest on every one-second status poll.
        timer = setTimeout(() => void poll(), Math.max(15_000, pollIntervalMs));
      }
    };
    void poll();
    return () => {
      active = false;
      if (timer !== undefined) {
        clearTimeout(timer);
      }
    };
  }, [liveStatusReady, pollIntervalMs, refreshCatalog]);

  const runAction = async (nextAction: "start" | "stop") => {
    setAction(nextAction);
    setActionError(null);
    try {
      const nextStatus =
        nextAction === "start" ? await api.startFieldRecording() : await api.stopFieldRecording();
      setStatus(nextStatus);
      if (nextAction === "stop") {
        // Publication completion is live control state. Do not leave Stop pending behind an
        // unrelated slow history read; refreshCatalog queues one trailing catalog read if needed.
        void refreshCatalog();
      }
    } catch (caught) {
      setActionError(errorMessage(caught));
    } finally {
      setAction(null);
    }
  };

  const recording = status?.nodes.some((node) => isRecording(node)) ?? false;
  const allReadyToStart =
    status?.nodes.length === 2 && status.nodes.every((node) => canStart(node));
  const durationMs = Math.max(0, ...(status?.nodes.map((node) => node.elapsed_ms) ?? []));
  const recordings = useMemo(
    () => [...catalog.recordings].sort((left, right) => recordingOrder(left, right)),
    [catalog],
  );
  const visibleError = actionError ?? statusLoadError ?? catalogLoadError;

  return (
    <section
      aria-labelledby="field-recording-heading"
      className="field-recording-panel"
      data-recording-catalog-state={catalogState}
    >
      <header className="field-recording-header">
        <div>
          <p className="section-kicker">Field dataset · 720p/30 fps + WAV</p>
          <h2 id="field-recording-heading">Continuous test recording</h2>
          <p>
            Records normal-speed video and raw audio on both phones for offline pose and impact
            analysis. Starting this mode safely disarms high-speed capture first.
          </p>
        </div>
        <div className="field-recording-actions">
          <div aria-live="polite" className={`field-recording-clock${recording ? " active" : ""}`}>
            <span>{recording ? "Recording" : status === null ? "Connecting" : "Stopped"}</span>
            <strong>{formatDuration(durationMs)}</strong>
          </div>
          {recording ? (
            <button disabled={action !== null} onClick={() => void runAction("stop")} type="button">
              {action === "stop" ? "Stopping both phones…" : "Stop both phones"}
            </button>
          ) : (
            <button
              disabled={action !== null || !allReadyToStart}
              onClick={() => void runAction("start")}
              type="button"
            >
              {action === "start" ? "Starting both phones…" : "Start field recording"}
            </button>
          )}
        </div>
      </header>

      {visibleError === null ? null : (
        <p className="review-error field-recording-error" role="alert">
          {visibleError}
        </p>
      )}

      <div className="field-node-grid">
        {status === null ? (
          <p className="field-recording-loading">Checking both phones…</p>
        ) : (
          status.nodes.map((node) => <FieldNodeStatus key={node.role} node={node} />)
        )}
      </div>

      {recordings.length === 0 ? null : (
        <details className="field-recording-library">
          <summary>
            Completed recordings <span>{recordings.length}</span>
          </summary>
          <div className="field-recording-list">
            {recordings.map((item) => (
              <article key={`${item.origin}-${item.recording_id}`}>
                <div>
                  <strong>{roleLabel(item.role)}</strong>
                  <span>{formatTimestamp(item.created_at_utc)}</span>
                  <small>
                    {formatDuration(Number(BigInt(item.duration_us) / 1_000n))} ·{" "}
                    {formatBytes(item.video_bytes)} video
                  </small>
                </div>
                <div className="field-recording-downloads">
                  <a download href={item.video_url}>
                    Video
                  </a>
                  <a download href={item.audio_url}>
                    Audio
                  </a>
                  <a href={item.manifest_url}>Manifest</a>
                </div>
              </article>
            ))}
          </div>
        </details>
      )}
    </section>
  );
}

function FieldNodeStatus({ node }: { node: FieldRecordingNodeStatus }) {
  return (
    <article className={`field-node field-node-${node.state}`}>
      <div>
        <span className="field-node-state">{stateLabel(node.state)}</span>
        <strong>{roleLabel(node.role)}</strong>
        <small>{node.origin}</small>
      </div>
      <dl>
        <div>
          <dt>Elapsed</dt>
          <dd>{formatDuration(node.elapsed_ms)}</dd>
        </div>
        <div>
          <dt>Video</dt>
          <dd>{formatBytes(node.video_bytes)}</dd>
        </div>
        <div>
          <dt>Audio</dt>
          <dd>{formatCount(node.audio_frames)} frames</dd>
        </div>
        <div>
          <dt>Limit</dt>
          <dd>{formatDuration(node.max_duration_seconds * 1_000)}</dd>
        </div>
      </dl>
      {node.error.length === 0 ? null : <p role="alert">{node.error}</p>}
    </article>
  );
}

function isRecording(node: FieldRecordingNodeStatus): boolean {
  return node.state === "starting" || node.state === "recording" || node.state === "stopping";
}

function canStart(node: FieldRecordingNodeStatus): boolean {
  // The Android service permits a new recorder to replace a terminal failed instance. Keeping the
  // action available lets an operator retry a transient camera/encoder failure without restarting
  // the application; active and publishing states remain non-startable.
  return node.state === "idle" || node.state === "ready" || node.state === "error";
}

function recordingOrder(left: FieldRecording, right: FieldRecording): number {
  const timestampOrder = right.created_at_utc.localeCompare(left.created_at_utc);
  return timestampOrder === 0 ? roleOrder(left.role) - roleOrder(right.role) : timestampOrder;
}

function roleOrder(role: ReviewRole): number {
  return role === "down_the_line" ? 0 : 1;
}

function roleLabel(role: ReviewRole): string {
  return role === "down_the_line" ? "Down the line" : "Face on";
}

function stateLabel(state: FieldRecordingNodeStatus["state"]): string {
  return state.charAt(0).toUpperCase() + state.slice(1);
}

function formatDuration(milliseconds: number): string {
  const seconds = Math.max(0, Math.floor(milliseconds / 1_000));
  const hours = Math.floor(seconds / 3_600);
  const minutes = Math.floor((seconds % 3_600) / 60);
  const remainder = seconds % 60;
  return hours > 0
    ? `${String(hours)}:${String(minutes).padStart(2, "0")}:${String(remainder).padStart(2, "0")}`
    : `${String(minutes).padStart(2, "0")}:${String(remainder).padStart(2, "0")}`;
}

function formatBytes(value: string): string {
  const bytes = BigInt(value);
  if (bytes < 1_000n) {
    return `${bytes.toString()} B`;
  }
  if (bytes < 1_000_000n) {
    return `${(Number(bytes) / 1_000).toFixed(1)} kB`;
  }
  return `${(Number(bytes) / 1_000_000).toFixed(1)} MB`;
}

function formatCount(value: string): string {
  return new Intl.NumberFormat().format(BigInt(value));
}

function formatTimestamp(timestamp: string): string {
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: "medium",
    timeStyle: "short",
  }).format(new Date(timestamp));
}

function errorMessage(caught: unknown): string {
  return caught instanceof Error ? caught.message : String(caught);
}
