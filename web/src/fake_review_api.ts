import {
  CAPTURE_SCHEMA_VERSION,
  REVIEW_SCHEMA_VERSION,
  PIPELINE_PROFILE_SCHEMA_VERSION,
  type CaptureStatus,
  type ClipFrame,
  type ClipManifest,
  type ClipTrack,
  type PipelineProfile,
  type ReviewApi,
  type ReviewRole,
  type SessionList,
  type SessionSummary,
  type SyntheticSwingHilEvidence,
  type SyntheticSwingHilStage,
} from "./review_api.js";

const FRAME_COUNT = 90;
const IMPACT_FRAME = 45;
const FRAME_PERIOD_US = 33_333;

export const FIXTURE_PIPELINE_PROFILE: PipelineProfile = {
  schema_version: PIPELINE_PROFILE_SCHEMA_VERSION,
  capture: {
    trigger_estimate_to_confirmation_ms: 2,
    confirmation_to_acceptance_ms: -0.4,
    acceptance_to_freeze_start_ms: 1_500,
    freeze_schedule_lateness_ms: -0.2,
    freeze_and_rotate_ms: 2.8,
    audio_stop_ms: 1.1,
  },
  session: {
    prepublication_analysis_ms: 4.6,
    publisher_planning_ms: 3.7,
    validation_and_timeline_ms: 5.2,
    output_setup_ms: 1.8,
    media_encoding_wall_ms: 82.5,
    frame_metadata_ms: 3.1,
    profile_snapshot_after_confirmation_ms: 1_594.6,
    profile_snapshot_host_monotonic_ns: "458385600000",
  },
  views: [
    {
      role: "down_the_line",
      frame_count: FRAME_COUNT,
      timeline_ms: 2.4,
      bayer_fit_demosaic_ms: 18.2,
      rgb_to_yuv420_ms: 9.1,
      codec_encode_ms: 43.8,
      webm_mux_ms: 4.2,
      finalize_ms: 0.8,
      output_verification_ms: 2.1,
      total_ms: 80.6,
    },
    {
      role: "face_on",
      frame_count: FRAME_COUNT,
      timeline_ms: 2.5,
      bayer_fit_demosaic_ms: 18.7,
      rgb_to_yuv420_ms: 9.3,
      codec_encode_ms: 44.1,
      webm_mux_ms: 4.4,
      finalize_ms: 0.9,
      output_verification_ms: 2.2,
      total_ms: 82.1,
    },
  ],
};

export const FIXTURE_MANIFEST: ClipManifest = {
  schema_version: REVIEW_SCHEMA_VERSION,
  session_id: "fixture-session-001",
  created_at_utc: "2026-08-09T16:30:00.000Z",
  trigger: {
    source: "audio",
    host_monotonic_time_ns: "456789000000",
    confirmation_host_monotonic_time_ns: "456791000000",
    sample_rate_hz: 32_000,
    peak_amplitude: 0.42,
    noise_floor: 0.012,
    threshold: 0.08,
  },
  mapped_nearest_frame_skew_us: 17,
  pipeline_profile: FIXTURE_PIPELINE_PROFILE,
  views: [
    fixtureTrack("down_the_line", "FDN22120654", "down-the-line.webm", 381_766, 8_100_000),
    fixtureTrack("face_on", "FDN23010199", "face-on.webm", 383_026, 9_300_000),
  ],
};

export const FIXTURE_SYNTHETIC_HIL_EVIDENCE: SyntheticSwingHilEvidence = {
  kind: "synthetic_swing",
  selected_brightness: 8,
  timeline: {
    step_duration_us: 20_000,
    pre_impact_step_count: 60,
    white_impact_duration_us: 20_000,
    post_impact_step_count: 25,
  },
  tone: { duration_us: 10_000, frequency_hz: 2_000 },
  optical_white_impact_frame_index: { down_the_line: 44, face_on: 46 },
  audio_trigger_estimate_offset_us: { down_the_line: 2_250, face_on: -1_750 },
  camera_schedule_alignment: {
    down_the_line: { mapped_time_correction_us: -3_500, uncertainty_us: 300 },
    face_on: { mapped_time_correction_us: 3_250, uncertainty_us: 275 },
  },
  optical_white_impact: {
    down_the_line: {
      passed: true,
      stable_frame_count: 4,
      matching_frame_count: 3,
      matching_fraction: 0.75,
      mean_signal_delta: 31.5,
      mean_expected_color_distance: 0.08,
      maximum_saturated_fraction: 0.01,
      maximum_bloom_fraction: 0.02,
      exposure_us: 500,
      gain_db: 24,
    },
    face_on: {
      passed: true,
      stable_frame_count: 5,
      matching_frame_count: 5,
      matching_fraction: 1,
      mean_signal_delta: 29.25,
      mean_expected_color_distance: 0.06,
      maximum_saturated_fraction: 0.02,
      maximum_bloom_fraction: 0.01,
      exposure_us: 500,
      gain_db: 24,
    },
  },
};

