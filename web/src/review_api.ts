export const REVIEW_SCHEMA_VERSION = 1 as const;
export const PIPELINE_PROFILE_SCHEMA_VERSION = 2 as const;
export const CAPTURE_SCHEMA_VERSION = 2 as const;

export type CaptureState =
  | "setup"
  | "arming"
  | "armed"
  | "waiting_post_roll"
  | "encoding"
  | "ready"
  | "error";

export type ReviewRole = "down_the_line" | "face_on";

export type SyntheticSwingHilStage =
  | "idle"
  | "calibrating"
  | "stimulus"
  | "capturing"
  | "encoding"
  | "ready"
  | "error";

export interface SyntheticSwingHilRun {
  session_id: string | null;
  stage: SyntheticSwingHilStage;
  error: string;
}

export interface SyntheticSwingHilStatus {
  enabled: boolean;
  busy: boolean;
  stage: SyntheticSwingHilStage;
  error: string;
  last_run: SyntheticSwingHilRun | null;
}

export interface CaptureStatus {
  schema_version: typeof CAPTURE_SCHEMA_VERSION;
  state: CaptureState;
  armed: boolean;
  active_session_id: string | null;
  error: string;
  hil: SyntheticSwingHilStatus;
}

export interface SessionSummary {
  session_id: string;
  state: CaptureState;
  created_at_utc: string;
  error: string;
}

export interface SessionList {
  schema_version: typeof REVIEW_SCHEMA_VERSION;
  sessions: SessionSummary[];
}

export interface ClipFrame {
  frame_index: number;
  frame_id: string;
  device_timestamp: string;
  time_from_impact_us: number;
  media_time_us: number;
}

export interface ClipMedia {
  path: string;
  url?: string;
  mime_type: string;
  codec: string;
  all_frames_keyframes: boolean;
  encoded_bytes: number;
}

export interface ClipTrigger {
  source: string;
  host_monotonic_time_ns: string;
  confirmation_host_monotonic_time_ns: string | null;
  sample_rate_hz: number | null;
  peak_amplitude: number | null;
  noise_floor: number | null;
  threshold: number | null;
}

export interface ClipImageGeometry {
  width: number;
  height: number;
}

export interface ClipSourceGeometry extends ClipImageGeometry {
  pixel_format: "BayerRG8";
}

export interface ClipTrack {
  role: ReviewRole;
  camera_serial: string;
  source: ClipSourceGeometry;
  encoded: ClipImageGeometry;
  frame_count: number;
  nominal_fps: number;
  impact_frame_index: number;
  media: ClipMedia;
  frames: ClipFrame[];
}

export interface HilRoleValues {
  down_the_line: number;
  face_on: number;
}

export interface SyntheticSwingTimeline {
  step_duration_us: number;
  pre_impact_step_count: number;
  white_impact_duration_us: number;
  post_impact_step_count: number;
}

export interface SyntheticSwingTone {
  duration_us: number;
  frequency_hz: number;
}

export interface SyntheticSwingScheduleAlignment {
  mapped_time_correction_us: number;
  uncertainty_us: number;
}

export interface SyntheticSwingWhiteImpact {
  passed: boolean;
  stable_frame_count: number;
  matching_frame_count: number;
  matching_fraction: number;
  mean_signal_delta: number;
  mean_expected_color_distance: number;
  maximum_saturated_fraction: number;
  maximum_bloom_fraction: number;
  exposure_us: number;
  gain_db: number;
}

export interface HilRoleEvidence<T> {
  down_the_line: T;
  face_on: T;
}

export interface SyntheticSwingHilEvidence {
  kind: "synthetic_swing";
  selected_brightness: number;
  timeline: SyntheticSwingTimeline;
  tone: SyntheticSwingTone;
  optical_white_impact_frame_index: HilRoleValues;
  audio_trigger_estimate_offset_us: HilRoleValues;
  camera_schedule_alignment: HilRoleEvidence<SyntheticSwingScheduleAlignment>;
  optical_white_impact: HilRoleEvidence<SyntheticSwingWhiteImpact>;
}

export interface CapturePipelineProfile {
  trigger_estimate_to_confirmation_ms: number;
  confirmation_to_acceptance_ms: number;
  acceptance_to_freeze_start_ms: number;
  freeze_schedule_lateness_ms: number;
  freeze_and_rotate_ms: number;
  audio_stop_ms: number;
}

