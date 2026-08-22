import { useState } from "react";
import {
  type ClipManifest,
  DIAGNOSTIC_CLASSIFICATIONS,
  DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
  type DiagnosticArchive,
  type DiagnosticClassification,
  type DiagnosticFeedback,
  type DiagnosticTimingMarks,
  MAX_DIAGNOSTIC_NOTE_LENGTH,
  type ReviewApi,
} from "./review_api.js";

interface DiagnosticFeedbackPanelProps {
  api: ReviewApi;
  manifest?: ClipManifest;
  diagnosticOnlySessionId?: string;
}

interface TimingWindow {
  minimumUs: number;
  maximumUs: number;
}

const CLASSIFICATION_LABELS: Record<DiagnosticClassification, string> = {
  good_capture: "Good capture",
  armed_too_early: "Armed too early",
  armed_too_late: "Armed too late",
  missed_shot: "Missed shot",
  false_impact: "False impact trigger",
  av_sync_wrong: "Audio/video sync is wrong",
  other: "Other",
};

const MISSED_SHOT_AUDIO_WINDOW: TimingWindow = {
  minimumUs: -10_000_000,
  maximumUs: 2_000_000,
};

export function DiagnosticFeedbackPanel({
  api,
  manifest,
  diagnosticOnlySessionId,
}: DiagnosticFeedbackPanelProps) {
  const sessionId = manifest?.session_id ?? diagnosticOnlySessionId;
  if (sessionId === undefined) {
    throw new Error("Diagnostic feedback requires a capture manifest or diagnostic session ID");
  }
  const diagnosticOnly = manifest === undefined;
  const [classification, setClassification] = useState<DiagnosticClassification>(
    manifest?.trigger.source === "missed_shot"
      ? "missed_shot"
      : diagnosticOnly
        ? "other"
        : "good_capture",
  );
  const [note, setNote] = useState("");
  const [desiredStartMs, setDesiredStartMs] = useState("");
  const [visualImpactMs, setVisualImpactMs] = useState("");
  const [audioImpactMs, setAudioImpactMs] = useState("");
  const [feedbackPending, setFeedbackPending] = useState(false);
  const [exportPending, setExportPending] = useState(false);
  const [feedbackStatus, setFeedbackStatus] = useState<string | null>(null);
  const [exportStatus, setExportStatus] = useState<string | null>(null);
  const [diagnosticError, setDiagnosticError] = useState<string | null>(null);
  const timingWindow = manifest === undefined ? null : manifestTimingWindow(manifest);
  const audioTimingWindow =
    manifest?.trigger.source === "missed_shot" ? MISSED_SHOT_AUDIO_WINDOW : timingWindow;

  const submitFeedback = async () => {
    setFeedbackPending(true);
    setFeedbackStatus(null);
    setDiagnosticError(null);
    try {
      const feedback = buildDiagnosticFeedback({
        classification,
        note,
        timingWindow,
        audioTimingWindow,
        desiredStartMs,
        visualImpactMs,
        audioImpactMs,
      });
      await api.submitDiagnosticFeedback(sessionId, feedback);
      setFeedbackStatus("Diagnostic feedback saved.");
    } catch (caught) {
      setDiagnosticError(errorMessage(caught));
    } finally {
      setFeedbackPending(false);
    }
  };

  const downloadDiagnostics = async () => {
    setExportPending(true);
    setExportStatus(null);
    setDiagnosticError(null);
    try {
      const archives = await api.getDiagnosticArchives(sessionId);
      if (archives.length === 0) {
        throw new Error("The station returned no diagnostic ZIP.");
      }
      for (const archive of archives) {
        startArchiveDownload(archive);
      }
      setExportStatus(
        archives.length === 1
          ? "Diagnostic ZIP download started."
          : `${archives.length} node diagnostic ZIP downloads started.`,
      );
    } catch (caught) {
      setDiagnosticError(errorMessage(caught));
    } finally {
      setExportPending(false);
    }
  };

  return (
    <section
      aria-busy={feedbackPending || exportPending}
      aria-labelledby="capture-diagnostics-heading"
      className="diagnostic-feedback"
    >
      <header>
        <div>
          <p className="section-kicker">{diagnosticOnly ? "Standby evidence" : "Post-capture"}</p>
          <h3 id="capture-diagnostics-heading">
            {diagnosticOnly ? "Standby diagnostics" : "Capture diagnostics"}
          </h3>
          <p>
            {diagnosticOnly
              ? "This session contains diagnostic audio and optional pose evidence, but no review video. Label it or retain a portable evidence bundle for debugging."
              : "Label this result and retain a portable evidence bundle for debugging."}
          </p>
        </div>
        <button
          className="secondary diagnostic-export"
          disabled={feedbackPending || exportPending}
          onClick={() => void downloadDiagnostics()}
          type="button"
        >
          {exportPending ? "Preparing diagnostic ZIP…" : "Download diagnostic ZIP"}
        </button>
      </header>

      <form
        className="diagnostic-form"
        onSubmit={(event) => {
          event.preventDefault();
          void submitFeedback();
        }}
      >
        <label className="diagnostic-classification">
          <span>Result</span>
          <select
            onChange={(event) =>
              setClassification(event.currentTarget.value as DiagnosticClassification)
            }
            value={classification}
          >
            {DIAGNOSTIC_CLASSIFICATIONS.map((value) => (
              <option key={value} value={value}>
                {CLASSIFICATION_LABELS[value]}
              </option>
            ))}
          </select>
        </label>

        <label className="diagnostic-note">
          <span>Note (optional)</span>
          <textarea
            maxLength={MAX_DIAGNOSTIC_NOTE_LENGTH}
            onChange={(event) => setNote(event.currentTarget.value)}
            placeholder={
              diagnosticOnly
                ? "What happened while the station was waiting?"
                : "What looked wrong, and in which view?"
            }
            rows={2}
            value={note}
          />
          <small aria-hidden="true">
            {note.length}/{MAX_DIAGNOSTIC_NOTE_LENGTH}
          </small>
        </label>

        {timingWindow === null ? null : (
          <details className="diagnostic-timing">
            <summary>Timing marks (optional)</summary>
            <p>Milliseconds relative to the detected impact; negative values are before impact.</p>
            {manifest?.trigger.source === "missed_shot" ? (
              <p>Missed-shot diagnostic audio spans up to 10 seconds before and 2 seconds after.</p>
            ) : null}
            <div>
              <TimingInput
                label="Desired high-speed start"
                onChange={setDesiredStartMs}
                timingWindow={timingWindow}
                value={desiredStartMs}
              />
              <TimingInput
                label="Visual impact"
                onChange={setVisualImpactMs}
                timingWindow={timingWindow}
                value={visualImpactMs}
              />
              <TimingInput
                label="Audio impact"
                onChange={setAudioImpactMs}
                timingWindow={audioTimingWindow ?? timingWindow}
                value={audioImpactMs}
              />
            </div>
          </details>
        )}

        <button disabled={feedbackPending || exportPending} type="submit">
          {feedbackPending ? "Saving feedback…" : "Save diagnostic feedback"}
        </button>
      </form>

      <div aria-live="polite" className="diagnostic-status">
        {feedbackStatus === null ? null : <p role="status">{feedbackStatus}</p>}
        {exportStatus === null ? null : <p role="status">{exportStatus}</p>}
      </div>
      {diagnosticError === null ? null : (
        <p className="review-error" role="alert">
          {diagnosticError}
        </p>
      )}
    </section>
  );
}

