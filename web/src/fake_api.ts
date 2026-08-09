import {
  STATION_STATUS_SCHEMA_VERSION,
  type CameraRole,
  type CameraSettingsUpdate,
  type CameraStatus,
  type StationApi,
  type StationStatus,
} from "./api.js";

export const FIXTURE_STATUS: StationStatus = {
  schema_version: STATION_STATUS_SCHEMA_VERSION,
  mode: "setup_preview_fixture",
  cameras: [
    {
      role: "down_the_line",
      serial: "FDN22120654",
      model: "MER2-160-227U3C",
      connected: true,
      error: "",
      stream_fps: 226.9,
      preview_sequence: 17,
      preview_width: 1440,
      preview_height: 1080,
      exposure_us: { value: 1500, min: 50, max: 4000, increment: 10 },
      gain_db: { value: 3, min: 0, max: 12, increment: 0.1 },
      image_quality: {
        assessment: "nominal",
        mean: 112.4,
        p99: 238,
        gradient_energy: 27.8,
      },
      preview_performance: {
        media_type: "image/jpeg",
        encoded_bytes: 42_000,
        source_age_ms: 18,
        rendered_age_ms: 12,
        quality_analysis_ms: 1,
        bayer_transform_ms: 3.2,
        resize_ms: 0,
        encode_ms: 1.1,
        total_ms: 5.3,
      },
    },
    {
      role: "face_on",
      serial: "FDN23010199",
      model: "MER2-160-227U3C",
      connected: true,
      error: "",
      stream_fps: 226.8,
      preview_sequence: 29,
      preview_width: 1440,
      preview_height: 1080,
      exposure_us: { value: 1800, min: 50, max: 4000, increment: 10 },
      gain_db: { value: 2.5, min: 0, max: 12, increment: 0.1 },
      image_quality: {
        assessment: "soft",
        mean: 106.1,
        p99: 231,
        gradient_energy: 11.2,
      },
      preview_performance: {
        media_type: "image/jpeg",
        encoded_bytes: 44_000,
        source_age_ms: 19,
        rendered_age_ms: 13,
        quality_analysis_ms: 1,
        bayer_transform_ms: 3.3,
        resize_ms: 0,
        encode_ms: 1.2,
        total_ms: 5.5,
      },
    },
  ],
};

export class FakeStationApi implements StationApi {
  #status: StationStatus;

  constructor(initialStatus: StationStatus = FIXTURE_STATUS) {
    this.#status = structuredClone(initialStatus);
  }

  async getStatus(): Promise<StationStatus> {
    for (const camera of this.#status.cameras) {
      if (camera.connected) {
        camera.preview_sequence += 1;
      }
    }
    return structuredClone(this.#status);
  }

  async getPreview(role: CameraRole, sequence: number): Promise<Blob> {
    return new Blob([`${role}:${sequence}`], { type: "image/png" });
  }

  async updateCameraSettings(
    role: CameraRole,
    settings: CameraSettingsUpdate,
  ): Promise<CameraStatus> {
    const camera = this.#status.cameras.find((candidate) => candidate.role === role);
    if (!camera) {
      throw new Error(`No fixture camera is assigned to ${role}`);
    }
    if (!camera.connected) {
      throw new Error(`${role} camera is disconnected`);
    }
    camera.exposure_us.value = clampAndRound(settings.exposure_us, camera.exposure_us);
    camera.gain_db.value = clampAndRound(settings.gain_db, camera.gain_db);
    return structuredClone(camera);
  }

  previewUrl(role: CameraRole, sequence: number): string {
    const filename = role === "down_the_line" ? "down-the-line.svg" : "face-on.svg";
    return `fixtures/${filename}?sequence=${sequence}`;
  }

  fullResolutionPreviewUrl(role: CameraRole, sequence: number): string {
    return `${this.previewUrl(role, sequence)}&full=1`;
  }

  snapshot(): StationStatus {
    return structuredClone(this.#status);
  }
}

function clampAndRound(
  value: number,
  range: { min: number; max: number; increment: number },
): number {
  const clamped = Math.min(range.max, Math.max(range.min, value));
  const steps = Math.round((clamped - range.min) / range.increment);
  return Number((range.min + steps * range.increment).toFixed(6));
}