const FIXTURE_READY_SESSION: SessionSummary = {
  session_id: FIXTURE_MANIFEST.session_id,
  state: "ready",
  created_at_utc: FIXTURE_MANIFEST.created_at_utc,
  error: "",
};

export class FakeReviewApi implements ReviewApi {
  #captureStatus: CaptureStatus = {
    schema_version: CAPTURE_SCHEMA_VERSION,
    state: "armed",
    armed: true,
    active_session_id: null,
    error: "",
    hil: {
      enabled: true,
      busy: false,
      stage: "idle",
      error: "",
      last_run: null,
    },
  };
  #sessions: SessionSummary[] = [FIXTURE_READY_SESSION];
  #pendingPolls = 0;
  #manualSequence = 1;
  #hilSequence = 0;
  #hilStageIndex = -1;
  #hilStageHold = 0;
  #hilSession: SessionSummary | null = null;
  #syntheticSessionIds = new Set<string>();

  constructor({ hilEnabled = true }: { hilEnabled?: boolean } = {}) {
    this.#captureStatus.hil.enabled = hilEnabled;
  }

  async getCaptureStatus(): Promise<CaptureStatus> {
    this.#advanceSyntheticSwing();
    return structuredClone(this.#captureStatus);
  }

  async setArmed(armed: boolean): Promise<CaptureStatus> {
    if (this.#captureStatus.hil.busy) {
      throw new Error("Synthetic swing HIL is already running");
    }
    this.#captureStatus = {
      schema_version: CAPTURE_SCHEMA_VERSION,
      state: armed ? "armed" : "setup",
      armed,
      active_session_id: null,
      error: "",
      hil: structuredClone(this.#captureStatus.hil),
    };
    return structuredClone(this.#captureStatus);
  }

  async triggerManualCapture(): Promise<SessionSummary> {
    if (this.#captureStatus.hil.busy) {
      throw new Error("Synthetic swing HIL is already running");
    }
    const session: SessionSummary = {
      session_id: `fixture-manual-${String(++this.#manualSequence).padStart(3, "0")}`,
      state: "waiting_post_roll",
      created_at_utc: "2026-08-09T16:31:00.000Z",
      error: "",
    };
    this.#sessions.unshift(session);
    this.#captureStatus = {
      schema_version: CAPTURE_SCHEMA_VERSION,
      state: "waiting_post_roll",
      armed: false,
      active_session_id: session.session_id,
      error: "",
      hil: structuredClone(this.#captureStatus.hil),
    };
    this.#pendingPolls = 2;
    return structuredClone(session);
  }

  async startSyntheticSwing(): Promise<CaptureStatus> {
    if (!this.#captureStatus.hil.enabled) {
      throw new Error("Synthetic swing HIL is not enabled on this station");
    }
    if (this.#captureStatus.hil.busy) {
      throw new Error("Synthetic swing HIL is already running");
    }
    if (
      this.#captureStatus.armed ||
      (this.#captureStatus.state !== "setup" && this.#captureStatus.state !== "ready")
    ) {
      throw new Error("Disarm normal capture before starting synthetic swing HIL");
    }
    const sequence = String(++this.#hilSequence).padStart(3, "0");
    const second = String(this.#hilSequence % 60).padStart(2, "0");
    this.#hilSession = {
      session_id: `fixture-synthetic-${sequence}`,
      state: "waiting_post_roll",
      created_at_utc: `2026-08-09T16:32:${second}.000Z`,
      error: "",
    };
    this.#hilStageIndex = 0;
    this.#hilStageHold = 1;
    this.#applySyntheticSwingStage("calibrating");
    return structuredClone(this.#captureStatus);
  }

  async getSessions(): Promise<SessionList> {
    const active = this.#sessions[0];
    if (active !== undefined && this.#pendingPolls > 0) {
      --this.#pendingPolls;
      active.state = this.#pendingPolls === 0 ? "ready" : "encoding";
      this.#captureStatus = {
        schema_version: CAPTURE_SCHEMA_VERSION,
        state: this.#pendingPolls === 0 ? "ready" : active.state,
        armed: false,
        active_session_id: active.session_id,
        error: "",
        hil: structuredClone(this.#captureStatus.hil),
      };
    }
    return {
      schema_version: REVIEW_SCHEMA_VERSION,
      sessions: structuredClone(this.#sessions),
    };
  }

  async getManifest(sessionId: string): Promise<ClipManifest> {
    const session = this.#sessions.find((candidate) => candidate.session_id === sessionId);
    if (session === undefined) {
      throw new Error(`Unknown fixture session ${sessionId}`);
    }
    if (session.state !== "ready") {
      throw new Error(`Fixture session ${sessionId} is not ready`);
    }
    const manifest = structuredClone(FIXTURE_MANIFEST);
    manifest.session_id = sessionId;
    manifest.created_at_utc = session.created_at_utc;
    if (this.#syntheticSessionIds.has(sessionId)) {
      manifest.hil_evidence = structuredClone(FIXTURE_SYNTHETIC_HIL_EVIDENCE);
    }
    for (const view of manifest.views) {
      view.media.path = `fixtures/review/${view.media.path}`;
    }
    manifest.client_delivery_profile = {
      manifest_fetch_duration_ms: 6.5,
      manifest_response_received_performance_ms: globalThis.performance?.now() ?? Date.now(),
      server_response_host_monotonic_ns: "458500000000",
    };
    return manifest;
  }

  #advanceSyntheticSwing() {
    if (!this.#captureStatus.hil.busy) {
      return;
    }
    if (this.#hilStageHold > 0) {
      --this.#hilStageHold;
      return;
    }
    const nextStage = SYNTHETIC_SWING_STAGES[++this.#hilStageIndex];
    if (nextStage !== undefined) {
      this.#applySyntheticSwingStage(nextStage);
    }
  }

  #applySyntheticSwingStage(stage: SyntheticSwingHilStage) {
    const session = this.#hilSession;
    if (session === null) {
      throw new Error("Synthetic swing fixture lost its session");
    }
    const hasSession = stage === "capturing" || stage === "encoding" || stage === "ready";
    if (stage === "capturing") {
      session.state = "waiting_post_roll";
      if (!this.#sessions.some((candidate) => candidate.session_id === session.session_id)) {
        this.#sessions.unshift(session);
      }
    } else if (stage === "encoding") {
      session.state = "encoding";
    } else if (stage === "ready") {
      session.state = "ready";
      this.#syntheticSessionIds.add(session.session_id);
    }
    let captureState: CaptureStatus["state"] = "setup";
    if (stage === "capturing") {
      captureState = "waiting_post_roll";
    } else if (stage === "encoding" || stage === "ready" || stage === "error") {
      captureState = stage;
    }
    this.#captureStatus = {
      schema_version: CAPTURE_SCHEMA_VERSION,
      state: captureState,
      armed: false,
      active_session_id: hasSession ? session.session_id : null,
      error: "",
      hil: {
        enabled: true,
        busy: stage !== "ready" && stage !== "error",
        stage,
        error: "",
        last_run: {
          session_id: hasSession ? session.session_id : null,
          stage,
          error: "",
        },
      },
    };
    this.#hilStageHold = this.#captureStatus.hil.busy ? 1 : 0;
  }
}

const SYNTHETIC_SWING_STAGES: SyntheticSwingHilStage[] = [
  "calibrating",
  "stimulus",
  "capturing",
  "encoding",
  "ready",
];

function fixtureTrack(
  role: ReviewRole,
  cameraSerial: string,
  path: string,
  encodedBytes: number,
  deviceTimestampOrigin: number,
): ClipTrack {
  return {
    role,
    camera_serial: cameraSerial,
    source: { pixel_format: "BayerRG8", width: 1440, height: 1080 },
    encoded: { width: 480, height: 270 },
    frame_count: FRAME_COUNT,
    nominal_fps: 30,
    impact_frame_index: IMPACT_FRAME,
    media: {
      path,
      mime_type: "video/webm",
      codec: "vp8",
      all_frames_keyframes: true,
      encoded_bytes: encodedBytes,
    },
    frames: Array.from({ length: FRAME_COUNT }, (_, frameIndex) =>
      fixtureFrame(frameIndex, deviceTimestampOrigin),
    ),
  };
}

function fixtureFrame(frameIndex: number, deviceTimestampOrigin: number): ClipFrame {
  const mediaTimeUs = frameIndex * FRAME_PERIOD_US;
  return {
    frame_index: frameIndex,
    frame_id: String(20_000 + frameIndex),
    device_timestamp: String(deviceTimestampOrigin + frameIndex * 4_405),
    time_from_impact_us: mediaTimeUs - IMPACT_FRAME * FRAME_PERIOD_US,
    media_time_us: mediaTimeUs,
  };
}