function TimingInput({
  label,
  onChange,
  timingWindow,
  value,
}: {
  label: string;
  onChange: (value: string) => void;
  timingWindow: TimingWindow;
  value: string;
}) {
  return (
    <label>
      <span>{label} (ms)</span>
      <input
        inputMode="decimal"
        max={Math.floor(timingWindow.maximumUs / 1_000)}
        min={Math.ceil(timingWindow.minimumUs / 1_000)}
        onChange={(event) => onChange(event.currentTarget.value)}
        step="0.1"
        type="number"
        value={value}
      />
    </label>
  );
}

function manifestTimingWindow(manifest: ClipManifest): TimingWindow | null {
  const windows = manifest.views.map((view) => {
    const times = view.frames.map((frame) => frame.time_from_impact_us);
    return times.length === 0
      ? null
      : { minimumUs: Math.min(...times), maximumUs: Math.max(...times) };
  });
  if (windows.length === 0 || windows.some((window) => window === null)) {
    return null;
  }
  const commonWindow = {
    minimumUs: Math.max(...windows.map((window) => window?.minimumUs ?? Number.POSITIVE_INFINITY)),
    maximumUs: Math.min(...windows.map((window) => window?.maximumUs ?? Number.NEGATIVE_INFINITY)),
  };
  return commonWindow.minimumUs <= commonWindow.maximumUs ? commonWindow : null;
}

