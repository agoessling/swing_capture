import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type {
  CaptureStatus,
  ClipManifest,
  ReviewApi,
  SessionSummary,
  SyntheticSwingHilStage,
  SyntheticSwingHilStatus,
} from "./review_api.js";
import { ReviewPlayer } from "./review_player.js";

export interface ReviewAppProps {
  api: ReviewApi;
  pollIntervalMs?: number;
}

export function ReviewApp({ api, pollIntervalMs = 1_000 }: ReviewAppProps) {
  const [capture, setCapture] = useState<CaptureStatus | null>(null);
  const [sessions, setSessions] = useState<SessionSummary[]>([]);
  const [selectedSessionId, setSelectedSessionId] = useState<string | null>(null);
  const [manifest, setManifest] = useState<ClipManifest | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [actionPending, setActionPending] = useState(false);
  const [refreshRevision, setRefreshRevision] = useState(0);
  const latestSeenRef = useRef<string | null>(null);
  const manifestRequestRef = useRef<string | null>(null);
  const selectedSessionIdRef = useRef<string | null>(null);
  const manifestSessionIdRef = useRef<string | null>(null);
  const pendingSessionRef = useRef<SessionSummary | null>(null);

  const openSession = useCallback(
    async (session: SessionSummary) => {
      selectedSessionIdRef.current = session.session_id;
      setSelectedSessionId(session.session_id);
      if (session.state !== "ready") {
        manifestSessionIdRef.current = null;
        setManifest(null);
        return;
      }
      if (manifestRequestRef.current === session.session_id) {
        return;
      }
      manifestRequestRef.current = session.session_id;
      try {
        const nextManifest = await api.getManifest(session.session_id);
        if (selectedSessionIdRef.current !== session.session_id) {
          return;
        }
        manifestSessionIdRef.current = nextManifest.session_id;
        setManifest(nextManifest);
        setError(null);
      } catch (caught) {
        manifestSessionIdRef.current = null;
        setManifest(null);
        setError(errorMessage(caught));
      } finally {
        if (manifestRequestRef.current === session.session_id) {
          manifestRequestRef.current = null;
        }
      }
    },
    [api],
  );

  const refresh = useCallback(async () => {
    try {
      const [nextCapture, sessionList] = await Promise.all([
        api.getCaptureStatus(),
        api.getSessions(),
      ]);
      const visibleSessions = [...sessionList.sessions];
      const pendingSession = pendingSessionRef.current;
      if (pendingSession !== null) {
        if (visibleSessions.some((session) => session.session_id === pendingSession.session_id)) {
          pendingSessionRef.current = null;
        } else if (nextCapture.active_session_id === pendingSession.session_id) {
          pendingSession.state = nextCapture.state;
          pendingSession.error = nextCapture.error;
          visibleSessions.unshift(structuredClone(pendingSession));
        } else {
          pendingSessionRef.current = null;
        }
      }
      setCapture(nextCapture);
      setSessions(visibleSessions);
      setRefreshRevision((revision) => revision + 1);
      setError(null);
      const latest = visibleSessions[0];
      if (latest !== undefined && latest.session_id !== latestSeenRef.current) {
        latestSeenRef.current = latest.session_id;
        await openSession(latest);
        return;
      }
      const selected = visibleSessions.find(
        (session) => session.session_id === selectedSessionIdRef.current,
      );
      if (
        selected !== undefined &&
        selected.state === "ready" &&
        manifestSessionIdRef.current !== selected.session_id
      ) {
        await openSession(selected);
      }
    } catch (caught) {
      setError(errorMessage(caught));
    }
  }, [api, openSession]);

  useEffect(() => {
    let active = true;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let unsubscribe: (() => void) | undefined;
    let refreshing = false;
    let refreshAgain = false;
    const poll = async () => {
      if (!active) {
        return;
      }
      if (refreshing) {
        refreshAgain = true;
        return;
      }
      refreshing = true;
      do {
        refreshAgain = false;
        await refresh();
      } while (active && refreshAgain);
      refreshing = false;
      if (active) {
        const nextRefreshMs = unsubscribe === undefined ? pollIntervalMs : 15_000;
        timer = setTimeout(() => void poll(), nextRefreshMs);
      }
    };
    unsubscribe = api.subscribeToChanges?.(() => {
      if (timer !== undefined) {
        clearTimeout(timer);
        timer = undefined;
      }
      void poll();
    });
    void poll();
    return () => {
      active = false;
      unsubscribe?.();
      if (timer !== undefined) {
        clearTimeout(timer);
      }
    };
  }, [api, pollIntervalMs, refresh]);

  const setArmed = async (armed: boolean) => {
    setActionPending(true);
    try {
      setCapture(await api.setArmed(armed));
      setError(null);
    } catch (caught) {
      setError(errorMessage(caught));
    } finally {
      setActionPending(false);
    }
  };

  const triggerManual = async () => {
    setActionPending(true);
    try {
      const session = await api.triggerManualCapture();
      setCapture((current) =>
        current === null
          ? current
          : {
              ...current,
              state: session.state,
              armed: false,
              active_session_id: session.session_id,
              error: session.error,
            },
      );
      pendingSessionRef.current = structuredClone(session);
      latestSeenRef.current = session.session_id;
      setSessions((current) => [session, ...current]);
      await openSession(session);
      setError(null);
    } catch (caught) {
      setError(errorMessage(caught));
    } finally {
      setActionPending(false);
    }
  };

  const startSyntheticSwing = async () => {
    setActionPending(true);
    try {
      setCapture(await api.startSyntheticSwing());
      setError(null);
    } catch (caught) {
      setError(errorMessage(caught));
    } finally {
      setActionPending(false);
    }
  };

  const selectedSession = sessions.find((session) => session.session_id === selectedSessionId);
  const captureBusy =
    capture?.state === "arming" ||
    capture?.state === "waiting_post_roll" ||
    capture?.state === "encoding";
  const captureOperationBusy = captureBusy || capture?.hil.busy === true;
  const hilCompatible =
    capture !== null && !capture.armed && (capture.state === "setup" || capture.state === "ready");
  const sortedSessions = useMemo(
    () =>
      [...sessions].sort((left, right) => right.created_at_utc.localeCompare(left.created_at_utc)),
    [sessions],
  );

  return (
    <div className="app-shell review-shell">
      <header className="masthead review-masthead">
        <div>
          <p className="eyebrow">Swing Capture Station</p>
          <h1>Swing review</h1>
          <p className="lede">
            Capture from the microphone trigger estimate, inspect both synchronized views, and move
            one exact source frame at a time.
          </p>
        </div>
        <div className={`capture-badge capture-${capture?.state ?? "loading"}`}>
          <span>Capture</span>
          <strong>{capture === null ? "Connecting" : stateLabel(capture.state)}</strong>
        </div>
      </header>

      <main>
        <section aria-labelledby="capture-heading" className="capture-panel">
          <div>
            <p className="section-kicker">Audio trigger</p>
            <h2 id="capture-heading">{captureHeading(capture)}</h2>
            <p>{captureDescription(capture)}</p>
          </div>
          <div className="capture-actions">
            <button
              disabled={capture === null || actionPending || captureOperationBusy}
              onClick={() => void setArmed(!capture?.armed)}
              type="button"
            >
              {capture?.armed === true ? "Disarm capture" : "Arm audio capture"}
            </button>
            <button
              className="secondary"
              disabled={
                capture?.armed !== true ||
                capture.state !== "armed" ||
                capture.hil.busy ||
                actionPending
              }
              onClick={() => void triggerManual()}
              type="button"
            >
              Manual diagnostic capture
            </button>
            <small>Manual capture is available only while an armed pre-roll buffer is full.</small>
          </div>
          {error !== null ? (
            <p className="review-error capture-error" role="alert">
              {error}
            </p>
          ) : null}
        </section>

        {capture?.hil.enabled === true ? (
          <section aria-labelledby="synthetic-hil-heading" className="synthetic-hil-panel">
            <div className="synthetic-hil-summary">
              <div>
                <p className="section-kicker">Hardware-in-the-loop · enabled</p>
                <h2 id="synthetic-hil-heading">{hilHeading(capture.hil)}</h2>
                <p>{hilDescription(capture.hil, hilCompatible)}</p>
              </div>
              <button
                disabled={actionPending || capture.hil.busy || !hilCompatible}
                onClick={() => void startSyntheticSwing()}
                type="button"
              >
                {capture.hil.busy ? "Synthetic swing running…" : "Run synthetic swing HIL"}
              </button>
            </div>
            <HilProgress status={capture.hil} />
            {hilError(capture.hil) !== null ? (
              <p className="review-error" role="alert">
                {hilError(capture.hil)}
              </p>
            ) : null}
          </section>
        ) : null}

        <section aria-labelledby="sessions-heading" className="sessions-panel">
          <div className="sessions-heading">
            <div>
              <p className="section-kicker">Recorded sessions</p>
              <h2 id="sessions-heading">{sessionHeading(selectedSession)}</h2>
            </div>
            {sortedSessions.length > 0 ? (
              <label>
                <span>Session</span>
                <select
                  aria-label="Recorded session"
                  onChange={(event) => {
                    const session = sortedSessions.find(
                      (candidate) => candidate.session_id === event.currentTarget.value,
                    );
                    if (session !== undefined) {
                      void openSession(session);
                    }
                  }}
                  value={selectedSessionId ?? ""}
                >
                  {sortedSessions.map((session) => (
                    <option key={session.session_id} value={session.session_id}>
                      {formatSessionTime(session.created_at_utc)} · {stateLabel(session.state)}
                    </option>
                  ))}
                </select>
              </label>
            ) : null}
          </div>

          {selectedSession?.state === "error" ? (
            <p className="review-error" role="alert">
              {selectedSession.error || "Capture session failed without an error detail."}
            </p>
          ) : null}
          {selectedSession !== undefined && selectedSession.state !== "ready" ? (
            <div className="impact-preview-panel" role="status">
              <div className="impact-preview-heading">
                <strong>{stateLabel(selectedSession.state)}</strong>
                <span>Impact-adjacent frames appear first; exact video playback is encoding.</span>
              </div>
              {api.impactPreviewUrl === undefined ? null : (
                <div className="impact-preview-grid">
                  {(["down_the_line", "face_on"] as const).map((role) => (
                    <figure key={role}>
                      <img
                        alt={`${roleLabel(role)} impact preview`}
                        src={api.impactPreviewUrl?.(
                          selectedSession.session_id,
                          role,
                          refreshRevision,
                        )}
                      />
                      <figcaption>{roleLabel(role)}</figcaption>
                    </figure>
                  ))}
                </div>
              )}
            </div>
          ) : null}
          {manifest !== null && selectedSession?.state === "ready" ? (
            <>
              <div className="clip-summary">
                <span>{triggerLabel(manifest.trigger.source)}</span>
                <span>{formatSessionTime(manifest.created_at_utc)}</span>
                <span>{mediaSummary(manifest)}</span>
                {manifest.hil_evidence !== undefined ? <span>Synthetic HIL evidence</span> : null}
              </div>
              <ReviewPlayer manifest={manifest} />
            </>
          ) : null}
          {sessions.length === 0 && error === null ? (
            <div className="review-placeholder">
              <strong>No recorded swings yet</strong>
              <span>Arm audio capture, then strike a ball to create the first session.</span>
            </div>
          ) : null}
        </section>
      </main>

      <footer>
        Playback uses encoded review media. Frame labels and synchronization come from retained
        camera timestamps, independently of the camera SDK.
      </footer>
    </div>
  );
}

