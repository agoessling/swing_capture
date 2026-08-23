package com.agoessling.swingcapture.pose;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/** Replays the production standby/high-speed lifecycle over labeled field observations. */
public final class PoseFieldSessionReplay {
  public enum AttemptOutcome {
    IMPACT_CAPTURED,
    IMPACT_RETAINED_AFTER_TAKEAWAY,
    NO_IMPACT_TIMEOUT,
    RECORDING_ENDED
  }

  public enum ImpactOutcome {
    CAPTURED,
    STANDBY_NOT_ARMED,
    VIDEO_STARTED_AFTER_TAKEAWAY,
    AUDIO_TRIGGER_NOT_READY,
    PREVIOUS_ATTEMPT_COMPLETING,
    CAMERA_RESTARTING
  }

  public record Target(String id, long impactNs, long takeawayNs) {
    public Target {
      if (id == null || id.isBlank()) {
        throw new IllegalArgumentException("target id cannot be blank");
      }
      if (impactNs <= 0 || takeawayNs < 0 || takeawayNs >= impactNs) {
        throw new IllegalArgumentException("target timing is invalid");
      }
    }
  }

  public record Config(
      PoseTriggerController.Config controllerConfig,
      long videoStartupBudgetNs,
      long audioTriggerStartupBudgetNs,
      long postImpactCompletionNs,
      long noImpactCompletionNs,
      long cameraRestartNs) {
    public Config {
      Objects.requireNonNull(controllerConfig, "controllerConfig");
      requireNonnegative(videoStartupBudgetNs, "videoStartupBudgetNs");
      requireNonnegative(audioTriggerStartupBudgetNs, "audioTriggerStartupBudgetNs");
      requireNonnegative(postImpactCompletionNs, "postImpactCompletionNs");
      requireNonnegative(noImpactCompletionNs, "noImpactCompletionNs");
      requireNonnegative(cameraRestartNs, "cameraRestartNs");
    }

    public static Config defaults() {
      return new Config(
          PoseTriggerController.Config.defaultsForFiveFramesPerSecond(),
          800_000_000L,
          2_450_000_000L,
          1_000_000_000L,
          1_000_000_000L,
          800_000_000L);
    }

    private static void requireNonnegative(long value, String name) {
      if (value < 0) {
        throw new IllegalArgumentException(name + " cannot be negative");
      }
    }
  }

  public record Attempt(
      long armNs,
      long videoReadyNs,
      long audioTriggerReadyNs,
      long terminalEventNs,
      long completionNs,
      AttemptOutcome outcome,
      String capturedTargetId) {
    public Attempt {
      Objects.requireNonNull(outcome, "outcome");
      Objects.requireNonNull(capturedTargetId, "capturedTargetId");
      if (armNs < 0
          || videoReadyNs < armNs
          || audioTriggerReadyNs < armNs
          || terminalEventNs < armNs
          || completionNs < terminalEventNs) {
        throw new IllegalArgumentException("attempt timing is invalid");
      }
      boolean impactAttempt =
          outcome == AttemptOutcome.IMPACT_CAPTURED
              || outcome == AttemptOutcome.IMPACT_RETAINED_AFTER_TAKEAWAY;
      if (impactAttempt != !capturedTargetId.isEmpty()) {
        throw new IllegalArgumentException("captured target must agree with attempt outcome");
      }
    }

    public long highSpeedDurationNs() {
      return completionNs - armNs;
    }
  }

  public record TargetResult(
      Target target,
      ImpactOutcome outcome,
      long armNs,
      long videoReadyNs,
      long audioTriggerReadyNs,
      long videoLeadBeforeTakeawayNs,
      long audioReadyLeadBeforeImpactNs) {
    public TargetResult {
      Objects.requireNonNull(target, "target");
      Objects.requireNonNull(outcome, "outcome");
      boolean armedOutcome =
          outcome == ImpactOutcome.CAPTURED
              || outcome == ImpactOutcome.VIDEO_STARTED_AFTER_TAKEAWAY
              || outcome == ImpactOutcome.AUDIO_TRIGGER_NOT_READY;
      if (armedOutcome) {
        if (armNs < 0 || videoReadyNs < armNs || audioTriggerReadyNs < armNs) {
          throw new IllegalArgumentException("armed target requires valid readiness timing");
        }
      } else if (armNs != -1
          || videoReadyNs != -1
          || audioTriggerReadyNs != -1
          || videoLeadBeforeTakeawayNs != Long.MIN_VALUE
          || audioReadyLeadBeforeImpactNs != Long.MIN_VALUE) {
        throw new IllegalArgumentException("unarmed target cannot retain readiness timing");
      }
    }

    public boolean captured() {
      return outcome == ImpactOutcome.CAPTURED;
    }
  }

