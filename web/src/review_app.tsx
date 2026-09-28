import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent as ReactKeyboardEvent,
} from "react";
import { DiagnosticFeedbackPanel } from "./diagnostic_feedback.js";
import { FieldRecordingPanel, supportsFieldRecording } from "./field_recording.js";
import { operationalHealthDescription, type OperationalHealth } from "./operational_health.js";
import {
  PairNetworkHealthNotice,
  UnknownPairNetworkHealthNotice,
} from "./pair_network_health_notice.js";
import type {
  CaptureStatus,
  ClipManifest,
  PeerArmStatus,
  PoseCaptureStatus,
  ReviewApi,
  SessionSummary,
  SyntheticSwingHilStage,
  SyntheticSwingHilStatus,
} from "./review_api.js";
import { ReviewPlayer } from "./review_player.js";

export interface ReviewAppProps {
  api: ReviewApi;
  pollIntervalMs?: number;
  nowMs?: () => number;
}

type InspectorTab = "capture" | "diagnostics" | "details";

export function ReviewApp({ api, pollIntervalMs = 1_000, nowMs = Date.now }: ReviewAppProps) {
  const [capture, setCapture] = useState<CaptureStatus | null>(null);
  const [sessions, setSessions] = useState<SessionSummary[]>([]);
  const [selectedSessionId, setSelectedSessionId] = useState<string | null>(null);
  const [manifest, setManifest] = useState<ClipManifest | null>(null);
  const [refreshError, setRefreshError] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [actionPending, setActionPending] = useState(false);
  const [allowDegradedNetwork, setAllowDegradedNetwork] = useState(false);
  const [refreshRevision, setRefreshRevision] = useState(0);
  const [libraryOpen, setLibraryOpen] = useState(false);
  const [inspectorOpen, setInspectorOpen] = useState(false);
  const [inspectorTab, setInspectorTab] = useState<InspectorTab>("capture");
  const latestSeenRef = useRef<string | null>(null);
  const manifestRequestRef = useRef<string | null>(null);
  const selectedSessionIdRef = useRef<string | null>(null);
  const manifestSessionIdRef = useRef<string | null>(null);
  const pendingSessionRef = useRef<SessionSummary | null>(null);
  const pendingSessionDeadlineMsRef = useRef<number | null>(null);
  const nowMsRef = useRef(nowMs);
  const libraryButtonRef = useRef<HTMLButtonElement | null>(null);
  const libraryCloseButtonRef = useRef<HTMLButtonElement | null>(null);
  const inspectorButtonRef = useRef<HTMLButtonElement | null>(null);
  const inspectorInvokerRef = useRef<HTMLButtonElement | null>(null);
  nowMsRef.current = nowMs;

  useEffect(() => {
    if (libraryOpen) {
      libraryCloseButtonRef.current?.focus();
    }
  }, [libraryOpen]);

  useEffect(() => {
    if (!inspectorOpen) {
      return;
    }
    const closeOnEscape = (event: globalThis.KeyboardEvent) => {
      if (event.key !== "Escape" || libraryOpen) {
        return;
      }
      event.preventDefault();
      setInspectorOpen(false);
      inspectorInvokerRef.current?.focus();
    };
    document.addEventListener("keydown", closeOnEscape);
    return () => document.removeEventListener("keydown", closeOnEscape);
  }, [inspectorOpen, libraryOpen]);

  const openSession = useCallback(
    async (session: SessionSummary) => {
      selectedSessionIdRef.current = session.session_id;
      setSelectedSessionId(session.session_id);
      if (session.session_kind === "standby_diagnostic") {
        setInspectorTab("diagnostics");
      }
      if (session.state !== "ready" || session.session_kind === "standby_diagnostic") {
        manifestSessionIdRef.current = null;
        setManifest(null);
        return;
      }
      if (manifestRequestRef.current === session.session_id) {
        return;
      }
      if (manifestSessionIdRef.current !== session.session_id) {
        manifestSessionIdRef.current = null;
        setManifest(null);
      }
      manifestRequestRef.current = session.session_id;
      try {
        const nextManifest = await api.getManifest(session.session_id);
        if (selectedSessionIdRef.current !== session.session_id) {
          return;
        }
        manifestSessionIdRef.current = nextManifest.session_id;
        setManifest(nextManifest);
        setRefreshError(null);
      } catch (caught) {
        if (selectedSessionIdRef.current !== session.session_id) {
          return;
        }
        manifestSessionIdRef.current = null;
        setManifest(null);
        setRefreshError(errorMessage(caught));
      } finally {
        if (manifestRequestRef.current === session.session_id) {
          manifestRequestRef.current = null;
        }
      }
    },
    [api],
  );

  const refresh = useCallback(async () => {
    const captureRequest = api.getCaptureStatus();
    const sessionsRequest = api.getSessions();
    let nextCapture: CaptureStatus;
    try {
      nextCapture = await captureRequest;
      setCapture(nextCapture);
      setRefreshError(null);
    } catch (caught) {
      // Observe the independently-started catalog request even when live status is unavailable.
      void sessionsRequest.catch(() => undefined);
      setCapture(null);
      setRefreshError(errorMessage(caught));
      return;
    }
    try {
      const sessionList = await sessionsRequest;
      const visibleSessions = [...sessionList.sessions];
      const pendingSession = pendingSessionRef.current;
      if (pendingSession !== null) {
        if (visibleSessions.some((session) => session.session_id === pendingSession.session_id)) {
          pendingSessionRef.current = null;
          pendingSessionDeadlineMsRef.current = null;
        } else if (
          pendingSession.session_kind === "standby_diagnostic" &&
          pendingSessionDeadlineMsRef.current !== null &&
          nowMsRef.current() < pendingSessionDeadlineMsRef.current
        ) {
          visibleSessions.unshift(structuredClone(pendingSession));
        } else if (
          pendingSession.session_kind !== "standby_diagnostic" &&
          nextCapture.active_session_id === pendingSession.session_id
        ) {
          pendingSession.state = nextCapture.state;
          pendingSession.error = nextCapture.error;
          visibleSessions.unshift(structuredClone(pendingSession));
        } else {
          pendingSessionRef.current = null;
          pendingSessionDeadlineMsRef.current = null;
        }
      }
      setSessions(visibleSessions);
      setRefreshRevision((revision) => revision + 1);
      setRefreshError(null);
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
        selected.session_kind !== "standby_diagnostic" &&
        manifestSessionIdRef.current !== selected.session_id
      ) {
        await openSession(selected);
      }
    } catch (caught) {
      // Historical catalog failure must not make otherwise-live capture controls unusable.
      setRefreshError(errorMessage(caught));
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
    const allowDegradedNetworkForAttempt = armed && allowDegradedNetwork;
    if (armed) {
      // The acknowledgement authorizes exactly this attempt, including when one node rejects and
      // the dual-node coordinator rolls the other node back.
      setAllowDegradedNetwork(false);
    }
    setActionPending(true);
    try {
      setCapture(
        await api.setArmed(
          armed,
          allowDegradedNetworkForAttempt ? { allowDegradedNetwork: true } : undefined,
        ),
      );
      setActionError(null);
    } catch (caught) {
      setActionError(errorMessage(caught));
    } finally {
      setActionPending(false);
    }
  };

  const saveMissedShot = async () => {
    setActionPending(true);
    try {
      const session = await api.saveMissedShot();
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
      pendingSessionDeadlineMsRef.current =
        session.session_kind === "standby_diagnostic" &&
        api.standbyDiagnosticPublicationTimeoutMs !== undefined
          ? nowMs() + api.standbyDiagnosticPublicationTimeoutMs
          : null;
      latestSeenRef.current = session.session_id;
      setSessions((current) => [session, ...current]);
      await openSession(session);
      if (session.session_kind === "standby_diagnostic") {
        setInspectorTab("diagnostics");
        setInspectorOpen(true);
      }
      setActionError(null);
    } catch (caught) {
      setActionError(errorMessage(caught));
    } finally {
      setActionPending(false);
    }
  };

  const startSyntheticSwing = async () => {
    setActionPending(true);
    try {
      setCapture(await api.startSyntheticSwing());
      setActionError(null);
    } catch (caught) {
      setActionError(errorMessage(caught));
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
  const networkHealth = capture?.pair_network_health;
  const configuredNetworkHealth = networkHealth?.configured === true ? networkHealth : undefined;
  const configuredNetworkUnknown = capture?.pose?.mode === "leader" && networkHealth === undefined;
  const degradedNetworkOverrideRequired =
    configuredNetworkHealth !== undefined &&
    !configuredNetworkHealth.stale &&
    configuredNetworkHealth.state === "degraded";
  const networkPreventsArm =
    capture?.armed !== true &&
    (configuredNetworkUnknown ||
      configuredNetworkHealth?.stale === true ||
      configuredNetworkHealth?.state === "unusable" ||
      (degradedNetworkOverrideRequired && !allowDegradedNetwork));
  const hilCompatible =
    capture !== null && !capture.armed && (capture.state === "setup" || capture.state === "ready");
  const sortedSessions = useMemo(
    () =>
      [...sessions].sort((left, right) => right.created_at_utc.localeCompare(left.created_at_utc)),
    [sessions],
  );
  const error = actionError ?? refreshError;

  useEffect(() => {
    if (capture?.armed === true || !degradedNetworkOverrideRequired) {
      setAllowDegradedNetwork(false);
    }
  }, [capture?.armed, degradedNetworkOverrideRequired]);

  const showInspector = (tab: InspectorTab, invoker?: HTMLButtonElement) => {
    if (invoker !== undefined) {
      inspectorInvokerRef.current = invoker;
    }
    setInspectorTab(tab);
    setInspectorOpen(true);
  };

  const closeInspector = () => {
    setInspectorOpen(false);
    window.requestAnimationFrame(() =>
      (inspectorInvokerRef.current ?? inspectorButtonRef.current)?.focus(),
    );
  };

  const closeLibrary = () => {
    setLibraryOpen(false);
    window.requestAnimationFrame(() => libraryButtonRef.current?.focus());
  };

  return (
    <div className="app-shell review-shell">
      <header className="review-toolbar">
        <div className="review-toolbar-title">
          <span aria-hidden="true" className="review-toolbar-mark">
            S
          </span>
          <div>
            <span>Swing Capture</span>
            <h1 id="review-workspace-heading">Swing review</h1>
          </div>
        </div>

        {sortedSessions.length > 0 ? (
          <label className="review-session-picker">
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
                  {session.session_kind === "standby_diagnostic" ? " · Diagnostics only" : ""}
                </option>
              ))}
            </select>
          </label>
        ) : (
          <span className="review-no-session">No sessions</span>
        )}

        <div
          aria-live="polite"
          className={`review-capture-status capture-${capture?.state ?? "loading"}`}
        >
          <span aria-hidden="true" className="review-status-dot" />
          <div>
            <span>{captureKicker(capture)}</span>
            <h2>{captureHeading(capture)}</h2>
          </div>
        </div>

        <div className="review-toolbar-actions">
          {networkPreventsArm ? (
            <button
              className="review-attention-action"
              id="capture-arm-availability"
              onClick={(event) => showInspector("capture", event.currentTarget)}
              type="button"
            >
              Capture blocked · Details
            </button>
          ) : null}
          <button
            aria-describedby={networkPreventsArm ? "capture-arm-availability" : undefined}
            className="review-arm-action"
            disabled={
              capture === null || actionPending || captureOperationBusy || networkPreventsArm
            }
            onClick={() => void setArmed(!capture?.armed)}
            type="button"
          >
            {capture?.armed === true ? "Disarm capture" : armCaptureLabel(capture?.pose)}
          </button>
          <button
            className="save-missed-shot"
            disabled={
              capture?.armed !== true ||
              capture.state !== "armed" ||
              capture.hil.busy ||
              actionPending
            }
            onClick={() => void saveMissedShot()}
            type="button"
          >
            {captureUsesPoseMonitoring(capture) ? "Tag missed shot" : "Save missed shot"}
          </button>
        </div>
      </header>

      <main
        aria-labelledby="review-workspace-heading"
        className={`review-workspace review-workspace-${capture?.state ?? "loading"}${
          inspectorOpen ? " inspector-open" : ""
        }`}
      >
        <nav aria-label="Review tools" className="review-tool-rail">
          <button
            aria-controls="session-library"
            aria-expanded={libraryOpen}
            aria-label="Library"
            className={libraryOpen ? "active" : ""}
            onClick={() => setLibraryOpen((open) => !open)}
            ref={libraryButtonRef}
            title="Session library"
            type="button"
          >
            <svg
              aria-hidden="true"
              className="review-tool-icon"
              fill="none"
              focusable="false"
              viewBox="0 0 24 24"
            >
              <rect height="14" rx="1.5" width="14" x="6" y="6" />
              <path d="M9 3h9a3 3 0 0 1 3 3v9M9.5 10h7M9.5 14h7M9.5 18h4" />
            </svg>
          </button>
          <button
            aria-controls="review-inspector"
            aria-expanded={inspectorOpen}
            aria-label="Inspector"
            className={inspectorOpen && inspectorTab === "capture" ? "active" : ""}
            onClick={(event) =>
              inspectorOpen && inspectorTab === "capture"
                ? closeInspector()
                : showInspector("capture", event.currentTarget)
            }
            ref={inspectorButtonRef}
            title="Capture inspector"
            type="button"
          >
            <svg
              aria-hidden="true"
              className="review-tool-icon"
              fill="none"
              focusable="false"
              viewBox="0 0 24 24"
            >
              <path d="M4 7h6M14 7h6M4 17h10M18 17h2M8 4v6M16 14v6" />
            </svg>
          </button>
          <button
            aria-controls="review-inspector"
            aria-expanded={inspectorOpen && inspectorTab === "diagnostics"}
            aria-label="Diagnostics"
            className={inspectorOpen && inspectorTab === "diagnostics" ? "active" : ""}
            onClick={(event) => showInspector("diagnostics", event.currentTarget)}
            title="Diagnostics and export"
            type="button"
          >
            <svg
              aria-hidden="true"
              className="review-tool-icon"
              fill="none"
              focusable="false"
              viewBox="0 0 24 24"
            >
              <path d="M3 12h4l2.2-5.5L13 18l2.5-6H21" />
            </svg>
          </button>
        </nav>

        {error !== null && (!inspectorOpen || inspectorTab !== "capture") ? (
          <div className="review-error-toast" role="alert">
            <div>
              <strong>Capture needs attention</strong>
              <span>{error}</span>
            </div>
            <button
              onClick={(event) => showInspector("capture", event.currentTarget)}
              type="button"
            >
              Open inspector
            </button>
          </div>
        ) : null}

        {libraryOpen ? (
          <>
            <button
              aria-label="Dismiss session library"
              className="review-drawer-backdrop"
              onClick={closeLibrary}
              type="button"
            />
            <div
              aria-labelledby="session-library-heading"
              aria-modal="true"
              className="session-library-drawer"
              id="session-library"
              onKeyDown={(event) => {
                if (event.key === "Escape") {
                  event.preventDefault();
                  closeLibrary();
                  return;
                }
                trapDialogFocus(event);
              }}
              role="dialog"
            >
              <header>
                <div>
                  <span>Library</span>
                  <h2 id="session-library-heading">Recorded sessions</h2>
                </div>
                <button
                  aria-label="Close session library"
                  onClick={closeLibrary}
                  ref={libraryCloseButtonRef}
                  type="button"
                >
                  ×
                </button>
              </header>
              {sortedSessions.length === 0 ? (
                <p>No recorded swings yet.</p>
              ) : (
                <ul className="session-library-list">
                  {sortedSessions.map((session) => (
                    <li key={session.session_id}>
                      <button
                        aria-current={session.session_id === selectedSessionId ? "true" : undefined}
                        onClick={() => {
                          void openSession(session);
                          closeLibrary();
                        }}
                        type="button"
                      >
                        <span>{formatSessionTime(session.created_at_utc)}</span>
                        <strong>{stateLabel(session.state)}</strong>
                        <small>
                          {session.session_kind === "standby_diagnostic"
                            ? "Diagnostics only"
                            : session.session_id}
                        </small>
                      </button>
                    </li>
                  ))}
                </ul>
              )}
            </div>
          </>
        ) : null}

        <section aria-label="Swing media" className="sessions-panel review-stage">
          {selectedSession?.state === "error" ? (
            <p className="review-error" role="alert">
              {selectedSession.error || "Capture session failed without an error detail."}
            </p>
          ) : null}
          {selectedSession !== undefined &&
          selectedSession.state !== "ready" &&
          selectedSession.session_kind === "standby_diagnostic" ? (
            <div className="diagnostic-pending" role="status">
              <strong>Saving standby diagnostics</strong>
              <span>Retaining audio post-roll and publishing the evidence bundle.</span>
            </div>
          ) : null}
          {selectedSession !== undefined &&
          selectedSession.state !== "ready" &&
          selectedSession.session_kind !== "standby_diagnostic" ? (
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
          {manifest !== null &&
          selectedSession?.state === "ready" &&
          manifest.session_id === selectedSession.session_id ? (
            <div className="review-player-stack">
              {manifest.android_capture?.peer_arm === undefined ? null : (
                <PeerArmNotice context="recorded" status={manifest.android_capture.peer_arm} />
              )}
              <ReviewPlayer key={`player-${manifest.session_id}`} manifest={manifest} />
            </div>
          ) : null}
          {sessions.length === 0 && error === null ? (
            <div className="review-placeholder">
              <strong>No recorded swings yet</strong>
              <span>Arm audio capture, then strike a ball to create the first session.</span>
            </div>
          ) : null}
        </section>

        <div
          aria-labelledby="review-inspector-heading"
          aria-modal={false}
          className="review-inspector"
          hidden={!inspectorOpen}
          id="review-inspector"
          onKeyDown={(event) => {
            if (event.key === "Escape") {
              event.preventDefault();
              closeInspector();
            }
          }}
          role="dialog"
        >
          <header className="review-inspector-header">
            <div>
              <span>Workspace</span>
              <h2 id="review-inspector-heading">Review inspector</h2>
            </div>
            <button aria-label="Close inspector" onClick={closeInspector} type="button">
              ×
            </button>
          </header>
          <div aria-label="Inspector sections" className="review-inspector-tabs" role="tablist">
            {(["capture", "diagnostics", "details"] as const).map((tab) => (
              <button
                aria-controls={`review-inspector-${tab}`}
                aria-selected={inspectorTab === tab}
                id={`review-inspector-${tab}-tab`}
                key={tab}
                onClick={() => setInspectorTab(tab)}
                role="tab"
                type="button"
              >
                {tab === "capture" ? "Capture" : tab === "diagnostics" ? "Diagnostics" : "Session"}
              </button>
            ))}
          </div>
          <div className="review-inspector-content">
            <div
              aria-labelledby="review-inspector-capture-tab"
              hidden={inspectorTab !== "capture"}
              id="review-inspector-capture"
              role="tabpanel"
            >
              <section aria-labelledby="capture-heading" className="capture-panel">
                <div>
                  <p className="section-kicker">{captureKicker(capture)}</p>
                  <strong className="capture-inspector-heading" id="capture-heading">
                    {captureHeading(capture)}
                  </strong>
                  <p>{captureDescription(capture)}</p>
                </div>
                <p className="capture-action-help">
                  {captureUsesPoseMonitoring(capture)
                    ? "Missed-shot tagging retains low-rate pose evidence and diagnostic audio without creating review video before high-speed starts."
                    : "Missed-shot save retains up to 1.4 seconds of preceding video and up to 10 seconds of diagnostic audio."}
                </p>
                {error !== null ? (
                  <p className="review-error capture-error" role="alert">
                    {error}
                  </p>
                ) : null}
                {capture?.operational_health === undefined ? null : (
                  <OperationalHealthNotice health={capture.operational_health} />
                )}
                {configuredNetworkHealth === undefined ? null : (
                  <PairNetworkHealthNotice health={configuredNetworkHealth} />
                )}
                {configuredNetworkUnknown ? <UnknownPairNetworkHealthNotice /> : null}
                {!degradedNetworkOverrideRequired || capture?.armed === true ? null : (
                  <label className="degraded-network-override">
                    <input
                      checked={allowDegradedNetwork}
                      onChange={(event) => setAllowDegradedNetwork(event.currentTarget.checked)}
                      type="checkbox"
                    />
                    Arm once using degraded pair network
                    <small>
                      This acknowledgement is not saved. Recheck phone-to-phone reliability before
                      every arm attempt.
                    </small>
                  </label>
                )}
                {capture?.pose === undefined ? null : (
                  <PeerArmNotice context="live" status={capture.pose.peer_arm} />
                )}
              </section>

              {supportsFieldRecording(api) ? (
                <FieldRecordingPanel api={api} pollIntervalMs={pollIntervalMs} />
              ) : null}

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
            </div>

            <div
              aria-labelledby="review-inspector-diagnostics-tab"
              hidden={inspectorTab !== "diagnostics"}
              id="review-inspector-diagnostics"
              role="tabpanel"
            >
              {manifest !== null &&
              selectedSession?.state === "ready" &&
              manifest.session_id === selectedSession.session_id ? (
                <DiagnosticFeedbackPanel api={api} key={manifest.session_id} manifest={manifest} />
              ) : selectedSession?.state === "ready" &&
                selectedSession.session_kind === "standby_diagnostic" ? (
                <DiagnosticFeedbackPanel
                  api={api}
                  diagnosticOnlySessionId={selectedSession.session_id}
                  key={selectedSession.session_id}
                />
              ) : (
                <div className="inspector-empty-state">
                  <strong>Diagnostics become available after capture</strong>
                  <span>Choose a ready session to label or export its retained evidence.</span>
                </div>
              )}
            </div>

            <div
              aria-labelledby="review-inspector-details-tab"
              hidden={inspectorTab !== "details"}
              id="review-inspector-details"
              role="tabpanel"
            >
              <section className="session-inspector-details">
                <p className="section-kicker">Selected session</p>
                <h2>{sessionHeading(selectedSession)}</h2>
                {manifest === null ? (
                  <p>Detailed clip metadata appears when the selected session is ready.</p>
                ) : (
                  <dl>
                    <div>
                      <dt>Trigger</dt>
                      <dd>{triggerLabel(manifest.trigger.source)}</dd>
                    </div>
                    <div>
                      <dt>Recorded</dt>
                      <dd>{formatSessionTime(manifest.created_at_utc)}</dd>
                    </div>
                    <div>
                      <dt>Media</dt>
                      <dd>{mediaSummary(manifest)}</dd>
                    </div>
                  </dl>
                )}
              </section>
            </div>
          </div>
        </div>
      </main>
    </div>
  );
}

function trapDialogFocus(event: ReactKeyboardEvent<HTMLElement>) {
  if (event.key !== "Tab") {
    return;
  }
  const controls = Array.from(
    event.currentTarget.querySelectorAll<HTMLElement>(
      'button:not([disabled]), select:not([disabled]), input:not([disabled]), textarea:not([disabled]), [href], [tabindex]:not([tabindex="-1"])',
    ),
  ).filter((element) => !element.hidden);
  const first = controls[0];
  const last = controls.at(-1);
  if (first === undefined || last === undefined) {
    return;
  }
  if (event.shiftKey && document.activeElement === first) {
    event.preventDefault();
    last.focus();
  } else if (!event.shiftKey && document.activeElement === last) {
    event.preventDefault();
    first.focus();
  }
}

function OperationalHealthNotice({ health }: { health: OperationalHealth }) {
  return (
    <div
      className={`peer-arm-notice ${health.ready_for_capture ? "peer-arm-accepted" : "peer-arm-failure"}`}
      role={health.ready_for_capture ? undefined : "alert"}
    >
      <strong>
        {health.ready_for_capture ? "Capture resources ready" : "Capture resources need attention"}
      </strong>
      <span>{operationalHealthDescription(health)}</span>
      {health.issues.map((issue) => (
        <span key={issue}>{issue}</span>
      ))}
    </div>
  );
}

function PeerArmNotice({
  status,
  context,
}: {
  status: PeerArmStatus;
  context: "live" | "recorded";
}) {
  if (status.state === "not_requested") {
    return null;
  }
  const recorded = context === "recorded";
  let heading: string;
  let detail: string;
  let tone: "pending" | "success" | "failure";
  switch (status.state) {
    case "pending":
      heading = recorded ? "Paired-phone arm outcome pending" : "Arming paired phone";
      detail = recorded
        ? "The session was published before the paired phone reported its final arm outcome."
        : "Waiting for the paired phone to accept this shared capture session.";
      tone = "pending";
      break;
    case "accepted":
      heading = recorded ? "Paired-phone arm confirmed" : "Paired phone armed";
      detail = recorded
        ? "The paired phone accepted this shared capture session."
        : "Both phones are ready to retain the same swing.";
      tone = "success";
      break;
    case "inbound_accepted":
      heading = recorded ? "Paired-phone arm confirmed" : "Arm accepted from paired phone";
      detail = recorded
        ? "This phone accepted the paired phone’s shared capture request."
        : "This phone is ready to retain the paired phone’s shared swing.";
      tone = "success";
      break;
    case "rejected":
      heading = "Paired phone rejected arm";
      detail = `The peer returned HTTP ${String(status.http_status)}. This capture may contain only one view.`;
      tone = "failure";
      break;
    case "failed":
      heading = "Paired-phone arm failed";
      detail = `The peer request failed (${status.failure_type ?? "unknown failure"}). This capture may contain only one view.`;
      tone = "failure";
      break;
  }
  return (
    <div
      className={`peer-arm-notice peer-arm-${tone}`}
      role={tone === "failure" ? "alert" : "status"}
    >
      <strong>{heading}</strong>
      <span>{detail}</span>
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
  if (
    status.state === "armed" &&
    status.pose?.mode !== undefined &&
    status.pose.mode !== "disabled"
  ) {
    if (status.pose.phase === "monitoring") {
      return status.pose.mode === "leader" ? "Watching for address" : "Waiting for the pose leader";
    }
    if (status.pose.phase === "high_speed") {
      return "High-speed capture is listening for impact";
    }
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
  if (
    status.state === "armed" &&
    status.pose?.mode !== undefined &&
    status.pose.mode !== "disabled"
  ) {
    if (status.pose.phase === "monitoring") {
      return status.pose.mode === "leader"
        ? "The 5 Hz pose camera is looking for a stable address. High-speed video and microphone impact capture start when address is confirmed."
        : "The 5 Hz pose camera is retaining diagnostics while this phone waits for the paired leader to start high-speed capture.";
    }
    if (status.pose.phase === "high_speed") {
      return "The 240 fps ring and microphone impact detector are active. Strike the ball before the no-impact timeout returns to pose standby.";
    }
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

function captureKicker(status: CaptureStatus | null): string {
  if (status?.pose?.mode === undefined || status.pose.mode === "disabled") {
    return "Audio trigger";
  }
  return status.pose.phase === "high_speed" ? "Impact trigger" : "Address trigger";
}

function armCaptureLabel(pose: PoseCaptureStatus | undefined): string {
  if (pose?.mode === "leader") {
    return "Arm pose capture";
  }
  if (pose?.mode === "shadow") {
    return "Arm paired capture";
  }
  return "Arm audio capture";
}

function captureUsesPoseMonitoring(status: CaptureStatus | null): boolean {
  return (
    status?.state === "armed" &&
    status.pose?.mode !== undefined &&
    status.pose.mode !== "disabled" &&
    status.pose.phase === "monitoring"
  );
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
