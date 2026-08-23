import { useEffect, useMemo, useRef, useState } from "react";
import type { StatusSubscriptionScheduler } from "./live_status.js";
import type {
  NodeSetupApi,
  NodeSetupConfigurationUpdate,
  NodeDiscoveryHint,
  NodeSetupSnapshot,
  PeerSetupUpdate,
  SetupChoice,
} from "./node_setup_api.js";
import {
  activatePairingBinding,
  assessNodeDiscoveries,
  authenticatedIdentity,
  browserPairingBindingStore,
  directlyVerifiedCandidate,
  pairingActionLabel,
  requiredPairingAction,
  resetPairingBinding,
  revokePairingBinding,
  verifyDiscoveredCandidate,
  type AuthenticatedNodeIdentity,
  type NodeDiscoveryObservation,
  type PairingBindingStore,
  type PeerPairingBinding,
} from "./node_pairing.js";
import { operationalHealthDescription, type OperationalHealth } from "./operational_health.js";
import type { ObjectUrlFactory } from "./paired_preview.js";

const FULL_FRAME_HITTING_REGION = { left: 0, top: 0, right: 1, bottom: 1 } as const;
const DEFAULT_SETUP_SNAPSHOT_REFRESH_MS = 3_000;
const DEFAULT_SETUP_PREVIEW_REFRESH_MS = 1_000;

const browserSetupScheduler: StatusSubscriptionScheduler = {
  setTimeout: (callback, delayMs) => globalThis.setTimeout(callback, delayMs),
  clearTimeout: (handle) => globalThis.clearTimeout(handle as ReturnType<typeof setTimeout>),
};

let nextPreviewInstance = 1;

