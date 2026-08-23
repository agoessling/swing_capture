package com.agoessling.swingcapture.trigger;

import com.agoessling.swingcapture.pose.PoseTriggerController;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;

/**
 * Deterministic replay of the leader-side pose-arm and audio capture lifecycle.
 *
 * <p>The simulator deliberately has no Android, Camera2, codec, network, or audio-runtime
 * dependency. It uses the production {@link PoseTriggerController} for standby decisions and for
 * the absolute active-evidence and thermal deadlines. The peer side is represented only by a
 * scripted readiness outcome; this is not a two-controller or network simulation.
 */
public final class CombinedTriggerLifecycleSimulator {
  private static final long NEVER = -1;

  public enum Mode {
    STANDBY,
    CAPTURE_ACTIVE,
    COMPLETING,
    CAMERA_RESTARTING,
    FAILED
  }

  public enum AttemptOutcome {
    AUDIO_TERMINAL,
    NO_IMPACT_TIMEOUT,
    THERMAL_HARD_CAP,
    LOCAL_STARTUP_FAILED,
    RECORDING_ENDED
  }

  public enum FailureReason {
    LOCAL_STARTUP_TIMEOUT,
    THERMAL_HARD_CAP
  }

  public enum PeerArmResolution {
    READY,
    FAILED,
    UNCONFIRMED,
    PENDING
  }

  public enum AudioDisposition {
    TERMINATED_CAPTURE,
    IGNORED_STANDBY,
    IGNORED_NOT_READY,
    IGNORED_COMPLETING,
    IGNORED_CAMERA_RESTARTING,
    IGNORED_FAILED
  }

  public enum TargetOutcome {
    CAPTURED,
    VIDEO_STARTED_AFTER_TAKEAWAY,
    AUDIO_TRIGGER_NOT_READY,
    AUDIO_TERMINAL_MISSING,
    STANDBY_NOT_ARMED,
    PREVIOUS_ATTEMPT_COMPLETING,
    CAMERA_RESTARTING,
    SESSION_FAILED
  }

  /** A negative delay means that the corresponding milestone never occurs. */
  public record StartupPlan(
      long videoStartedDelayNs,
      long localReadyDelayNs,
      long peerResolutionDelayNs,
      PeerArmResolution peerResolution) {
    public StartupPlan {
      requireDelayOrNever(videoStartedDelayNs, "videoStartedDelayNs");
      requireDelayOrNever(localReadyDelayNs, "localReadyDelayNs");
      requireDelayOrNever(peerResolutionDelayNs, "peerResolutionDelayNs");
      Objects.requireNonNull(peerResolution, "peerResolution");
      if (localReadyDelayNs != NEVER
          && (videoStartedDelayNs == NEVER || localReadyDelayNs < videoStartedDelayNs)) {
        throw new IllegalArgumentException("local readiness cannot precede video startup");
      }
      if ((peerResolutionDelayNs == NEVER) != (peerResolution == PeerArmResolution.PENDING)) {
        throw new IllegalArgumentException(
            "a pending peer must have no resolution milestone, and vice versa");
      }
    }

    /** Shorthand for a paired READY response that arrives with local readiness. */
    public StartupPlan(long videoStartedDelayNs, long localReadyDelayNs) {
      this(
          videoStartedDelayNs,
          localReadyDelayNs,
          localReadyDelayNs,
          PeerArmResolution.READY);
    }

    public static StartupPlan neverReady(long videoStartedDelayNs) {
      return new StartupPlan(
          videoStartedDelayNs,
          NEVER,
          videoStartedDelayNs,
          PeerArmResolution.READY);
    }

    private static void requireDelayOrNever(long value, String name) {
      if (value < NEVER) {
        throw new IllegalArgumentException(name + " must be nonnegative or -1");
      }
    }
  }

