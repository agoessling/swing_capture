import {
  useEffect,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent as ReactKeyboardEvent,
  type PointerEvent as ReactPointerEvent,
  type RefObject,
} from "react";
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
  media_time_seconds: number;
  method: PresentationMethod;
}

interface TimelinePreview {
  frameIndex: number;
  leftPercent: number;
}

export interface BrowserPipelineTiming {
  schema_version: 1;
  session_id: string;
  presentation_method: PresentationMethod | "mixed";
  impact_frame_media_time_seconds: Partial<Record<ReviewRole, number>>;
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
  const [timelinePreview, setTimelinePreview] = useState<TimelinePreview | null>(null);
  const [impactPresentations, setImpactPresentations] = useState<
    Partial<Record<ReviewRole, ImpactFramePresentation>>
  >({});
  const currentFrameRef = useRef(referenceTrack.impact_frame_index);
  const operatorSeekRevisionRef = useRef(0);
  const videoRefs = useRef<Partial<Record<ReviewRole, HTMLVideoElement>>>({});
  const timelinePreviewVideoRef = useRef<HTMLVideoElement | null>(null);
  const presentationCleanupsRef = useRef<Partial<Record<ReviewRole, () => void>>>({});
  const initializedRef = useRef(false);
  const initializedSessionRef = useRef(manifest.session_id);
  const reviewDetailsButtonRef = useRef<HTMLButtonElement | null>(null);
  const reviewDetailsCloseButtonRef = useRef<HTMLButtonElement | null>(null);
  const [reviewDetailsOpen, setReviewDetailsOpen] = useState(false);

  if (initializedSessionRef.current !== manifest.session_id) {
    initializedSessionRef.current = manifest.session_id;
    initializedRef.current = false;
  }

