package com.agoessling.swingcapture.pose;

import java.util.Objects;
import java.util.OptionalLong;

/**
 * Model-independent hysteresis for deciding when a low-rate standby stream should start the
 * high-speed capture pipeline.
 *
 * <p>The pose adapter owns image decoding and inference. It supplies normalized confidence and
 * motion values at a nominal five frames per second. This class deliberately has no Android,
 * Camera2, or ML-runtime dependency, so recorded observations can exercise exactly the controller
 * used on a phone.
 */
public final class PoseTriggerController {
  public enum State {
    WATCHING,
    QUALIFYING,
    ARM_REQUESTED,
    WAITING_FOR_CLEAR
  }

  public enum Command {
    NONE,
    START_HIGH_SPEED,
    STOP_HIGH_SPEED
  }

  /** Values are normalized to [0, 1], and timestamps use one monotonic clock. */
  public record Observation(
      long timestampNs,
      double personConfidence,
      double addressConfidence,
      double motionMagnitude,
      boolean insideHittingRegion) {
    public Observation {
      if (timestampNs < 0) {
        throw new IllegalArgumentException("timestampNs cannot be negative");
      }
      requireUnitInterval(personConfidence, "personConfidence");
      requireUnitInterval(addressConfidence, "addressConfidence");
      requireUnitInterval(motionMagnitude, "motionMagnitude");
    }

    private static void requireUnitInterval(double value, String name) {
      if (!Double.isFinite(value) || value < 0.0 || value > 1.0) {
        throw new IllegalArgumentException(name + " must be finite and in [0, 1]");
      }
    }
  }