  public record Config(
      PoseTriggerController.Config poseConfig,
      StartupPlan defaultStartup,
      boolean peerReadinessRequired,
      long localStartupTimeoutNs,
      long postAudioCompletionNs,
      long timeoutCompletionNs,
      long cameraRestartNs) {
    public Config {
      Objects.requireNonNull(poseConfig, "poseConfig");
      Objects.requireNonNull(defaultStartup, "defaultStartup");
      requirePositive(localStartupTimeoutNs, "localStartupTimeoutNs");
      requireNonnegative(postAudioCompletionNs, "postAudioCompletionNs");
      requireNonnegative(timeoutCompletionNs, "timeoutCompletionNs");
      requireNonnegative(cameraRestartNs, "cameraRestartNs");
    }

    /** Production policy with the measured Pixel 6 startup sample as replay input. */
    public static Config pairedPixel6Reference() {
      return new Config(
          PoseTriggerController.Config.defaultsForFiveFramesPerSecond(),
          new StartupPlan(1_066_450_000L, 3_677_558_000L),
          true,
          6_000_000_000L,
          1_000_000_000L,
          1_000_000_000L,
          800_000_000L);
    }

    private static void requirePositive(long value, String name) {
      if (value <= 0) {
        throw new IllegalArgumentException(name + " must be positive");
      }
    }

    private static void requireNonnegative(long value, String name) {
      if (value < 0) {
        throw new IllegalArgumentException(name + " cannot be negative");
      }
    }
  }

  public sealed interface Event permits PoseEvent, AudioCandidateEvent, TargetEvent {
    long timestampNs();
  }

  public record PoseEvent(
      long timestampNs,
      double personConfidence,
      double addressConfidence,
      double motionMagnitude)
      implements Event {
    public PoseEvent {
      // The production record supplies the range validation.
      new PoseTriggerController.Observation(
          timestampNs, personConfidence, addressConfidence, motionMagnitude, false);
    }

    PoseTriggerController.Observation observation() {
      return new PoseTriggerController.Observation(
          timestampNs, personConfidence, addressConfidence, motionMagnitude, false);
    }
  }

  public record AudioCandidateEvent(long timestampNs, String candidateId) implements Event {
    public AudioCandidateEvent {
      requireTimestamp(timestampNs);
      if (candidateId == null || candidateId.isBlank()) {
        throw new IllegalArgumentException("candidateId cannot be blank");
      }
    }
  }

  public record TargetEvent(String id, long takeawayNs, long impactNs) implements Event {
    public TargetEvent {
      if (id == null || id.isBlank()) {
        throw new IllegalArgumentException("target id cannot be blank");
      }
      if (takeawayNs < 0 || impactNs <= takeawayNs) {
        throw new IllegalArgumentException("target timing is invalid");
      }
    }

    @Override
    public long timestampNs() {
      return impactNs;
    }
  }

  public record Scenario(
      List<Event> events, Map<Integer, StartupPlan> startupOverrides, long recordingEndNs) {
    public Scenario {
      events = List.copyOf(Objects.requireNonNull(events, "events"));
      startupOverrides = Map.copyOf(Objects.requireNonNull(startupOverrides, "startupOverrides"));
      requireTimestamp(recordingEndNs);
      if (events.isEmpty()) {
        throw new IllegalArgumentException("scenario requires events");
      }
      long latestEventNs = events.stream().mapToLong(Event::timestampNs).max().orElseThrow();
      if (recordingEndNs < latestEventNs) {
        throw new IllegalArgumentException("recordingEndNs precedes an event");
      }
      for (Map.Entry<Integer, StartupPlan> entry : startupOverrides.entrySet()) {
        if (entry.getKey() < 0 || entry.getValue() == null) {
          throw new IllegalArgumentException("startup override is invalid");
        }
      }
    }
  }

  public record Attempt(
      int index,
      long armNs,
      long videoStartedNs,
      long localReadyNs,
      long peerResolvedNs,
      PeerArmResolution peerResolution,
      long triggerReadyNs,
      long activeEvidenceStopNs,
      long thermalHardStopNs,
      long terminalNs,
      long completionNs,
      AttemptOutcome outcome,
      String terminalCandidateId) {
    public Attempt {
      if (index < 0 || armNs < 0 || terminalNs < armNs || completionNs < terminalNs) {
        throw new IllegalArgumentException("attempt timing is invalid");
      }
      requireMilestoneAfterArmOrNever(videoStartedNs, armNs, "videoStartedNs");
      requireMilestoneAfterArmOrNever(localReadyNs, armNs, "localReadyNs");
      requireMilestoneAfterArmOrNever(peerResolvedNs, armNs, "peerResolvedNs");
      requireMilestoneAfterArmOrNever(triggerReadyNs, armNs, "triggerReadyNs");
      requireMilestoneAfterArmOrNever(activeEvidenceStopNs, armNs, "activeEvidenceStopNs");
      requireMilestoneAfterArmOrNever(thermalHardStopNs, armNs, "thermalHardStopNs");
      Objects.requireNonNull(peerResolution, "peerResolution");
      Objects.requireNonNull(outcome, "outcome");
      Objects.requireNonNull(terminalCandidateId, "terminalCandidateId");
      if ((outcome == AttemptOutcome.AUDIO_TERMINAL) != !terminalCandidateId.isEmpty()) {
        throw new IllegalArgumentException("only an audio terminal may retain a candidate id");
      }
    }

    public long highSpeedDurationNs() {
      return completionNs - armNs;
    }
  }