  useEffect(() => {
    currentFrameRef.current = referenceTrack.impact_frame_index;
    operatorSeekRevisionRef.current = 0;
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

  useEffect(() => {
    if (!reviewDetailsOpen) {
      return;
    }
    reviewDetailsCloseButtonRef.current?.focus();
    const handleKeyDown = (event: globalThis.KeyboardEvent) => {
      if (event.key !== "Escape") {
        return;
      }
      event.preventDefault();
      setReviewDetailsOpen(false);
      reviewDetailsButtonRef.current?.focus();
    };
    document.addEventListener("keydown", handleKeyDown);
    return () => document.removeEventListener("keydown", handleKeyDown);
  }, [reviewDetailsOpen]);

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
    const targetFrame = requiredFrame(track.frames, track.impact_frame_index);
    const targetSeconds = targetFrame.media_time_us / 1_000_000;
    const recoverySeekSeconds = presentationRecoverySeekTimeSeconds(track, targetFrame);
    const toleranceSeconds = Math.max(0.002, 0.55 / track.nominal_fps);
    const operatorSeekRevision = operatorSeekRevisionRef.current;
    if (
      typeof video.requestVideoFrameCallback === "function" &&
      typeof video.cancelVideoFrameCallback === "function"
    ) {
      let active = true;
      let callbackId = 0;
      let stalledSeekTimer: ReturnType<typeof setTimeout> | undefined;
      let presentationNudgeDeadline: ReturnType<typeof setTimeout> | undefined;
      let presentationNudge = false;
      const originalPlaybackRate = video.playbackRate;
      const stopPresentationNudge = () => {
        if (!presentationNudge) {
          return;
        }
        video.pause();
        video.playbackRate = originalPlaybackRate;
        presentationNudge = false;
        if (presentationNudgeDeadline !== undefined) {
          clearTimeout(presentationNudgeDeadline);
          presentationNudgeDeadline = undefined;
        }
      };
      const inspectFrame: VideoFrameRequestCallback = (now, metadata) => {
        if (!active) {
          return;
        }
        if (Math.abs(metadata.mediaTime - targetSeconds) <= toleranceSeconds) {
          if (stalledSeekTimer !== undefined) {
            clearTimeout(stalledSeekTimer);
          }
          stopPresentationNudge();
          markImpactPresented(
            track.role,
            {
              performance_ms: Math.max(now, metadata.expectedDisplayTime),
              media_time_seconds: metadata.mediaTime,
              method: "requestVideoFrameCallback",
            },
            sessionId,
          );
          return;
        }
        if (operatorSeekRevisionRef.current !== operatorSeekRevision) {
          active = false;
          stopPresentationNudge();
          return;
        }
        if (!presentationNudge) {
          // A frame callback registered before the initialization seek may first observe the
          // previously presented sample. Recover through a second, distinct interior point. The
          // distinct value forces a new paused seek even when initialization already selected the
          // midpoint, while avoiding the raw MP4 boundary that may resolve to the prior sample.
          video.currentTime = recoverySeekSeconds;
        }
        callbackId = video.requestVideoFrameCallback(inspectFrame);
      };
      const recoverStalledPausedSeek = () => {
        if (
          !active ||
          presentationNudge ||
          !video.paused ||
          operatorSeekRevisionRef.current !== operatorSeekRevision ||
          !navigator.userAgent.includes("Firefox/")
        ) {
          return;
        }
        if (stalledSeekTimer !== undefined) {
          clearTimeout(stalledSeekTimer);
        }
        stalledSeekTimer = setTimeout(() => {
          if (
            !active ||
            !video.paused ||
            operatorSeekRevisionRef.current !== operatorSeekRevision
          ) {
            return;
          }
          presentationNudge = true;
          video.playbackRate = 0.25;
          const previousFrame = requiredFrame(
            track.frames,
            Math.max(0, track.impact_frame_index - 1),
          );
          video.currentTime = presentationSeekTimeSeconds(track, previousFrame);
          presentationNudgeDeadline = setTimeout(stopPresentationNudge, 1_000);
          void video.play().catch(() => {
            stopPresentationNudge();
          });
        }, 200);
      };
      callbackId = video.requestVideoFrameCallback(inspectFrame);
      video.addEventListener("seeked", recoverStalledPausedSeek);
      presentationCleanupsRef.current[track.role] = () => {
        active = false;
        video.removeEventListener("seeked", recoverStalledPausedSeek);
        if (stalledSeekTimer !== undefined) {
          clearTimeout(stalledSeekTimer);
        }
        stopPresentationNudge();
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
            {
              performance_ms: highResolutionNow(),
              media_time_seconds:
                nearestMediaFrame(track, video.currentTime * 1_000_000).media_time_us / 1_000_000,
              method: "seeked-paint-fallback",
            },
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
    operatorSeekRevisionRef.current += 1;
    currentFrameRef.current = frameIndex;
    if (pause) {
      pauseAll();
    }
    const referenceFrame = requiredFrame(referenceTrack.frames, frameIndex);
    for (const track of tracks) {
      const video = videoRefs.current[track.role];
      if (video !== undefined) {
        video.currentTime = presentationSeekTimeSeconds(
          track,
          nearestImpactFrame(track, referenceFrame.time_from_impact_us),
        );
      }
    }
    setCurrentFrame(frameIndex);
  };

  const initializeVideo = (track: ClipTrack, video: HTMLVideoElement) => {
    video.playbackRate = playbackRate;
    if (video.dataset.reviewSession === manifest.session_id) {
      return;
    }
    const operatorAlreadySought = operatorSeekRevisionRef.current > 0;
    const selectedReferenceFrame = requiredFrame(referenceTrack.frames, currentFrameRef.current);
    const targetTime = operatorAlreadySought
      ? presentationSeekTimeSeconds(
          track,
          nearestImpactFrame(track, selectedReferenceFrame.time_from_impact_us),
        )
      : presentationSeekTimeSeconds(track, requiredFrame(track.frames, track.impact_frame_index));
    if (!operatorAlreadySought) {
      // Register before seeking so a fast decoder cannot present the target frame
      // between the seek assignment and requestVideoFrameCallback registration.
      watchImpactPresentation(track, video);
    }
    if (Math.abs(video.currentTime - targetTime) > 0.000_001) {
      video.currentTime = targetTime;
    }
    video.dataset.reviewSession = manifest.session_id;
    if (track.role === referenceTrack.role) {
      initializedRef.current = true;
      if (!operatorAlreadySought) {
        currentFrameRef.current = referenceTrack.impact_frame_index;
        setCurrentFrame(referenceTrack.impact_frame_index);
      }
    }
  };

  const togglePlayback = async () => {
    if (playing) {
      pauseAll();
      return;
    }
    if (currentFrameRef.current === referenceTrack.frame_count - 1) {
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
    if (!initializedRef.current || video.paused) {
      return;
    }
    const nextFrame = nearestMediaFrame(referenceTrack, video.currentTime * 1_000_000);
    currentFrameRef.current = nextFrame.frame_index;
    setCurrentFrame(nextFrame.frame_index);
    const relativeTimeUs = nextFrame.time_from_impact_us;
    for (const track of tracks.slice(1)) {
      const secondary = videoRefs.current[track.role];
      if (secondary === undefined) {
        continue;
      }
      const expected = presentationSeekTimeSeconds(
        track,
        nearestImpactFrame(track, relativeTimeUs),
      );
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
  const step = (amount: number) => seekFrame(currentFrameRef.current + amount);
  const browserTiming = buildBrowserPipelineTiming(manifest, impactPresentations);

  useEffect(() => {
    const previewVideo = timelinePreviewVideoRef.current;
    if (previewVideo === null || timelinePreview === null) {
      return;
    }
    const mediaTimeSeconds = presentationSeekTimeSeconds(
      referenceTrack,
      requiredFrame(referenceTrack.frames, timelinePreview.frameIndex),
    );
    if (Math.abs(previewVideo.currentTime - mediaTimeSeconds) > 0.000_001) {
      previewVideo.currentTime = mediaTimeSeconds;
    }
  }, [referenceTrack.frames, timelinePreview]);

  const updateTimelinePreview = (event: ReactPointerEvent<HTMLInputElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect();
    if (bounds.width <= 0) {
      return;
    }
    const fraction = clamp((event.clientX - bounds.left) / bounds.width, 0, 1);
    setTimelinePreview({
      frameIndex: Math.round(fraction * (referenceTrack.frame_count - 1)),
      leftPercent: fraction * 100,
    });
  };

  const handlePlayerKeyDown = (event: ReactKeyboardEvent<HTMLElement>) => {
    if (isInteractiveTarget(event.target)) {
      return;
    }
    if (
      event.key === " " &&
      event.target instanceof HTMLElement &&
      event.target.closest("button") !== null
    ) {
      return;
    }
    if (event.key === " " || event.key.toLowerCase() === "k") {
      event.preventDefault();
      void togglePlayback();
      return;
    }
    if (event.key === "ArrowLeft" || event.key === ",") {
      event.preventDefault();
      step(-1);
      return;
    }
    if (event.key === "ArrowRight" || event.key === ".") {
      event.preventDefault();
      step(1);
      return;
    }
    if (event.key === "Home") {
      event.preventDefault();
      seekFrame(0);
      return;
    }
    if (event.key === "End") {
      event.preventDefault();
      seekFrame(referenceTrack.frame_count - 1);
    }
  };

  return (
    <section
      aria-describedby="review-keyboard-help"
      aria-label="Synchronized clip player"
      className="review-player"
    >
      <div className={`review-video-grid${tracks.length === 1 ? " single-view" : ""}`}>
        {tracks.map((track, index) => {
          const mappedFrame = nearestImpactFrame(track, frame.time_from_impact_us);
          return (
            <figure
              className={`review-view${
                track.encoded.height > track.encoded.width ? " portrait" : ""
              }`}
              key={track.role}
            >
              <div
                className="review-video-shell"
                style={{
                  aspectRatio: `${String(track.encoded.width)} / ${String(track.encoded.height)}`,
                }}
              >
                {/* Metadata plus the explicit impact-frame seek is sufficient for initial paused
                    review. Avoid downloading whole high-speed clips before Play; it wastes phone
                    and network work and can starve live API requests behind slow Range readers. */}
                <video
                  aria-label={`${roleLabel(track.role)} recorded swing`}
                  crossOrigin="anonymous"
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
                  preload="metadata"
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
            </figure>
          );
        })}
      </div>

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
        <div className="timeline-scrubber" onPointerLeave={() => setTimelinePreview(null)}>
          {timelinePreview === null ? null : (
            <div
              aria-hidden="true"
              className="timeline-thumbnail"
              style={{
                left: `${timelinePreview.leftPercent}%`,
                transform: `translateX(-${timelinePreview.leftPercent}%)`,
              }}
            >
              <video
                crossOrigin="anonymous"
                data-timeline-thumbnail
                muted
                onLoadedMetadata={(event) => {
                  const mediaTimeSeconds = presentationSeekTimeSeconds(
                    referenceTrack,
                    requiredFrame(referenceTrack.frames, timelinePreview.frameIndex),
                  );
                  event.currentTarget.currentTime = mediaTimeSeconds;
                }}
                playsInline
                preload="metadata"
                ref={timelinePreviewVideoRef}
                tabIndex={-1}
              >
                <source
                  src={referenceTrack.media.url ?? referenceTrack.media.path}
                  type={`${referenceTrack.media.mime_type}; codecs="${referenceTrack.media.codec}"`}
                />
              </video>
              <span>
                Frame {timelinePreview.frameIndex + 1} ·{" "}
                {formatImpactTime(requiredFrame(referenceTrack.frames, timelinePreview.frameIndex))}
              </span>
            </div>
          )}
          <label className="sr-only" htmlFor="review-timeline">
            Review timeline
          </label>
          <input
            aria-valuetext={`Frame ${currentFrame + 1}, ${formatImpactTime(frame)}`}
            aria-keyshortcuts="Home End"
            id="review-timeline"
            max={referenceTrack.frame_count - 1}
            min={0}
            onChange={(event) => seekFrame(event.currentTarget.valueAsNumber)}
            onPointerMove={updateTimelinePreview}
            step={1}
            type="range"
            value={currentFrame}
          />
        </div>
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
        <div
          aria-describedby="review-keyboard-help"
          aria-label="Playback controls"
          className="transport-controls"
          onKeyDown={handlePlayerKeyDown}
          role="toolbar"
        >
          <button
            aria-label="Previous frame"
            aria-keyshortcuts="ArrowLeft ,"
            className="transport-button secondary"
            disabled={currentFrame === 0}
            onClick={() => step(-1)}
            type="button"
          >
            <span aria-hidden="true">‹</span>
            <small>1 frame</small>
          </button>
          <button
            aria-keyshortcuts="Space K"
            className="play-button"
            onClick={() => void togglePlayback()}
            type="button"
          >
            <span aria-hidden="true">{playing ? "Ⅱ" : "▶"}</span>
            {playing ? "Pause" : "Play"}
          </button>
          <button
            aria-label="Next frame"
            aria-keyshortcuts="ArrowRight ."
            className="transport-button secondary"
            disabled={currentFrame === referenceTrack.frame_count - 1}
            onClick={() => step(1)}
            type="button"
          >
            <small>1 frame</small>
            <span aria-hidden="true">›</span>
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
          <button
            className="review-details-button secondary"
            onClick={() => setReviewDetailsOpen(true)}
            ref={reviewDetailsButtonRef}
            type="button"
          >
            Review details
          </button>
        </div>
        <p className="keyboard-help" id="review-keyboard-help">
          Keyboard: Space or K play/pause · ←/→ or ,/. step one frame · Home/End jump
        </p>
      </div>
      {reviewDetailsOpen ? (
        <ReviewDetailsDialog
          browserTiming={browserTiming}
          closeButtonRef={reviewDetailsCloseButtonRef}
          manifest={manifest}
          onClose={() => {
            setReviewDetailsOpen(false);
            reviewDetailsButtonRef.current?.focus();
          }}
          tracks={tracks}
        />
      ) : null}
    </section>
  );
}

function ReviewDetailsDialog({
  manifest,
  tracks,
  browserTiming,
  closeButtonRef,
  onClose,
}: {
  manifest: ClipManifest;
  tracks: readonly ClipTrack[];
  browserTiming: BrowserPipelineTiming | null;
  closeButtonRef: RefObject<HTMLButtonElement | null>;
  onClose: () => void;
}) {
  return (
    <div className="review-details-backdrop">
      <div
        aria-labelledby="review-details-heading"
        aria-modal="true"
        className="review-details-dialog"
        role="dialog"
      >
        <header className="review-details-header">
          <div>
            <span className="section-kicker">Recorded session</span>
            <h2 id="review-details-heading">Review details</h2>
          </div>
          <button
            aria-label="Close review details"
            className="review-details-close secondary"
            onClick={onClose}
            ref={closeButtonRef}
            type="button"
          >
            Close
          </button>
        </header>
        <div className="review-details-content">
          {manifest.hil_evidence === undefined ? null : (
            <>
              <p className="review-hil-context">
                The {manifest.hil_evidence.timeline.pre_impact_step_count} pre-impact and{" "}
                {manifest.hil_evidence.timeline.post_impact_step_count} post-impact LED colors are
                visual timeline context for human playback. The automated optical gate checks only
                the white impact marker.
              </p>
              <div className="review-details-hil-evidence">
                {tracks.map((track) => (
                  <HilEvidencePanel key={track.role} manifest={manifest} track={track} />
                ))}
              </div>
            </>
          )}
          {manifest.pipeline_profile === undefined ? null : (
            <PipelineProfilePanel
              browserTiming={browserTiming}
              profile={manifest.pipeline_profile}
            />
          )}
          {manifest.hil_evidence === undefined && manifest.pipeline_profile === undefined ? (
            <p className="review-details-empty">No additional capture details are available.</p>
          ) : null}
        </div>
      </div>
    </div>
  );
}

function HilEvidencePanel({ manifest, track }: { manifest: ClipManifest; track: ClipTrack }) {
  const hilEvidence = manifest.hil_evidence;
  if (hilEvidence === undefined) {
    return null;
  }
  const whiteImpact = hilEvidence.optical_white_impact[track.role];
  const scheduleAlignment = hilEvidence.camera_schedule_alignment[track.role];
  const whiteImpactFrame = requiredFrame(
    track.frames,
    hilEvidence.optical_white_impact_frame_index[track.role],
  );
  return (
    <section
      aria-label={`${roleLabel(track.role)} synthetic HIL evidence`}
      className="review-hil-evidence"
    >
      <span>
        Automated white-impact check · {whiteImpact.passed ? "passed" : "failed"}
        {" · frame "}
        {hilEvidence.optical_white_impact_frame_index[track.role] + 1} · source ID{" "}
        {whiteImpactFrame.frame_id}
      </span>
      <span>
        White match · {whiteImpact.matching_frame_count} of {whiteImpact.stable_frame_count} stable
        frames ({(whiteImpact.matching_fraction * 100).toFixed(0)}%)
      </span>
      <span>
        Signal {formatDiagnostic(whiteImpact.mean_signal_delta)} · color distance{" "}
        {formatDiagnostic(whiteImpact.mean_expected_color_distance)} · saturation{" "}
        {formatPercent(whiteImpact.maximum_saturated_fraction)} · bloom{" "}
        {formatPercent(whiteImpact.maximum_bloom_fraction)}
      </span>
      <span>
        Camera profile · {formatDiagnostic(whiteImpact.exposure_us)} µs ·{" "}
        {formatDiagnostic(whiteImpact.gain_db)} dB gain
      </span>
      <span>
        Schedule mapping · {formatSignedMicroseconds(scheduleAlignment.mapped_time_correction_us)}{" "}
        correction · ±{formatUnsignedMicroseconds(scheduleAlignment.uncertainty_us)} uncertainty
      </span>
      <span>
        Audio trigger estimate relative to white ·{" "}
        {formatSignedMicroseconds(hilEvidence.audio_trigger_estimate_offset_us[track.role])}{" "}
        (uncalibrated)
      </span>
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
  const targetPresentations = manifest.views.map((track) => presentations[track.role]);
  if (delivery === undefined || targetPresentations.some((value) => value === undefined)) {
    return null;
  }
  const completed = targetPresentations as ImpactFramePresentation[];
  const bothPresented = Math.max(...completed.map((value) => value.performance_ms));
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
    presentation_method:
      new Set(completed.map((value) => value.method)).size === 1
        ? requiredPresentation(completed, 0).method
        : "mixed",
    impact_frame_media_time_seconds: Object.fromEntries(
      manifest.views.map((track, index) => [
        track.role,
        requiredPresentation(completed, index).media_time_seconds,
      ]),
    ),
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

function orderedTracks(tracks: ClipTrack[]): [ClipTrack] | [ClipTrack, ClipTrack] {
  if (tracks.length < 1 || tracks.length > 2) {
    throw new Error("A review clip requires one or two camera roles");
  }
  const sorted = [...tracks].sort((left, right) => {
    if (left.role === right.role) {
      return 0;
    }
    return left.role === "down_the_line" ? -1 : 1;
  });
  const first = sorted[0];
  if (first === undefined) {
    throw new Error("A review clip requires at least one camera role");
  }
  const second = sorted[1];
  return second === undefined ? [first] : [first, second];
}

function requiredPresentation(
  presentations: ImpactFramePresentation[],
  index: number,
): ImpactFramePresentation {
  const presentation = presentations[index];
  if (presentation === undefined) {
    throw new Error(`Presentation ${String(index)} is unavailable`);
  }
  return presentation;
}

function nearestImpactFrame(track: ClipTrack, timeFromImpactUs: number): ClipFrame {
  return nearestFrame(track.frames, timeFromImpactUs, (frame) => frame.time_from_impact_us);
}

function presentationSeekTimeSeconds(track: ClipTrack, frame: ClipFrame): number {
  const frameDurationUs = presentationIntervalUs(track, frame);
  // Seeking exactly to an MP4 sample timestamp is a boundary operation. Chromium can resolve that
  // boundary to the preceding decoded sample, especially immediately after a GOP's IDR. A point
  // inside the selected sample's interval makes the desired frame unambiguous while the retained
  // frame timestamp remains the synchronization and labeling authority.
  return (frame.media_time_us + Math.max(1, Math.floor(frameDurationUs / 2))) / 1_000_000;
}

function presentationRecoverySeekTimeSeconds(track: ClipTrack, frame: ClipFrame): number {
  const frameDurationUs = presentationIntervalUs(track, frame);
  const offsetUs = Math.min(
    frameDurationUs - 1,
    Math.max(1, Math.floor((3 * frameDurationUs) / 4)),
  );
  return (frame.media_time_us + offsetUs) / 1_000_000;
}

function presentationIntervalUs(track: ClipTrack, frame: ClipFrame): number {
  const next = track.frames[frame.frame_index + 1];
  const frameDurationUs =
    next === undefined
      ? Math.round(1_000_000 / track.nominal_fps)
      : next.media_time_us - frame.media_time_us;
  if (!Number.isSafeInteger(frameDurationUs) || frameDurationUs <= 1) {
    throw new Error(`Frame ${String(frame.frame_index)} has no usable presentation interval`);
  }
  return frameDurationUs;
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

function isInteractiveTarget(target: EventTarget): boolean {
  return (
    target instanceof HTMLElement &&
    (target.isContentEditable ||
      target.closest("input, select, textarea, a[href], summary") !== null)
  );
}

function errorMessage(caught: unknown): string {
  return caught instanceof Error ? caught.message : "Unknown playback error";
}

function highResolutionNow(): number {
  return globalThis.performance?.now() ?? Date.now();
}
