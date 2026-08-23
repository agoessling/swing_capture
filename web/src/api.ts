export const STATION_STATUS_SCHEMA_VERSION = 1 as const;

export type CameraRole = "down_the_line" | "face_on";
export type ImageAssessment = "nominal" | "too_dark" | "too_bright" | "soft" | "unavailable";

export interface NumericSetting {
  value: number;
  min: number;
  max: number;
  increment: number;
}

export interface ImageQuality {
  assessment: ImageAssessment;
  mean: number;
  p99: number;
  gradient_energy: number;
}

export interface PreviewPerformance {
  media_type: string;
  encoded_bytes: number;
  source_age_ms: number;
  rendered_age_ms: number;
  quality_analysis_ms: number;
  bayer_transform_ms: number;
  resize_ms: number;
  encode_ms: number;
  total_ms: number;
  latest_capture_frame_id: string;
  latest_capture_age_ms: number;
  latest_sink_frame_id: string;
  latest_sink_completion_age_ms: number;
  latest_sampler_frame_id: string;
  latest_sampler_completion_age_ms: number;
  sampled_sequence: number;
  sampled_age_ms: number;
  render_queue_ms: number;
  renderer_stage: "idle" | "routine" | "full_resolution" | "unknown";
  render_pending: boolean;
}

export interface PreviewServerTelemetry {
  requested_sequence: number | null;
  served_sequence: number;
  latest_capture_frame_id: string | null;
  latest_capture_age_ms: number | null;
  latest_sink_frame_id: string | null;
  latest_sink_completion_age_ms: number | null;
  latest_sampler_frame_id: string | null;
  latest_sampler_completion_age_ms: number | null;
  sampled_sequence: number | null;
  sampled_age_ms: number | null;
  source_age_ms: number | null;
  rendered_age_ms: number | null;
  render_queue_ms: number | null;
  render_ms: number | null;
  renderer_stage: PreviewPerformance["renderer_stage"] | null;
  render_pending: boolean | null;
  backend_ms: number | null;
  response_prepare_ms: number | null;
  handler_ms: number | null;
}

export interface PreviewFetchTelemetry {
  headers_ms: number;
  body_ms: number;
  total_ms: number;
  server: PreviewServerTelemetry;
}

export interface FetchedPreview {
  image: Blob;
  telemetry: PreviewFetchTelemetry;
}

export interface CameraStatus {
  role: CameraRole;
  serial: string;
  model: string;
  connected: boolean;
  error: string;
  stream_fps: number;
  preview_sequence: number;
  preview_width: number;
  preview_height: number;
  exposure_us: NumericSetting;
  gain_db: NumericSetting;
  image_quality: ImageQuality;
  preview_performance: PreviewPerformance;
}

export interface StationStatus {
  schema_version: typeof STATION_STATUS_SCHEMA_VERSION;
  service_instance_id: string;
  mode: string;
  cameras: CameraStatus[];
}

export interface CameraSettingsUpdate {
  exposure_us: number;
  gain_db: number;
}

export interface StationApi {
  getStatus(): Promise<StationStatus>;
  getPreview(role: CameraRole, sequence: number): Promise<FetchedPreview>;
  updateCameraSettings(role: CameraRole, settings: CameraSettingsUpdate): Promise<CameraStatus>;
  previewUrl(role: CameraRole, sequence: number): string;
  fullResolutionPreviewUrl(role: CameraRole, sequence: number): string;
}

type Fetcher = typeof fetch;
type MonotonicClock = () => number;

export class HttpStationApi implements StationApi {
  readonly #baseUrl: string;
  readonly #fetcher: Fetcher;
  readonly #clock: MonotonicClock;

  constructor(
    baseUrl = "",
    fetcher: Fetcher = globalThis.fetch.bind(globalThis),
    clock: MonotonicClock = () => globalThis.performance.now(),
  ) {
    this.#baseUrl = baseUrl.replace(/\/$/, "");
    this.#fetcher = fetcher;
    this.#clock = clock;
  }