  /**
   * Controller thresholds and time bounds.
   *
   * <p>The defaults require three coherent observations at 5 fps, tolerate one isolated missed
   * observation, and never keep an unproductive high-speed session alive for more than 15 seconds.
   */
  public record Config(
      double minimumPersonConfidence,
      double maximumClearPersonConfidence,
      double minimumAddressConfidence,
      double maximumMotionMagnitude,
      long minimumQualificationNs,
      long maximumObservationGapNs,
      long qualificationDropoutGraceNs,
      long maximumArmedDurationNs,
      long clearDurationNs,
      long cooldownNs) {
    private static final long MILLIS_TO_NANOS = 1_000_000L;

    public Config {
      requireUnitInterval(minimumPersonConfidence, "minimumPersonConfidence");
      requireUnitInterval(maximumClearPersonConfidence, "maximumClearPersonConfidence");
      requireUnitInterval(minimumAddressConfidence, "minimumAddressConfidence");
      requireUnitInterval(maximumMotionMagnitude, "maximumMotionMagnitude");
      if (maximumClearPersonConfidence >= minimumPersonConfidence) {
        throw new IllegalArgumentException(
            "maximumClearPersonConfidence must be below minimumPersonConfidence");
      }
      requirePositive(minimumQualificationNs, "minimumQualificationNs");
      requirePositive(maximumObservationGapNs, "maximumObservationGapNs");
      requireNonnegative(qualificationDropoutGraceNs, "qualificationDropoutGraceNs");
      requirePositive(maximumArmedDurationNs, "maximumArmedDurationNs");
      requirePositive(clearDurationNs, "clearDurationNs");
      requireNonnegative(cooldownNs, "cooldownNs");
      if (qualificationDropoutGraceNs > maximumObservationGapNs) {
        throw new IllegalArgumentException(
            "qualificationDropoutGraceNs cannot exceed maximumObservationGapNs");
      }
    }

    public static Config defaultsForFiveFramesPerSecond() {
      return new Config(
          0.55,
          0.25,
          0.45,
          0.45,
          400 * MILLIS_TO_NANOS,
          450 * MILLIS_TO_NANOS,
          250 * MILLIS_TO_NANOS,
          15_000 * MILLIS_TO_NANOS,
          1_000 * MILLIS_TO_NANOS,
          2_000 * MILLIS_TO_NANOS);
    }

    private static void requireUnitInterval(double value, String name) {
      if (!Double.isFinite(value) || value < 0.0 || value > 1.0) {
        throw new IllegalArgumentException(name + " must be finite and in [0, 1]");
      }
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

  public record Decision(
      State state,
      Command command,
      String reason,
      OptionalLong qualificationStartedNs,
      OptionalLong armRequestedNs) {
    public Decision {
      Objects.requireNonNull(state, "state");
      Objects.requireNonNull(command, "command");
      Objects.requireNonNull(reason, "reason");
      Objects.requireNonNull(qualificationStartedNs, "qualificationStartedNs");
      Objects.requireNonNull(armRequestedNs, "armRequestedNs");
    }
  }

  private final Config config;
  private State state = State.WATCHING;
  private long lastTimestampNs = -1;
  private long qualificationStartedNs = -1;
  private long lastQualifiedNs = -1;
  private long armRequestedNs = -1;
  private long cooldownUntilNs = -1;
  private long clearStartedNs = -1;

  public PoseTriggerController(Config config) {
    this.config = Objects.requireNonNull(config, "config");
  }

  public Decision observe(Observation observation) {
    Objects.requireNonNull(observation, "observation");
    requireIncreasingTimestamp(observation.timestampNs());
    long previousTimestampNs = lastTimestampNs;
    lastTimestampNs = observation.timestampNs();

    if (state == State.ARM_REQUESTED
        && elapsedNs(observation.timestampNs(), armRequestedNs)
            >= config.maximumArmedDurationNs()) {
      enterWaitingForClear(observation.timestampNs());
      updateClearEvidence(observation);
      return decision(Command.STOP_HIGH_SPEED, "high-speed arm timed out without an impact");
    }

    return switch (state) {
      case WATCHING -> observeWatching(observation);
      case QUALIFYING -> observeQualifying(observation, previousTimestampNs);
      case ARM_REQUESTED -> decision(Command.NONE, "high-speed capture already requested");
      case WAITING_FOR_CLEAR -> observeWaitingForClear(observation);
    };
  }

  /** Marks a completed or externally canceled high-speed attempt and suppresses finish-pose rearm. */
  public Decision captureEnded(long timestampNs) {
    requireIncreasingTimestamp(timestampNs);
    if (state != State.ARM_REQUESTED) {
      throw new IllegalStateException("captureEnded requires ARM_REQUESTED state");
    }
    lastTimestampNs = timestampNs;
    enterWaitingForClear(timestampNs);
    return decision(Command.NONE, "capture ended; waiting for the golfer to clear");
  }

  public void reset() {
    state = State.WATCHING;
    lastTimestampNs = -1;
    qualificationStartedNs = -1;
    lastQualifiedNs = -1;
    armRequestedNs = -1;
    cooldownUntilNs = -1;
    clearStartedNs = -1;
  }

  public State state() {
    return state;
  }

  private Decision observeWatching(Observation observation) {
    if (!qualifies(observation)) {
      return decision(Command.NONE, "waiting for a golfer approaching address");
    }
    state = State.QUALIFYING;
    qualificationStartedNs = observation.timestampNs();
    lastQualifiedNs = observation.timestampNs();
    return decision(Command.NONE, "first qualifying pose observation");
  }

  private Decision observeQualifying(Observation observation, long previousTimestampNs) {
    if (elapsedNs(observation.timestampNs(), previousTimestampNs)
        > config.maximumObservationGapNs()) {
      return restartOrClearQualification(observation, "observation gap reset qualification");
    }

    if (qualifies(observation)) {
      lastQualifiedNs = observation.timestampNs();
      if (elapsedNs(observation.timestampNs(), qualificationStartedNs)
          >= config.minimumQualificationNs()) {
        state = State.ARM_REQUESTED;
        armRequestedNs = observation.timestampNs();
        return decision(
            Command.START_HIGH_SPEED, "stable address approach requested high-speed capture");
      }
      return decision(Command.NONE, "address approach is still qualifying");
    }

    if (elapsedNs(observation.timestampNs(), lastQualifiedNs)
        > config.qualificationDropoutGraceNs()) {
      clearQualification();
      return decision(Command.NONE, "pose evidence dropped out before qualification");
    }
    return decision(Command.NONE, "isolated pose dropout tolerated");
  }

  private Decision observeWaitingForClear(Observation observation) {
    updateClearEvidence(observation);
    if (clearStartedNs >= 0
        && observation.timestampNs() >= cooldownUntilNs
        && elapsedNs(observation.timestampNs(), clearStartedNs) >= config.clearDurationNs()) {
      state = State.WATCHING;
      armRequestedNs = -1;
      cooldownUntilNs = -1;
      clearStartedNs = -1;
      return decision(Command.NONE, "golfer cleared; standby trigger is ready again");
    }
    return decision(Command.NONE, "waiting for cooldown and a clear hitting region");
  }

  private Decision restartOrClearQualification(Observation observation, String reason) {
    if (qualifies(observation)) {
      qualificationStartedNs = observation.timestampNs();
      lastQualifiedNs = observation.timestampNs();
      return decision(Command.NONE, reason + "; current observation starts a new candidate");
    }
    clearQualification();
    return decision(Command.NONE, reason);
  }

  private void clearQualification() {
    state = State.WATCHING;
    qualificationStartedNs = -1;
    lastQualifiedNs = -1;
  }

  private void enterWaitingForClear(long timestampNs) {
    state = State.WAITING_FOR_CLEAR;
    cooldownUntilNs = Math.addExact(timestampNs, config.cooldownNs());
    clearStartedNs = -1;
    qualificationStartedNs = -1;
    lastQualifiedNs = -1;
  }

  private void updateClearEvidence(Observation observation) {
    boolean clear =
        !observation.insideHittingRegion()
            || observation.personConfidence() <= config.maximumClearPersonConfidence();
    if (!clear) {
      clearStartedNs = -1;
    } else if (clearStartedNs < 0) {
      clearStartedNs = observation.timestampNs();
    }
  }

  private boolean qualifies(Observation observation) {
    return observation.insideHittingRegion()
        && observation.personConfidence() >= config.minimumPersonConfidence()
        && observation.addressConfidence() >= config.minimumAddressConfidence()
        && observation.motionMagnitude() <= config.maximumMotionMagnitude();
  }

  private void requireIncreasingTimestamp(long timestampNs) {
    if (timestampNs < 0) {
      throw new IllegalArgumentException("timestampNs cannot be negative");
    }
    if (lastTimestampNs >= 0 && timestampNs <= lastTimestampNs) {
      throw new IllegalArgumentException("timestamps must increase strictly");
    }
  }

  private static long elapsedNs(long laterNs, long earlierNs) {
    return Math.subtractExact(laterNs, earlierNs);
  }

  private Decision decision(Command command, String reason) {
    return new Decision(
        state,
        command,
        reason,
        qualificationStartedNs >= 0
            ? OptionalLong.of(qualificationStartedNs)
            : OptionalLong.empty(),
        armRequestedNs >= 0 ? OptionalLong.of(armRequestedNs) : OptionalLong.empty());
  }
}