export interface SessionPipelineProfile {
  prepublication_analysis_ms: number;
  publisher_planning_ms: number;
  validation_and_timeline_ms: number;
  output_setup_ms: number;
  media_encoding_wall_ms: number;
  frame_metadata_ms: number;
  profile_snapshot_after_confirmation_ms: number;
  profile_snapshot_host_monotonic_ns: string;
}

export interface ViewPipelineProfile {
  role: ReviewRole;
  frame_count: number;
  timeline_ms: number;
  bayer_fit_demosaic_ms: number;
  rgb_to_yuv420_ms: number;
  codec_encode_ms: number;
  webm_mux_ms: number;
  finalize_ms: number;
  output_verification_ms: number;
  total_ms: number;
}

export interface PipelineProfile {
  schema_version: 1 | typeof PIPELINE_PROFILE_SCHEMA_VERSION;
  capture: CapturePipelineProfile;
  session: SessionPipelineProfile;
  views: ViewPipelineProfile[];
}

// Added only by a ReviewApi implementation after transport. This object is
// never accepted from persisted manifest JSON.
export interface ManifestDeliveryProfile {
  manifest_fetch_duration_ms: number;
  manifest_response_received_performance_ms: number;
  server_response_host_monotonic_ns: string | null;
}

export interface ClipManifest {
  schema_version: typeof REVIEW_SCHEMA_VERSION;
  session_id: string;
  created_at_utc: string;
  trigger: ClipTrigger;
  mapped_nearest_frame_skew_us: number | null;
  views: ClipTrack[];
  hil_evidence?: SyntheticSwingHilEvidence;
  pipeline_profile?: PipelineProfile;
  client_delivery_profile?: ManifestDeliveryProfile;
}

export interface ReviewApi {
  getCaptureStatus(): Promise<CaptureStatus>;
  setArmed(armed: boolean): Promise<CaptureStatus>;
  triggerManualCapture(): Promise<SessionSummary>;
  startSyntheticSwing(): Promise<CaptureStatus>;
  getSessions(): Promise<SessionList>;
  getManifest(sessionId: string): Promise<ClipManifest>;
}

type Fetcher = typeof fetch;
type HighResolutionNow = () => number;

export class HttpReviewApi implements ReviewApi {
  readonly #baseUrl: string;
  readonly #fetcher: Fetcher;
  readonly #now: HighResolutionNow;

  constructor(
    baseUrl = "",
    fetcher: Fetcher = globalThis.fetch.bind(globalThis),
    now: HighResolutionNow = highResolutionNow,
  ) {
    this.#baseUrl = baseUrl.replace(/\/$/, "");
    this.#fetcher = fetcher;
    this.#now = now;
  }

  async getCaptureStatus(): Promise<CaptureStatus> {
    return this.#get("/api/v1/capture/status", parseCaptureStatus);
  }

  async setArmed(armed: boolean): Promise<CaptureStatus> {
    return this.#request("/api/v1/capture/arm", parseCaptureStatus, {
      method: "POST",
      headers: { Accept: "application/json", "Content-Type": "application/json" },
      body: JSON.stringify({ armed }),
    });
  }

  async triggerManualCapture(): Promise<SessionSummary> {
    return this.#request("/api/v1/capture/manual", parseSessionSummary, {
      method: "POST",
      headers: { Accept: "application/json", "Content-Type": "application/json" },
    });
  }

  async startSyntheticSwing(): Promise<CaptureStatus> {
    return this.#request("/api/v1/hil/synthetic-swing", parseCaptureStatus, {
      method: "POST",
      headers: { Accept: "application/json", "Content-Type": "application/json" },
      body: "{}",
    });
  }

  async getSessions(): Promise<SessionList> {
    return this.#get("/api/v1/sessions", parseSessionList);
  }

  async getManifest(sessionId: string): Promise<ClipManifest> {
    const encodedId = encodeURIComponent(sessionId);
    const requestStarted = this.#now();
    const response = await this.#fetcher(
      this.#absoluteUrl(`/api/v1/sessions/${encodedId}/manifest`),
      { headers: { Accept: "application/json" } },
    );
    const responseReceived = this.#now();
    if (!response.ok) {
      throw await reviewRequestError(response);
    }
    const manifest = parseClipManifest(await response.json());
    if (manifest.session_id !== sessionId) {
      throw new Error(
        `Review manifest session ${manifest.session_id} does not match requested session ${sessionId}`,
      );
    }
    const serverMonotonicHeader = response.headers.get("X-Swing-Capture-Server-Monotonic-Ns");
    const serverMonotonic =
      serverMonotonicHeader === null
        ? null
        : asDecimalString(serverMonotonicHeader, "X-Swing-Capture-Server-Monotonic-Ns");
    return {
      ...resolveManifestMedia(manifest, this.#absoluteUrl(`/api/v1/sessions/${encodedId}/`)),
      client_delivery_profile: {
        manifest_fetch_duration_ms: nonnegativeDuration(
          responseReceived - requestStarted,
          "manifest fetch duration",
        ),
        manifest_response_received_performance_ms: nonnegativeDuration(
          responseReceived,
          "manifest response performance timestamp",
        ),
        server_response_host_monotonic_ns: serverMonotonic,
      },
    };
  }

  #get<T>(path: string, parser: (value: unknown) => T): Promise<T> {
    return this.#request(path, parser, { headers: { Accept: "application/json" } });
  }

  async #request<T>(path: string, parser: (value: unknown) => T, init: RequestInit): Promise<T> {
    const response = await this.#fetcher(this.#absoluteUrl(path), init);
    if (!response.ok) {
      throw await reviewRequestError(response);
    }
    return parser(await response.json());
  }

  #absoluteUrl(path: string): string {
    return `${this.#baseUrl}${path}`;
  }
}