  async getStatus(): Promise<StationStatus> {
    const response = await this.#fetcher(`${this.#baseUrl}/api/v1/status`, {
      headers: { Accept: "application/json" },
    });
    return parseResponse(response, parseStationStatus);
  }

  async getPreview(role: CameraRole, sequence: number): Promise<FetchedPreview> {
    const startedAt = this.#clock();
    const response = await this.#fetcher(this.previewUrl(role, sequence), {
      headers: { Accept: "image/jpeg, image/png" },
    });
    const headersAt = this.#clock();
    if (!response.ok) {
      throw await stationRequestError(response);
    }
    const server = parsePreviewServerTelemetry(response.headers, sequence);
    const image = await response.blob();
    const bodyAt = this.#clock();
    if (image.type !== "image/jpeg" && image.type !== "image/png") {
      throw new Error(`Station preview returned ${image.type || "an unknown content type"}`);
    }
    return {
      image,
      telemetry: {
        headers_ms: Math.max(0, headersAt - startedAt),
        body_ms: Math.max(0, bodyAt - headersAt),
        total_ms: Math.max(0, bodyAt - startedAt),
        server,
      },
    };
  }

  async updateCameraSettings(
    role: CameraRole,
    settings: CameraSettingsUpdate,
  ): Promise<CameraStatus> {
    const response = await this.#fetcher(`${this.#baseUrl}/api/v1/cameras/${role}/settings`, {
      method: "PATCH",
      headers: {
        Accept: "application/json",
        "Content-Type": "application/json",
      },
      body: JSON.stringify(settings),
    });
    return parseResponse(response, parseCameraStatus);
  }

  previewUrl(role: CameraRole, sequence: number): string {
    if (!Number.isSafeInteger(sequence) || sequence < 0) {
      throw new Error("Station preview sequence must be a nonnegative safe integer");
    }
    return `${this.#baseUrl}/api/v1/cameras/${role}/preview?sequence=${sequence}`;
  }

  fullResolutionPreviewUrl(role: CameraRole, sequence: number): string {
    return `${this.previewUrl(role, sequence)}&full=1`;
  }
}

async function parseResponse<T>(response: Response, parser: (value: unknown) => T): Promise<T> {
  if (!response.ok) {
    throw await stationRequestError(response);
  }
  return parser(await response.json());
}

async function stationRequestError(response: Response): Promise<Error> {
  let detail = "";
  try {
    const body = asObject(await response.json(), "station error");
    if (typeof body.error === "string") {
      detail = body.error;
    }
  } catch {
    // Preserve the HTTP status when a proxy or server returns a non-JSON body.
  }
  const suffix = detail.length === 0 ? "" : `: ${detail}`;
  return new Error(`Station request failed (${response.status})${suffix}`);
}

export function parseStationStatus(value: unknown): StationStatus {
  const object = asObject(value, "station status");
  if (object.schema_version !== STATION_STATUS_SCHEMA_VERSION) {
    throw new Error(`Unsupported station status schema: ${String(object.schema_version)}`);
  }
  if (typeof object.mode !== "string" || object.mode.length === 0) {
    throw new Error("Station status mode must be a non-empty string");
  }
  if (!Array.isArray(object.cameras)) {
    throw new Error("Station status cameras must be an array");
  }
  return {
    schema_version: STATION_STATUS_SCHEMA_VERSION,
    service_instance_id: asBoundedIdentifier(object.service_instance_id, "service instance ID"),
    mode: object.mode,
    cameras: object.cameras.map(parseCameraStatus),
  };
}

function asBoundedIdentifier(value: unknown, description: string): string {
  const parsed = asString(value, description);
  if (parsed.length === 0 || parsed.length > 128 || !/^[A-Za-z0-9._:-]+$/.test(parsed)) {
    throw new Error(`${description} must be a non-empty bounded identifier`);
  }
  return parsed;
}