function mediaSummary(manifest: ClipManifest): string {
  const track = manifest.views[0];
  if (track === undefined) {
    return "Encoded review media";
  }
  const frameStructure = track.media.all_frames_keyframes ? "all-intra" : "inter-frame";
  const geometry =
    track.source.width === track.encoded.width && track.source.height === track.encoded.height
      ? "full resolution"
      : `${track.encoded.width}×${track.encoded.height}`;
  return `${track.media.codec.toUpperCase()} · ${frameStructure} · ${geometry}`;
}

const HIL_PROGRESS_STEPS: Array<{ stage: SyntheticSwingHilStage; label: string }> = [
  { stage: "calibrating", label: "Calibration" },
  { stage: "stimulus", label: "LED + audio sequence" },
  { stage: "capturing", label: "Capture" },
  { stage: "encoding", label: "Encode" },
  { stage: "ready", label: "Review ready" },
];

function HilProgress({ status }: { status: SyntheticSwingHilStatus }) {
  const currentIndex = HIL_PROGRESS_STEPS.findIndex((step) => step.stage === status.stage);
  const completedIndex =
    status.stage === "ready" ? HIL_PROGRESS_STEPS.length - 1 : currentIndex - 1;
  return (
    <ol aria-label="Synthetic swing HIL progress" className="synthetic-hil-progress">
      {HIL_PROGRESS_STEPS.map((step, index) => {
        const current = index === currentIndex && status.stage !== "ready";
        const complete = index <= completedIndex;
        return (
          <li
            aria-current={current ? "step" : undefined}
            className={current ? "current" : complete ? "complete" : "pending"}
            key={step.stage}
          >
            <span aria-hidden="true">{complete ? "✓" : index + 1}</span>
            {step.label}
          </li>
        );
      })}
    </ol>
  );
}