  public record Failure(long timestampNs, FailureReason reason, int attemptIndex) {
    public Failure {
      requireTimestamp(timestampNs);
      Objects.requireNonNull(reason, "reason");
      if (attemptIndex < 0) {
        throw new IllegalArgumentException("attemptIndex cannot be negative");
      }
    }
  }

  public record AudioCandidateResult(
      long timestampNs, String candidateId, AudioDisposition disposition, int attemptIndex) {
    public AudioCandidateResult {
      requireTimestamp(timestampNs);
      if (candidateId == null || candidateId.isBlank()) {
        throw new IllegalArgumentException("candidateId cannot be blank");
      }
      Objects.requireNonNull(disposition, "disposition");
      if (attemptIndex < -1) {
        throw new IllegalArgumentException("attemptIndex cannot be below -1");
      }
    }
  }

  public record TargetResult(
      String id,
      long takeawayNs,
      long impactNs,
      TargetOutcome outcome,
      int attemptIndex,
      long videoLeadBeforeTakeawayNs,
      long triggerReadyLeadBeforeImpactNs) {
    public TargetResult {
      if (id == null || id.isBlank() || takeawayNs < 0 || impactNs <= takeawayNs) {
        throw new IllegalArgumentException("target result is invalid");
      }
      Objects.requireNonNull(outcome, "outcome");
      if (attemptIndex < -1) {
        throw new IllegalArgumentException("attemptIndex cannot be below -1");
      }
    }

    public boolean captured() {
      return outcome == TargetOutcome.CAPTURED;
    }
  }

  public record Result(
      List<Attempt> attempts,
      List<AudioCandidateResult> audioCandidates,
      List<TargetResult> targets,
      int processedPoseObservations,
      int skippedPoseObservations,
      long totalHighSpeedNs,
      Optional<Failure> failure) {
    public Result {
      attempts = List.copyOf(Objects.requireNonNull(attempts, "attempts"));
      audioCandidates =
          List.copyOf(Objects.requireNonNull(audioCandidates, "audioCandidates"));
      targets = List.copyOf(Objects.requireNonNull(targets, "targets"));
      Objects.requireNonNull(failure, "failure");
      if (processedPoseObservations < 0 || skippedPoseObservations < 0 || totalHighSpeedNs < 0) {
        throw new IllegalArgumentException("aggregate metrics cannot be negative");
      }
    }

    public long capturedTargetCount() {
      return targets.stream().filter(TargetResult::captured).count();
    }

    public long falseAudioTerminalCount() {
      Set<String> targetIds = new HashSet<>();
      for (TargetResult target : targets) {
        targetIds.add(target.id());
      }
      return attempts.stream()
          .filter(attempt -> attempt.outcome() == AttemptOutcome.AUDIO_TERMINAL)
          .filter(attempt -> !targetIds.contains(attempt.terminalCandidateId()))
          .count();
    }
  }

  private record TargetSnapshot(TargetEvent target, Mode mode, ActiveAttempt attempt) {}

  private static final class ActiveAttempt {
    private final int index;
    private final long armNs;
    private final long videoStartedNs;
    private final long localReadyNs;
    private final long peerResolvedNs;
    private final PeerArmResolution peerResolution;
    private final long triggerReadyNs;
    private final long activeEvidenceStopNs;
    private final long thermalNs;
    private long terminalNs = NEVER;
    private long completionNs = NEVER;
    private AttemptOutcome outcome;
    private String terminalCandidateId = "";

