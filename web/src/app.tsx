import { useEffect, useMemo, useState } from "react";
import type { CameraRole, CameraStatus, NumericSetting, StationApi, StationStatus } from "./api.js";
import { PairedPreviewLoader, type PreviewPair } from "./paired_preview.js";

export interface AppProps {
  api: StationApi;
  pollIntervalMs?: number;
}

export function App({ api, pollIntervalMs = 200 }: AppProps) {
  const [status, setStatus] = useState<StationStatus | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [previewPair, setPreviewPair] = useState<PreviewPair | null>(null);
  const [previewError, setPreviewError] = useState<string | null>(null);

  const previewLoader = useMemo(
    () =>
      new PairedPreviewLoader(
        api,
        (pair) => {
          setPreviewPair(pair);
          setPreviewError(null);
        },
        (caught) => setPreviewError(caught.message),
      ),
    [api],
  );

  useEffect(() => () => previewLoader.stop(), [previewLoader]);

  useEffect(() => {
    let active = true;
    let timer: ReturnType<typeof setTimeout> | undefined;

    const poll = async () => {
      try {
        const nextStatus = await api.getStatus();
        if (active) {
          setStatus(nextStatus);
          setError(null);
        }
      } catch (caught) {
        if (active) {
          setStatus(null);
          setError(errorMessage(caught));
        }
      } finally {
        if (active) {
          timer = setTimeout(() => void poll(), pollIntervalMs);
        }
      }
    };

    void poll();
    return () => {
      active = false;
      if (timer !== undefined) {
        clearTimeout(timer);
      }
    };
  }, [api, pollIntervalMs]);

  useEffect(() => {
    const downTheLine = status?.cameras.find(
      (camera) => camera.role === "down_the_line" && camera.connected,
    );
    const faceOn = status?.cameras.find((camera) => camera.role === "face_on" && camera.connected);
    if (downTheLine !== undefined && faceOn !== undefined) {
      previewLoader.enqueue({
        down_the_line: downTheLine.preview_sequence,
        face_on: faceOn.preview_sequence,
      });
    }
  }, [previewLoader, status]);

  const notices = useMemo(() => deriveNotices(status), [status]);

  const updateCamera = (updated: CameraStatus) => {
    setStatus((current) =>
      current === null
        ? current
        : {
            ...current,
            cameras: current.cameras.map((camera) =>
              camera.role === updated.role ? updated : camera,
            ),
          },
    );
  };

  return (
    <div className="app-shell">
      <header className="masthead">
        <div>
          <p className="eyebrow">Swing Capture Station</p>
          <h1>Camera setup</h1>
          <p className="lede">
            Frame both views, set exposure, and focus each lens before arming capture.
          </p>
        </div>
        <StationBadge error={error} status={status} />
      </header>

      <main>
        <section aria-labelledby="readiness-heading" className="readiness-panel">
          <div>
            <p className="section-kicker">Station readiness</p>
            <h2 id="readiness-heading">{readinessTitle(status, error)}</h2>
          </div>
          <div className="notices" aria-live="polite">
            {error !== null ? <p className="notice notice-error">{error}</p> : null}
            {status === null && error === null ? (
              <p className="notice">Loading camera status…</p>
            ) : null}
            {notices.map((notice) => (
              <p className={`notice notice-${notice.tone}`} key={notice.text}>
                {notice.text}
              </p>
            ))}
          </div>
        </section>

        {status !== null ? (
          <section aria-label="Camera previews" className="camera-grid">
            {orderedCameras(status.cameras).map((camera) => (
              <CameraCard
                api={api}
                camera={camera}
                key={camera.role}
                onCameraUpdated={updateCamera}
                previewError={previewError}
                previewUrl={previewPair?.urls[camera.role]}
              />
            ))}
          </section>
        ) : null}
      </main>

      <footer>
        Live preview is intentionally low rate. Capture transport continues at the measured stream
        rate shown on each card.
      </footer>
    </div>
  );
}

interface CameraCardProps {
  api: StationApi;
  camera: CameraStatus;
  onCameraUpdated(camera: CameraStatus): void;
  previewError: string | null;
  previewUrl: string | undefined;
}

