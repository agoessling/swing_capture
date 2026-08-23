import {
  STATION_STATUS_SCHEMA_VERSION,
  type CameraRole,
  type CameraSettingsUpdate,
  type CameraStatus,
  type FetchedPreview,
  type StationApi,
  type StationStatus,
} from "./api.js";

export const FIXTURE_STATUS: StationStatus = {
  schema_version: STATION_STATUS_SCHEMA_VERSION,
  service_instance_id: "fixture-service-instance-1",
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
        latest_capture_frame_id: "800",
        latest_capture_age_ms: 3,
        latest_sink_frame_id: "800",
        latest_sink_completion_age_ms: 2,
        latest_sampler_frame_id: "800",
        latest_sampler_completion_age_ms: 1,
        sampled_sequence: 18,
        sampled_age_ms: 7,
        render_queue_ms: 1.4,
        renderer_stage: "idle",
        render_pending: false,
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
        latest_capture_frame_id: "900",
        latest_capture_age_ms: 4,
        latest_sink_frame_id: "900",
        latest_sink_completion_age_ms: 3,
        latest_sampler_frame_id: "900",
        latest_sampler_completion_age_ms: 2,
        sampled_sequence: 30,
        sampled_age_ms: 8,
        render_queue_ms: 1.5,
        renderer_stage: "idle",
        render_pending: false,
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

  async getPreview(role: CameraRole, sequence: number): Promise<FetchedPreview> {
    const camera = this.#status.cameras.find((candidate) => candidate.role === role);
    if (camera === undefined) {
      throw new Error(`No fixture camera is assigned to ${role}`);
    }
    return {
      image: new Blob([`${role}:${sequence}`], { type: "image/png" }),
      telemetry: {
        headers_ms: 2,
        body_ms: 1,
        total_ms: 3,
        server: {
          requested_sequence: sequence,
          served_sequence: sequence,
          latest_capture_frame_id: String(camera.preview_performance.latest_capture_frame_id),
          latest_capture_age_ms: camera.preview_performance.latest_capture_age_ms,
          latest_sink_frame_id: camera.preview_performance.latest_sink_frame_id,
          latest_sink_completion_age_ms: camera.preview_performance.latest_sink_completion_age_ms,
          latest_sampler_frame_id: camera.preview_performance.latest_sampler_frame_id,
          latest_sampler_completion_age_ms:
            camera.preview_performance.latest_sampler_completion_age_ms,
          sampled_sequence: camera.preview_performance.sampled_sequence,
          sampled_age_ms: camera.preview_performance.sampled_age_ms,
          source_age_ms: camera.preview_performance.source_age_ms,
          rendered_age_ms: camera.preview_performance.rendered_age_ms,
          render_queue_ms: camera.preview_performance.render_queue_ms,
          render_ms: camera.preview_performance.total_ms,
          renderer_stage: camera.preview_performance.renderer_stage,
          render_pending: camera.preview_performance.render_pending,
          backend_ms: 0.2,
          response_prepare_ms: 0.1,
          handler_ms: 0.3,
        },
      },
    };
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