export function parseCaptureStatus(value: unknown): CaptureStatus {
  const object = asObject(value, "capture status");
  if (object.schema_version !== CAPTURE_SCHEMA_VERSION) {
    throw new Error(`Unsupported capture schema: ${String(object.schema_version)}`);
  }
  return {
    schema_version: CAPTURE_SCHEMA_VERSION,
    state: parseState(object.state, "capture state"),
    armed: asBoolean(object.armed, "capture armed"),
    active_session_id: asNullableString(object.active_session_id, "active_session_id"),
    error: asString(object.error, "capture error"),
    hil: parseSyntheticSwingHilStatus(object.hil),
  };
}

export function parseSessionList(value: unknown): SessionList {
  const object = asObject(value, "session list");
  requireSchema(object);
  if (!Array.isArray(object.sessions)) {
    throw new Error("session list sessions must be an array");
  }
  return {
    schema_version: REVIEW_SCHEMA_VERSION,
    sessions: object.sessions.map(parseSessionSummary),
  };
}

export function parseSessionSummary(value: unknown): SessionSummary {
  const object = asObject(value, "session summary");
  return {
    session_id: asNonemptyString(object.session_id, "session_id"),
    state: parseState(object.state, "session state"),
    created_at_utc: parseTimestamp(object.created_at_utc, "session created_at_utc"),
    error: asString(object.error, "session error"),
  };
}

export function parseClipManifest(value: unknown): ClipManifest {
  const object = asObject(value, "clip manifest");
  requireSchema(object);
  if (object.client_delivery_profile !== undefined) {
    throw new Error("client_delivery_profile is transport-derived and must not be persisted");
  }
  if (!Array.isArray(object.views)) {
    throw new Error("clip manifest views must be an array");
  }
  const tracks = object.views.map(parseClipTrack);
  const roles = new Set(tracks.map((track) => track.role));
  if (
    tracks.length !== 2 ||
    roles.size !== 2 ||
    !roles.has("down_the_line") ||
    !roles.has("face_on")
  ) {
    throw new Error("clip manifest must contain one view for each camera role");
  }
  const trigger = asObject(object.trigger, "clip trigger");
  const mappedSkew = object.mapped_nearest_frame_skew_us;
  if (mappedSkew !== null && (typeof mappedSkew !== "number" || !Number.isFinite(mappedSkew))) {
    throw new Error("mapped_nearest_frame_skew_us must be finite or null");
  }
  const hilEvidence = parseSyntheticSwingHilEvidence(object.hil_evidence, tracks);
  const pipelineProfile = parsePipelineProfile(object.pipeline_profile, tracks);
  return {
    schema_version: REVIEW_SCHEMA_VERSION,
    session_id: asNonemptyString(object.session_id, "manifest session_id"),
    created_at_utc: parseTimestamp(object.created_at_utc, "manifest created_at_utc"),
    trigger: {
      source: asNonemptyString(trigger.source, "trigger.source"),
      host_monotonic_time_ns: asDecimalString(
        trigger.host_monotonic_time_ns,
        "trigger.host_monotonic_time_ns",
      ),
      confirmation_host_monotonic_time_ns: asNullableDecimalString(
        trigger.confirmation_host_monotonic_time_ns,
        "trigger.confirmation_host_monotonic_time_ns",
      ),
      sample_rate_hz:
        trigger.sample_rate_hz === null
          ? null
          : asPositiveNumber(trigger.sample_rate_hz, "trigger.sample_rate_hz"),
      peak_amplitude: asNullableNonnegativeNumber(trigger.peak_amplitude, "trigger.peak_amplitude"),
      noise_floor: asNullableNonnegativeNumber(trigger.noise_floor, "trigger.noise_floor"),
      threshold: asNullableNonnegativeNumber(trigger.threshold, "trigger.threshold"),
    },
    mapped_nearest_frame_skew_us: mappedSkew,
    views: tracks,
    ...(hilEvidence === undefined ? {} : { hil_evidence: hilEvidence }),
    ...(pipelineProfile === undefined ? {} : { pipeline_profile: pipelineProfile }),
  };
}