export function parseCameraStatus(value: unknown): CameraStatus {
  const object = asObject(value, "camera status");
  const role = object.role;
  if (role !== "down_the_line" && role !== "face_on") {
    throw new Error(`Unknown camera role: ${String(role)}`);
  }
  return {
    role,
    serial: asString(object.serial, "camera serial"),
    model: asString(object.model, "camera model"),
    connected: asBoolean(object.connected, "camera connected"),
    error: asString(object.error, "camera error"),
    stream_fps: asFiniteNumber(object.stream_fps, "camera stream_fps"),
    preview_sequence: asNonnegativeSafeInteger(object.preview_sequence, "camera preview_sequence"),
    preview_width: asNonnegativeSafeInteger(object.preview_width, "camera preview_width"),
    preview_height: asNonnegativeSafeInteger(object.preview_height, "camera preview_height"),
    exposure_us: parseNumericSetting(object.exposure_us, "exposure_us"),
    gain_db: parseNumericSetting(object.gain_db, "gain_db"),
    image_quality: parseImageQuality(object.image_quality),
    preview_performance: parsePreviewPerformance(object.preview_performance),
  };
}

function parseNumericSetting(value: unknown, label: string): NumericSetting {
  const object = asObject(value, label);
  const setting = {
    value: asFiniteNumber(object.value, `${label}.value`),
    min: asFiniteNumber(object.min, `${label}.min`),
    max: asFiniteNumber(object.max, `${label}.max`),
    increment: asFiniteNumber(object.increment, `${label}.increment`),
  };
  if (
    setting.min > setting.max ||
    setting.value < setting.min ||
    setting.value > setting.max ||
    setting.increment <= 0
  ) {
    throw new Error(`Invalid numeric setting range for ${label}`);
  }
  return setting;
}

function parseImageQuality(value: unknown): ImageQuality {
  const object = asObject(value, "image_quality");
  const assessment = object.assessment;
  if (
    assessment !== "nominal" &&
    assessment !== "too_dark" &&
    assessment !== "too_bright" &&
    assessment !== "soft" &&
    assessment !== "unavailable"
  ) {
    throw new Error(`Unknown image quality assessment: ${String(assessment)}`);
  }
  return {
    assessment,
    mean: asFiniteNumber(object.mean, "image_quality.mean"),
    p99: asFiniteNumber(object.p99, "image_quality.p99"),
    gradient_energy: asFiniteNumber(object.gradient_energy, "image_quality.gradient_energy"),
  };
}

