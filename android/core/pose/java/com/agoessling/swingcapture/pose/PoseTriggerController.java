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

  /** Stable machine-readable explanations for controller decisions and state transitions. */
  public enum TransitionReason {
    LEGACY,
    WAITING_FOR_ADDRESS,
    QUALIFICATION_STARTED,
    QUALIFICATION_CONTINUING,
    QUALIFICATION_DROPOUT_TOLERATED,
    QUALIFICATION_DROPPED,
    OBSERVATION_GAP_RESTARTED,
    OBSERVATION_GAP_CLEARED,
    ADDRESS_STABLE_ARMED,
    ACTIVE_WINDOW_EXTENDED,
    ACTIVE_CLEARING,
    ACTIVE_CLEAR_STOPPED,
    ACTIVE_EVIDENCE_EXPIRED,
    THERMAL_HARD_CAP_REACHED,
    EXTERNAL_CAPTURE_STARTED,
    CAPTURE_ENDED,
    WAITING_FOR_CLEAR,
    CLEAR_COMPLETE_REARMED
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
   * <p>The defaults require three coherent observations at 5 fps and tolerate one isolated missed
   * observation. {@code maximumArmedDurationNs} is a sliding evidence lease: observations that
   * still place the golfer in the hitting region extend it. {@code thermalHardCapNs} is absolute
   * from the first arm request and cannot be extended. Defaults therefore allow an address dwell
   * to remain armed beyond 15 seconds, but force a stop after 30 seconds without an impact.
   *
   * <p>The ten-argument constructor preserves the former API and its exact fixed-timeout behavior
   * by using {@code maximumArmedDurationNs} as both the evidence lease and hard cap. New callers
   * should specify the hard cap explicitly.
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
      long cooldownNs,
      long thermalHardCapNs) {
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
      requirePositive(thermalHardCapNs, "thermalHardCapNs");
      if (qualificationDropoutGraceNs > maximumObservationGapNs) {
        throw new IllegalArgumentException(
            "qualificationDropoutGraceNs cannot exceed maximumObservationGapNs");
      }
      if (thermalHardCapNs < maximumArmedDurationNs) {
        throw new IllegalArgumentException(
            "thermalHardCapNs cannot be shorter than maximumArmedDurationNs");
      }
    }

    /** Compatibility constructor retaining the former fixed maximum armed duration. */
    public Config(
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
      this(
          minimumPersonConfidence,
          maximumClearPersonConfidence,
          minimumAddressConfidence,
          maximumMotionMagnitude,
          minimumQualificationNs,
          maximumObservationGapNs,
          qualificationDropoutGraceNs,
          maximumArmedDurationNs,
          clearDurationNs,
          cooldownNs,
          maximumArmedDurationNs);
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
          2_000 * MILLIS_TO_NANOS,
          30_000 * MILLIS_TO_NANOS);
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
      OptionalLong armRequestedNs,
      OptionalLong activeUntilNs,
      OptionalLong thermalHardStopNs,
      TransitionReason transitionReason) {
    public Decision {
      Objects.requireNonNull(state, "state");
      Objects.requireNonNull(command, "command");
      Objects.requireNonNull(reason, "reason");
      Objects.requireNonNull(qualificationStartedNs, "qualificationStartedNs");
      Objects.requireNonNull(armRequestedNs, "armRequestedNs");
      Objects.requireNonNull(activeUntilNs, "activeUntilNs");
      Objects.requireNonNull(thermalHardStopNs, "thermalHardStopNs");
      Objects.requireNonNull(transitionReason, "transitionReason");
    }

    /** Source-compatible constructor for consumers that construct legacy decisions in tests. */
    public Decision(
        State state,
        Command command,
        String reason,
        OptionalLong qualificationStartedNs,
        OptionalLong armRequestedNs) {
      this(
          state,
          command,
          reason,
          qualificationStartedNs,
          armRequestedNs,
          OptionalLong.empty(),
          OptionalLong.empty(),
          TransitionReason.LEGACY);
    }
  }

  private final Config config;
  private State state = State.WATCHING;
  private long lastTimestampNs = -1;
  private long qualificationStartedNs = -1;
  private long lastQualifiedNs = -1;
  private long armRequestedNs = -1;
  private long activeUntilNs = -1;
  private long thermalHardStopNs = -1;
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

    return switch (state) {
      case WATCHING -> observeWatching(observation);
      case QUALIFYING -> observeQualifying(observation, previousTimestampNs);
      case ARM_REQUESTED -> observeArmed(observation, previousTimestampNs);
      case WAITING_FOR_CLEAR -> observeWaitingForClear(observation, previousTimestampNs);
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
    return decision(
        Command.NONE,
        "capture ended; waiting for the golfer to clear",
        TransitionReason.CAPTURE_ENDED);
  }

  /**
   * Records that a paired leader has started this node's high-speed capture.
   *
   * <p>Shadow nodes still run the local pose controller for diagnostics, so the controller may be
   * watching, qualifying, or already locally arm-requested when the leader request arrives. This
   * transition makes the subsequent capture lifecycle explicit and keeps {@link #captureEnded}
   * valid. A node waiting for the golfer to clear must reject a new paired capture rather than
   * bypassing the finish-pose suppression latch.
   */
  public Decision externalCaptureStarted(long timestampNs) {
    requireIncreasingTimestamp(timestampNs);
    if (state == State.WAITING_FOR_CLEAR) {
      throw new IllegalStateException("external capture cannot bypass WAITING_FOR_CLEAR");
    }
    lastTimestampNs = timestampNs;
    state = State.ARM_REQUESTED;
    qualificationStartedNs = -1;
    lastQualifiedNs = -1;
    armRequestedNs = timestampNs;
    thermalHardStopNs = saturatedAdd(timestampNs, config.thermalHardCapNs());
    activeUntilNs =
        Math.min(saturatedAdd(timestampNs, config.maximumArmedDurationNs()), thermalHardStopNs);
    clearStartedNs = -1;
    return decision(
        Command.NONE,
        "paired leader started high-speed capture",
        TransitionReason.EXTERNAL_CAPTURE_STARTED);
  }

  public void reset() {
    state = State.WATCHING;
    lastTimestampNs = -1;
    qualificationStartedNs = -1;
    lastQualifiedNs = -1;
    armRequestedNs = -1;
    activeUntilNs = -1;
    thermalHardStopNs = -1;
    cooldownUntilNs = -1;
    clearStartedNs = -1;
  }

  public State state() {
    return state;
  }

  private Decision observeWatching(Observation observation) {
    if (!qualifies(observation)) {
      return decision(
          Command.NONE,
          "waiting for a golfer approaching address",
          TransitionReason.WAITING_FOR_ADDRESS);
    }
    state = State.QUALIFYING;
    qualificationStartedNs = observation.timestampNs();
    lastQualifiedNs = observation.timestampNs();
    return decision(
        Command.NONE,
        "first qualifying pose observation",
        TransitionReason.QUALIFICATION_STARTED);
  }

  private Decision observeQualifying(Observation observation, long previousTimestampNs) {
    if (elapsedNs(observation.timestampNs(), previousTimestampNs)
        > config.maximumObservationGapNs()) {
      return restartOrClearQualification(observation);
    }

    if (qualifies(observation)) {
      lastQualifiedNs = observation.timestampNs();
      if (elapsedNs(observation.timestampNs(), qualificationStartedNs)
          >= config.minimumQualificationNs()) {
        state = State.ARM_REQUESTED;
        armRequestedNs = observation.timestampNs();
        qualificationStartedNs = -1;
        lastQualifiedNs = -1;
        thermalHardStopNs = saturatedAdd(armRequestedNs, config.thermalHardCapNs());
        activeUntilNs =
            Math.min(
                saturatedAdd(armRequestedNs, config.maximumArmedDurationNs()),
                thermalHardStopNs);
        return decision(
            Command.START_HIGH_SPEED,
            "stable address approach requested high-speed capture",
            TransitionReason.ADDRESS_STABLE_ARMED);
      }
      return decision(
          Command.NONE,
          "address approach is still qualifying",
          TransitionReason.QUALIFICATION_CONTINUING);
    }

    if (elapsedNs(observation.timestampNs(), lastQualifiedNs)
        > config.qualificationDropoutGraceNs()) {
      clearQualification();
      return decision(
          Command.NONE,
          "pose evidence dropped out before qualification",
          TransitionReason.QUALIFICATION_DROPPED);
    }
    return decision(
        Command.NONE,
        "isolated pose dropout tolerated",
        TransitionReason.QUALIFICATION_DROPOUT_TOLERATED);
  }

  private Decision observeArmed(Observation observation, long previousTimestampNs) {
    if (observation.timestampNs() >= thermalHardStopNs) {
      enterWaitingForClear(observation.timestampNs());
      updateClearEvidence(observation);
      return decision(
          Command.STOP_HIGH_SPEED,
          "absolute thermal hard cap stopped high-speed capture",
          TransitionReason.THERMAL_HARD_CAP_REACHED);
    }

    if (observation.timestampNs() >= activeUntilNs) {
      enterWaitingForClear(observation.timestampNs());
      updateClearEvidence(observation);
      return decision(
          Command.STOP_HIGH_SPEED,
          "active pose evidence expired before the thermal hard cap",
          TransitionReason.ACTIVE_EVIDENCE_EXPIRED);
    }

    if (isEngaged(observation)) {
      clearStartedNs = -1;
      activeUntilNs =
          Math.min(
              saturatedAdd(observation.timestampNs(), config.maximumArmedDurationNs()),
              thermalHardStopNs);
      return decision(
          Command.NONE,
          "golfer remains engaged; active high-speed window extended",
          TransitionReason.ACTIVE_WINDOW_EXTENDED);
    }

    if (previousTimestampNs >= 0
        && elapsedNs(observation.timestampNs(), previousTimestampNs)
            > config.maximumObservationGapNs()) {
      clearStartedNs = -1;
    }
    updateClearEvidence(observation);
    if (clearStartedNs >= 0
        && elapsedNs(observation.timestampNs(), clearStartedNs) >= config.clearDurationNs()) {
      enterWaitingForClear(observation.timestampNs());
      updateClearEvidence(observation);
      return decision(
          Command.STOP_HIGH_SPEED,
          "golfer cleared; stopped high-speed capture and entered rearm cooldown",
          TransitionReason.ACTIVE_CLEAR_STOPPED);
    }
    return decision(
        Command.NONE,
        "clear-region evidence is accumulating while high-speed remains active",
        TransitionReason.ACTIVE_CLEARING);
  }

  private Decision observeWaitingForClear(
      Observation observation, long previousTimestampNs) {
    if (previousTimestampNs >= 0
        && elapsedNs(observation.timestampNs(), previousTimestampNs)
            > config.maximumObservationGapNs()) {
      clearStartedNs = -1;
    }
    updateClearEvidence(observation);
    if (clearStartedNs >= 0
        && elapsedNs(observation.timestampNs(), clearStartedNs) >= config.clearDurationNs()
        && observation.timestampNs() >= cooldownUntilNs) {
      enterWatchingAfterClear();
      return decision(
          Command.NONE,
          "golfer cleared and cooldown elapsed; standby trigger is ready again",
          TransitionReason.CLEAR_COMPLETE_REARMED);
    }
    return decision(
        Command.NONE,
        "waiting for continuous clear-region evidence",
        TransitionReason.WAITING_FOR_CLEAR);
  }

  private Decision restartOrClearQualification(Observation observation) {
    if (qualifies(observation)) {
      qualificationStartedNs = observation.timestampNs();
      lastQualifiedNs = observation.timestampNs();
      return decision(
          Command.NONE,
          "observation gap reset qualification; current observation starts a new candidate",
          TransitionReason.OBSERVATION_GAP_RESTARTED);
    }
    clearQualification();
    return decision(
        Command.NONE,
        "observation gap reset qualification",
        TransitionReason.OBSERVATION_GAP_CLEARED);
  }

  private void clearQualification() {
    state = State.WATCHING;
    qualificationStartedNs = -1;
    lastQualifiedNs = -1;
  }

  private void enterWaitingForClear(long timestampNs) {
    state = State.WAITING_FOR_CLEAR;
    cooldownUntilNs = saturatedAdd(timestampNs, config.cooldownNs());
    clearStartedNs = -1;
    qualificationStartedNs = -1;
    lastQualifiedNs = -1;
    activeUntilNs = -1;
    thermalHardStopNs = -1;
  }

  private void enterWatchingAfterClear() {
    state = State.WATCHING;
    qualificationStartedNs = -1;
    lastQualifiedNs = -1;
    armRequestedNs = -1;
    activeUntilNs = -1;
    thermalHardStopNs = -1;
    cooldownUntilNs = -1;
    clearStartedNs = -1;
  }

  private void updateClearEvidence(Observation observation) {
    if (!isClear(observation)) {
      clearStartedNs = -1;
    } else if (clearStartedNs < 0) {
      clearStartedNs = observation.timestampNs();
    }
  }

  private boolean isClear(Observation observation) {
    return !observation.insideHittingRegion()
        || observation.personConfidence() <= config.maximumClearPersonConfidence();
  }

  private boolean isEngaged(Observation observation) {
    return observation.insideHittingRegion()
        && observation.personConfidence() >= config.minimumPersonConfidence();
  }

  private boolean qualifies(Observation observation) {
    return isEngaged(observation)
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

  private static long saturatedAdd(long value, long increment) {
    if (value > Long.MAX_VALUE - increment) {
      return Long.MAX_VALUE;
    }
    return value + increment;
  }

  private Decision decision(Command command, String reason, TransitionReason transitionReason) {
    return new Decision(
        state,
        command,
        reason,
        qualificationStartedNs >= 0
            ? OptionalLong.of(qualificationStartedNs)
            : OptionalLong.empty(),
        armRequestedNs >= 0 ? OptionalLong.of(armRequestedNs) : OptionalLong.empty(),
        activeUntilNs >= 0 ? OptionalLong.of(activeUntilNs) : OptionalLong.empty(),
        thermalHardStopNs >= 0 ? OptionalLong.of(thermalHardStopNs) : OptionalLong.empty(),
        transitionReason);
  }
}