function parsePipelineProfile(value: unknown, tracks: ClipTrack[]): PipelineProfile | undefined {
  if (value === undefined) {
    return undefined;
  }
  const object = asObject(value, "pipeline_profile");
  if (object.schema_version !== 1 && object.schema_version !== PIPELINE_PROFILE_SCHEMA_VERSION) {
    throw new Error(`Unsupported pipeline profile schema: ${String(object.schema_version)}`);
  }
  const schemaVersion = object.schema_version;
  const capture = asObject(object.capture, "pipeline_profile.capture");
  const session = asObject(object.session, "pipeline_profile.session");
  if (!Array.isArray(object.views)) {
    throw new Error("pipeline_profile.views must be an array");
  }
  const profileViews = object.views.map((view) => parseViewPipelineProfile(view, schemaVersion));
  const roles = new Set(profileViews.map((view) => view.role));
  if (
    profileViews.length !== 2 ||
    roles.size !== 2 ||
    !roles.has("down_the_line") ||
    !roles.has("face_on")
  ) {
    throw new Error("pipeline_profile.views must contain one profile for each camera role");
  }
  for (const profileView of profileViews) {
    const track = tracks.find((candidate) => candidate.role === profileView.role);
    if (track === undefined || track.frame_count !== profileView.frame_count) {
      throw new Error(`pipeline_profile ${profileView.role} frame_count disagrees with its clip`);
    }
  }
  return {
    schema_version: schemaVersion,
    capture: {
      trigger_estimate_to_confirmation_ms: asDuration(
        capture.trigger_estimate_to_confirmation_ms,
        "pipeline_profile.capture.trigger_estimate_to_confirmation_ms",
      ),
      confirmation_to_acceptance_ms: asSignedFiniteDuration(
        capture.confirmation_to_acceptance_ms,
        "pipeline_profile.capture.confirmation_to_acceptance_ms",
      ),
      acceptance_to_freeze_start_ms: asDuration(
        capture.acceptance_to_freeze_start_ms,
        "pipeline_profile.capture.acceptance_to_freeze_start_ms",
      ),
      freeze_schedule_lateness_ms: asSignedFiniteDuration(
        capture.freeze_schedule_lateness_ms,
        "pipeline_profile.capture.freeze_schedule_lateness_ms",
      ),
      freeze_and_rotate_ms: asDuration(
        capture.freeze_and_rotate_ms,
        "pipeline_profile.capture.freeze_and_rotate_ms",
      ),
      audio_stop_ms: asDuration(capture.audio_stop_ms, "pipeline_profile.capture.audio_stop_ms"),
    },
    session: {
      prepublication_analysis_ms: asDuration(
        session.prepublication_analysis_ms,
        "pipeline_profile.session.prepublication_analysis_ms",
      ),
      publisher_planning_ms: asDuration(
        session.publisher_planning_ms,
        "pipeline_profile.session.publisher_planning_ms",
      ),
      validation_and_timeline_ms: asDuration(
        session.validation_and_timeline_ms,
        "pipeline_profile.session.validation_and_timeline_ms",
      ),
      output_setup_ms: asDuration(
        session.output_setup_ms,
        "pipeline_profile.session.output_setup_ms",
      ),
      media_encoding_wall_ms: asDuration(
        session.media_encoding_wall_ms,
        "pipeline_profile.session.media_encoding_wall_ms",
      ),
      frame_metadata_ms: asDuration(
        session.frame_metadata_ms,
        "pipeline_profile.session.frame_metadata_ms",
      ),
      profile_snapshot_after_confirmation_ms: asDuration(
        session.profile_snapshot_after_confirmation_ms,
        "pipeline_profile.session.profile_snapshot_after_confirmation_ms",
      ),
      profile_snapshot_host_monotonic_ns: asDecimalString(
        session.profile_snapshot_host_monotonic_ns,
        "pipeline_profile.session.profile_snapshot_host_monotonic_ns",
      ),
    },
    views: profileViews,
  };
}