function CameraCard({ api, camera, onCameraUpdated, previewError, previewUrl }: CameraCardProps) {
  const [draftExposure, setDraftExposure] = useState(camera.exposure_us.value);
  const [draftGain, setDraftGain] = useState(camera.gain_db.value);
  const [dirty, setDirty] = useState(false);
  const [saving, setSaving] = useState(false);
  const [saveError, setSaveError] = useState<string | null>(null);
  const [decodeError, setDecodeError] = useState(false);

  useEffect(() => {
    if (!dirty) {
      setDraftExposure(camera.exposure_us.value);
      setDraftGain(camera.gain_db.value);
    }
  }, [camera.exposure_us.value, camera.gain_db.value, dirty]);

  const title = roleLabel(camera.role);
  const headingId = `${camera.role}-camera-heading`;
  const fullResolutionPreviewUrl = api.fullResolutionPreviewUrl(
    camera.role,
    camera.preview_sequence,
  );

  useEffect(() => setDecodeError(false), [previewUrl]);

  const revert = () => {
    setDraftExposure(camera.exposure_us.value);
    setDraftGain(camera.gain_db.value);
    setDirty(false);
    setSaveError(null);
  };

  const apply = async () => {
    setSaving(true);
    setSaveError(null);
    try {
      const updated = await api.updateCameraSettings(camera.role, {
        exposure_us: draftExposure,
        gain_db: draftGain,
      });
      onCameraUpdated(updated);
      setDraftExposure(updated.exposure_us.value);
      setDraftGain(updated.gain_db.value);
      setDirty(false);
    } catch (caught) {
      setSaveError(errorMessage(caught));
    } finally {
      setSaving(false);
    }
  };

  return (
    <article
      aria-labelledby={headingId}
      className={`camera-card${camera.connected ? "" : " is-disconnected"}`}
    >
      <header className="camera-header">
        <div>
          <h2 className="role-label" id={headingId}>
            {title}
          </h2>
          <p className="camera-model">{camera.model}</p>
          <p className="serial">Serial {camera.serial}</p>
        </div>
        <span className={`connection ${camera.connected ? "online" : "offline"}`}>
          <span aria-hidden="true" className="connection-dot" />
          {camera.connected ? "Connected" : "Disconnected"}
        </span>
      </header>

      <figure className="preview-frame">
        {camera.connected ? (
          <div className="preview-image-shell">
            {previewUrl === undefined ? (
              <div className="preview-loading" role="status">
                Loading synchronized previews…
              </div>
            ) : (
              <img
                alt={`${title} live setup preview`}
                height={camera.preview_height}
                onError={() => setDecodeError(true)}
                src={previewUrl}
                width={camera.preview_width}
              />
            )}
            {previewError !== null || decodeError ? (
              <span className="preview-state preview-error" role="alert">
                <strong>Preview update delayed</strong>
                {decodeError ? "The latest paired image could not be decoded." : previewError}
              </span>
            ) : null}
          </div>
        ) : (
          <div className="preview-missing" role="img" aria-label={`${title} unavailable`}>
            <span>Camera unavailable</span>
            {camera.error.length === 0
              ? "Check USB power and the station role assignment."
              : camera.error}
          </div>
        )}
        <figcaption>
          <span>{camera.stream_fps.toFixed(1)} fps capture</span>
          <span>
            {camera.preview_width} × {camera.preview_height}
          </span>
          {camera.connected ? (
            <a href={fullResolutionPreviewUrl} target="_blank" rel="noreferrer">
              Open full resolution
              <span className="sr-only"> for {title} manual focus</span>
            </a>
          ) : null}
        </figcaption>
      </figure>

      <div className="quality-row">
        <div className={`quality-assessment quality-${camera.image_quality.assessment}`}>
          <span className="quality-icon" aria-hidden="true" />
          <div>
            <strong>{qualityTitle(camera)}</strong>
            <span>{qualityGuidance(camera)}</span>
          </div>
        </div>
        <dl className="quality-metrics" aria-label={`${title} image metrics`}>
          <div>
            <dt>Mean</dt>
            <dd>{camera.image_quality.mean.toFixed(1)}</dd>
          </div>
          <div>
            <dt>P99</dt>
            <dd>{camera.image_quality.p99.toFixed(0)}</dd>
          </div>
          <div>
            <dt>Focus energy</dt>
            <dd>{camera.image_quality.gradient_energy.toFixed(1)}</dd>
          </div>
        </dl>
      </div>

      <form
        className="camera-controls"
        onSubmit={(event) => {
          event.preventDefault();
          void apply();
        }}
      >
        <SettingControl
          disabled={!camera.connected || saving}
          label={`${title} exposure`}
          range={camera.exposure_us}
          unit="µs"
          value={draftExposure}
          onChange={(value) => {
            setDraftExposure(value);
            setDirty(true);
          }}
        />
        <SettingControl
          disabled={!camera.connected || saving}
          label={`${title} gain`}
          range={camera.gain_db}
          unit="dB"
          value={draftGain}
          onChange={(value) => {
            setDraftGain(value);
            setDirty(true);
          }}
        />

        <p className="readback">
          Camera read-back: {camera.exposure_us.value.toFixed(0)} µs ·{" "}
          {camera.gain_db.value.toFixed(1)} dB
        </p>
        {saveError !== null ? (
          <p className="control-error" role="alert">
            {saveError}
          </p>
        ) : null}
        <div className="control-actions">
          <button disabled={!dirty || saving || !camera.connected} type="submit">
            {saving ? "Applying…" : "Apply settings"}
          </button>
          <button className="secondary" disabled={!dirty || saving} onClick={revert} type="button">
            Revert
          </button>
        </div>
      </form>
    </article>
  );
}