function hilHeading(status: SyntheticSwingHilStatus): string {
  switch (status.stage) {
    case "calibrating":
      return "Calibrating the optical signal";
    case "stimulus":
      return "Running the LED and audio sequence";
    case "capturing":
      return "Capturing the synthetic swing";
    case "encoding":
      return "Encoding the synchronized review";
    case "ready":
      return "Synthetic swing ready";
    case "error":
      return "Synthetic swing HIL failed";
    default:
      return "Synthetic swing station check";
  }
}

function hilDescription(status: SyntheticSwingHilStatus, compatible: boolean): string {
  if (status.busy) {
    return "Keep the camera views clear while the station calibrates, commands the Feather, captures, and publishes the review.";
  }
  if (status.stage === "ready") {
    return "The completed diagnostic session is open below. Run again only when another station check is needed.";
  }
  if (!compatible) {
    return "Disarm normal capture and wait for any active capture to finish before running this station diagnostic.";
  }
  return "Commands the Feather LED and speaker, then validates the full optical, audio, capture, and review path.";
}

function hilError(status: SyntheticSwingHilStatus): string | null {
  if (status.error.length > 0) {
    return status.error;
  }
  if (status.last_run?.error !== undefined && status.last_run.error.length > 0) {
    return status.last_run.error;
  }
  return null;
}

