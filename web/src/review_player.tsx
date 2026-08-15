import { useEffect, useMemo, useRef, useState } from "react";
import type {
  ClipFrame,
  ClipManifest,
  ClipTrack,
  PipelineProfile,
  ReviewRole,
} from "./review_api.js";

const PLAYBACK_RATES = [0.25, 0.5, 1, 2] as const;

type PresentationMethod = "requestVideoFrameCallback" | "seeked-paint-fallback";

interface ImpactFramePresentation {
  performance_ms: number;
  method: PresentationMethod;
}

export interface BrowserPipelineTiming {
  schema_version: 1;
  session_id: string;
  presentation_method: PresentationMethod | "mixed";
  manifest_fetch_duration_ms: number;
  manifest_response_received_performance_ms: number;
  both_impact_frames_presented_performance_ms: number;
  manifest_response_to_both_frames_ms: number;
  manifest_request_to_both_frames_ms: number;
  server_response_host_monotonic_ns: string | null;
  audio_confirmation_to_both_frames_lower_bound_ms: number | null;
  audio_confirmation_to_both_frames_upper_bound_ms: number | null;
}

export function ReviewPlayer({ manifest }: { manifest: ClipManifest }) {
  const tracks = useMemo(() => orderedTracks(manifest.views), [manifest]);
  const referenceTrack = tracks[0];
  const [currentFrame, setCurrentFrame] = useState(referenceTrack.impact_frame_index);
  const [playing, setPlaying] = useState(false);
  const [playbackRate, setPlaybackRate] = useState(1);
  const [mediaError, setMediaError] = useState<string | null>(null);
  const [impactPresentations, setImpactPresentations] = useState<
    Partial<Record<ReviewRole, ImpactFramePresentation>>
  >({});
  const videoRefs = useRef<Partial<Record<ReviewRole, HTMLVideoElement>>>({});
  const presentationCleanupsRef = useRef<Partial<Record<ReviewRole, () => void>>>({});
  const initializedRef = useRef(false);
  const initializedSessionRef = useRef(manifest.session_id);

  if (initializedSessionRef.current !== manifest.session_id) {
    initializedSessionRef.current = manifest.session_id;
    initializedRef.current = false;
  }

  useEffect(() => {
    setCurrentFrame(referenceTrack.impact_frame_index);
    setPlaying(false);
    setMediaError(null);
    setImpactPresentations({});
    return () => {
      for (const cleanup of Object.values(presentationCleanupsRef.current)) {
        cleanup?.();
      }
      presentationCleanupsRef.current = {};
    };
  }, [manifest.session_id, referenceTrack.impact_frame_index]);

  const markImpactPresented = (
    role: ReviewRole,
    presentation: ImpactFramePresentation,
    sessionId: string,
  ) => {
    if (initializedSessionRef.current !== sessionId) {
      return;
    }
    setImpactPresentations((current) =>
      current[role] === undefined ? { ...current, [role]: presentation } : current,
    );
  };

  const watchImpactPresentation = (track: ClipTrack, video: HTMLVideoElement) => {
    presentationCleanupsRef.current[track.role]?.();
    const sessionId = manifest.session_id;
    const targetSeconds =
      requiredFrame(track.frames, track.impact_frame_index).media_time_us / 1_000_000;
    const toleranceSeconds = Math.max(0.002, 0.55 / track.nominal_fps);
    if (
      typeof video.requestVideoFrameCallback === "function" &&
      typeof video.cancelVideoFrameCallback === "function"
    ) {
      let active = true;
      let callbackId = 0;
      const inspectFrame: VideoFrameRequestCallback = (now, metadata) => {
        if (!active) {
          return;
        }
        if (Math.abs(metadata.mediaTime - targetSeconds) <= toleranceSeconds) {
          markImpactPresented(
            track.role,
            {
              performance_ms: Math.max(now, metadata.expectedDisplayTime),
              method: "requestVideoFrameCallback",
            },
            sessionId,
          );
          return;
        }
        video.currentTime = targetSeconds;
        callbackId = video.requestVideoFrameCallback(inspectFrame);
      };
      callbackId = video.requestVideoFrameCallback(inspectFrame);
      presentationCleanupsRef.current[track.role] = () => {
        active = false;
        video.cancelVideoFrameCallback(callbackId);
      };
      return;
    }

    let active = true;
    let animationFrame = 0;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const confirmAfterPaint = () => {
      if (!active || Math.abs(video.currentTime - targetSeconds) > toleranceSeconds) {
        return;
      }
      const confirm = () => {
        if (active) {
          markImpactPresented(
            track.role,
            { performance_ms: highResolutionNow(), method: "seeked-paint-fallback" },
            sessionId,
          );
        }
      };
      if (typeof requestAnimationFrame === "function") {
        animationFrame = requestAnimationFrame(() => {
          animationFrame = requestAnimationFrame(confirm);
        });
      } else {
        timer = setTimeout(confirm, 0);
      }
    };
    video.addEventListener("seeked", confirmAfterPaint);
    video.addEventListener("canplay", confirmAfterPaint);
    if (video.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA) {
      confirmAfterPaint();
    }
    presentationCleanupsRef.current[track.role] = () => {
      active = false;
      video.removeEventListener("seeked", confirmAfterPaint);
      video.removeEventListener("canplay", confirmAfterPaint);
      if (animationFrame !== 0) {
        cancelAnimationFrame(animationFrame);
      }
      if (timer !== undefined) {
        clearTimeout(timer);
      }
    };
  };

  const pauseAll = () => {
    for (const video of Object.values(videoRefs.current)) {
      video?.pause();
    }
    setPlaying(false);
  };

  const seekFrame = (requestedFrame: number, pause = true) => {
    const frameIndex = clamp(Math.round(requestedFrame), 0, referenceTrack.frame_count - 1);
    if (pause) {
      pauseAll();
    }
    const referenceFrame = requiredFrame(referenceTrack.frames, frameIndex);
    for (const track of tracks) {
      const video = videoRefs.current[track.role];
      if (video !== undefined) {
        video.currentTime =
          nearestImpactFrame(track, referenceFrame.time_from_impact_us).media_time_us / 1_000_000;
      }
    }
    setCurrentFrame(frameIndex);
  };

  const initializeVideo = (track: ClipTrack, video: HTMLVideoElement) => {
    video.playbackRate = playbackRate;
    if (video.dataset.reviewSession === manifest.session_id) {
      return;
    }
    const targetTime =
      requiredFrame(track.frames, track.impact_frame_index).media_time_us / 1_000_000;
    // Register before seeking so a fast decoder cannot present the target frame
    // between the seek assignment and requestVideoFrameCallback registration.
    watchImpactPresentation(track, video);
    if (Math.abs(video.currentTime - targetTime) > 0.000_001) {
      video.currentTime = targetTime;
    }
    video.dataset.reviewSession = manifest.session_id;
    if (track.role === referenceTrack.role) {
      initializedRef.current = true;
      setCurrentFrame(referenceTrack.impact_frame_index);
    }
  };

  const togglePlayback = async () => {
    if (playing) {
      pauseAll();
      return;
    }
    if (currentFrame === referenceTrack.frame_count - 1) {
      seekFrame(0, false);
    }
    setMediaError(null);
    for (const video of Object.values(videoRefs.current)) {
      if (video !== undefined) {
        video.playbackRate = playbackRate;
      }
    }
    try {
      await Promise.all(
        tracks.map(async (track) => {
          const video = videoRefs.current[track.role];
          if (video === undefined) {
            throw new Error(`${roleLabel(track.role)} media is not mounted`);
          }
          await video.play();
        }),
      );
      setPlaying(true);
    } catch (caught) {
      pauseAll();
      setMediaError(errorMessage(caught));
    }
  };

  const changePlaybackRate = (rate: number) => {
    setPlaybackRate(rate);
    for (const video of Object.values(videoRefs.current)) {
      if (video !== undefined) {
        video.playbackRate = rate;
      }
    }
  };

  const updateFromReferenceVideo = (video: HTMLVideoElement) => {
    if (!initializedRef.current) {
      return;
    }
    const nextFrame = nearestMediaFrame(referenceTrack, video.currentTime * 1_000_000);
    setCurrentFrame(nextFrame.frame_index);
    const relativeTimeUs = nextFrame.time_from_impact_us;
    for (const track of tracks.slice(1)) {
      const secondary = videoRefs.current[track.role];
      if (secondary === undefined) {
        continue;
      }
      const expected = nearestImpactFrame(track, relativeTimeUs).media_time_us / 1_000_000;
      const tolerance = Math.max(0.004, 0.75 / track.nominal_fps);
      if (Math.abs(secondary.currentTime - expected) > tolerance) {
        secondary.currentTime = expected;
      }
    }
  };

  useEffect(() => {
    if (!playing || typeof requestAnimationFrame !== "function") {
      return;
    }
    let animationFrame = 0;
    const synchronize = () => {
      const referenceVideo = videoRefs.current[referenceTrack.role];
      if (referenceVideo !== undefined) {
        updateFromReferenceVideo(referenceVideo);
      }
      animationFrame = requestAnimationFrame(synchronize);
    };
    animationFrame = requestAnimationFrame(synchronize);
    return () => cancelAnimationFrame(animationFrame);
  }, [manifest.session_id, playing, referenceTrack.role]);

  const frame = requiredFrame(referenceTrack.frames, currentFrame);
  const impactOffsetFrames = currentFrame - referenceTrack.impact_frame_index;
  const step = (amount: number) => seekFrame(currentFrame + amount);
  const browserTiming = buildBrowserPipelineTiming(manifest, impactPresentations);

  return (
    <section aria-label="Synchronized clip player" className="review-player">
      <div className="review-video-grid">
        {tracks.map((track, index) => {
          const mappedFrame = nearestImpactFrame(track, frame.time_from_impact_us);
          const hilEvidence = manifest.hil_evidence;
          const whiteImpact = hilEvidence?.optical_white_impact[track.role];
          const scheduleAlignment = hilEvidence?.camera_schedule_alignment[track.role];
          const whiteImpactFrame =
            hilEvidence === undefined
              ? undefined
              : requiredFrame(
                  track.frames,
                  hilEvidence.optical_white_impact_frame_index[track.role],
                );
          return (
            <figure className="review-view" key={track.role}>
              <div className="review-video-shell">
                <video
                  aria-label={`${roleLabel(track.role)} recorded swing`}
                  muted
                  onCanPlay={(event) => initializeVideo(track, event.currentTarget)}
                  onEnded={index === 0 ? () => setPlaying(false) : undefined}
                  onError={() => {
                    setMediaError(`${roleLabel(track.role)} encoded clip could not be decoded.`);
                    pauseAll();
                  }}
                  onLoadedMetadata={(event) => initializeVideo(track, event.currentTarget)}
                  onPause={index === 0 ? () => setPlaying(false) : undefined}
                  onPlay={index === 0 ? () => setPlaying(true) : undefined}
                  onTimeUpdate={
                    index === 0
                      ? (event) => updateFromReferenceVideo(event.currentTarget)
                      : undefined
                  }
                  playsInline
                  preload="auto"
                  ref={(video) => {
                    if (video === null) {
                      delete videoRefs.current[track.role];
                    } else {
                      videoRefs.current[track.role] = video;
                    }
                  }}
                >
                  <source
                    src={track.media.url ?? track.media.path}
                    type={`${track.media.mime_type}; codecs="${track.media.codec}"`}
                  />
                </video>
                <span className="review-frame-chip">
                  Frame {mappedFrame.frame_index + 1} · {formatImpactTime(mappedFrame)}
                </span>
              </div>
              <figcaption>
                <span>
                  <strong>{roleLabel(track.role)}</strong>
                  <small>{track.camera_serial}</small>
                </span>
                <span>
                  {track.source.width}×{track.source.height} · {formatRate(track.nominal_fps)} fps
                </span>
              </figcaption>
              {hilEvidence !== undefined ? (
                <section
                  aria-label={`${roleLabel(track.role)} synthetic HIL evidence`}
                  className="review-hil-evidence"
                >
                  <span>
                    Automated white-impact check ·{" "}
                    {whiteImpact?.passed === true ? "passed" : "failed"}
                    {" · frame "}
                    {hilEvidence.optical_white_impact_frame_index[track.role] + 1} · source ID{" "}
                    {whiteImpactFrame?.frame_id}
                  </span>
                  <span>
                    White match · {whiteImpact?.matching_frame_count} of{" "}
                    {whiteImpact?.stable_frame_count} stable frames (
                    {((whiteImpact?.matching_fraction ?? 0) * 100).toFixed(0)}
                    %)
                  </span>
                  <span>
                    Signal {formatDiagnostic(whiteImpact?.mean_signal_delta)} · color distance{" "}
                    {formatDiagnostic(whiteImpact?.mean_expected_color_distance)} · saturation{" "}
                    {formatPercent(whiteImpact?.maximum_saturated_fraction)} · bloom{" "}
                    {formatPercent(whiteImpact?.maximum_bloom_fraction)}
                  </span>
                  <span>
                    Camera profile · {formatDiagnostic(whiteImpact?.exposure_us)} µs ·{" "}
                    {formatDiagnostic(whiteImpact?.gain_db)} dB gain
                  </span>
                  <span>
                    Schedule mapping ·{" "}
                    {formatSignedMicroseconds(scheduleAlignment?.mapped_time_correction_us ?? 0)}{" "}
                    correction · ±
                    {formatUnsignedMicroseconds(scheduleAlignment?.uncertainty_us ?? 0)} uncertainty
                  </span>
                  <span>
                    Audio trigger estimate relative to white ·{" "}
                    {formatSignedMicroseconds(
                      hilEvidence.audio_trigger_estimate_offset_us[track.role],
                    )}{" "}
                    (uncalibrated)
                  </span>
                </section>
              ) : null}
            </figure>
          );
        })}
      </div>

      {manifest.hil_evidence !== undefined ? (
        <p className="review-hil-context">
          The {manifest.hil_evidence.timeline.pre_impact_step_count} pre-impact and{" "}
          {manifest.hil_evidence.timeline.post_impact_step_count} post-impact LED colors are visual
          timeline context for human playback. The automated optical gate checks only the white
          impact marker.
        </p>
      ) : null}

      {manifest.pipeline_profile !== undefined ? (
        <PipelineProfilePanel browserTiming={browserTiming} profile={manifest.pipeline_profile} />
      ) : null}

      {mediaError !== null ? (
        <p className="review-error" role="alert">
          {mediaError}
        </p>
      ) : null}

      <div className="timeline-panel">
        <div className="timeline-summary" aria-live="polite">
          <strong>{playing ? "Playing" : "Paused"}</strong>
          <span>
            Frame {currentFrame + 1} of {referenceTrack.frame_count}
          </span>
          <span>{impactFrameLabel(impactOffsetFrames)}</span>
          <span>{formatImpactTime(frame)}</span>
        </div>
        <label className="sr-only" htmlFor="review-timeline">
          Review timeline
        </label>
        <input
          aria-valuetext={`Frame ${currentFrame + 1}, ${formatImpactTime(frame)}`}
          id="review-timeline"
          max={referenceTrack.frame_count - 1}
          min={0}
          onChange={(event) => seekFrame(event.currentTarget.valueAsNumber)}
          step={1}
          type="range"
          value={currentFrame}
        />
        <div className="timeline-markers" aria-hidden="true">
          <span>Start</span>
          <span
            className="impact-marker"
            style={{ left: `${impactMarkerPosition(referenceTrack)}%` }}
          >
            Trigger estimate
          </span>
          <span>End</span>
        </div>
        <div className="transport-controls">
          <button
            aria-label="Previous frame"
            className="transport-button secondary"
            disabled={currentFrame === 0}
            onClick={() => step(-1)}
            type="button"
          >
            −1 frame
          </button>
          <button className="play-button" onClick={() => void togglePlayback()} type="button">
            {playing ? "Pause" : "Play"}
          </button>
          <button
            aria-label="Next frame"
            className="transport-button secondary"
            disabled={currentFrame === referenceTrack.frame_count - 1}
            onClick={() => step(1)}
            type="button"
          >
            +1 frame
          </button>
          <label className="speed-control">
            <span>Playback speed</span>
            <select
              onChange={(event) => changePlaybackRate(Number(event.currentTarget.value))}
              value={playbackRate}
            >
              {PLAYBACK_RATES.map((rate) => (
                <option key={rate} value={rate}>
                  {rate}×
                </option>
              ))}
            </select>
          </label>
        </div>
      </div>
    </section>
  );
}