    private ActiveAttempt(
        int index,
        PoseTriggerController.Decision armDecision,
        StartupPlan startup,
        Config config) {
      this.index = index;
      long decisionArmNs = armDecision.armRequestedNs().orElseThrow();
      armNs = decisionArmNs;
      videoStartedNs = milestone(decisionArmNs, startup.videoStartedDelayNs());
      localReadyNs = milestone(decisionArmNs, startup.localReadyDelayNs());
      peerResolvedNs = milestone(decisionArmNs, startup.peerResolutionDelayNs());
      peerResolution = startup.peerResolution();
      triggerReadyNs = jointTriggerReadyNs(config.peerReadinessRequired());
      activeEvidenceStopNs = armDecision.activeUntilNs().orElseThrow();
      thermalNs = armDecision.thermalHardStopNs().orElseThrow();
    }

    private Attempt finalizedAttempt() {
      return new Attempt(
          index,
          armNs,
          videoStartedNs,
          localReadyNs,
          peerResolvedNs,
          peerResolution,
          triggerReadyNs,
          activeEvidenceStopNs,
          thermalNs,
          terminalNs,
          completionNs,
          outcome,
          terminalCandidateId);
    }

    private long jointTriggerReadyNs(boolean peerReadinessRequired) {
      if (localReadyNs == NEVER) {
        return NEVER;
      }
      if (!peerReadinessRequired) {
        return localReadyNs;
      }
      return peerResolvedNs == NEVER ? NEVER : Math.max(localReadyNs, peerResolvedNs);
    }

    private long activeEvidenceTerminalNs(boolean peerReadinessRequired) {
      if (localReadyNs == NEVER) {
        return NEVER;
      }
      if (!peerReadinessRequired) {
        return Math.max(activeEvidenceStopNs, localReadyNs);
      }
      if (peerResolvedNs == NEVER || peerResolution == PeerArmResolution.UNCONFIRMED) {
        return NEVER;
      }
      return Math.max(activeEvidenceStopNs, Math.max(localReadyNs, peerResolvedNs));
    }

    private long startupFailureNs(long timeoutNs) {
      long timeout = saturatedAdd(armNs, timeoutNs);
      return localReadyNs == NEVER || localReadyNs > timeout ? timeout : NEVER;
    }
  }

  private final Config config;
  private final Scenario scenario;
  private final PoseTriggerController controller;
  private final ArrayList<Attempt> attempts = new ArrayList<>();
  private final ArrayList<AudioCandidateResult> audioResults = new ArrayList<>();
  private final ArrayList<TargetSnapshot> targetSnapshots = new ArrayList<>();
  private ActiveAttempt activeAttempt;
  private Mode mode = Mode.STANDBY;
  private long restartCompleteNs = Long.MAX_VALUE;
  private int attemptCount;
  private int processedPoseObservations;
  private int skippedPoseObservations;
  private Failure failure;

  private CombinedTriggerLifecycleSimulator(Config config, Scenario scenario) {
    this.config = config;
    this.scenario = scenario;
    controller = new PoseTriggerController(config.poseConfig());
  }

  public static Result evaluate(Scenario scenario, Config config) {
    Objects.requireNonNull(scenario, "scenario");
    Objects.requireNonNull(config, "config");
    return new CombinedTriggerLifecycleSimulator(config, scenario).evaluate();
  }

  private Result evaluate() {
    ArrayList<Event> orderedEvents = new ArrayList<>(scenario.events());
    orderedEvents.sort(
        Comparator.comparingLong(Event::timestampNs)
            .thenComparingInt(CombinedTriggerLifecycleSimulator::eventPriority));
    validateTargets(orderedEvents);

    for (Event event : orderedEvents) {
      advanceTo(event.timestampNs());
      if (event instanceof TargetEvent target) {
        targetSnapshots.add(new TargetSnapshot(target, mode, activeAttempt));
      } else if (event instanceof AudioCandidateEvent candidate) {
        acceptAudioCandidate(candidate);
      } else if (event instanceof PoseEvent pose) {
        acceptPose(pose);
      } else {
        throw new IllegalStateException("unknown event type: " + event.getClass());
      }
    }
    advanceTo(scenario.recordingEndNs());
    closeAtRecordingEnd(scenario.recordingEndNs());
    for (int attemptIndex : scenario.startupOverrides().keySet()) {
      if (attemptIndex >= attemptCount) {
        throw new IllegalArgumentException(
            "startup override does not identify an observed attempt: " + attemptIndex);
      }
    }

    ArrayList<TargetResult> targetResults = buildTargetResults();
    long totalHighSpeedNs =
        attempts.stream().mapToLong(Attempt::highSpeedDurationNs).reduce(0L, Math::addExact);
    return new Result(
        attempts,
        audioResults,
        targetResults,
        processedPoseObservations,
        skippedPoseObservations,
        totalHighSpeedNs,
        Optional.ofNullable(failure));
  }