function parseViewPipelineProfile(value: unknown, schemaVersion: 1 | 2): ViewPipelineProfile {
  const object = asObject(value, "pipeline_profile view");
  const role = parseRole(object.role);
  const duration = (field: string) =>
    asDuration(object[field], `pipeline_profile.${role}.${field}`);
  return {
    role,
    frame_count: asPositiveInteger(object.frame_count, `pipeline_profile.${role}.frame_count`),
    timeline_ms: duration("timeline_ms"),
    bayer_fit_demosaic_ms: duration("bayer_fit_demosaic_ms"),
    rgb_to_yuv420_ms:
      schemaVersion === 1 ? duration("rgb_to_i420_ms") : duration("rgb_to_yuv420_ms"),
    codec_encode_ms: schemaVersion === 1 ? duration("vp8_encode_ms") : duration("codec_encode_ms"),
    webm_mux_ms: duration("webm_mux_ms"),
    finalize_ms: duration("finalize_ms"),
    output_verification_ms: duration("output_verification_ms"),
    total_ms: duration("total_ms"),
  };
}

function parseSyntheticSwingHilStatus(value: unknown): SyntheticSwingHilStatus {
  const object = asObject(value, "capture hil status");
  return {
    enabled: asBoolean(object.enabled, "capture hil enabled"),
    busy: asBoolean(object.busy, "capture hil busy"),
    stage: parseSyntheticSwingHilStage(object.stage, "capture hil stage"),
    error: asString(object.error, "capture hil error"),
    last_run:
      object.last_run === null ? null : parseSyntheticSwingHilRun(object.last_run, "hil last_run"),
  };
}

function parseSyntheticSwingHilRun(value: unknown, label: string): SyntheticSwingHilRun {
  const object = asObject(value, label);
  return {
    session_id: asNullableString(object.session_id, `${label}.session_id`),
    stage: parseSyntheticSwingHilStage(object.stage, `${label}.stage`),
    error: asString(object.error, `${label}.error`),
  };
}

function parseSyntheticSwingHilStage(value: unknown, label: string): SyntheticSwingHilStage {
  if (
    value !== "idle" &&
    value !== "calibrating" &&
    value !== "stimulus" &&
    value !== "capturing" &&
    value !== "encoding" &&
    value !== "ready" &&
    value !== "error"
  ) {
    throw new Error(`Unknown ${label}: ${String(value)}`);
  }
  return value;
}

function parseSyntheticSwingHilEvidence(
  value: unknown,
  tracks: ClipTrack[],
): SyntheticSwingHilEvidence | undefined {
  if (value === undefined) {
    return undefined;
  }
  const object = asObject(value, "hil_evidence");
  if (object.kind !== "synthetic_swing") {
    throw new Error(`Unknown hil_evidence.kind: ${String(object.kind)}`);
  }
  const opticalFrames = parseHilRoleValues(
    object.optical_white_impact_frame_index,
    "hil_evidence.optical_white_impact_frame_index",
    true,
  );
  for (const track of tracks) {
    if (opticalFrames[track.role] >= track.frame_count) {
      throw new Error(`hil_evidence optical frame is outside the ${track.role} clip`);
    }
  }
  return {
    kind: "synthetic_swing",
    selected_brightness: asPositiveInteger(
      object.selected_brightness,
      "hil_evidence.selected_brightness",
    ),
    timeline: parseSyntheticSwingTimeline(object.timeline),
    tone: parseSyntheticSwingTone(object.tone),
    optical_white_impact_frame_index: opticalFrames,
    audio_trigger_estimate_offset_us: parseHilRoleValues(
      object.audio_trigger_estimate_offset_us,
      "hil_evidence.audio_trigger_estimate_offset_us",
      false,
    ),
    camera_schedule_alignment: parseHilRoleEvidence(
      object.camera_schedule_alignment,
      "hil_evidence.camera_schedule_alignment",
      parseSyntheticSwingScheduleAlignment,
    ),
    optical_white_impact: parseHilRoleEvidence(
      object.optical_white_impact,
      "hil_evidence.optical_white_impact",
      parseSyntheticSwingWhiteImpact,
    ),
  };
}

