export interface FieldRecorderCleanupResult {
  origin: string;
  stop_status: number | null;
  final_state: string;
}

export interface FieldRecorderCleanupTransport {
  stop(origin: string, token: string): Promise<number>;
  status(origin: string, token: string): Promise<string>;
  wait(milliseconds: number): Promise<void>;
}

export interface PrimaryStopEvidence {
  origin: string;
  statuses: number[];
}

export interface ReviewMediaAbortEvidence {
  origin: string;
  path: string;
  error: "net::ERR_ABORTED";
}

export interface FreshDecodeMediaAbortEvidence {
  role: "down_the_line" | "face_on";
  origin: string;
  media_identity: string;
  error: "net::ERR_ABORTED";
}

const FIELD_RECORDING_STOP_PATH = "/api/v1/field-recording/stop";
const MEDIA_ACCESS_PATTERN = /^[A-Za-z0-9_-]{43}$/;

/**
 * Identifies immutable media independently of its scoped access capability.
 *
 * A capability changes when the control credential rotates, and is deliberately present on the
 * native media URL. Using the full href for before/after catalog comparisons therefore makes every
 * retained recording look new after a rotation or query-shape change.
 */
export function stableMediaIdentity(value: string): string {
  const url = new URL(value);
  return `${url.origin}${url.pathname}`;
}

/** Returns the one valid scoped media capability, or null for any ambiguous/invalid query. */
export function scopedMediaCapability(value: string): string | null {
  const url = new URL(value);
  const keys = [...url.searchParams.keys()];
  if (keys.length !== 1 || keys[0] !== "media_access") return null;
  const capability = url.searchParams.get("media_access");
  return capability !== null && MEDIA_ACCESS_PATTERN.test(capability) ? capability : null;
}

/**
 * Classifies Chrome cancelling an old review player's Range request when React replaces that
 * player's selected session. Fresh field-recording media and all transport/HTTP failures remain
 * release-gate failures; the caller separately proves that this exact historical media path
 * returned HTTP 206 before accepting the cancellation as benign.
 */
export function reviewMediaAbortEvidence(
  value: string,
  error: string | undefined,
  allowedOrigins: readonly string[],
): ReviewMediaAbortEvidence | null {
  const url = new URL(value);
  if (
    error !== "net::ERR_ABORTED" ||
    !allowedOrigins.includes(url.origin) ||
    !/^\/api\/v1\/sessions\/[A-Za-z0-9._-]+\/(?:down_the_line|face_on)\.mp4$/.test(url.pathname)
  ) {
    return null;
  }
  return { origin: url.origin, path: url.pathname, error };
}

/** Classifies only cancellation deliberately caused when the HIL releases a proven decoder. */
export function freshDecodeMediaAbortEvidence(
  value: string,
  error: string | undefined,
  allowedMedia: ReadonlyMap<string, FreshDecodeMediaAbortEvidence["role"]>,
): FreshDecodeMediaAbortEvidence | null {
  const mediaIdentity = stableMediaIdentity(value);
  const role = allowedMedia.get(mediaIdentity);
  if (error !== "net::ERR_ABORTED" || role === undefined) return null;
  return {
    role,
    origin: new URL(value).origin,
    media_identity: mediaIdentity,
    error,
  };
}

export async function restoreStoppedFieldRecorder(
  origin: string,
  token: string,
  transport: FieldRecorderCleanupTransport,
): Promise<FieldRecorderCleanupResult> {
  let stopStatus: number | null = null;
  try {
    stopStatus = await transport.stop(origin, token);
  } catch {
    // A timed-out redundant stop may still have reached a recorder that the primary UI action
    // already stopped. Always inspect terminal state independently before judging restoration.
  }

  let finalState = "status_unreachable";
  for (let attempt = 0; attempt < 10; attempt += 1) {
    try {
      finalState = await transport.status(origin, token);
      if (isTerminalFieldRecorderState(finalState)) break;
    } catch {
      finalState = "status_unreachable";
    }
    if (attempt + 1 < 10) await transport.wait(100);
  }
  return { origin, stop_status: stopStatus, final_state: finalState };
}

export function primaryStopEvidence(
  apiStatuses: Readonly<Record<string, readonly number[]>>,
  origins: readonly string[],
): PrimaryStopEvidence[] {
  return origins.map((origin) => ({
    origin,
    statuses: [...(apiStatuses[`POST ${origin}${FIELD_RECORDING_STOP_PATH}`] ?? [])],
  }));
}

export function cleanupRestored(cleanup: readonly FieldRecorderCleanupResult[]): boolean {
  return (
    cleanup.length === 2 && cleanup.every((item) => isTerminalFieldRecorderState(item.final_state))
  );
}

function isTerminalFieldRecorderState(state: string): boolean {
  return state === "ready" || state === "idle";
}