interface SettingControlProps {
  disabled: boolean;
  label: string;
  range: NumericSetting;
  unit: string;
  value: number;
  onChange(value: number): void;
}

function SettingControl({ disabled, label, range, unit, value, onChange }: SettingControlProps) {
  const id = label.toLowerCase().replaceAll(/[^a-z0-9]+/g, "-");
  return (
    <div className="setting-control">
      <div className="setting-heading">
        <label htmlFor={id}>{label}</label>
        <output htmlFor={id}>
          {formatSetting(value, range.increment)} {unit}
        </output>
      </div>
      <input
        disabled={disabled}
        id={id}
        max={range.max}
        min={range.min}
        onChange={(event) => onChange(event.currentTarget.valueAsNumber)}
        step={range.increment}
        type="range"
        value={value}
      />
      <div className="range-labels" aria-hidden="true">
        <span>{formatSetting(range.min, range.increment)}</span>
        <span>{formatSetting(range.max, range.increment)}</span>
      </div>
    </div>
  );
}

function StationBadge({ error, status }: { error: string | null; status: StationStatus | null }) {
  const label =
    error !== null
      ? "Unavailable"
      : status === null
        ? "Connecting"
        : status.mode.replaceAll("_", " ");
  return (
    <div className="station-badge">
      <span>Mode</span>
      <strong>{label}</strong>
    </div>
  );
}

interface Notice {
  tone: "ok" | "warning";
  text: string;
}

function deriveNotices(status: StationStatus | null): Notice[] {
  if (status === null) {
    return [];
  }
  const disconnected = status.cameras.filter((camera) => !camera.connected);
  const needsAttention = status.cameras.filter(
    (camera) => camera.connected && camera.image_quality.assessment !== "nominal",
  );
  const notices: Notice[] = [];
  if (disconnected.length === 0) {
    notices.push({ tone: "ok", text: "Both configured camera roles are online." });
  } else {
    notices.push({
      tone: "warning",
      text: `${disconnected.map((camera) => roleLabel(camera.role)).join(" and ")} camera is offline.`,
    });
  }
  if (needsAttention.length > 0) {
    notices.push({
      tone: "warning",
      text: `${needsAttention.map((camera) => roleLabel(camera.role)).join(" and ")} image needs attention.`,
    });
  }
  return notices;
}

function readinessTitle(status: StationStatus | null, error: string | null): string {
  if (status === null) {
    return error === null ? "Connecting to station" : "Station unavailable";
  }
  const nominal = status.cameras.every(
    (camera) => camera.connected && camera.image_quality.assessment === "nominal",
  );
  return nominal ? "Ready to frame and focus" : "Setup needs attention";
}

function orderedCameras(cameras: CameraStatus[]): CameraStatus[] {
  const order: Record<CameraRole, number> = { down_the_line: 0, face_on: 1 };
  return [...cameras].sort((left, right) => order[left.role] - order[right.role]);
}

function roleLabel(role: CameraRole): string {
  return role === "down_the_line" ? "Down-the-line" : "Face-on";
}

function qualityTitle(camera: CameraStatus): string {
  if (hasNearlyZeroSignal(camera)) {
    return "No optical signal detected";
  }
  switch (camera.image_quality.assessment) {
    case "nominal":
      return "Image looks usable";
    case "too_dark":
      return "Image is too dark";
    case "too_bright":
      return "Highlights are clipping";
    case "soft":
      return "Check focus";
    case "unavailable":
      return "Quality unavailable";
  }
}

function qualityGuidance(camera: CameraStatus): string {
  if (hasNearlyZeroSignal(camera)) {
    const exposureNote = exposureNearMaximum(camera)
      ? " Exposure is already near its frame-rate-safe maximum."
      : "";
    return `Check the lens cap, lens aperture, and physical obstruction.${exposureNote}`;
  }
  switch (camera.image_quality.assessment) {
    case "nominal":
      return "Exposure and edge detail are in range.";
    case "too_dark":
      return "Add light or increase exposure before gain.";
    case "too_bright":
      return "Reduce exposure until bright detail returns.";
    case "soft":
      return "Open full resolution and adjust the lens focus ring.";
    case "unavailable":
      return "Wait for a valid preview frame.";
  }
}

function hasNearlyZeroSignal(camera: CameraStatus): boolean {
  return (
    camera.image_quality.assessment === "too_dark" &&
    camera.image_quality.mean < 1 &&
    camera.image_quality.p99 <= 1
  );
}

function exposureNearMaximum(camera: CameraStatus): boolean {
  const range = camera.exposure_us.max - camera.exposure_us.min;
  return range > 0 && (camera.exposure_us.max - camera.exposure_us.value) / range <= 0.1;
}

function formatSetting(value: number, increment: number): string {
  return increment < 1 ? value.toFixed(1) : value.toFixed(0);
}

function errorMessage(caught: unknown): string {
  return caught instanceof Error ? caught.message : "Unknown station error";
}