  private void advanceTo(long timestampNs) {
    while (true) {
      if (mode == Mode.CAPTURE_ACTIVE) {
        long startupFailureNs = activeAttempt.startupFailureNs(config.localStartupTimeoutNs());
        long noImpactNs =
            activeAttempt.activeEvidenceTerminalNs(config.peerReadinessRequired());
        long deadlineNs = minimumMilestone(startupFailureNs, noImpactNs, activeAttempt.thermalNs);
        if (deadlineNs == NEVER || deadlineNs > timestampNs) {
          return;
        }
        if (deadlineNs == startupFailureNs) {
          failAttempt(
              deadlineNs,
              AttemptOutcome.LOCAL_STARTUP_FAILED,
              FailureReason.LOCAL_STARTUP_TIMEOUT);
          continue;
        }
        if (deadlineNs == activeAttempt.thermalNs) {
          failAttempt(
              deadlineNs,
              AttemptOutcome.THERMAL_HARD_CAP,
              FailureReason.THERMAL_HARD_CAP);
          continue;
        }
        beginCompletion(deadlineNs, AttemptOutcome.NO_IMPACT_TIMEOUT, "", config.timeoutCompletionNs());
        continue;
      }
      if (mode == Mode.COMPLETING) {
        if (activeAttempt.completionNs > timestampNs) {
          return;
        }
        controller.captureEnded(activeAttempt.completionNs);
        attempts.add(activeAttempt.finalizedAttempt());
        restartCompleteNs = saturatedAdd(activeAttempt.completionNs, config.cameraRestartNs());
        mode = config.cameraRestartNs() == 0 ? Mode.STANDBY : Mode.CAMERA_RESTARTING;
        activeAttempt = null;
        continue;
      }
      if (mode == Mode.CAMERA_RESTARTING) {
        if (restartCompleteNs > timestampNs) {
          return;
        }
        restartCompleteNs = Long.MAX_VALUE;
        mode = Mode.STANDBY;
        continue;
      }
      return;
    }
  }

  private void acceptAudioCandidate(AudioCandidateEvent candidate) {
    switch (mode) {
      case STANDBY ->
          audioResults.add(
              new AudioCandidateResult(
                  candidate.timestampNs(),
                  candidate.candidateId(),
                  AudioDisposition.IGNORED_STANDBY,
                  -1));
      case CAMERA_RESTARTING ->
          audioResults.add(
              new AudioCandidateResult(
                  candidate.timestampNs(),
                  candidate.candidateId(),
                  AudioDisposition.IGNORED_CAMERA_RESTARTING,
                  -1));
      case FAILED ->
          audioResults.add(
              new AudioCandidateResult(
                  candidate.timestampNs(),
                  candidate.candidateId(),
                  AudioDisposition.IGNORED_FAILED,
                  failure.attemptIndex()));
      case COMPLETING ->
          audioResults.add(
              new AudioCandidateResult(
                  candidate.timestampNs(),
                  candidate.candidateId(),
                  AudioDisposition.IGNORED_COMPLETING,
                  activeAttempt.index));
      case CAPTURE_ACTIVE -> {
        if (activeAttempt.triggerReadyNs == NEVER
            || candidate.timestampNs() < activeAttempt.triggerReadyNs) {
          audioResults.add(
              new AudioCandidateResult(
                  candidate.timestampNs(),
                  candidate.candidateId(),
                  AudioDisposition.IGNORED_NOT_READY,
                  activeAttempt.index));
          return;
        }
        audioResults.add(
            new AudioCandidateResult(
                candidate.timestampNs(),
                candidate.candidateId(),
                AudioDisposition.TERMINATED_CAPTURE,
                activeAttempt.index));
        beginCompletion(
            candidate.timestampNs(),
            AttemptOutcome.AUDIO_TERMINAL,
            candidate.candidateId(),
            config.postAudioCompletionNs());
      }
    }
  }