  public record Result(
      List<Attempt> attempts,
      List<TargetResult> targets,
      long totalHighSpeedNs,
      int skippedPoseObservations) {
    public Result {
      attempts = List.copyOf(Objects.requireNonNull(attempts, "attempts"));
      targets = List.copyOf(Objects.requireNonNull(targets, "targets"));
      if (totalHighSpeedNs < 0 || skippedPoseObservations < 0) {
        throw new IllegalArgumentException("aggregate replay metrics cannot be negative");
      }
    }

    public long capturedTargetCount() {
      return targets.stream().filter(TargetResult::captured).count();
    }
  }

  private enum Mode {
    STANDBY,
    WAITING_FOR_IMPACT,
    COMPLETING,
    RESTARTING
  }

  private PoseFieldSessionReplay() {}

  public static Result evaluate(
      List<PoseTriggerController.Observation> observations,
      List<Target> targets,
      Config config) {
    Objects.requireNonNull(observations, "observations");
    Objects.requireNonNull(targets, "targets");
    Objects.requireNonNull(config, "config");
    validateInputs(observations, targets);

    PoseTriggerController controller = new PoseTriggerController(config.controllerConfig());
    ArrayList<Attempt> attempts = new ArrayList<>();
    ArrayList<TargetResult> targetResults = new ArrayList<>();
    int observationIndex = 0;
    int targetIndex = 0;
    int skippedObservations = 0;
    long totalHighSpeedNs = 0;
    long currentArmNs = -1;
    long currentVideoReadyNs = -1;
    long currentAudioReadyNs = -1;
    long noImpactDeadlineNs = Long.MAX_VALUE;
    long completionNs = Long.MAX_VALUE;
    long restartCompleteNs = Long.MAX_VALUE;
    long terminalEventNs = -1;
    long standbyObservationsAfterNs = -1;
    AttemptOutcome pendingOutcome = null;
    String capturedTargetId = "";
    Mode mode = Mode.STANDBY;

    while (observationIndex < observations.size() || targetIndex < targets.size()) {
      long observationNs =
          observationIndex < observations.size()
              ? observations.get(observationIndex).timestampNs()
              : Long.MAX_VALUE;
      long impactNs =
          targetIndex < targets.size() ? targets.get(targetIndex).impactNs() : Long.MAX_VALUE;

      if (mode == Mode.WAITING_FOR_IMPACT
          && noImpactDeadlineNs <= observationNs
          && noImpactDeadlineNs <= impactNs) {
        terminalEventNs = noImpactDeadlineNs;
        completionNs = saturatedAdd(noImpactDeadlineNs, config.noImpactCompletionNs());
        pendingOutcome = AttemptOutcome.NO_IMPACT_TIMEOUT;
        mode = Mode.COMPLETING;
        continue;
      }
      if (mode == Mode.COMPLETING && completionNs <= observationNs && completionNs <= impactNs) {
        controller.captureEnded(completionNs);
        attempts.add(
            new Attempt(
                currentArmNs,
                currentVideoReadyNs,
                currentAudioReadyNs,
                terminalEventNs,
                completionNs,
                pendingOutcome,
                capturedTargetId));
        totalHighSpeedNs = Math.addExact(totalHighSpeedNs, completionNs - currentArmNs);
        restartCompleteNs = saturatedAdd(completionNs, config.cameraRestartNs());
        standbyObservationsAfterNs = restartCompleteNs;
        mode = config.cameraRestartNs() == 0 ? Mode.STANDBY : Mode.RESTARTING;
        currentArmNs = -1;
        currentVideoReadyNs = -1;
        currentAudioReadyNs = -1;
        noImpactDeadlineNs = Long.MAX_VALUE;
        completionNs = Long.MAX_VALUE;
        terminalEventNs = -1;
        pendingOutcome = null;
        capturedTargetId = "";
        continue;
      }
      if (mode == Mode.RESTARTING
          && restartCompleteNs <= observationNs
          && restartCompleteNs <= impactNs) {
        restartCompleteNs = Long.MAX_VALUE;
        mode = Mode.STANDBY;
        continue;
      }

      if (impactNs <= observationNs) {
        Target target = targets.get(targetIndex++);
        if (mode == Mode.WAITING_FOR_IMPACT) {
          long videoLeadNs = target.takeawayNs() - currentVideoReadyNs;
          long audioLeadNs = target.impactNs() - currentAudioReadyNs;
          if (audioLeadNs >= 0) {
            ImpactOutcome targetOutcome =
                videoLeadNs >= 0
                    ? ImpactOutcome.CAPTURED
                    : ImpactOutcome.VIDEO_STARTED_AFTER_TAKEAWAY;
            targetResults.add(
                new TargetResult(
                    target,
                    targetOutcome,
                    currentArmNs,
                    currentVideoReadyNs,
                    currentAudioReadyNs,
                    videoLeadNs,
                    audioLeadNs));
            terminalEventNs = target.impactNs();
            completionNs = saturatedAdd(target.impactNs(), config.postImpactCompletionNs());
            pendingOutcome =
                targetOutcome == ImpactOutcome.CAPTURED
                    ? AttemptOutcome.IMPACT_CAPTURED
                    : AttemptOutcome.IMPACT_RETAINED_AFTER_TAKEAWAY;
            capturedTargetId = target.id();
            mode = Mode.COMPLETING;
          } else {
            targetResults.add(
                new TargetResult(
                    target,
                    ImpactOutcome.AUDIO_TRIGGER_NOT_READY,
                    currentArmNs,
                    currentVideoReadyNs,
                    currentAudioReadyNs,
                    videoLeadNs,
                    audioLeadNs));
          }
        } else {
          ImpactOutcome outcome =
              switch (mode) {
                case STANDBY -> ImpactOutcome.STANDBY_NOT_ARMED;
                case WAITING_FOR_IMPACT -> ImpactOutcome.AUDIO_TRIGGER_NOT_READY;
                case COMPLETING -> ImpactOutcome.PREVIOUS_ATTEMPT_COMPLETING;
                case RESTARTING -> ImpactOutcome.CAMERA_RESTARTING;
              };
          targetResults.add(missedTarget(target, outcome));
        }
        continue;
      }

      PoseTriggerController.Observation observation = observations.get(observationIndex++);
      if (mode != Mode.STANDBY || observation.timestampNs() <= standbyObservationsAfterNs) {
        ++skippedObservations;
        continue;
      }
      PoseTriggerController.Decision decision = controller.observe(observation);
      if (decision.command() == PoseTriggerController.Command.START_HIGH_SPEED) {
        currentArmNs = observation.timestampNs();
        currentVideoReadyNs = saturatedAdd(currentArmNs, config.videoStartupBudgetNs());
        currentAudioReadyNs =
            saturatedAdd(currentArmNs, config.audioTriggerStartupBudgetNs());
        noImpactDeadlineNs = decision.activeUntilNs().orElseThrow();
        mode = Mode.WAITING_FOR_IMPACT;
      }
    }

    if (mode == Mode.WAITING_FOR_IMPACT || mode == Mode.COMPLETING) {
      long recordingEndNs =
          observations.isEmpty()
              ? targets.get(targets.size() - 1).impactNs()
              : observations.get(observations.size() - 1).timestampNs();
      long endNs = Math.max(currentArmNs, recordingEndNs);
      attempts.add(
          new Attempt(
              currentArmNs,
              currentVideoReadyNs,
              currentAudioReadyNs,
              mode == Mode.COMPLETING ? terminalEventNs : endNs,
              mode == Mode.COMPLETING ? Math.max(terminalEventNs, endNs) : endNs,
              mode == Mode.COMPLETING ? pendingOutcome : AttemptOutcome.RECORDING_ENDED,
              capturedTargetId));
      totalHighSpeedNs = Math.addExact(totalHighSpeedNs, endNs - currentArmNs);
    }
    return new Result(attempts, targetResults, totalHighSpeedNs, skippedObservations);
  }

  private static TargetResult missedTarget(Target target, ImpactOutcome outcome) {
    return new TargetResult(
        target, outcome, -1, -1, -1, Long.MIN_VALUE, Long.MIN_VALUE);
  }

  private static void validateInputs(
      List<PoseTriggerController.Observation> observations, List<Target> targets) {
    if (observations.isEmpty() || targets.isEmpty()) {
      throw new IllegalArgumentException("field replay requires observations and targets");
    }
    long previousObservationNs = -1;
    for (PoseTriggerController.Observation observation : observations) {
      Objects.requireNonNull(observation, "observation");
      if (observation.timestampNs() <= previousObservationNs) {
        throw new IllegalArgumentException("observation timestamps must increase");
      }
      previousObservationNs = observation.timestampNs();
    }
    long previousImpactNs = -1;
    for (Target target : targets) {
      Objects.requireNonNull(target, "target");
      if (target.impactNs() <= previousImpactNs) {
        throw new IllegalArgumentException("target impacts must increase");
      }
      previousImpactNs = target.impactNs();
    }
  }

  private static long saturatedAdd(long value, long increment) {
    return value > Long.MAX_VALUE - increment ? Long.MAX_VALUE : value + increment;
  }
}