function parseSyntheticSwingTimeline(value: unknown): SyntheticSwingTimeline {
  const object = asObject(value, "hil_evidence.timeline");
  return {
    step_duration_us: asPositiveInteger(
      object.step_duration_us,
      "hil_evidence.timeline.step_duration_us",
    ),
    pre_impact_step_count: asPositiveInteger(
      object.pre_impact_step_count,
      "hil_evidence.timeline.pre_impact_step_count",
    ),
    white_impact_duration_us: asPositiveInteger(
      object.white_impact_duration_us,
      "hil_evidence.timeline.white_impact_duration_us",
    ),
    post_impact_step_count: asPositiveInteger(
      object.post_impact_step_count,
      "hil_evidence.timeline.post_impact_step_count",
    ),
  };
}

function parseSyntheticSwingTone(value: unknown): SyntheticSwingTone {
  const object = asObject(value, "hil_evidence.tone");
  return {
    duration_us: asPositiveInteger(object.duration_us, "hil_evidence.tone.duration_us"),
    frequency_hz: asPositiveInteger(object.frequency_hz, "hil_evidence.tone.frequency_hz"),
  };
}

function parseSyntheticSwingScheduleAlignment(
  value: unknown,
  label: string,
): SyntheticSwingScheduleAlignment {
  const object = asObject(value, label);
  return {
    mapped_time_correction_us: asInteger(
      object.mapped_time_correction_us,
      `${label}.mapped_time_correction_us`,
    ),
    uncertainty_us: asNonnegativeInteger(object.uncertainty_us, `${label}.uncertainty_us`),
  };
}

function parseSyntheticSwingWhiteImpact(value: unknown, label: string): SyntheticSwingWhiteImpact {
  const object = asObject(value, label);
  const stableFrameCount = asNonnegativeInteger(
    object.stable_frame_count,
    `${label}.stable_frame_count`,
  );
  const matchingFrameCount = asNonnegativeInteger(
    object.matching_frame_count,
    `${label}.matching_frame_count`,
  );
  if (matchingFrameCount > stableFrameCount) {
    throw new Error(`${label}.matching_frame_count cannot exceed stable_frame_count`);
  }
  const matchingFraction = asFraction(object.matching_fraction, `${label}.matching_fraction`);
  const expectedFraction = stableFrameCount === 0 ? 0 : matchingFrameCount / stableFrameCount;
  if (Math.abs(matchingFraction - expectedFraction) > 1e-12) {
    throw new Error(`${label}.matching_fraction disagrees with its frame counts`);
  }
  return {
    passed: asBoolean(object.passed, `${label}.passed`),
    stable_frame_count: stableFrameCount,
    matching_frame_count: matchingFrameCount,
    matching_fraction: matchingFraction,
    mean_signal_delta: asNonnegativeNumber(object.mean_signal_delta, `${label}.mean_signal_delta`),
    mean_expected_color_distance: asNonnegativeNumber(
      object.mean_expected_color_distance,
      `${label}.mean_expected_color_distance`,
    ),
    maximum_saturated_fraction: asFraction(
      object.maximum_saturated_fraction,
      `${label}.maximum_saturated_fraction`,
    ),
    maximum_bloom_fraction: asFraction(
      object.maximum_bloom_fraction,
      `${label}.maximum_bloom_fraction`,
    ),
    exposure_us: asPositiveNumber(object.exposure_us, `${label}.exposure_us`),
    gain_db: asNonnegativeNumber(object.gain_db, `${label}.gain_db`),
  };
}

function parseHilRoleEvidence<T>(
  value: unknown,
  label: string,
  parser: (value: unknown, label: string) => T,
): HilRoleEvidence<T> {
  const object = asObject(value, label);
  return {
    down_the_line: parser(object.down_the_line, `${label}.down_the_line`),
    face_on: parser(object.face_on, `${label}.face_on`),
  };
}

function parseHilRoleValues(value: unknown, label: string, nonnegative: boolean): HilRoleValues {
  const object = asObject(value, label);
  const parseValue = (role: ReviewRole) => {
    const result = asInteger(object[role], `${label}.${role}`);
    if (nonnegative && result < 0) {
      throw new Error(`${label}.${role} must be nonnegative`);
    }
    return result;
  };
  return {
    down_the_line: parseValue("down_the_line"),
    face_on: parseValue("face_on"),
  };
}

