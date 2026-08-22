package com.agoessling.swingcapture.pose;

import java.util.List;
import java.util.Objects;
import java.util.OptionalLong;

/** Deterministic scoring of recorded 5 fps pose observations against hand-labeled timing. */
public final class PoseTriggerReplay {
  /**
   * Safety/readiness outcome for a replay.
   *
   * <p>{@link #ACCEPTABLE_EARLY} is intentionally a success: the preferred timestamp is useful for
   * diagnostics, but any arm at or after the safe start that is ready by takeaway is correct.
   */
  public enum Outcome {
    ACCEPTABLE,
    ACCEPTABLE_EARLY,
    NO_ARM_REQUEST,
    UNSAFE_EARLY_ARM,
    FORBIDDEN_ARM,
    NOT_READY_BY_TAKEAWAY;

    public boolean acceptable() {
      return this == ACCEPTABLE || this == ACCEPTABLE_EARLY;
    }
  }

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
      PoseTriggerController.State finalState,
      Outcome outcome,
      int armRequestCount) {
    public Result {
      Objects.requireNonNull(armRequestNs, "armRequestNs");
      Objects.requireNonNull(armOffsetFromPreferredNs, "armOffsetFromPreferredNs");
      Objects.requireNonNull(highSpeedReadyNs, "highSpeedReadyNs");
      Objects.requireNonNull(readyLeadBeforeTakeawayNs, "readyLeadBeforeTakeawayNs");
      Objects.requireNonNull(finalState, "finalState");
      Objects.requireNonNull(outcome, "outcome");
      if (armRequestCount < 0) {
        throw new IllegalArgumentException("armRequestCount cannot be negative");
      }
      if (passed != outcome.acceptable()) {
        throw new IllegalArgumentException("passed must agree with outcome");
      }
      boolean hasArm = armRequestNs.isPresent();
      if (hasArm != (armRequestCount > 0)) {
        throw new IllegalArgumentException("armRequestCount must agree with armRequestNs");
      }
      if (armOffsetFromPreferredNs.isPresent() != hasArm
          || highSpeedReadyNs.isPresent() != hasArm
          || readyLeadBeforeTakeawayNs.isPresent() != hasArm) {
        throw new IllegalArgumentException("arm-derived timing fields must be all present or absent");
      }
      if (hasArm
          && (armRequestNs.orElseThrow() < 0
              || highSpeedReadyNs.orElseThrow() < armRequestNs.orElseThrow()
              || readyByTakeaway != (readyLeadBeforeTakeawayNs.orElseThrow() >= 0))) {
        throw new IllegalArgumentException("arm-derived timing fields contradict readiness");
      }
      Outcome expectedOutcome;
      if (!hasArm) {
        if (armedBeforeSafeWindow || armedInForbiddenInterval || readyByTakeaway) {
          throw new IllegalArgumentException("a no-arm result cannot retain arm evidence");
        }
        expectedOutcome = Outcome.NO_ARM_REQUEST;
      } else if (armedInForbiddenInterval) {
        expectedOutcome = Outcome.FORBIDDEN_ARM;
      } else if (armedBeforeSafeWindow) {
        expectedOutcome = Outcome.UNSAFE_EARLY_ARM;
      } else if (!readyByTakeaway) {
        expectedOutcome = Outcome.NOT_READY_BY_TAKEAWAY;
      } else if (armOffsetFromPreferredNs.orElseThrow() < 0) {
        expectedOutcome = Outcome.ACCEPTABLE_EARLY;
      } else {
        expectedOutcome = Outcome.ACCEPTABLE;
      }
      if (outcome != expectedOutcome) {
        throw new IllegalArgumentException("outcome contradicts replay timing evidence");
      }
    }

    /** Source-compatible constructor for the former single-arm result shape. */
    public Result(
        OptionalLong armRequestNs,
        OptionalLong armOffsetFromPreferredNs,
        OptionalLong highSpeedReadyNs,
        OptionalLong readyLeadBeforeTakeawayNs,
        boolean armedBeforeSafeWindow,
        boolean armedInForbiddenInterval,
        boolean readyByTakeaway,
        boolean passed,
        PoseTriggerController.State finalState) {
      this(
          armRequestNs,
          armOffsetFromPreferredNs,
          highSpeedReadyNs,
          readyLeadBeforeTakeawayNs,
          armedBeforeSafeWindow,
          armedInForbiddenInterval,
          readyByTakeaway,
          passed,
          finalState,
          legacyOutcome(
              armRequestNs,
              armOffsetFromPreferredNs,
              armedBeforeSafeWindow,
              armedInForbiddenInterval,
              readyByTakeaway,
              passed),
          armRequestNs.isPresent() ? 1 : 0);
    }

    private static Outcome legacyOutcome(
        OptionalLong armRequestNs,
        OptionalLong armOffsetFromPreferredNs,
        boolean armedBeforeSafeWindow,
        boolean armedInForbiddenInterval,
        boolean readyByTakeaway,
        boolean passed) {
      if (passed) {
        return armOffsetFromPreferredNs.isPresent()
                && armOffsetFromPreferredNs.orElseThrow() < 0
            ? Outcome.ACCEPTABLE_EARLY
            : Outcome.ACCEPTABLE;
      }
      if (armRequestNs.isEmpty()) {
        return Outcome.NO_ARM_REQUEST;
      }
      if (armedInForbiddenInterval) {
        return Outcome.FORBIDDEN_ARM;
      }
      if (armedBeforeSafeWindow) {
        return Outcome.UNSAFE_EARLY_ARM;
      }
      if (!readyByTakeaway) {
        return Outcome.NOT_READY_BY_TAKEAWAY;
      }
      throw new IllegalArgumentException("legacy failed result has no failure condition");
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
    int armRequestCount = 0;
    boolean beforeSafeWindow = false;
    boolean forbidden = false;
    for (PoseTriggerController.Observation observation : observations) {
      PoseTriggerController.Decision decision = controller.observe(observation);
      if (decision.command() == PoseTriggerController.Command.START_HIGH_SPEED) {
        armRequestCount++;
        if (armRequestNs.isEmpty()) {
          armRequestNs = OptionalLong.of(observation.timestampNs());
        }
        beforeSafeWindow |= observation.timestampNs() < annotation.safeArmStartNs();
        forbidden |=
            annotation.mustNotArm().stream()
                .anyMatch(range -> range.contains(observation.timestampNs()));
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
          controller.state(),
          Outcome.NO_ARM_REQUEST,
          0);
    }

    long armNs = armRequestNs.orElseThrow();
    long armOffsetFromPreferredNs = Math.subtractExact(armNs, annotation.preferredArmNs());
    long readyNs = saturatedAdd(armNs, annotation.highSpeedStartupBudgetNs());
    long leadNs = annotation.takeawayNs() - readyNs;
    boolean readyByTakeaway = readyNs <= annotation.takeawayNs();
    Outcome outcome;
    if (forbidden) {
      outcome = Outcome.FORBIDDEN_ARM;
    } else if (beforeSafeWindow) {
      outcome = Outcome.UNSAFE_EARLY_ARM;
    } else if (!readyByTakeaway) {
      outcome = Outcome.NOT_READY_BY_TAKEAWAY;
    } else if (armNs < annotation.preferredArmNs()) {
      outcome = Outcome.ACCEPTABLE_EARLY;
    } else {
      outcome = Outcome.ACCEPTABLE;
    }
    return new Result(
        armRequestNs,
        OptionalLong.of(armOffsetFromPreferredNs),
        OptionalLong.of(readyNs),
        OptionalLong.of(leadNs),
        beforeSafeWindow,
        forbidden,
        readyByTakeaway,
        outcome.acceptable(),
        controller.state(),
        outcome,
        armRequestCount);
  }

  private static long saturatedAdd(long value, long increment) {
    if (value > Long.MAX_VALUE - increment) {
      return Long.MAX_VALUE;
    }
    return value + increment;
  }
}