function PipelineProfilePanel({
  profile,
  browserTiming,
}: {
  profile: PipelineProfile;
  browserTiming: BrowserPipelineTiming | null;
}) {
  return (
    <section
      aria-label="Pipeline profile"
      className="pipeline-profile"
      data-browser-timing={browserTiming === null ? undefined : JSON.stringify(browserTiming)}
    >
      <header>
        <span className="section-kicker">Pipeline profile</span>
        <h3>Capture to both impact frames displayed</h3>
        <p>Wall-clock stage durations from the station, followed by browser delivery timing.</p>
      </header>
      <div className="pipeline-profile-groups">
        <ProfileGroup
          label="Capture"
          values={[
            [
              "Trigger estimate → confirmation",
              profile.capture.trigger_estimate_to_confirmation_ms,
            ],
            ["Confirmation → acceptance", profile.capture.confirmation_to_acceptance_ms],
            ["Acceptance → freeze start", profile.capture.acceptance_to_freeze_start_ms],
            ["Freeze schedule lateness", profile.capture.freeze_schedule_lateness_ms],
            ["Freeze + ring rotation", profile.capture.freeze_and_rotate_ms],
            ["Audio stop", profile.capture.audio_stop_ms],
          ]}
        />
        <ProfileGroup
          label="Session publication"
          values={[
            ["Prepublication analysis", profile.session.prepublication_analysis_ms],
            ["Publisher planning", profile.session.publisher_planning_ms],
            ["Impact preview render", profile.session.impact_preview_render_ms],
            [
              "Confirmation → impact preview ready",
              profile.session.impact_preview_ready_after_confirmation_ms,
            ],
            ["Validation + timeline", profile.session.validation_and_timeline_ms],
            ["Output setup", profile.session.output_setup_ms],
            ["Media encoding wall", profile.session.media_encoding_wall_ms],
            ["Frame metadata", profile.session.frame_metadata_ms],
            ["Snapshot after confirmation", profile.session.profile_snapshot_after_confirmation_ms],
          ]}
        />
      </div>
      <div className="pipeline-view-table">
        <table aria-label="Per-camera encoding profile">
          <thead>
            <tr>
              <th scope="col">View</th>
              <th scope="col">Frames</th>
              <th scope="col">Timeline</th>
              <th scope="col">Demosaic</th>
              <th scope="col">YUV420</th>
              <th scope="col">Codec</th>
              <th scope="col">WebM</th>
              <th scope="col">Finalize</th>
              <th scope="col">Verify</th>
              <th scope="col">Total</th>
            </tr>
          </thead>
          <tbody>
            {orderedProfileViews(profile.views).map((view) => (
              <tr key={view.role}>
                <th scope="row">{roleLabel(view.role)}</th>
                <td>{view.frame_count}</td>
                <td>{formatMilliseconds(view.timeline_ms)}</td>
                <td>{formatMilliseconds(view.bayer_fit_demosaic_ms)}</td>
                <td>{formatMilliseconds(view.rgb_to_yuv420_ms)}</td>
                <td>{formatMilliseconds(view.codec_encode_ms)}</td>
                <td>{formatMilliseconds(view.webm_mux_ms)}</td>
                <td>{formatMilliseconds(view.finalize_ms)}</td>
                <td>{formatMilliseconds(view.output_verification_ms)}</td>
                <td>{formatMilliseconds(view.total_ms)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <div className="browser-profile" aria-live="polite">
        <h4>Browser delivery</h4>
        {browserTiming === null ? (
          <p>Waiting for both target impact frames to be presented…</p>
        ) : (
          <dl>
            <div>
              <dt>Frame presentation observer</dt>
              <dd>{formatPresentationMethod(browserTiming.presentation_method)}</dd>
            </div>
            <div>
              <dt>Manifest request → response</dt>
              <dd>{formatMilliseconds(browserTiming.manifest_fetch_duration_ms)}</dd>
            </div>
            <div>
              <dt>Manifest response → both impact frames displayed</dt>
              <dd>{formatMilliseconds(browserTiming.manifest_response_to_both_frames_ms)}</dd>
            </div>
            <div>
              <dt>Manifest request → both impact frames displayed</dt>
              <dd>{formatMilliseconds(browserTiming.manifest_request_to_both_frames_ms)}</dd>
            </div>
            <div>
              <dt>Audio confirmation → both frames displayed, lower bound</dt>
              <dd>
                {formatOptionalMilliseconds(
                  browserTiming.audio_confirmation_to_both_frames_lower_bound_ms,
                )}
              </dd>
            </div>
            <div>
              <dt>Audio confirmation → both frames displayed, upper bound</dt>
              <dd>
                {formatOptionalMilliseconds(
                  browserTiming.audio_confirmation_to_both_frames_upper_bound_ms,
                )}
              </dd>
            </div>
          </dl>
        )}
        <p className="pipeline-clock-caveat">
          Bounds combine a station monotonic interval with a browser performance interval; no clock
          epochs are assumed to match. The manifest fetch duration widens the upper bound. Bounds
          are unavailable when the response omits a valid server monotonic timestamp.
        </p>
      </div>
    </section>
  );
}

function ProfileGroup({ label, values }: { label: string; values: Array<[string, number]> }) {
  return (
    <section aria-label={`${label} timing`}>
      <h4>{label}</h4>
      <dl>
        {values.map(([name, duration]) => (
          <div key={name}>
            <dt>{name}</dt>
            <dd>{formatMilliseconds(duration)}</dd>
          </div>
        ))}
      </dl>
    </section>
  );
}

function buildBrowserPipelineTiming(
  manifest: ClipManifest,
  presentations: Partial<Record<ReviewRole, ImpactFramePresentation>>,
): BrowserPipelineTiming | null {
  const delivery = manifest.client_delivery_profile;
  const downTheLine = presentations.down_the_line;
  const faceOn = presentations.face_on;
  if (delivery === undefined || downTheLine === undefined || faceOn === undefined) {
    return null;
  }
  const bothPresented = Math.max(downTheLine.performance_ms, faceOn.performance_ms);
  const responseToPresentation = Math.max(
    0,
    bothPresented - delivery.manifest_response_received_performance_ms,
  );
  const hostConfirmationToResponse = hostMonotonicIntervalMilliseconds(
    manifest.trigger.confirmation_host_monotonic_time_ns,
    delivery.server_response_host_monotonic_ns,
  );
  const lowerBound =
    hostConfirmationToResponse === null
      ? null
      : hostConfirmationToResponse + responseToPresentation;
  return {
    schema_version: 1,
    session_id: manifest.session_id,
    presentation_method: downTheLine.method === faceOn.method ? downTheLine.method : "mixed",
    manifest_fetch_duration_ms: delivery.manifest_fetch_duration_ms,
    manifest_response_received_performance_ms: delivery.manifest_response_received_performance_ms,
    both_impact_frames_presented_performance_ms: bothPresented,
    manifest_response_to_both_frames_ms: responseToPresentation,
    manifest_request_to_both_frames_ms:
      delivery.manifest_fetch_duration_ms + responseToPresentation,
    server_response_host_monotonic_ns: delivery.server_response_host_monotonic_ns,
    audio_confirmation_to_both_frames_lower_bound_ms: lowerBound,
    audio_confirmation_to_both_frames_upper_bound_ms:
      lowerBound === null ? null : lowerBound + delivery.manifest_fetch_duration_ms,
  };
}

function hostMonotonicIntervalMilliseconds(
  begin: string | null,
  end: string | null,
): number | null {
  if (begin === null || end === null) {
    return null;
  }
  try {
    const difference = BigInt(end) - BigInt(begin);
    if (difference < 0) {
      return null;
    }
    const milliseconds = Number(difference) / 1_000_000;
    return Number.isFinite(milliseconds) ? milliseconds : null;
  } catch {
    return null;
  }
}

function orderedProfileViews(views: PipelineProfile["views"]): PipelineProfile["views"] {
  const downTheLine = views.find((view) => view.role === "down_the_line");
  const faceOn = views.find((view) => view.role === "face_on");
  if (downTheLine === undefined || faceOn === undefined) {
    throw new Error("A pipeline profile requires both camera roles");
  }
  return [downTheLine, faceOn];
}

function orderedTracks(tracks: ClipTrack[]): [ClipTrack, ClipTrack] {
  const downTheLine = tracks.find((track) => track.role === "down_the_line");
  const faceOn = tracks.find((track) => track.role === "face_on");
  if (downTheLine === undefined || faceOn === undefined) {
    throw new Error("A review clip requires both camera roles");
  }
  return [downTheLine, faceOn];
}

function nearestImpactFrame(track: ClipTrack, timeFromImpactUs: number): ClipFrame {
  return nearestFrame(track.frames, timeFromImpactUs, (frame) => frame.time_from_impact_us);
}

function nearestMediaFrame(track: ClipTrack, mediaTimeUs: number): ClipFrame {
  return nearestFrame(track.frames, mediaTimeUs, (frame) => frame.media_time_us);
}

function nearestFrame(
  frames: ClipFrame[],
  value: number,
  timestamp: (frame: ClipFrame) => number,
): ClipFrame {
  let low = 0;
  let high = frames.length - 1;
  while (low < high) {
    const middle = Math.floor((low + high) / 2);
    if (timestamp(requiredFrame(frames, middle)) < value) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low > 0) {
    const before = requiredFrame(frames, low - 1);
    const after = requiredFrame(frames, low);
    if (Math.abs(timestamp(before) - value) <= Math.abs(timestamp(after) - value)) {
      return before;
    }
  }
  return requiredFrame(frames, low);
}

function requiredFrame(frames: ClipFrame[], index: number): ClipFrame {
  const frame = frames[index];
  if (frame === undefined) {
    throw new Error(`Frame index ${index} is outside the retained clip`);
  }
  return frame;
}

function roleLabel(role: ReviewRole): string {
  return role === "down_the_line" ? "Down-the-line" : "Face-on";
}

function formatRate(fps: number): string {
  return fps.toFixed(fps >= 100 ? 1 : 2).replace(/\.0+$/, "");
}

function formatMilliseconds(milliseconds: number): string {
  return `${milliseconds.toFixed(milliseconds >= 100 ? 1 : 2).replace(/\.00$/, "")} ms`;
}

function formatOptionalMilliseconds(milliseconds: number | null): string {
  return milliseconds === null ? "Unavailable" : formatMilliseconds(milliseconds);
}

function formatPresentationMethod(method: BrowserPipelineTiming["presentation_method"]): string {
  switch (method) {
    case "requestVideoFrameCallback":
      return "Video frame callback";
    case "seeked-paint-fallback":
      return "Seeked + paint fallback";
    case "mixed":
      return "Mixed browser observers";
  }
}

function formatSignedMicroseconds(microseconds: number): string {
  if (microseconds === 0) {
    return "0.0 ms";
  }
  return `${microseconds > 0 ? "+" : "−"}${Math.abs(microseconds / 1_000).toFixed(1)} ms`;
}

function formatUnsignedMicroseconds(microseconds: number): string {
  return `${(microseconds / 1_000).toFixed(1)} ms`;
}

function formatDiagnostic(value: number | undefined): string {
  return value?.toFixed(2).replace(/\.00$/, "") ?? "—";
}

function formatPercent(value: number | undefined): string {
  return value === undefined ? "—" : `${(value * 100).toFixed(1)}%`;
}

function formatImpactTime(frame: ClipFrame): string {
  const milliseconds = frame.time_from_impact_us / 1_000;
  if (milliseconds === 0) {
    return "Trigger estimate";
  }
  return `${milliseconds > 0 ? "+" : "−"}${Math.abs(milliseconds).toFixed(1)} ms`;
}

function impactFrameLabel(offset: number): string {
  if (offset === 0) {
    return "Audio-trigger estimate frame";
  }
  return `${Math.abs(offset)} frame${Math.abs(offset) === 1 ? "" : "s"} ${
    offset < 0 ? "before" : "after"
  } trigger estimate`;
}

function impactMarkerPosition(track: ClipTrack): number {
  return (track.impact_frame_index / Math.max(1, track.frame_count - 1)) * 100;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.min(maximum, Math.max(minimum, value));
}

function errorMessage(caught: unknown): string {
  return caught instanceof Error ? caught.message : "Unknown playback error";
}

function highResolutionNow(): number {
  return globalThis.performance?.now() ?? Date.now();
}