function parseClipTrack(value: unknown): ClipTrack {
  const object = asObject(value, "clip track");
  const role = parseRole(object.role);
  const frameCount = asPositiveInteger(object.frame_count, `${role}.frame_count`);
  const impactFrameIndex = asNonnegativeInteger(
    object.impact_frame_index,
    `${role}.impact_frame_index`,
  );
  if (impactFrameIndex >= frameCount) {
    throw new Error(`${role}.impact_frame_index must identify a retained frame`);
  }
  if (!Array.isArray(object.frames) || object.frames.length !== frameCount) {
    throw new Error(`${role}.frames must contain exactly frame_count entries`);
  }
  const frames = object.frames.map((frame, index) => parseClipFrame(frame, role, index));
  for (let index = 1; index < frames.length; ++index) {
    const current = frames[index];
    const previous = frames[index - 1];
    if (current === undefined || previous === undefined) {
      throw new Error(`${role}.frames is unexpectedly sparse`);
    }
    if (current.media_time_us <= previous.media_time_us) {
      throw new Error(`${role}.frames media_time_us must be strictly increasing`);
    }
  }
  return {
    role,
    camera_serial: asNonemptyString(object.camera_serial, `${role}.camera_serial`),
    source: parseSourceGeometry(object.source, role),
    encoded: parseGeometry(object.encoded, `${role}.encoded`),
    frame_count: frameCount,
    nominal_fps: asPositiveNumber(object.nominal_fps, `${role}.nominal_fps`),
    impact_frame_index: impactFrameIndex,
    media: parseClipMedia(object.media, role),
    frames,
  };
}

function parseClipFrame(value: unknown, role: ReviewRole, expectedIndex: number): ClipFrame {
  const object = asObject(value, `${role} frame`);
  const frameIndex = asNonnegativeInteger(object.frame_index, `${role}.frame_index`);
  if (frameIndex !== expectedIndex) {
    throw new Error(`${role}.frames must use contiguous zero-based frame_index values`);
  }
  return {
    frame_index: frameIndex,
    frame_id: asDecimalString(object.frame_id, `${role}.frame_id`),
    device_timestamp: asDecimalString(object.device_timestamp, `${role}.device_timestamp`),
    time_from_impact_us: asInteger(object.time_from_impact_us, `${role}.time_from_impact_us`),
    media_time_us: asNonnegativeInteger(object.media_time_us, `${role}.media_time_us`),
  };
}

function parseClipMedia(value: unknown, role: ReviewRole): ClipMedia {
  const object = asObject(value, `${role}.media`);
  if (object.url !== undefined) {
    throw new Error(`${role}.media.url is response-derived and must not be persisted`);
  }
  const path = asSafeRelativePath(object.path, `${role}.media.path`);
  const mimeType = asNonemptyString(object.mime_type, `${role}.media.mime_type`);
  if (!mimeType.startsWith("video/")) {
    throw new Error(`${role}.media.mime_type must be video media`);
  }
  return {
    path,
    mime_type: mimeType,
    codec: asNonemptyString(object.codec, `${role}.media.codec`),
    all_frames_keyframes: asBoolean(
      object.all_frames_keyframes,
      `${role}.media.all_frames_keyframes`,
    ),
    encoded_bytes: asNonnegativeInteger(object.encoded_bytes, `${role}.media.encoded_bytes`),
  };
}

function parseSourceGeometry(value: unknown, role: ReviewRole): ClipSourceGeometry {
  const object = asObject(value, `${role}.source`);
  if (object.pixel_format !== "BayerRG8") {
    throw new Error(`${role}.source.pixel_format must be BayerRG8`);
  }
  return { ...parseGeometry(object, `${role}.source`), pixel_format: "BayerRG8" };
}

function parseGeometry(value: unknown, label: string): ClipImageGeometry {
  const object = asObject(value, label);
  return {
    width: asPositiveInteger(object.width, `${label}.width`),
    height: asPositiveInteger(object.height, `${label}.height`),
  };
}

function resolveManifestMedia(manifest: ClipManifest, baseUrl: string): ClipManifest {
  const absoluteBase = new URL(baseUrl, globalThis.location?.href ?? "http://station.invalid/");
  return {
    ...manifest,
    views: manifest.views.map((track) => ({
      ...track,
      media: {
        ...track.media,
        url: new URL(track.media.url ?? track.media.path, absoluteBase).toString(),
      },
    })),
  };
}

async function reviewRequestError(response: Response): Promise<Error> {
  let detail = "";
  try {
    const body = asObject(await response.json(), "review error");
    if (typeof body.error === "string") {
      detail = body.error;
    }
  } catch {
    // Preserve the HTTP status when a proxy returns a non-JSON response.
  }
  return new Error(
    `Review request failed (${response.status})${detail.length === 0 ? "" : `: ${detail}`}`,
  );
}