export function NodeSetupApp({
  apis,
  pairingStore,
  nowMs = Date.now,
  scheduler = browserSetupScheduler,
  setupSnapshotRefreshMs = DEFAULT_SETUP_SNAPSHOT_REFRESH_MS,
  setupPreviewRefreshMs = DEFAULT_SETUP_PREVIEW_REFRESH_MS,
  setupPreviewObjectUrls = URL,
}: {
  apis: readonly NodeSetupApi[];
  pairingStore?: PairingBindingStore;
  nowMs?: () => number;
  scheduler?: StatusSubscriptionScheduler;
  setupSnapshotRefreshMs?: number;
  setupPreviewRefreshMs?: number;
  setupPreviewObjectUrls?: ObjectUrlFactory;
}) {
  requirePositiveInterval(setupSnapshotRefreshMs, "setup snapshot refresh interval");
  requirePositiveInterval(setupPreviewRefreshMs, "setup preview refresh interval");
  const fallbackPairingStoreRef = useRef<PairingBindingStore | null>(null);
  if (fallbackPairingStoreRef.current === null) {
    fallbackPairingStoreRef.current = browserPairingBindingStore();
  }
  const bindings = pairingStore ?? fallbackPairingStoreRef.current;
  const [nodes, setNodes] = useState<
    Array<{ api: NodeSetupApi; setup: NodeSetupSnapshot | null; error: string | null }>
  >(() => apis.map((api) => ({ api, setup: null, error: null })));
  const discoveryApi = apis.find((api) => api.discoverNodes !== undefined);
  const [discoveries, setDiscoveries] = useState<readonly NodeDiscoveryHint[]>([]);
  const [discoveryTokens, setDiscoveryTokens] = useState<Record<string, string>>({});
  const [discoveryError, setDiscoveryError] = useState<string | null>(null);
  const [discoveryLoading, setDiscoveryLoading] = useState(false);
  const nodesRef = useRef(nodes);
  const needsPreviewSnapshotRefresh = nodes.some(
    (node) => node.setup !== null && !node.setup.preview.available,
  );

  useEffect(() => {
    nodesRef.current = nodes;
  }, [nodes]);

  useEffect(() => {
    let active = true;
    setNodes(apis.map((api) => ({ api, setup: null, error: null })));
    for (const api of apis) {
      void api.getSetup().then(
        (setup) => {
          if (active) {
            setNodes((current) =>
              current.map((node) => (node.api === api ? { ...node, setup, error: null } : node)),
            );
          }
        },
        (caught: unknown) => {
          if (active) {
            setNodes((current) =>
              current.map((node) =>
                node.api === api ? { ...node, setup: null, error: errorMessage(caught) } : node,
              ),
            );
          }
        },
      );
    }
    return () => {
      active = false;
    };
  }, [apis]);

  useEffect(() => {
    if (!needsPreviewSnapshotRefresh) {
      return;
    }
    let active = true;
    let timer: unknown;

    const schedule = () => {
      if (active) {
        timer = scheduler.setTimeout(
          () => void refreshUnavailablePreviews(),
          setupSnapshotRefreshMs,
        );
      }
    };
    const refreshUnavailablePreviews = async () => {
      const candidates = nodesRef.current.filter(
        (node) => node.setup !== null && !node.setup.preview.available,
      );
      await Promise.all(
        candidates.map(async ({ api }) => {
          try {
            const refreshed = await api.getSetup();
            if (!active) {
              return;
            }
            setNodes((current) =>
              current.map((node) => {
                if (
                  node.api !== api ||
                  node.setup === null ||
                  node.setup.node.node_id !== refreshed.node.node_id
                ) {
                  return node;
                }
                // This background request discovers preview readiness only. In particular, it
                // must not replace the configuration/revision that owns an in-progress form.
                return { ...node, setup: { ...node.setup, preview: refreshed.preview } };
              }),
            );
          } catch {
            // Keep the last authenticated setup. An unavailable preview remains visibly
            // retrying, while a transient status failure cannot erase or reset the form.
          }
        }),
      );
      schedule();
    };

    schedule();
    return () => {
      active = false;
      if (timer !== undefined) {
        scheduler.clearTimeout(timer);
      }
    };
  }, [apis, needsPreviewSnapshotRefresh, scheduler, setupSnapshotRefreshMs]);

  const refreshDiscoveries = async () => {
    if (discoveryApi?.discoverNodes === undefined) {
      return;
    }
    setDiscoveryLoading(true);
    setDiscoveryError(null);
    try {
      setDiscoveries(await discoveryApi.discoverNodes());
    } catch (caught) {
      setDiscoveryError(errorMessage(caught));
    } finally {
      setDiscoveryLoading(false);
    }
  };

  useEffect(() => {
    void refreshDiscoveries();
    // The configured API list owns discovery lifecycle; users refresh explicitly after that.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [discoveryApi]);

  const connectDiscovery = async (observation: NodeDiscoveryHint) => {
    if (discoveryApi?.connectDiscovered === undefined) {
      return;
    }
    const token = discoveryTokens[observation.service_instance]?.trim() ?? "";
    if (token.length === 0) {
      setDiscoveryError("Enter that phone's current control token before verifying it.");
      return;
    }
    const localNode = nodes.find((node) => node.api === discoveryApi && node.setup !== null);
    if (localNode?.setup === null || localNode === undefined) {
      setDiscoveryError("Wait for this phone's authenticated setup before verifying a peer.");
      return;
    }
    setDiscoveryLoading(true);
    setDiscoveryError(null);
    try {
      const connected = await discoveryApi.connectDiscovered(observation, token);
      verifyDiscoveredCandidate(
        authenticatedIdentity(discoveryApi.displayOrigin, localNode.setup),
        discoveryObservation(observation),
        authenticatedIdentity(connected.api.displayOrigin, connected.setup),
        nowMs(),
      );
      setNodes((current) => {
        const withoutSameOrigin = current.filter(
          (node) => node.api.displayOrigin !== connected.api.displayOrigin,
        );
        return [...withoutSameOrigin, { ...connected, error: null }];
      });
    } catch (caught) {
      setDiscoveryError(errorMessage(caught));
    } finally {
      setDiscoveryLoading(false);
    }
  };

  const reload = async (api: NodeSetupApi) => {
    setNodes((current) =>
      current.map((node) => (node.api === api ? { ...node, error: null } : node)),
    );
    try {
      const setup = await api.getSetup();
      setNodes((current) =>
        current.map((node) => (node.api === api ? { ...node, setup, error: null } : node)),
      );
    } catch (caught) {
      setNodes((current) =>
        current.map((node) => (node.api === api ? { ...node, error: errorMessage(caught) } : node)),
      );
    }
  };

  const loaded = nodes.flatMap((node) => (node.setup === null ? [] : [node.setup]));
  const assignment = assignmentSummary(loaded, nodes.length);

  return (
    <div className="app-shell phone-setup-shell">
      <header className="masthead phone-setup-masthead">
        <div>
          <p className="eyebrow">Swing Capture Station</p>
          <h1>Phone setup</h1>
          <p className="lede">
            Assign each phone to a view, confirm the 720p/240 fps profile, and associate the pose
            leader with its capture peer.
          </p>
        </div>
        <div className={`station-badge ${assignment.ready ? "ready" : "warning"}`}>
          <span>Station assignment</span>
          <strong>{assignment.label}</strong>
        </div>
      </header>

      <main>
        <section aria-labelledby="phone-readiness-heading" className="readiness-panel">
          <div>
            <p className="section-kicker">Connected phones</p>
            <h2 id="phone-readiness-heading">{assignment.heading}</h2>
          </div>
          <div className="notices" aria-live="polite">
            <p className={`notice notice-${assignment.ready ? "good" : "warning"}`}>
              {assignment.guidance}
            </p>
          </div>
        </section>

        <section aria-label="Phone configuration" className="phone-setup-grid">
          {nodes.map((node) => (
            <div className="phone-setup-slot" key={node.api.displayOrigin}>
              {node.setup === null ? (
                <SetupUnavailable
                  error={node.error}
                  origin={node.api.displayOrigin}
                  onReload={() => void reload(node.api)}
                />
              ) : (
                <NodeSetupCard
                  api={node.api}
                  pairingStore={bindings}
                  error={node.error}
                  nowMs={nowMs}
                  onReload={() => void reload(node.api)}
                  onUpdated={(setup) =>
                    setNodes((current) =>
                      current.map((candidate) =>
                        candidate.api === node.api
                          ? { ...candidate, setup, error: null }
                          : candidate,
                      ),
                    )
                  }
                  setup={node.setup}
                  setupPreviewRefreshMs={setupPreviewRefreshMs}
                  setupPreviewObjectUrls={setupPreviewObjectUrls}
                  scheduler={scheduler}
                  verifiedPeers={nodes.flatMap((candidate) =>
                    candidate.setup === null || candidate.api === node.api
                      ? []
                      : [authenticatedIdentity(candidate.api.displayOrigin, candidate.setup)],
                  )}
                />
              )}
            </div>
          ))}
        </section>

        {discoveryApi === undefined ? null : (
          <section
            aria-labelledby="lan-discovery-heading"
            className="readiness-panel lan-discovery-panel"
          >
            <div>
              <p className="section-kicker">Local network</p>
              <h2 id="lan-discovery-heading">Discovered phones</h2>
              <p>
                mDNS supplies address hints only. Enter the discovered phone&apos;s control token;
                its authenticated node ID and role must match before the address can be used.
              </p>
            </div>
            <button
              className="secondary"
              disabled={discoveryLoading}
              onClick={() => void refreshDiscoveries()}
              type="button"
            >
              {discoveryLoading ? "Checking…" : "Refresh phones"}
            </button>
            {discoveryError === null ? null : <p role="alert">{discoveryError}</p>}
            <div className="lan-discovery-list">
              {assessNodeDiscoveries(discoveries.map(discoveryObservation), nowMs()).map(
                (assessment) => {
                  const hint = discoveries.find(
                    (candidate) =>
                      candidate.service_instance === assessment.observation.service_instance,
                  );
                  if (hint === undefined) {
                    return null;
                  }
                  return (
                    <article key={hint.service_instance}>
                      <strong>{hint.label}</strong>
                      <span>
                        {hint.role} · {hint.origin}
                      </span>
                      <small>{assessment.detail}</small>
                      <input
                        aria-label={`Control token for ${hint.label}`}
                        autoComplete="new-password"
                        onChange={(event) => {
                          const token = event.currentTarget.value;
                          setDiscoveryTokens((current) => ({
                            ...current,
                            [hint.service_instance]: token,
                          }));
                        }}
                        placeholder="Phone control token"
                        type="password"
                        value={discoveryTokens[hint.service_instance] ?? ""}
                      />
                      <button
                        className="secondary"
                        disabled={assessment.state !== "available" || discoveryLoading}
                        onClick={() => void connectDiscovery(hint)}
                        type="button"
                      >
                        Authenticate and add
                      </button>
                    </article>
                  );
                },
              )}
            </div>
          </section>
        )}
      </main>

      <footer>
        Peer control credentials are write-only. The browser can retain an existing association,
        replace it with a newly entered token, or clear it; a phone never returns stored tokens.
      </footer>
    </div>
  );
}

function discoveryObservation(hint: NodeDiscoveryHint): NodeDiscoveryObservation {
  const advertisedRole =
    hint.role === "down_the_line" || hint.role === "face_on" || hint.role === "unassigned"
      ? hint.role
      : null;
  return {
    schema_version: 1 as const,
    service_instance: hint.service_instance,
    origin: hint.origin,
    advertised_node_id: hint.node_id,
    advertised_role: advertisedRole,
    advertised_label: hint.label,
    observed_at_epoch_ms: hint.observed_at_epoch_ms,
    expires_at_epoch_ms: hint.expires_at_epoch_ms,
  };
}

function SetupUnavailable({
  origin,
  error,
  onReload,
}: {
  origin: string;
  error: string | null;
  onReload(): void;
}) {
  return (
    <article className="phone-card unavailable">
      <p className="section-kicker">{origin}</p>
      <h2>{error === null ? "Connecting to phone…" : "Phone setup unavailable"}</h2>
      {error === null ? <p>Loading authenticated configuration.</p> : <p role="alert">{error}</p>}
      <button className="secondary" disabled={error === null} onClick={onReload} type="button">
        Retry connection
      </button>
    </article>
  );
}

export function SetupPreview({
  label,
  loadPreview,
  objectUrls = URL,
  preview,
  refreshIntervalMs = DEFAULT_SETUP_PREVIEW_REFRESH_MS,
  scheduler = browserSetupScheduler,
}: {
  label: string;
  loadPreview?: ((url: string) => Promise<Blob>) | undefined;
  objectUrls?: ObjectUrlFactory;
  preview: NodeSetupSnapshot["preview"];
  refreshIntervalMs?: number;
  scheduler?: StatusSubscriptionScheduler;
}) {
  requirePositiveInterval(refreshIntervalMs, "setup preview refresh interval");
  const [instance] = useState(() => {
    const value = nextPreviewInstance;
    nextPreviewInstance =
      nextPreviewInstance === Number.MAX_SAFE_INTEGER ? 1 : nextPreviewInstance + 1;
    return value;
  });
  const retainedObjectUrl = useRef<string | null>(null);
  const [imageUrl, setImageUrl] = useState<string | null>(null);
  const [loadState, setLoadState] = useState<"loading" | "live" | "stale">("loading");

  useEffect(() => {
    const previewUrl = preview.url;
    setLoadState("loading");
    if (!preview.available || previewUrl === null || loadPreview === undefined) {
      if (retainedObjectUrl.current !== null) {
        objectUrls.revokeObjectURL(retainedObjectUrl.current);
        retainedObjectUrl.current = null;
      }
      setImageUrl(null);
      return;
    }
    let active = true;
    let timer: unknown;
    let requestSequence = 0;
    const schedule = () => {
      if (active) {
        timer = scheduler.setTimeout(() => void load(), refreshIntervalMs);
      }
    };
    const load = async () => {
      const source = noCachePreviewUrl(previewUrl, `${instance}-${requestSequence}`);
      requestSequence = requestSequence === Number.MAX_SAFE_INTEGER ? 0 : requestSequence + 1;
      try {
        const blob = await loadPreview(source);
        if (!active) {
          return;
        }
        if (blob.size === 0 || blob.type !== "image/jpeg") {
          throw new Error("Authenticated setup preview did not return a nonempty JPEG");
        }
        const nextObjectUrl = objectUrls.createObjectURL(blob);
        if (!active) {
          objectUrls.revokeObjectURL(nextObjectUrl);
          return;
        }
        const previousObjectUrl = retainedObjectUrl.current;
        retainedObjectUrl.current = nextObjectUrl;
        setImageUrl(nextObjectUrl);
        setLoadState("loading");
        if (previousObjectUrl !== null) {
          objectUrls.revokeObjectURL(previousObjectUrl);
        }
      } catch {
        if (active) {
          setLoadState("stale");
        }
      } finally {
        schedule();
      }
    };
    void load();
    return () => {
      active = false;
      if (timer !== undefined) {
        scheduler.clearTimeout(timer);
      }
    };
  }, [
    instance,
    loadPreview,
    objectUrls,
    preview.available,
    preview.url,
    refreshIntervalMs,
    scheduler,
  ]);

  useEffect(
    () => () => {
      if (retainedObjectUrl.current !== null) {
        objectUrls.revokeObjectURL(retainedObjectUrl.current);
        retainedObjectUrl.current = null;
      }
    },
    [objectUrls],
  );

  if (!preview.available || preview.url === null) {
    const stale = preview.state === "stale";
    const detail = stale
      ? stalePreviewDescription(preview.frame_age_ms)
      : preview.reason === undefined || preview.reason === null
        ? "Standby may still be starting, or high-speed capture may own the camera. " +
          "Retrying setup status automatically."
        : `${friendlyPreviewReason(preview.reason)} Retrying setup status automatically.`;
    return (
      <div className="phone-preview-placeholder" role="status">
        <strong>{stale ? "Preview stale" : "Preview unavailable"}</strong>
        <span>{detail}</span>
      </div>
    );
  }

  if (loadPreview === undefined) {
    return (
      <div className="phone-preview-placeholder" role="status">
        <strong>Preview unavailable</strong>
        <span>Authenticated preview transport is not configured for this phone.</span>
      </div>
    );
  }

  const rotation = preview.image_rotation_degrees ?? 0;
  return (
    <figure className="phone-preview" data-preview-state={loadState}>
      <div className={`phone-preview-image-shell rotation-${rotation}`}>
        {imageUrl === null ? null : (
          <img
            alt={`${label} setup preview`}
            key={imageUrl}
            onError={() => setLoadState("stale")}
            onLoad={() => setLoadState("live")}
            src={imageUrl}
          />
        )}
      </div>
      <figcaption aria-live="polite">
        {loadState === "loading"
          ? "Loading low-rate standby preview…"
          : loadState === "stale"
            ? "Preview update delayed; the last frame may be stale. Retrying automatically."
            : "Live low-rate standby preview · refreshes about once per second"}
      </figcaption>
    </figure>
  );
}

function NodeSetupCard({
  api,
  setup,
  error,
  pairingStore,
  verifiedPeers,
  nowMs,
  onUpdated,
  onReload,
  scheduler,
  setupPreviewRefreshMs,
  setupPreviewObjectUrls,
}: {
  api: NodeSetupApi;
  setup: NodeSetupSnapshot;
  error: string | null;
  pairingStore: PairingBindingStore;
  verifiedPeers: readonly AuthenticatedNodeIdentity[];
  nowMs(): number;
  onUpdated(setup: NodeSetupSnapshot): void;
  onReload(): void;
  scheduler: StatusSubscriptionScheduler;
  setupPreviewRefreshMs: number;
  setupPreviewObjectUrls: ObjectUrlFactory;
}) {
  const loadSetupPreview = useMemo(() => api.getSetupPreview?.bind(api), [api]);
  const localIdentity = authenticatedIdentity(api.displayOrigin, setup);
  const initialBinding = serverPairingBinding(setup) ?? pairingStore.get(localIdentity.node_id);
  const [draft, setDraft] = useState(setup.configuration);
  const [peerOrigin, setPeerOrigin] = useState(setup.configuration.pose.peer?.origin ?? "");
  const [peerToken, setPeerToken] = useState("");
  const [clearPeer, setClearPeer] = useState(false);
  const [peerLabel, setPeerLabel] = useState(initialBinding?.peer_label ?? "");
  const [binding, setBinding] = useState<PeerPairingBinding | null>(initialBinding);
  const [resetConfirmation, setResetConfirmation] = useState("");
  const [rotationConfirmation, setRotationConfirmation] = useState("");
  const [rotatedControlToken, setRotatedControlToken] = useState<string | null>(null);
  const [rotatingCredential, setRotatingCredential] = useState(false);
  const [saving, setSaving] = useState(false);
  const [saveError, setSaveError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);
  const renderedRevisionRef = useRef(setup.revision);

  useEffect(() => {
    if (renderedRevisionRef.current === setup.revision) {
      return;
    }
    renderedRevisionRef.current = setup.revision;
    setDraft(setup.configuration);
    setPeerOrigin(setup.configuration.pose.peer?.origin ?? "");
    setPeerToken("");
    setClearPeer(false);
    const storedBinding = serverPairingBinding(setup) ?? pairingStore.get(setup.node.node_id);
    setBinding(storedBinding);
    setPeerLabel(storedBinding?.peer_label ?? "");
    setResetConfirmation("");
    setSaveError(null);
  }, [pairingStore, setup]);

  const idPrefix = `node-${setup.node.node_id.replaceAll(/[^a-zA-Z0-9_-]/g, "-")}`;
  const configuredPeerOrigin = setup.configuration.pose.peer?.origin ?? "";
  const profile = setup.capabilities.capture_profiles.find(
    (candidate) => candidate.value === draft.capture_profile,
  );
  const selectedVerifiedPeer = verifiedPeers.find(
    (peer) => peer.origin === normalizedOriginOrNull(peerOrigin),
  );

  const updatePose = (update: Partial<typeof draft.pose>) => {
    setDraft((current) => ({ ...current, pose: { ...current.pose, ...update } }));
    setSaved(false);
  };

  const save = async () => {
    setSaving(true);
    setSaveError(null);
    setSaved(false);
    try {
      const peerUpdate = buildPeerUpdate(
        draft.pose.mode,
        configuredPeerOrigin,
        peerOrigin,
        peerToken,
        clearPeer,
      );
      const configuration: NodeSetupConfigurationUpdate = {
        role: draft.role,
        capture_profile: draft.capture_profile,
        pose: {
          mode: draft.pose.mode,
          inference_delegate: draft.pose.inference_delegate,
          debug_evidence_enabled: draft.pose.debug_evidence_enabled,
          hitting_region: FULL_FRAME_HITTING_REGION,
          peer_update: peerUpdate,
        },
      };
      let nextBinding: PeerPairingBinding | null | undefined;
      if (peerUpdate.operation === "clear") {
        nextBinding =
          binding === null || binding.credential_state === "revoked"
            ? binding
            : revokePairingBinding(binding, localIdentity.node_id, nowMs());
      } else if (peerUpdate.operation === "replace" && selectedVerifiedPeer !== undefined) {
        const candidate = directlyVerifiedCandidate(localIdentity, selectedVerifiedPeer);
        const action = requiredPairingAction(binding, localIdentity, candidate);
        nextBinding = activatePairingBinding(
          binding,
          localIdentity,
          candidate,
          action,
          peerLabel.trim().length === 0 ? candidate.label : peerLabel,
          nowMs(),
        );
      }
      const updated = await api.updateSetup(setup.revision, configuration);
      const authoritativeBinding = serverPairingBinding(updated);
      const committedBinding = authoritativeBinding ?? nextBinding;
      if (committedBinding !== undefined && committedBinding !== null) {
        pairingStore.set(committedBinding);
        setBinding(committedBinding);
      }
      onUpdated(updated);
      setSaved(true);
    } catch (caught) {
      setSaveError(errorMessage(caught));
    } finally {
      setSaving(false);
    }
  };

  const forgetRevokedIdentity = async () => {
    if (binding === null || binding.credential_state !== "revoked") {
      return;
    }
    try {
      const confirmedId = resetConfirmation.trim();
      if (api.resetPairing !== undefined) {
        const updated = await api.resetPairing(setup.revision, confirmedId);
        onUpdated(updated);
      }
      resetPairingBinding(pairingStore, binding, confirmedId);
      setBinding(null);
      setPeerLabel("");
      setResetConfirmation("");
      setSaveError(null);
    } catch (caught) {
      setSaveError(errorMessage(caught));
    }
  };

  const rotateControlCredential = async () => {
    if (api.rotateControlCredential === undefined) {
      return;
    }
    setRotatingCredential(true);
    setSaveError(null);
    try {
      const rotation = await api.rotateControlCredential(
        setup.revision,
        rotationConfirmation.trim(),
      );
      setRotatedControlToken(rotation.control_token);
      setRotationConfirmation("");
      try {
        const updated = await api.getSetup();
        if (
          updated.node.node_id !== rotation.node_id ||
          updated.revision !== rotation.setup_revision ||
          updated.node.control_credential_generation !== rotation.control_credential_generation
        ) {
          throw new Error("Rotated credential metadata does not match the refreshed phone setup");
        }
        onUpdated(updated);
      } catch (caught) {
        setSaveError(
          `Credential rotated, but setup refresh failed: ${errorMessage(caught)}. Use the new token shown below.`,
        );
      }
    } catch (caught) {
      setSaveError(errorMessage(caught));
    } finally {
      setRotatingCredential(false);
    }
  };

  return (
    <article aria-labelledby={`${idPrefix}-heading`} className="phone-card">
      <header className="phone-card-header">
        <div>
          <p className="section-kicker">{api.displayOrigin}</p>
          <h2 id={`${idPrefix}-heading`}>{roleLabel(draft.role)}</h2>
          <p>
            {setup.node.device_model} · <code>{shortNodeId(setup.node.node_id)}</code>
          </p>
        </div>
        <span
          className={`connection ${setup.readiness.issues.length === 0 ? "online" : "offline"}`}
        >
          <span aria-hidden="true" className="connection-dot" />
          {setup.readiness.editable ? "Ready to configure" : setup.readiness.capture_state}
        </span>
      </header>

      <SetupPreview
        label={roleLabel(draft.role)}
        loadPreview={loadSetupPreview}
        objectUrls={setupPreviewObjectUrls}
        preview={setup.preview}
        refreshIntervalMs={setupPreviewRefreshMs}
        scheduler={scheduler}
      />

      {setup.readiness.issues.length > 0 ? (
        <ul className="setup-issues" aria-label="Setup issues">
          {setup.readiness.issues.map((issue) => (
            <li key={issue}>{issue}</li>
          ))}
        </ul>
      ) : null}

      {setup.operational_health === undefined ? null : (
        <OperationalHealthNotice
          health={setup.operational_health}
          phoneLabel={roleLabel(draft.role)}
        />
      )}

      <form
        className="node-setup-form"
        onSubmit={(event) => {
          event.preventDefault();
          void save();
        }}
      >
        <fieldset disabled={saving || !setup.readiness.editable}>
          <legend>Capture assignment</legend>
          <div className="setup-field-grid">
            <ChoiceField
              choices={setup.capabilities.roles}
              id={`${idPrefix}-role`}
              label="Camera view"
              onChange={(role) => {
                setDraft((current) => ({ ...current, role }));
                setSaved(false);
              }}
              value={draft.role}
            />
            <ChoiceField
              choices={setup.capabilities.capture_profiles}
              id={`${idPrefix}-profile`}
              label="High-speed profile"
              onChange={(capture_profile) => {
                setDraft((current) => ({ ...current, capture_profile }));
                setSaved(false);
              }}
              value={draft.capture_profile}
            />
          </div>
          {profile === undefined ? null : (
            <p className="setup-help">
              {profile.width}×{profile.height} · {profile.fps} fps
              {profile.value === "720p240" ? " · recommended continuous-capture profile" : ""}
            </p>
          )}
        </fieldset>

        <fieldset disabled={saving || !setup.readiness.editable}>
          <legend>Address trigger</legend>
          <div className="setup-field-grid">
            <ChoiceField
              choices={setup.capabilities.pose_modes}
              id={`${idPrefix}-pose-mode`}
              label="Pose behavior"
              onChange={(mode) => updatePose({ mode })}
              value={draft.pose.mode}
            />
            <ChoiceField
              choices={setup.capabilities.inference_delegates}
              id={`${idPrefix}-delegate`}
              label="Inference hardware"
              onChange={(inference_delegate) => updatePose({ inference_delegate })}
              value={draft.pose.inference_delegate}
            />
          </div>
          <label className="setup-checkbox">
            <input
              checked={draft.pose.debug_evidence_enabled}
              onChange={(event) =>
                updatePose({ debug_evidence_enabled: event.currentTarget.checked })
              }
              type="checkbox"
            />
            Retain low-rate pose and continuous-audio debug evidence
          </label>
          <p className="setup-help">
            Address detection uses the full frame; no hitting-area calibration is required.
          </p>
        </fieldset>

        <fieldset disabled={saving || !setup.readiness.editable}>
          <legend>Peer association</legend>
          {draft.pose.mode === "leader" ? (
            <p className="setup-help">
              A pose leader arms itself and this authenticated shadow peer. Stored tokens remain
              write-only.
            </p>
          ) : (
            <p className="setup-help">
              Only the pose leader stores an outbound peer association.
              {configuredPeerOrigin.length === 0
                ? ""
                : " This phone's existing association will be cleared when you save."}
            </p>
          )}
          <div className="setup-peer-state" role="status">
            <strong>
              {draft.pose.mode !== "leader"
                ? configuredPeerOrigin.length === 0
                  ? "No outbound association needed"
                  : "Peer association will be cleared"
                : configuredPeerOrigin.length === 0
                  ? "No peer association stored"
                  : "Peer association stored"}
            </strong>
            <span>
              {draft.pose.mode !== "leader"
                ? "Set this phone to leader mode before associating its shadow peer."
                : configuredPeerOrigin.length === 0
                  ? "Associate the second phone before enabling leader mode."
                  : `${configuredPeerOrigin} · Pending, accepted, and failed arm outcomes appear in Swing review.`}
            </span>
          </div>
          <PairingIdentityState
            binding={binding}
            configuredOrigin={configuredPeerOrigin}
            credentialStatus={setup.pairing?.credential_status ?? null}
          />
          {draft.pose.mode === "leader" ? (
            <>
              {verifiedPeers.length === 0 ? (
                <p className="setup-help">
                  No second authenticated phone is connected to this setup page. A discovered
                  address must still be opened with that phone&apos;s credential before it can be
                  identity-bound.
                </p>
              ) : (
                <section aria-label="Authenticated peer candidates" className="verified-peer-list">
                  <p>
                    <strong>Authenticated phones</strong>
                    <span>
                      These stable identities were read through each phone&apos;s authenticated
                      setup connection. Selecting one fills its current address; saving still
                      requires its write-only control token.
                    </span>
                  </p>
                  {verifiedPeers.map((peer) => {
                    const compatibility = peerCompatibility(localIdentity, peer);
                    const action =
                      compatibility === null
                        ? requiredPairingAction(
                            binding,
                            localIdentity,
                            directlyVerifiedCandidate(localIdentity, peer),
                          )
                        : null;
                    return (
                      <button
                        className="secondary verified-peer-choice"
                        disabled={compatibility !== null || clearPeer}
                        key={`${peer.node_id}-${peer.origin}`}
                        onClick={() => {
                          setPeerOrigin(peer.origin);
                          setPeerLabel(peer.label);
                          setClearPeer(false);
                          setSaved(false);
                        }}
                        title={compatibility ?? undefined}
                        type="button"
                      >
                        <strong>{peer.label}</strong>
                        <span>
                          {roleLabel(peer.role)} · {shortNodeId(peer.node_id)} · {peer.origin}
                        </span>
                        <em>
                          {compatibility ??
                            `Confirm ${pairingActionLabel(action ?? "pair")} with this phone's current token`}
                        </em>
                      </button>
                    );
                  })}
                </section>
              )}
              <label className="setup-text-field" htmlFor={`${idPrefix}-peer-origin`}>
                <span>Peer phone origin</span>
                <input
                  disabled={clearPeer}
                  id={`${idPrefix}-peer-origin`}
                  onChange={(event) => {
                    setPeerOrigin(event.currentTarget.value);
                    setSaved(false);
                  }}
                  placeholder="http://192.168.1.20:8088"
                  type="url"
                  value={peerOrigin}
                />
              </label>
              <label className="setup-text-field" htmlFor={`${idPrefix}-peer-label`}>
                <span>Peer label</span>
                <input
                  disabled={clearPeer}
                  id={`${idPrefix}-peer-label`}
                  maxLength={64}
                  onChange={(event) => {
                    setPeerLabel(event.currentTarget.value);
                    setSaved(false);
                  }}
                  placeholder="For example, Hitting bay face-on"
                  type="text"
                  value={peerLabel}
                />
              </label>
              <label className="setup-text-field" htmlFor={`${idPrefix}-peer-token`}>
                <span>Peer control token</span>
                <input
                  autoComplete="new-password"
                  disabled={clearPeer}
                  id={`${idPrefix}-peer-token`}
                  onChange={(event) => {
                    setPeerToken(event.currentTarget.value);
                    setSaved(false);
                  }}
                  placeholder={
                    configuredPeerOrigin.length === 0
                      ? "Required to associate"
                      : "Leave blank to keep stored token"
                  }
                  type="password"
                  value={peerToken}
                />
              </label>
              <label className="setup-checkbox danger-choice">
                <input
                  checked={clearPeer}
                  disabled={configuredPeerOrigin.length === 0}
                  onChange={(event) => {
                    setClearPeer(event.currentTarget.checked);
                    setSaved(false);
                  }}
                  type="checkbox"
                />
                Clear the existing peer association
              </label>
              {binding?.credential_state === "revoked" ? (
                <div className="pairing-reset">
                  <label className="setup-text-field" htmlFor={`${idPrefix}-reset-node-id`}>
                    <span>Confirm full peer node ID to forget revoked identity metadata</span>
                    <input
                      id={`${idPrefix}-reset-node-id`}
                      onChange={(event) => setResetConfirmation(event.currentTarget.value)}
                      placeholder={binding.peer_node_id}
                      type="text"
                      value={resetConfirmation}
                    />
                  </label>
                  <button
                    className="secondary"
                    disabled={resetConfirmation.trim() !== binding.peer_node_id}
                    onClick={() => void forgetRevokedIdentity()}
                    type="button"
                  >
                    Forget revoked identity
                  </button>
                </div>
              ) : null}
            </>
          ) : null}
        </fieldset>

        {api.rotateControlCredential === undefined ? null : (
          <fieldset disabled={saving || rotatingCredential || !setup.readiness.editable}>
            <legend>Phone control credential</legend>
            <p className="setup-help">
              Current generation {setup.node.control_credential_generation}. Rotation immediately
              signs this browser in with a new token and invalidates the old one. Any other browser
              or paired leader must explicitly enter the new token and re-pair.
            </p>
            <label className="setup-text-field" htmlFor={`${idPrefix}-rotate-node-id`}>
              <span>Confirm the full local node ID to rotate</span>
              <input
                autoComplete="off"
                id={`${idPrefix}-rotate-node-id`}
                onChange={(event) => setRotationConfirmation(event.currentTarget.value)}
                placeholder={setup.node.node_id}
                type="text"
                value={rotationConfirmation}
              />
            </label>
            <button
              className="secondary danger-choice"
              disabled={rotationConfirmation.trim() !== setup.node.node_id}
              onClick={() => void rotateControlCredential()}
              type="button"
            >
              {rotatingCredential ? "Rotating…" : "Rotate this phone's control credential"}
            </button>
            {rotatedControlToken === null ? null : (
              <div className="pairing-identity-state pairing-warning" role="status">
                <strong>New control token — copy it now</strong>
                <code>{rotatedControlToken}</code>
                <span>
                  The old token no longer works. Reconnect stale browsers and explicitly re-pair any
                  leader that controls this phone.
                </span>
              </div>
            )}
          </fieldset>
        )}

        {saveError !== null || error !== null ? (
          <p className="control-error" role="alert">
            {saveError ?? error}
          </p>
        ) : null}
        {saved ? (
          <p className="setup-saved" role="status">
            Phone configuration saved.
          </p>
        ) : null}
        <div className="control-actions phone-setup-actions">
          <button disabled={saving || !setup.readiness.editable} type="submit">
            {saving ? "Saving…" : "Save phone configuration"}
          </button>
          <button className="secondary" disabled={saving} onClick={onReload} type="button">
            Reload
          </button>
        </div>
      </form>
    </article>
  );
}

function OperationalHealthNotice({
  health,
  phoneLabel,
}: {
  health: OperationalHealth;
  phoneLabel: string;
}) {
  return (
    <section aria-label={`Operational health for ${phoneLabel}`}>
      <div
        className={`setup-peer-state ${health.ready_for_capture ? "" : "pairing-warning"}`}
        role={health.ready_for_capture ? "status" : "alert"}
      >
        <strong>
          {health.ready_for_capture
            ? "Capture resources ready"
            : "Capture resources need attention"}
        </strong>
        <span>{operationalHealthDescription(health)}</span>
        {health.issues.map((issue) => (
          <span key={issue}>{issue}</span>
        ))}
      </div>
    </section>
  );
}

function serverPairingBinding(setup: NodeSetupSnapshot): PeerPairingBinding | null {
  const pairing = setup.pairing;
  if (pairing === undefined || pairing === null) {
    return null;
  }
  if (pairing.expected_role !== "down_the_line" && pairing.expected_role !== "face_on") {
    return null;
  }
  return {
    schema_version: 1,
    binding_revision: setup.revision,
    leader_node_id: setup.node.node_id,
    peer_node_id: pairing.peer_node_id,
    peer_role: pairing.expected_role,
    peer_origin: pairing.origin,
    peer_label: pairing.label,
    credential_generation: pairing.credential_generation,
    credential_state: pairing.state,
    verified_at_epoch_ms: pairing.verified_at_epoch_ms,
    revoked_at_epoch_ms: pairing.state === "revoked" ? pairing.revoked_at_epoch_ms : null,
  };
}

function PairingIdentityState({
  binding,
  configuredOrigin,
  credentialStatus,
}: {
  binding: PeerPairingBinding | null;
  configuredOrigin: string;
  credentialStatus: "verified" | "re_pair_required" | "unavailable" | "revoked" | null;
}) {
  if (binding === null) {
    return (
      <div className="pairing-identity-state pairing-unverified" role="status">
        <strong>
          {configuredOrigin.length === 0
            ? "No stable peer identity bound"
            : "Address-only peer association"}
        </strong>
        <span>
          {configuredOrigin.length === 0
            ? "Select an authenticated phone and enter its current control token to create a stable identity binding."
            : "This legacy association has an address but no browser-retained node-ID binding. Verify and re-pair it before relying on automatic address recovery."}
        </span>
      </div>
    );
  }
  const configuredMatches = normalizedOriginOrNull(configuredOrigin) === binding.peer_origin;
  const needsRepair = credentialStatus === "re_pair_required";
  const unavailable = credentialStatus === "unavailable";
  return (
    <div
      className={`pairing-identity-state ${
        binding.credential_state === "revoked" || !configuredMatches || needsRepair || unavailable
          ? "pairing-warning"
          : "pairing-verified"
      }`}
      role="status"
    >
      <strong>
        {binding.credential_state === "revoked"
          ? "Peer credential revoked"
          : needsRepair
            ? "Peer credential needs re-pairing"
            : unavailable
              ? "Peer credential verification unavailable"
              : configuredMatches
                ? "Verified peer identity bound"
                : "Verified peer address changed"}
      </strong>
      <span>
        {binding.peer_label} · {roleLabel(binding.peer_role)} · {shortNodeId(binding.peer_node_id)}{" "}
        · credential generation {binding.credential_generation}
      </span>
      <span>{binding.peer_origin}</span>
      {needsRepair ? (
        <span>
          The peer rejected the stored token. Enter its current control token and save an explicit
          credential rotation/re-pair before arming.
        </span>
      ) : unavailable ? (
        <span>
          Peer verification is temporarily unavailable; the stored token is not claimed valid.
        </span>
      ) : null}
    </div>
  );
}

function peerCompatibility(
  local: AuthenticatedNodeIdentity,
  peer: AuthenticatedNodeIdentity,
): string | null {
  try {
    directlyVerifiedCandidate(local, peer);
    return null;
  } catch (caught) {
    return errorMessage(caught);
  }
}

function ChoiceField({
  id,
  label,
  choices,
  value,
  onChange,
}: {
  id: string;
  label: string;
  choices: SetupChoice[];
  value: string;
  onChange(value: string): void;
}) {
  return (
    <label className="setup-choice" htmlFor={id}>
      <span>{label}</span>
      <select id={id} onChange={(event) => onChange(event.currentTarget.value)} value={value}>
        {choices.map((choice) => (
          <option key={choice.value} value={choice.value}>
            {choice.label}
          </option>
        ))}
      </select>
    </label>
  );
}

function buildPeerUpdate(
  poseMode: string,
  configuredOrigin: string,
  draftOrigin: string,
  token: string,
  clear: boolean,
): PeerSetupUpdate {
  if (poseMode !== "leader") {
    return configuredOrigin.length === 0 ? { operation: "keep" } : { operation: "clear" };
  }
  if (clear) {
    return { operation: "clear" };
  }
  const origin = draftOrigin.trim();
  const controlToken = token.trim();
  if (origin === configuredOrigin && controlToken.length === 0) {
    return { operation: "keep" };
  }
  if (origin.length === 0 || controlToken.length === 0) {
    throw new Error("Enter both the peer origin and its control token to replace the association.");
  }
  return { operation: "replace", origin, control_token: controlToken };
}

function assignmentSummary(setups: NodeSetupSnapshot[], expectedNodes: number) {
  if (setups.length < expectedNodes) {
    return {
      ready: false,
      label: "Connecting",
      heading: `Found ${setups.length} of ${expectedNodes} phone${expectedNodes === 1 ? "" : "s"}`,
      guidance: "Check the phone address and node control token, then retry the unavailable card.",
    };
  }
  const nodeIds = setups.map((setup) => setup.node.node_id);
  if (new Set(nodeIds).size !== nodeIds.length) {
    return {
      ready: false,
      label: "Identity conflict",
      heading: "Duplicate phone identity detected",
      guidance:
        "Two authenticated addresses returned the same stable node ID. Do not pair either entry until the duplicated installation identity is reset deliberately.",
    };
  }
  const roles = new Set(setups.map((setup) => setup.configuration.role));
  const readinessIssues = setups.flatMap((setup) => setup.readiness.issues);
  if (readinessIssues.length > 0) {
    return {
      ready: false,
      label: "Attention needed",
      heading: "Resolve phone readiness",
      guidance: readinessIssues[0] ?? "Resolve the setup issues shown on each phone card.",
    };
  }
  if (setups.length === 2 && roles.has("down_the_line") && roles.has("face_on")) {
    const poseModes = setups.map((setup) => setup.configuration.pose.mode);
    const poseDisabled = poseModes.every((mode) => mode === "disabled");
    const leader = setups.find((setup) => setup.configuration.pose.mode === "leader");
    const shadow = setups.find((setup) => setup.configuration.pose.mode === "shadow");
    const pairedPoseTopology =
      leader !== undefined &&
      shadow !== undefined &&
      leader.configuration.pose.peer !== null &&
      shadow.configuration.pose.peer === null &&
      shadow.node.service_urls.some((url) =>
        sameHttpOrigin(url, leader.configuration.pose.peer?.origin ?? ""),
      );
    const disabledPoseTopology =
      poseDisabled && setups.every((setup) => setup.configuration.pose.peer === null);
    if (!disabledPoseTopology && !pairedPoseTopology) {
      return {
        ready: false,
        label: "Pose assignment needed",
        heading: "Resolve pose assignments",
        guidance:
          "Use one pose leader associated with one peer-free shadow phone, or disable pose monitoring and clear peers on both phones.",
      };
    }
    return {
      ready: true,
      label: "Both views assigned",
      heading: "Two-phone station configured",
      guidance: "One phone is down-the-line and one is across-the-line (face-on).",
    };
  }
  if (setups.length === 1 && !roles.has("unassigned")) {
    const setup = setups[0];
    const pose = setup?.configuration.pose;
    const validPoseAssociation =
      pose !== undefined && (pose.mode === "leader" ? pose.peer !== null : pose.peer === null);
    if (!validPoseAssociation) {
      return {
        ready: false,
        label: "Pose assignment needed",
        heading: "Resolve pose assignment",
        guidance:
          "A pose leader needs its shadow peer; shadow and disabled phones must not retain an outbound peer.",
      };
    }
    return {
      ready: true,
      label: "View assigned",
      heading: "Phone configuration loaded",
      guidance: "Save any changes before returning to review.",
    };
  }
  return {
    ready: false,
    label: "Assignment needed",
    heading: "Resolve camera view assignments",
    guidance:
      setups.length === 2
        ? "Assign one phone to each distinct view."
        : "Choose the view this phone will record.",
  };
}

function sameHttpOrigin(left: string, right: string): boolean {
  try {
    const leftUrl = new URL(left);
    const rightUrl = new URL(right);
    return (
      (leftUrl.protocol === "http:" || leftUrl.protocol === "https:") &&
      (rightUrl.protocol === "http:" || rightUrl.protocol === "https:") &&
      leftUrl.origin === rightUrl.origin
    );
  } catch {
    return false;
  }
}

function normalizedOriginOrNull(value: string): string | null {
  try {
    const url = new URL(value);
    return url.protocol === "http:" || url.protocol === "https:" ? url.origin : null;
  } catch {
    return null;
  }
}

function roleLabel(role: string): string {
  switch (role) {
    case "down_the_line":
      return "Down-the-line phone";
    case "face_on":
      return "Across-the-line phone";
    default:
      return "Unassigned phone";
  }
}

function shortNodeId(nodeId: string): string {
  return nodeId.length <= 12 ? nodeId : `${nodeId.slice(0, 8)}…${nodeId.slice(-4)}`;
}

function noCachePreviewUrl(url: string, requestToken: string): string {
  const hashOffset = url.indexOf("#");
  const beforeHash = hashOffset < 0 ? url : url.slice(0, hashOffset);
  const hash = hashOffset < 0 ? "" : url.slice(hashOffset);
  const separator = beforeHash.includes("?") ? "&" : "?";
  return `${beforeHash}${separator}swing_preview_request=${encodeURIComponent(requestToken)}${hash}`;
}

function friendlyPreviewReason(reason: string): string {
  switch (reason) {
    case "starting":
    case "no_frame":
      return "Waiting for the first low-rate standby frame.";
    case "high_speed_capture":
      return "High-speed capture currently owns the camera.";
    case "pose_disabled":
      return "Pose standby is disabled.";
    case "source_paused":
      return "The low-rate camera source is paused.";
    case "encoding_failed":
      return "The latest setup frame could not be encoded.";
    case "source_error":
      return "The low-rate camera source reported an error.";
    default:
      return `Preview unavailable (${reason.replaceAll("_", " ")}).`;
  }
}

function stalePreviewDescription(frameAgeMs: number | null | undefined): string {
  const age =
    frameAgeMs === undefined || frameAgeMs === null ? "" : ` (${Math.round(frameAgeMs)} ms)`;
  return `The last frame is too old${age}. Retrying setup status automatically.`;
}

function requirePositiveInterval(value: number, label: string): void {
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`${label} must be a positive integer`);
  }
}

function errorMessage(caught: unknown): string {
  return caught instanceof Error ? caught.message : "Unknown phone setup error";
}