function parsePreviewPerformance(value: unknown): PreviewPerformance {
  const object = asObject(value, "preview_performance");
  const performance = {
    media_type: asString(object.media_type, "preview_performance.media_type"),
    encoded_bytes: asNonnegativeSafeInteger(
      object.encoded_bytes,
      "preview_performance.encoded_bytes",
    ),
    source_age_ms: asFiniteNumber(object.source_age_ms, "preview_performance.source_age_ms"),
    rendered_age_ms: asFiniteNumber(object.rendered_age_ms, "preview_performance.rendered_age_ms"),
    quality_analysis_ms: asFiniteNumber(
      object.quality_analysis_ms,
      "preview_performance.quality_analysis_ms",
    ),
    bayer_transform_ms: asFiniteNumber(
      object.bayer_transform_ms,
      "preview_performance.bayer_transform_ms",
    ),
    resize_ms: asFiniteNumber(object.resize_ms, "preview_performance.resize_ms"),
    encode_ms: asFiniteNumber(object.encode_ms, "preview_performance.encode_ms"),
    total_ms: asFiniteNumber(object.total_ms, "preview_performance.total_ms"),
    latest_capture_frame_id: asUnsignedDecimalString(
      object.latest_capture_frame_id,
      "preview_performance.latest_capture_frame_id",
    ),
    latest_capture_age_ms: asFiniteNumber(
      object.latest_capture_age_ms,
      "preview_performance.latest_capture_age_ms",
    ),
    latest_sink_frame_id: asUnsignedDecimalString(
      object.latest_sink_frame_id,
      "preview_performance.latest_sink_frame_id",
    ),
    latest_sink_completion_age_ms: asFiniteNumber(
      object.latest_sink_completion_age_ms,
      "preview_performance.latest_sink_completion_age_ms",
    ),
    latest_sampler_frame_id: asUnsignedDecimalString(
      object.latest_sampler_frame_id,
      "preview_performance.latest_sampler_frame_id",
    ),
    latest_sampler_completion_age_ms: asFiniteNumber(
      object.latest_sampler_completion_age_ms,
      "preview_performance.latest_sampler_completion_age_ms",
    ),
    sampled_sequence: asNonnegativeSafeInteger(
      object.sampled_sequence,
      "preview_performance.sampled_sequence",
    ),
    sampled_age_ms: asFiniteNumber(object.sampled_age_ms, "preview_performance.sampled_age_ms"),
    render_queue_ms: asFiniteNumber(object.render_queue_ms, "preview_performance.render_queue_ms"),
    renderer_stage: parseRendererStage(object.renderer_stage),
    render_pending: asBoolean(object.render_pending, "preview_performance.render_pending"),
  };
  if (
    performance.encoded_bytes < 0 ||
    performance.source_age_ms < 0 ||
    performance.rendered_age_ms < 0 ||
    performance.quality_analysis_ms < 0 ||
    performance.bayer_transform_ms < 0 ||
    performance.resize_ms < 0 ||
    performance.encode_ms < 0 ||
    performance.total_ms < 0 ||
    performance.latest_capture_age_ms < 0 ||
    performance.latest_sink_completion_age_ms < 0 ||
    performance.latest_sampler_completion_age_ms < 0 ||
    performance.sampled_age_ms < 0 ||
    performance.render_queue_ms < 0
  ) {
    throw new Error("Preview performance values must be nonnegative");
  }
  return performance;
}

function parseRendererStage(value: unknown): PreviewPerformance["renderer_stage"] {
  if (
    value !== "idle" &&
    value !== "routine" &&
    value !== "full_resolution" &&
    value !== "unknown"
  ) {
    throw new Error(`Unknown preview renderer stage: ${String(value)}`);
  }
  return value;
}

function parsePreviewServerTelemetry(
  headers: Headers,
  requestedSequence: number,
): PreviewServerTelemetry {
  const servedSequence = requiredHeaderInteger(headers, "X-Preview-Sequence");
  const echoedRequest = optionalHeaderInteger(headers, "X-Preview-Requested-Sequence");
  if (echoedRequest !== null && echoedRequest !== requestedSequence) {
    throw new Error(
      `Station preview request attribution mismatch: requested ${requestedSequence}, server reported ${echoedRequest}`,
    );
  }
  if (servedSequence < requestedSequence) {
    throw new Error(
      `Station preview regressed from requested sequence ${requestedSequence} to ${servedSequence}`,
    );
  }
  return {
    requested_sequence: echoedRequest,
    served_sequence: servedSequence,
    latest_capture_frame_id: optionalHeaderUnsignedIntegerString(
      headers,
      "X-Preview-Latest-Capture-Frame-Id",
    ),
    latest_capture_age_ms: optionalHeaderNumber(headers, "X-Preview-Latest-Capture-Age-Ms"),
    latest_sink_frame_id: optionalHeaderUnsignedIntegerString(
      headers,
      "X-Preview-Latest-Sink-Frame-Id",
    ),
    latest_sink_completion_age_ms: optionalHeaderNumber(
      headers,
      "X-Preview-Latest-Sink-Completion-Age-Ms",
    ),
    latest_sampler_frame_id: optionalHeaderUnsignedIntegerString(
      headers,
      "X-Preview-Latest-Sampler-Frame-Id",
    ),
    latest_sampler_completion_age_ms: optionalHeaderNumber(
      headers,
      "X-Preview-Latest-Sampler-Completion-Age-Ms",
    ),
    sampled_sequence: optionalHeaderInteger(headers, "X-Preview-Sampled-Sequence"),
    sampled_age_ms: optionalHeaderNumber(headers, "X-Preview-Sampled-Age-Ms"),
    source_age_ms: optionalHeaderNumber(headers, "X-Preview-Source-Age-Ms"),
    rendered_age_ms: optionalHeaderNumber(headers, "X-Preview-Rendered-Age-Ms"),
    render_queue_ms: optionalHeaderNumber(headers, "X-Preview-Render-Queue-Ms"),
    render_ms: optionalHeaderNumber(headers, "X-Preview-Render-Ms"),
    renderer_stage: optionalHeaderRendererStage(headers, "X-Preview-Renderer-Stage"),
    render_pending: optionalHeaderBoolean(headers, "X-Preview-Render-Pending"),
    backend_ms: optionalHeaderNumber(headers, "X-Preview-Server-Backend-Ms"),
    response_prepare_ms: optionalHeaderNumber(headers, "X-Preview-Server-Response-Prepare-Ms"),
    handler_ms: optionalHeaderNumber(headers, "X-Preview-Server-Handler-Ms"),
  };
}