  private void acceptPose(PoseEvent pose) {
    if (mode != Mode.STANDBY) {
      ++skippedPoseObservations;
      return;
    }
    PoseTriggerController.Decision decision = controller.observe(pose.observation());
    ++processedPoseObservations;
    if (decision.command() != PoseTriggerController.Command.START_HIGH_SPEED) {
      return;
    }
    int attemptIndex = attemptCount++;
    StartupPlan startup =
        scenario.startupOverrides().getOrDefault(attemptIndex, config.defaultStartup());
    activeAttempt = new ActiveAttempt(attemptIndex, decision, startup, config);
    mode = Mode.CAPTURE_ACTIVE;
  }

  private void beginCompletion(
      long terminalNs, AttemptOutcome outcome, String candidateId, long completionDelayNs) {
    activeAttempt.terminalNs = terminalNs;
    activeAttempt.completionNs = saturatedAdd(terminalNs, completionDelayNs);
    activeAttempt.outcome = outcome;
    activeAttempt.terminalCandidateId = candidateId;
    mode = Mode.COMPLETING;
  }

  private void failAttempt(
      long terminalNs, AttemptOutcome outcome, FailureReason failureReason) {
    activeAttempt.terminalNs = terminalNs;
    activeAttempt.completionNs = terminalNs;
    activeAttempt.outcome = outcome;
    attempts.add(activeAttempt.finalizedAttempt());
    failure = new Failure(terminalNs, failureReason, activeAttempt.index);
    activeAttempt = null;
    mode = Mode.FAILED;
  }

  private void closeAtRecordingEnd(long recordingEndNs) {
    if (mode == Mode.CAPTURE_ACTIVE) {
      activeAttempt.terminalNs = recordingEndNs;
      activeAttempt.completionNs = recordingEndNs;
      activeAttempt.outcome = AttemptOutcome.RECORDING_ENDED;
      attempts.add(activeAttempt.finalizedAttempt());
      activeAttempt = null;
      mode = Mode.STANDBY;
    } else if (mode == Mode.COMPLETING) {
      // The terminal has already fixed the production completion deadline. The end of fixture
      // input must not shorten that post-roll or relabel the terminal as recording-ended.
      attempts.add(activeAttempt.finalizedAttempt());
      activeAttempt = null;
      mode = Mode.STANDBY;
    }
  }

  private ArrayList<TargetResult> buildTargetResults() {
    Map<String, Attempt> terminalAttempts = new HashMap<>();
    Map<String, AudioCandidateResult> candidateResults = new HashMap<>();
    for (Attempt attempt : attempts) {
      if (attempt.outcome() == AttemptOutcome.AUDIO_TERMINAL) {
        terminalAttempts.putIfAbsent(attempt.terminalCandidateId(), attempt);
      }
    }
    for (AudioCandidateResult candidate : audioResults) {
      candidateResults.putIfAbsent(candidate.candidateId(), candidate);
    }

    ArrayList<TargetResult> results = new ArrayList<>();
    for (TargetSnapshot snapshot : targetSnapshots) {
      TargetEvent target = snapshot.target();
      Attempt terminalAttempt = terminalAttempts.get(target.id());
      if (terminalAttempt != null
          && snapshot.attempt() != null
          && terminalAttempt.index() == snapshot.attempt().index) {
        long videoLeadNs = lead(target.takeawayNs(), terminalAttempt.videoStartedNs());
        long triggerLeadNs = lead(target.impactNs(), terminalAttempt.triggerReadyNs());
        TargetOutcome outcome =
            triggerLeadNs < 0
                ? TargetOutcome.AUDIO_TRIGGER_NOT_READY
                : videoLeadNs >= 0
                    ? TargetOutcome.CAPTURED
                    : TargetOutcome.VIDEO_STARTED_AFTER_TAKEAWAY;
        results.add(
            new TargetResult(
                target.id(),
                target.takeawayNs(),
                target.impactNs(),
                outcome,
                terminalAttempt.index(),
                videoLeadNs,
                triggerLeadNs));
        continue;
      }

      AudioCandidateResult candidate = candidateResults.get(target.id());
      if (candidate != null && candidate.disposition() == AudioDisposition.IGNORED_NOT_READY) {
        ActiveAttempt attempt = snapshot.attempt();
        results.add(
            new TargetResult(
                target.id(),
                target.takeawayNs(),
                target.impactNs(),
                TargetOutcome.AUDIO_TRIGGER_NOT_READY,
                candidate.attemptIndex(),
                attempt == null ? Long.MIN_VALUE : lead(target.takeawayNs(), attempt.videoStartedNs),
                attempt == null ? Long.MIN_VALUE : lead(target.impactNs(), attempt.triggerReadyNs)));
        continue;
      }

      results.add(missedTarget(snapshot));
    }
    return results;
  }