function requireSchema(object: Record<string, unknown>): void {
  if (object.schema_version !== REVIEW_SCHEMA_VERSION) {
    throw new Error(`Unsupported review schema: ${String(object.schema_version)}`);
  }
}

function parseState(value: unknown, label: string): CaptureState {
  if (
    value !== "setup" &&
    value !== "arming" &&
    value !== "armed" &&
    value !== "waiting_post_roll" &&
    value !== "encoding" &&
    value !== "ready" &&
    value !== "error"
  ) {
    throw new Error(`Unknown ${label}: ${String(value)}`);
  }
  return value;
}

function parseRole(value: unknown): ReviewRole {
  if (value !== "down_the_line" && value !== "face_on") {
    throw new Error(`Unknown review role: ${String(value)}`);
  }
  return value;
}

function parseTimestamp(value: unknown, label: string): string {
  const timestamp = asNonemptyString(value, label);
  if (!Number.isFinite(Date.parse(timestamp))) {
    throw new Error(`${label} must be an ISO-8601 timestamp`);
  }
  return timestamp;
}

function asObject(value: unknown, label: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

function asString(value: unknown, label: string): string {
  if (typeof value !== "string") {
    throw new Error(`${label} must be a string`);
  }
  return value;
}

function asBoolean(value: unknown, label: string): boolean {
  if (typeof value !== "boolean") {
    throw new Error(`${label} must be a boolean`);
  }
  return value;
}

function asNonemptyString(value: unknown, label: string): string {
  const text = asString(value, label);
  if (text.length === 0) {
    throw new Error(`${label} must not be empty`);
  }
  return text;
}

function asSafeRelativePath(value: unknown, label: string): string {
  const relativePath = asNonemptyString(value, label);
  if (
    relativePath.startsWith("/") ||
    relativePath.includes("\\") ||
    relativePath.split("/").some((part) => part === "" || part === "." || part === "..") ||
    relativePath.includes(":") ||
    relativePath.includes("?") ||
    relativePath.includes("#")
  ) {
    throw new Error(`${label} must be a safe relative artifact path`);
  }
  return relativePath;
}

function asNullableString(value: unknown, label: string): string | null {
  return value === null ? null : asNonemptyString(value, label);
}

function asDecimalString(value: unknown, label: string): string {
  const text = asNonemptyString(value, label);
  if (!/^\d+$/.test(text)) {
    throw new Error(`${label} must be an unsigned decimal string`);
  }
  return text;
}

function asNullableDecimalString(value: unknown, label: string): string | null {
  return value === null ? null : asDecimalString(value, label);
}

function asNullableNonnegativeNumber(value: unknown, label: string): number | null {
  if (value === null) {
    return null;
  }
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) {
    throw new Error(`${label} must be a nonnegative finite number or null`);
  }
  return value;
}

function asNonnegativeNumber(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) {
    throw new Error(`${label} must be a nonnegative finite number`);
  }
  return value;
}

function asDuration(value: unknown, label: string): number {
  return nonnegativeDuration(asNonnegativeNumber(value, label), label);
}

function asSignedFiniteDuration(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new Error(`${label} must be a finite duration`);
  }
  return value;
}

function nonnegativeDuration(value: number, label: string): number {
  if (!Number.isFinite(value) || value < 0) {
    throw new Error(`${label} must be a nonnegative finite duration`);
  }
  return value;
}

function asFraction(value: unknown, label: string): number {
  const number = asNonnegativeNumber(value, label);
  if (number > 1) {
    throw new Error(`${label} must be in [0, 1]`);
  }
  return number;
}

function asPositiveNumber(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isFinite(value) || value <= 0) {
    throw new Error(`${label} must be a positive finite number`);
  }
  return value;
}

function asInteger(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isSafeInteger(value)) {
    throw new Error(`${label} must be a safe integer`);
  }
  return value;
}

function asNonnegativeInteger(value: unknown, label: string): number {
  const integer = asInteger(value, label);
  if (integer < 0) {
    throw new Error(`${label} must be nonnegative`);
  }
  return integer;
}

function asPositiveInteger(value: unknown, label: string): number {
  const integer = asInteger(value, label);
  if (integer <= 0) {
    throw new Error(`${label} must be positive`);
  }
  return integer;
}

function highResolutionNow(): number {
  return globalThis.performance?.now() ?? Date.now();
}
