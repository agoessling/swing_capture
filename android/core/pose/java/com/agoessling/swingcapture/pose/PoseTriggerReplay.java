package com.agoessling.swingcapture.pose;

import java.util.List;
import java.util.Objects;
import java.util.OptionalLong;

/** Deterministic scoring of recorded 5 fps pose observations against hand-labeled timing. */
public final class PoseTriggerReplay {
  public record Interval(long startNs, long endNs) {
    public Interval {
      if (startNs < 0 || endNs <= startNs) {
        throw new IllegalArgumentException("interval must be a nonempty nonnegative half-open range");
      }
    }

    public boolean contains(long timestampNs) {
      return timestampNs >= startNs && timestampNs < endNs;
    }
  }

  public record Annotation(
      long safeArmStartNs,
      long preferredArmNs,
      long takeawayNs,
      long highSpeedStartupBudgetNs,
      List<Interval> mustNotArm) {
    public Annotation {
      if (safeArmStartNs < 0 || takeawayNs <= safeArmStartNs) {
        throw new IllegalArgumentException("safe arm start must precede takeaway");
      }
      if (preferredArmNs < safeArmStartNs || preferredArmNs >= takeawayNs) {
        throw new IllegalArgumentException(
            "preferred arm must be within the safe arm-to-takeaway window");
      }
      if (highSpeedStartupBudgetNs < 0) {
        throw new IllegalArgumentException("highSpeedStartupBudgetNs cannot be negative");
      }
      mustNotArm = List.copyOf(Objects.requireNonNull(mustNotArm, "mustNotArm"));
    }

    public Annotation(
        long safeArmStartNs,
        long takeawayNs,
        long highSpeedStartupBudgetNs,
        List<Interval> mustNotArm) {
      this(
          safeArmStartNs,
          safeArmStartNs,
          takeawayNs,
          highSpeedStartupBudgetNs,
          mustNotArm);
    }
  }

  public record Result(
      OptionalLong armRequestNs,
      OptionalLong armOffsetFromPreferredNs,
      OptionalLong highSpeedReadyNs,
      OptionalLong readyLeadBeforeTakeawayNs,
      boolean armedBeforeSafeWindow,
      boolean armedInForbiddenInterval,
      boolean readyByTakeaway,
      boolean passed,
      PoseTriggerController.State finalState) {
    public Result {
      Objects.requireNonNull(armRequestNs, "armRequestNs");
      Objects.requireNonNull(armOffsetFromPreferredNs, "armOffsetFromPreferredNs");
      Objects.requireNonNull(highSpeedReadyNs, "highSpeedReadyNs");
      Objects.requireNonNull(readyLeadBeforeTakeawayNs, "readyLeadBeforeTakeawayNs");
      Objects.requireNonNull(finalState, "finalState");
    }
  }

  private PoseTriggerReplay() {}

  public static Result evaluate(
      PoseTriggerController.Config config,
      List<PoseTriggerController.Observation> observations,
      Annotation annotation) {
    Objects.requireNonNull(config, "config");
    Objects.requireNonNull(observations, "observations");
    Objects.requireNonNull(annotation, "annotation");

    PoseTriggerController controller = new PoseTriggerController(config);
    OptionalLong armRequestNs = OptionalLong.empty();
    for (PoseTriggerController.Observation observation : observations) {
      PoseTriggerController.Decision decision = controller.observe(observation);
      if (decision.command() == PoseTriggerController.Command.START_HIGH_SPEED) {
        if (armRequestNs.isPresent()) {
          throw new IllegalStateException("replay emitted more than one arm request");
        }
        armRequestNs = OptionalLong.of(observation.timestampNs());
      }
    }

    if (armRequestNs.isEmpty()) {
      return new Result(
          OptionalLong.empty(),
          OptionalLong.empty(),
          OptionalLong.empty(),
          OptionalLong.empty(),
          false,
          false,
          false,
          false,
          controller.state());
    }

    long armNs = armRequestNs.orElseThrow();
    long armOffsetFromPreferredNs = Math.subtractExact(armNs, annotation.preferredArmNs());
    long readyNs = Math.addExact(armNs, annotation.highSpeedStartupBudgetNs());
    long leadNs = Math.subtractExact(annotation.takeawayNs(), readyNs);
    boolean beforeSafeWindow = armNs < annotation.safeArmStartNs();
    boolean forbidden = annotation.mustNotArm().stream().anyMatch(range -> range.contains(armNs));
    boolean readyByTakeaway = readyNs <= annotation.takeawayNs();
    return new Result(
        armRequestNs,
        OptionalLong.of(armOffsetFromPreferredNs),
        OptionalLong.of(readyNs),
        OptionalLong.of(leadNs),
        beforeSafeWindow,
        forbidden,
        readyByTakeaway,
        !beforeSafeWindow && !forbidden && readyByTakeaway,
        controller.state());
  }
}