function stateLabel(state: CaptureStatus["state"]): string {
  switch (state) {
    case "waiting_post_roll":
      return "Waiting for post-roll";
    case "encoding":
      return "Encoding review clip";
    case "setup":
      return "Not armed";
    default:
      return state.charAt(0).toUpperCase() + state.slice(1);
  }
}

function roleLabel(role: "down_the_line" | "face_on"): string {
  return role === "down_the_line" ? "Down the line" : "Face on";
}

function captureHeading(status: CaptureStatus | null): string {
  if (status === null) {
    return "Connecting to capture engine";
  }
  switch (status.state) {
    case "armed":
      return "Listening for an audio trigger";
    case "waiting_post_roll":
      return "Audio trigger detected";
    case "encoding":
      return "Building synchronized review media";
    case "error":
      return "Capture needs attention";
    default:
      return stateLabel(status.state);
  }
}

function captureDescription(status: CaptureStatus | null): string {
  if (status === null) {
    return "Loading the station capture state.";
  }
  if (status.error.length > 0) {
    return status.error;
  }
  switch (status.state) {
    case "armed":
      return "The microphone trigger is armed. A detected transient will retain both camera views.";
    case "waiting_post_roll":
      return "The trigger estimate is retained while the configured post-roll completes.";
    case "encoding":
      return "Capture continues while the immutable retained window is encoded.";
    case "ready":
      return "Review media is ready. Re-arm the audio trigger when you are ready for another swing.";
    case "setup":
      return "Arm the trigger when framing, focus, exposure, and gain are ready.";
    case "error":
      return "Resolve the station error before arming another capture.";
    default:
      return "The station is changing capture state.";
  }
}

function sessionHeading(session: SessionSummary | undefined): string {
  return session === undefined ? "Review" : formatSessionTime(session.created_at_utc);
}

function formatSessionTime(timestamp: string): string {
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: "medium",
    timeStyle: "medium",
  }).format(new Date(timestamp));
}

function triggerLabel(source: string): string {
  if (source === "audio") {
    return "Audio trigger estimate";
  }
  if (source === "manual") {
    return "Manual diagnostic";
  }
  return source.replaceAll("_", " ");
}

function errorMessage(caught: unknown): string {
  return caught instanceof Error ? caught.message : "Unknown review application error";
}
