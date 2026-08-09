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
  mode: string;
  cameras: CameraStatus[];
}

export interface CameraSettingsUpdate {
  exposure_us: number;
  gain_db: number;
}

export interface StationApi {
  getStatus(): Promise<StationStatus>;
  getPreview(role: CameraRole, sequence: number): Promise<Blob>;
  updateCameraSettings(role: CameraRole, settings: CameraSettingsUpdate): Promise<CameraStatus>;
  previewUrl(role: CameraRole, sequence: number): string;
  fullResolutionPreviewUrl(role: CameraRole, sequence: number): string;
}

type Fetcher = typeof fetch;

export class HttpStationApi implements StationApi {
  readonly #baseUrl: string;
  readonly #fetcher: Fetcher;

  constructor(baseUrl = "", fetcher: Fetcher = globalThis.fetch.bind(globalThis)) {
    this.#baseUrl = baseUrl.replace(/\/$/, "");
    this.#fetcher = fetcher;
  }

  async getStatus(): Promise<StationStatus> {
    const response = await this.#fetcher(`${this.#baseUrl}/api/v1/status`, {
      headers: { Accept: "application/json" },
    });
    return parseResponse(response, parseStationStatus);
  }

  async getPreview(role: CameraRole, sequence: number): Promise<Blob> {
    const response = await this.#fetcher(this.previewUrl(role, sequence), {
      headers: { Accept: "image/jpeg, image/png" },
    });
    if (!response.ok) {
      throw await stationRequestError(response);
    }
    const preview = await response.blob();
    if (preview.type !== "image/jpeg" && preview.type !== "image/png") {
      throw new Error(`Station preview returned ${preview.type || "an unknown content type"}`);
    }
    return preview;
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
    mode: object.mode,
    cameras: object.cameras.map(parseCameraStatus),
  };
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
    preview_sequence: asFiniteNumber(object.preview_sequence, "camera preview_sequence"),
    preview_width: asFiniteNumber(object.preview_width, "camera preview_width"),
    preview_height: asFiniteNumber(object.preview_height, "camera preview_height"),
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
    encoded_bytes: asFiniteNumber(object.encoded_bytes, "preview_performance.encoded_bytes"),
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
  };
  if (
    performance.encoded_bytes < 0 ||
    performance.source_age_ms < 0 ||
    performance.rendered_age_ms < 0 ||
    performance.quality_analysis_ms < 0 ||
    performance.bayer_transform_ms < 0 ||
    performance.resize_ms < 0 ||
    performance.encode_ms < 0 ||
    performance.total_ms < 0
  ) {
    throw new Error("Preview performance values must be nonnegative");
  }
  return performance;
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