function requiredHeaderInteger(headers: Headers, name: string): number {
  const value = optionalHeaderInteger(headers, name);
  if (value === null) {
    throw new Error(`Station preview response is missing ${name}`);
  }
  return value;
}

function optionalHeaderInteger(headers: Headers, name: string): number | null {
  const raw = headers.get(name);
  if (raw === null) {
    return null;
  }
  if (!/^(0|[1-9][0-9]*)$/.test(raw)) {
    throw new Error(`Station preview response has invalid ${name}`);
  }
  const value = Number(raw);
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error(`Station preview response has invalid ${name}`);
  }
  return value;
}

function optionalHeaderUnsignedIntegerString(headers: Headers, name: string): string | null {
  const raw = headers.get(name);
  if (raw === null) {
    return null;
  }
  if (!/^(0|[1-9][0-9]*)$/.test(raw)) {
    throw new Error(`Station preview response has invalid ${name}`);
  }
  return raw;
}

function optionalHeaderNumber(headers: Headers, name: string): number | null {
  const raw = headers.get(name);
  if (raw === null) {
    return null;
  }
  if (!/^(?:0|[1-9][0-9]*)(?:\.[0-9]+)?$/.test(raw)) {
    throw new Error(`Station preview response has invalid ${name}`);
  }
  const value = Number(raw);
  if (!Number.isFinite(value) || value < 0) {
    throw new Error(`Station preview response has invalid ${name}`);
  }
  return value;
}

function optionalHeaderRendererStage(
  headers: Headers,
  name: string,
): PreviewPerformance["renderer_stage"] | null {
  const raw = headers.get(name);
  if (raw === null) {
    return null;
  }
  return parseRendererStage(raw);
}

function optionalHeaderBoolean(headers: Headers, name: string): boolean | null {
  const raw = headers.get(name);
  if (raw === null) {
    return null;
  }
  if (raw === "true") {
    return true;
  }
  if (raw === "false") {
    return false;
  }
  throw new Error(`Station preview response has invalid ${name}`);
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

function asFiniteNumber(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new Error(`${label} must be a finite number`);
  }
  return value;
}

function asNonnegativeSafeInteger(value: unknown, label: string): number {
  if (!Number.isSafeInteger(value) || (value as number) < 0) {
    throw new Error(`${label} must be a nonnegative safe integer`);
  }
  return value as number;
}

function asUnsignedDecimalString(value: unknown, label: string): string {
  if (typeof value !== "string" || !/^(0|[1-9][0-9]*)$/.test(value)) {
    throw new Error(`${label} must be an unsigned decimal string`);
  }
  return value;
}