function buildDiagnosticFeedback({
  classification,
  note,
  timingWindow,
  audioTimingWindow,
  desiredStartMs,
  visualImpactMs,
  audioImpactMs,
}: {
  classification: DiagnosticClassification;
  note: string;
  timingWindow: TimingWindow | null;
  audioTimingWindow: TimingWindow | null;
  desiredStartMs: string;
  visualImpactMs: string;
  audioImpactMs: string;
}): DiagnosticFeedback {
  const trimmedNote = note.trim();
  const timingMarks =
    timingWindow === null
      ? undefined
      : compactTimingMarks({
          desired_high_speed_start_us: parseTimingMark(
            desiredStartMs,
            "Desired high-speed start",
            timingWindow,
          ),
          visual_impact_us: parseTimingMark(visualImpactMs, "Visual impact", timingWindow),
          audio_impact_us: parseTimingMark(
            audioImpactMs,
            "Audio impact",
            audioTimingWindow ?? timingWindow,
          ),
        });
  return {
    schema_version: DIAGNOSTIC_FEEDBACK_SCHEMA_VERSION,
    classification,
    ...(trimmedNote.length === 0 ? {} : { note: trimmedNote }),
    ...(timingMarks === undefined ? {} : { timing_marks_us: timingMarks }),
  };
}

function parseTimingMark(value: string, label: string, window: TimingWindow): number | undefined {
  if (value.trim().length === 0) {
    return undefined;
  }
  const microseconds = Math.round(Number(value) * 1_000);
  if (
    !Number.isSafeInteger(microseconds) ||
    microseconds < window.minimumUs ||
    microseconds > window.maximumUs
  ) {
    throw new Error(
      `${label} must be within the retained clip (${formatMilliseconds(window.minimumUs)} to ${formatMilliseconds(window.maximumUs)} ms).`,
    );
  }
  return microseconds;
}

function compactTimingMarks(
  marks: { [Key in keyof DiagnosticTimingMarks]: number | undefined },
): DiagnosticTimingMarks | undefined {
  const present = Object.fromEntries(
    Object.entries(marks).filter((entry): entry is [string, number] => entry[1] !== undefined),
  ) as DiagnosticTimingMarks;
  return Object.keys(present).length === 0 ? undefined : present;
}

function formatMilliseconds(microseconds: number): string {
  return (microseconds / 1_000).toFixed(1);
}

function startArchiveDownload(archive: DiagnosticArchive): void {
  const url = URL.createObjectURL(archive.data);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = archive.filename;
  anchor.hidden = true;
  document.body.append(anchor);
  anchor.click();
  anchor.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 0);
}

function errorMessage(caught: unknown): string {
  return caught instanceof Error ? caught.message : "Unknown diagnostic operation error";
}
