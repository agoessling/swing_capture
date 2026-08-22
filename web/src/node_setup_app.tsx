import { useEffect, useRef, useState } from "react";
import type {
  HittingRegion,
  NodeSetupApi,
  NodeSetupConfigurationUpdate,
  NodeSetupSnapshot,
  PeerSetupUpdate,
  SetupChoice,
} from "./node_setup_api.js";

export function NodeSetupApp({ apis }: { apis: readonly NodeSetupApi[] }) {
  const [nodes, setNodes] = useState<
    Array<{ api: NodeSetupApi; setup: NodeSetupSnapshot | null; error: string | null }>
  >(() => apis.map((api) => ({ api, setup: null, error: null })));

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
                  error={node.error}
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
                />
              )}
            </div>
          ))}
        </section>
      </main>

      <footer>
        Peer control credentials are write-only. The browser can retain an existing association,
        replace it with a newly entered token, or clear it; a phone never returns stored tokens.
      </footer>
    </div>
  );
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

function NodeSetupCard({
  api,
  setup,
  error,
  onUpdated,
  onReload,
}: {
  api: NodeSetupApi;
  setup: NodeSetupSnapshot;
  error: string | null;
  onUpdated(setup: NodeSetupSnapshot): void;
  onReload(): void;
}) {
  const [draft, setDraft] = useState(setup.configuration);
  const [peerOrigin, setPeerOrigin] = useState(setup.configuration.pose.peer?.origin ?? "");
  const [peerToken, setPeerToken] = useState("");
  const [clearPeer, setClearPeer] = useState(false);
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
    setSaveError(null);
  }, [setup]);

  const idPrefix = `node-${setup.node.node_id.replaceAll(/[^a-zA-Z0-9_-]/g, "-")}`;
  const configuredPeerOrigin = setup.configuration.pose.peer?.origin ?? "";
  const profile = setup.capabilities.capture_profiles.find(
    (candidate) => candidate.value === draft.capture_profile,
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
          hitting_region: draft.pose.hitting_region,
          peer_update: peerUpdate,
        },
      };
      onUpdated(await api.updateSetup(setup.revision, configuration));
      setSaved(true);
    } catch (caught) {
      setSaveError(errorMessage(caught));
    } finally {
      setSaving(false);
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

      {setup.preview.available && setup.preview.url !== null ? (
        <figure className="phone-preview">
          <img alt={`${roleLabel(draft.role)} setup preview`} src={setup.preview.url} />
          <figcaption>Low-rate standby preview</figcaption>
        </figure>
      ) : (
        <div className="phone-preview-placeholder">
          <strong>Preview unavailable</strong>
          <span>Use the phone display for physical framing in this build.</span>
        </div>
      )}

      {setup.readiness.issues.length > 0 ? (
        <ul className="setup-issues" aria-label="Setup issues">
          {setup.readiness.issues.map((issue) => (
            <li key={issue}>{issue}</li>
          ))}
        </ul>
      ) : null}

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
          <HittingRegionFields
            idPrefix={idPrefix}
            onChange={(hitting_region) => updatePose({ hitting_region })}
            region={draft.pose.hitting_region}
          />
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
          {draft.pose.mode === "leader" ? (
            <>
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
            </>
          ) : null}
        </fieldset>

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

function HittingRegionFields({
  idPrefix,
  region,
  onChange,
}: {
  idPrefix: string;
  region: HittingRegion;
  onChange(region: HittingRegion): void;
}) {
  return (
    <div className="hitting-region-fields">
      {(Object.keys(region) as Array<keyof HittingRegion>).map((edge) => (
        <label htmlFor={`${idPrefix}-region-${edge}`} key={edge}>
          <span>{capitalize(edge)}</span>
          <input
            id={`${idPrefix}-region-${edge}`}
            max={1}
            min={0}
            onChange={(event) => onChange({ ...region, [edge]: event.currentTarget.valueAsNumber })}
            step={0.01}
            type="number"
            value={region[edge]}
          />
        </label>
      ))}
    </div>
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

function capitalize(value: string): string {
  return value.charAt(0).toUpperCase() + value.slice(1);
}

function errorMessage(caught: unknown): string {
  return caught instanceof Error ? caught.message : "Unknown phone setup error";
}