  private static TargetResult missedTarget(TargetSnapshot snapshot) {
    TargetEvent target = snapshot.target();
    ActiveAttempt attempt = snapshot.attempt();
    TargetOutcome outcome =
        switch (snapshot.mode()) {
          case STANDBY -> TargetOutcome.STANDBY_NOT_ARMED;
          case COMPLETING -> TargetOutcome.PREVIOUS_ATTEMPT_COMPLETING;
          case CAMERA_RESTARTING -> TargetOutcome.CAMERA_RESTARTING;
          case FAILED -> TargetOutcome.SESSION_FAILED;
          case CAPTURE_ACTIVE ->
              attempt.triggerReadyNs == NEVER || attempt.triggerReadyNs > target.impactNs()
                  ? TargetOutcome.AUDIO_TRIGGER_NOT_READY
                  : TargetOutcome.AUDIO_TERMINAL_MISSING;
        };
    return new TargetResult(
        target.id(),
        target.takeawayNs(),
        target.impactNs(),
        outcome,
        attempt == null ? -1 : attempt.index,
        attempt == null ? Long.MIN_VALUE : lead(target.takeawayNs(), attempt.videoStartedNs),
        attempt == null ? Long.MIN_VALUE : lead(target.impactNs(), attempt.triggerReadyNs));
  }

  private static void validateTargets(List<Event> events) {
    HashSet<String> ids = new HashSet<>();
    long previousImpactNs = -1;
    long previousPoseNs = -1;
    for (Event event : events) {
      if (event instanceof TargetEvent target) {
        if (!ids.add(target.id())) {
          throw new IllegalArgumentException("duplicate target id: " + target.id());
        }
        if (target.impactNs() <= previousImpactNs) {
          throw new IllegalArgumentException("target impacts must increase");
        }
        previousImpactNs = target.impactNs();
      } else if (event instanceof PoseEvent pose) {
        if (pose.timestampNs() <= previousPoseNs) {
          throw new IllegalArgumentException("pose timestamps must increase");
        }
        previousPoseNs = pose.timestampNs();
      }
    }
  }

  private static int eventPriority(Event event) {
    if (event instanceof TargetEvent) {
      return 0;
    }
    if (event instanceof AudioCandidateEvent) {
      return 1;
    }
    return 2;
  }

  private static long milestone(long armNs, long delayNs) {
    return delayNs == NEVER ? NEVER : saturatedAdd(armNs, delayNs);
  }

  private static long minimumMilestone(long first, long second, long third) {
    long result = Long.MAX_VALUE;
    if (first != NEVER) {
      result = Math.min(result, first);
    }
    if (second != NEVER) {
      result = Math.min(result, second);
    }
    if (third != NEVER) {
      result = Math.min(result, third);
    }
    return result == Long.MAX_VALUE ? NEVER : result;
  }

  private static long lead(long eventNs, long milestoneNs) {
    return milestoneNs == NEVER ? Long.MIN_VALUE : Math.subtractExact(eventNs, milestoneNs);
  }

  private static void requireMilestoneAfterArmOrNever(long milestoneNs, long armNs, String name) {
    if (milestoneNs != NEVER && milestoneNs < armNs) {
      throw new IllegalArgumentException(name + " precedes the arm");
    }
  }

  private static void requireTimestamp(long timestampNs) {
    if (timestampNs < 0) {
      throw new IllegalArgumentException("timestamp cannot be negative");
    }
  }

  private static long saturatedAdd(long value, long increment) {
    return value > Long.MAX_VALUE - increment ? Long.MAX_VALUE : value + increment;
  }
}
