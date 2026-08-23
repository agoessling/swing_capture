package com.agoessling.swingcapture.core.coordination;

import java.util.EnumSet;
import java.util.Objects;
import java.util.Set;

/**
 * Leader-owned, one-swing-at-a-time lifecycle for an autonomous two-phone station.
 *
 * <p>The state machine is deliberately side-effect free. Production executes returned actions and
 * reports their outcomes back through the matching methods. Session mismatches, duplicate local
 * clips, and immutable-record conflicts are terminal; transport loss and one-sided publication
 * retain whatever local evidence exists and rearm in degraded mode.
 */
public final class AutonomousPairLifecycle {
  public enum State {
    STOPPED,
    STARTING_STATION,
    MONITORING,
    DEGRADED_MONITORING,
    ARMING_SWING,
    CAPTURING,
    WAITING_EVIDENCE,
    ADMITTING_PAIR,
    STORING_LOCAL_RECORD,
    REPLICATING_RECORD,
    REARMING,
    STOPPING,
    TERMINAL_FAILURE
  }

  public enum Action {
    START_PEER_STANDBY,
    STOP_PEER_STANDBY,
    ARM_PEER_FOR_SWING,
    SEND_PEER_IMPACT,
    POLL_PEER_TRIGGER,
    POLL_PEER_PUBLICATION,
    ADMIT_PAIR,
    STORE_LOCAL_RECORD,
    REPLICATE_RECORD_TO_PEER,
    REARM_LOCAL,
    REARM_PEER
  }

  public enum ImmutableStoreOutcome {
    STORED,
    ALREADY_PRESENT,
    UNAVAILABLE,
    CONFLICT
  }

  public enum LastOutcome {
    NONE,
    PAIRED,
    LOCAL_ONLY,
    PEER_ONLY,
    NO_EVIDENCE,
    CONFLICT
  }

  public record Config(long evidenceDeadlineNanos) {
    public Config {
      if (evidenceDeadlineNanos <= 0) {
        throw new IllegalArgumentException("evidence deadline must be positive");
      }
    }

    public static Config productionDefaults() {
      return new Config(10_000_000_000L);
    }
  }

  public record Snapshot(
      State state,
      String activeSessionId,
      boolean peerAvailable,
      boolean peerArmed,
      boolean localTriggered,
      boolean peerTriggered,
      boolean localPublished,
      boolean peerPublished,
      boolean clockFresh,
      LastOutcome lastOutcome,
      String lastCompletedSessionId,
      String diagnostic) {}

  /** Complete durable state needed to resume without allocating another shared session. */
  public record Checkpoint(
      int schemaVersion,
      State state,
      String activeSessionId,
      String localSessionId,
      String peerSessionId,
      boolean peerAvailable,
      boolean peerArmed,
      boolean peerArmUnconfirmed,
      boolean localTriggered,
      boolean peerTriggered,
      boolean localPublished,
      boolean peerPublished,
      boolean clockFresh,
      long evidenceDeadlineNanos,
      LastOutcome lastOutcome,
      String lastCompletedSessionId,
      String diagnostic) {
    public static final int SCHEMA_VERSION = 1;

    public Checkpoint {
      if (schemaVersion != SCHEMA_VERSION) {
        throw new IllegalArgumentException("unsupported autonomous-pair checkpoint schema");
      }
      Objects.requireNonNull(state, "state");
      activeSessionId = requireOptionalIdentifier(activeSessionId, "activeSessionId");
      localSessionId = requireOptionalIdentifier(localSessionId, "localSessionId");
      peerSessionId = requireOptionalIdentifier(peerSessionId, "peerSessionId");
      Objects.requireNonNull(lastOutcome, "lastOutcome");
      lastCompletedSessionId =
          requireOptionalIdentifier(lastCompletedSessionId, "lastCompletedSessionId");
      Objects.requireNonNull(diagnostic, "diagnostic");
      if (diagnostic.length() > 1_024) {
        throw new IllegalArgumentException("checkpoint diagnostic is too long");
      }
      if (evidenceDeadlineNanos < 0) {
        throw new IllegalArgumentException("checkpoint evidence deadline cannot be negative");
      }
      if (activeSessionId.isEmpty()) {
        if (!localSessionId.isEmpty()
            || !peerSessionId.isEmpty()
            || localTriggered
            || peerTriggered
            || localPublished
            || peerPublished
            || clockFresh
            || evidenceDeadlineNanos != 0) {
          throw new IllegalArgumentException("inactive checkpoint contains swing evidence");
        }
      } else if (evidenceDeadlineNanos == 0) {
        throw new IllegalArgumentException("active checkpoint is missing an evidence deadline");
      }
      boolean activeState =
          switch (state) {
            case ARMING_SWING,
                CAPTURING,
                WAITING_EVIDENCE,
                ADMITTING_PAIR,
                STORING_LOCAL_RECORD,
                REPLICATING_RECORD,
                REARMING -> true;
            default -> false;
          };
      if (activeState != !activeSessionId.isEmpty()
          && state != State.STOPPING
          && state != State.TERMINAL_FAILURE) {
        throw new IllegalArgumentException("checkpoint lifecycle state disagrees with active swing");
      }
      if (localTriggered != !localSessionId.isEmpty()
          || peerTriggered != !peerSessionId.isEmpty()) {
        throw new IllegalArgumentException("trigger flags disagree with local session IDs");
      }
      if (localPublished && !localTriggered) {
        throw new IllegalArgumentException("local publication requires a local trigger");
      }
      if (peerPublished && !peerTriggered) {
        throw new IllegalArgumentException("peer publication requires a peer trigger");
      }
      if (clockFresh && !peerTriggered) {
        throw new IllegalArgumentException("fresh clock evidence requires a peer trigger");
      }
      if ((state == State.ADMITTING_PAIR
              || state == State.STORING_LOCAL_RECORD
              || state == State.REPLICATING_RECORD)
          && (!localPublished || !peerPublished || !clockFresh)) {
        throw new IllegalArgumentException(
            "record-publication checkpoint lacks admitted paired evidence");
      }
    }
  }

  public record Transition(Snapshot snapshot, Checkpoint checkpoint, Set<Action> actions) {
    public Transition {
      Objects.requireNonNull(snapshot, "snapshot");
      Objects.requireNonNull(checkpoint, "checkpoint");
      actions = Set.copyOf(actions);
    }

    public boolean requests(Action action) {
      return actions.contains(action);
    }
  }

  private final Config config;
  private State state = State.STOPPED;
  private String activeSessionId = "";
  private String localSessionId = "";
  private String peerSessionId = "";
  private boolean peerAvailable;
  private boolean peerArmed;
  private boolean peerArmUnconfirmed;
  private boolean localTriggered;
  private boolean peerTriggered;
  private boolean localPublished;
  private boolean peerPublished;
  private boolean clockFresh;
  private long evidenceDeadlineNanos;
  private LastOutcome lastOutcome = LastOutcome.NONE;
  private String lastCompletedSessionId = "";
  private String diagnostic = "";

  public AutonomousPairLifecycle(Config config) {
    this.config = Objects.requireNonNull(config, "config");
  }

  /** Restores a previously validated checkpoint without performing any side effect. */
  public static AutonomousPairLifecycle restore(Config config, Checkpoint checkpoint) {
    Objects.requireNonNull(checkpoint, "checkpoint");
    AutonomousPairLifecycle lifecycle = new AutonomousPairLifecycle(config);
    lifecycle.state = checkpoint.state();
    lifecycle.activeSessionId = checkpoint.activeSessionId();
    lifecycle.localSessionId = checkpoint.localSessionId();
    lifecycle.peerSessionId = checkpoint.peerSessionId();
    lifecycle.peerAvailable = checkpoint.peerAvailable();
    lifecycle.peerArmed = checkpoint.peerArmed();
    lifecycle.peerArmUnconfirmed = checkpoint.peerArmUnconfirmed();
    lifecycle.localTriggered = checkpoint.localTriggered();
    lifecycle.peerTriggered = checkpoint.peerTriggered();
    lifecycle.localPublished = checkpoint.localPublished();
    lifecycle.peerPublished = checkpoint.peerPublished();
    lifecycle.clockFresh = checkpoint.clockFresh();
    lifecycle.evidenceDeadlineNanos = checkpoint.evidenceDeadlineNanos();
    lifecycle.lastOutcome = checkpoint.lastOutcome();
    lifecycle.lastCompletedSessionId = checkpoint.lastCompletedSessionId();
    lifecycle.diagnostic = checkpoint.diagnostic();
    return lifecycle;
  }

  /**
   * Returns the safe idempotent work after process restart.
   *
   * <p>Capture memory is process-local, so an interrupted arm/capture is converted to bounded
   * evidence recovery for the same shared ID. Complete published evidence is admitted only when a
   * durable record was already constructed before the restart.
   */
  public synchronized Transition recover(boolean durablePendingRecord, long nowNanos) {
    if (nowNanos < 0) {
      throw new IllegalArgumentException("recovery timestamp cannot be negative");
    }
    if (hasActiveSwing()) {
      long maximumDeadline = Math.addExact(nowNanos, config.evidenceDeadlineNanos());
      evidenceDeadlineNanos = Math.min(evidenceDeadlineNanos, maximumDeadline);
    }
    return switch (state) {
      case STOPPED, TERMINAL_FAILURE -> transition();
      case STARTING_STATION, MONITORING, DEGRADED_MONITORING -> {
        peerAvailable = false;
        peerArmed = false;
        peerArmUnconfirmed = false;
        state = State.STARTING_STATION;
        diagnostic = "recovering peer standby after leader process restart";
        yield transition(Action.START_PEER_STANDBY);
      }
      case ARMING_SWING, CAPTURING -> {
        // Never restart high-speed capture and publish a partial clip as though it contained the
        // original pre-roll. Existing same-session peer evidence may still be recovered.
        state = State.WAITING_EVIDENCE;
        peerAvailable = false;
        diagnostic = "recovering existing shared session after interrupted capture";
        yield peerArmed || peerArmUnconfirmed
            ? transition(
                Action.START_PEER_STANDBY,
                Action.POLL_PEER_TRIGGER,
                Action.POLL_PEER_PUBLICATION)
            : transition(Action.START_PEER_STANDBY);
      }
      case WAITING_EVIDENCE -> {
        Transition evaluated = evaluateEvidence();
        if (!evaluated.actions().isEmpty() || evaluated.snapshot().state() != State.WAITING_EVIDENCE) {
          yield evaluated;
        }
        peerAvailable = false;
        diagnostic = "recovering published evidence for existing shared session";
        yield peerArmed || peerArmUnconfirmed
            ? transition(
                Action.START_PEER_STANDBY,
                Action.POLL_PEER_TRIGGER,
                Action.POLL_PEER_PUBLICATION)
            : transition(Action.START_PEER_STANDBY);
      }
      case ADMITTING_PAIR -> {
        if (!durablePendingRecord) {
          yield terminal(
              "leader restart interrupted pair admission before its immutable record was durable");
        }
        peerAvailable = false;
        state = State.STORING_LOCAL_RECORD;
        diagnostic = "recovering durable admitted coordination record";
        yield transition(Action.START_PEER_STANDBY, Action.STORE_LOCAL_RECORD);
      }
      case STORING_LOCAL_RECORD -> {
        if (!durablePendingRecord) {
          yield terminal("durable coordination record is missing during local-store recovery");
        }
        peerAvailable = false;
        yield transition(Action.START_PEER_STANDBY, Action.STORE_LOCAL_RECORD);
      }
      case REPLICATING_RECORD -> {
        if (!durablePendingRecord) {
          yield terminal("durable coordination record is missing during replication recovery");
        }
        peerAvailable = false;
        yield transition(Action.START_PEER_STANDBY, Action.REPLICATE_RECORD_TO_PEER);
      }
      case REARMING -> {
        peerAvailable = false;
        yield transition(
            Action.START_PEER_STANDBY, Action.REARM_LOCAL, Action.REARM_PEER);
      }
      case STOPPING -> transition(Action.STOP_PEER_STANDBY);
    };
  }

  /** A background backlog conflict is terminal even after the originating swing was rearmed. */
  public synchronized Transition backlogReplicationConflict(String sharedSessionId) {
    requireIdentifier(sharedSessionId, "sharedSessionId");
    lastOutcome = LastOutcome.CONFLICT;
    lastCompletedSessionId = sharedSessionId;
    return terminal(
        "peer retains conflicting immutable coordination evidence for " + sharedSessionId);
  }

  /** Stops autonomous operation when its durability boundary cannot be proven. */
  public synchronized Transition durabilityFailed(String reason) {
    requireReason(reason);
    return terminal("autonomous-pair durability failure: " + reason);
  }

  public synchronized Transition startStation() {
    requireState(State.STOPPED, "station can start only while stopped");
    state = State.STARTING_STATION;
    diagnostic = "starting peer standby";
    return transition(Action.START_PEER_STANDBY);
  }

  public synchronized Transition peerStandbyStarted() {
    if (state == State.MONITORING) {
      return transition();
    }
    if (hasActiveSwing()) {
      peerAvailable = true;
      return transition();
    }
    if (state != State.STARTING_STATION && state != State.DEGRADED_MONITORING) {
      return terminal("peer standby acknowledgement arrived in " + state);
    }
    peerAvailable = true;
    peerArmed = false;
    peerArmUnconfirmed = false;
    state = State.MONITORING;
    diagnostic = "station monitoring";
    return transition();
  }

  public synchronized Transition peerUnavailable(String reason) {
    requireReason(reason);
    peerAvailable = false;
    if (state == State.STARTING_STATION || state == State.MONITORING) {
      peerArmed = false;
      peerArmUnconfirmed = false;
      state = State.DEGRADED_MONITORING;
      diagnostic = "peer unavailable: " + reason;
      return transition();
    }
    if (hasActiveSwing()) {
      diagnostic = "peer unavailable during " + activeSessionId + ": " + reason;
      return transition();
    }
    return transition();
  }

  public synchronized Transition retryPeer() {
    requireState(
        State.DEGRADED_MONITORING, "peer recovery can start only from degraded monitoring");
    state = State.STARTING_STATION;
    diagnostic = "retrying peer standby";
    return transition(Action.START_PEER_STANDBY);
  }

  public synchronized Transition claimSwing(String sharedSessionId, long nowNanos) {
    requireIdentifier(sharedSessionId, "sharedSessionId");
    if (nowNanos < 0) {
      throw new IllegalArgumentException("claim timestamp cannot be negative");
    }
    if (state != State.MONITORING && state != State.DEGRADED_MONITORING) {
      return terminal("new swing claimed while lifecycle is " + state);
    }
    clearActiveSwing();
    activeSessionId = sharedSessionId;
    // The request can take longer than the local warm transition. Until an explicit response
    // arrives, a missing acknowledgement cannot prove whether the peer applied the arm.
    peerArmUnconfirmed = true;
    evidenceDeadlineNanos = Math.addExact(nowNanos, config.evidenceDeadlineNanos());
    state = State.ARMING_SWING;
    diagnostic =
        peerAvailable ? "arming peer" : "retrying peer arm while retaining local evidence";
    return transition(Action.ARM_PEER_FOR_SWING);
  }

  public synchronized Transition peerArmAccepted(String sharedSessionId) {
    if (!matchesActive(sharedSessionId)) {
      return terminal("peer arm acknowledgement belongs to another shared session");
    }
    if (peerArmed && (state == State.CAPTURING || state == State.WAITING_EVIDENCE)) {
      return transition();
    }
    if (state == State.WAITING_EVIDENCE && localTriggered) {
      peerAvailable = true;
      peerArmed = true;
      peerArmUnconfirmed = false;
      diagnostic = "peer arm confirmed after local trigger; waiting for peer evidence";
      return transition(Action.POLL_PEER_TRIGGER, Action.POLL_PEER_PUBLICATION);
    }
    if (state != State.ARMING_SWING) {
      return terminal("peer arm acknowledgement arrived in " + state);
    }
    peerAvailable = true;
    peerArmed = true;
    peerArmUnconfirmed = false;
    state = State.CAPTURING;
    diagnostic = "both phones capturing";
    return transition();
  }

  public synchronized Transition peerArmFailed(String sharedSessionId, String reason) {
    requireReason(reason);
    if (!matchesActive(sharedSessionId)) {
      return terminal("peer arm failure belongs to another shared session");
    }
    if (state == State.WAITING_EVIDENCE && localTriggered) {
      peerAvailable = false;
      peerArmed = false;
      peerArmUnconfirmed = false;
      diagnostic = "late peer arm failure; retaining local evidence: " + reason;
      return localPublished ? evaluateEvidence() : transition();
    }
    if (state != State.ARMING_SWING) {
      return terminal("peer arm failure arrived in " + state);
    }
    peerAvailable = false;
    peerArmed = false;
    peerArmUnconfirmed = false;
    state = State.CAPTURING;
    diagnostic = "peer arm failed; retaining local evidence: " + reason;
    return transition();
  }

  /**
   * Records a transport failure for which the peer may have applied the arm request before its
   * acknowledgement was lost. Same-session trigger evidence can still prove that the peer armed;
   * the evidence deadline bounds how long the leader waits for that proof.
   */
  public synchronized Transition peerArmUnconfirmed(String sharedSessionId, String reason) {
    requireReason(reason);
    if (!matchesActive(sharedSessionId)) {
      return terminal("unconfirmed peer arm belongs to another shared session");
    }
    if (state != State.ARMING_SWING && state != State.WAITING_EVIDENCE) {
      return terminal("unconfirmed peer arm arrived in " + state);
    }
    peerAvailable = false;
    peerArmed = false;
    peerArmUnconfirmed = true;
    if (state == State.ARMING_SWING) {
      state = State.CAPTURING;
    }
    diagnostic =
        "peer arm acknowledgement unavailable; awaiting same-session evidence: " + reason;
    return transition();
  }

  public synchronized Transition localTriggered(
      String sharedSessionId, String capturedLocalSessionId) {
    if (!matchesActive(sharedSessionId)) {
      return terminal("local trigger belongs to another shared session");
    }
    requireIdentifier(capturedLocalSessionId, "capturedLocalSessionId");
    if (localTriggered) {
      return localSessionId.equals(capturedLocalSessionId)
          ? transition()
          : terminal("one shared session produced duplicate local clips");
    }
    if (state != State.CAPTURING && state != State.ARMING_SWING) {
      return terminal("local trigger arrived in " + state);
    }
    localTriggered = true;
    localSessionId = capturedLocalSessionId;
    state = State.WAITING_EVIDENCE;
    diagnostic =
        peerArmed || peerArmUnconfirmed
            ? "waiting for peer trigger and publication"
            : "waiting for local publication";
    return peerArmed || peerArmUnconfirmed
        ? transition(
            Action.SEND_PEER_IMPACT,
            Action.POLL_PEER_TRIGGER,
            Action.POLL_PEER_PUBLICATION)
        : transition();
  }

  public synchronized Transition peerTriggered(
      String sharedSessionId, String capturedPeerSessionId, boolean freshClockEvidence) {
    if (!matchesActive(sharedSessionId)) {
      return terminal("peer trigger belongs to another shared session");
    }
    requireIdentifier(capturedPeerSessionId, "capturedPeerSessionId");
    if (!peerArmed && !peerArmUnconfirmed) {
      return terminal("unarmed peer reported a trigger for the active session");
    }
    if (peerTriggered) {
      return peerSessionId.equals(capturedPeerSessionId)
          ? transition()
          : terminal("one shared session produced duplicate peer clips");
    }
    peerTriggered = true;
    peerSessionId = capturedPeerSessionId;
    peerAvailable = true;
    peerArmed = true;
    peerArmUnconfirmed = false;
    clockFresh = freshClockEvidence;
    state = State.WAITING_EVIDENCE;
    diagnostic = freshClockEvidence ? "waiting for publications" : "peer clock evidence is stale";
    return evaluateEvidence();
  }

  public synchronized Transition localPublished(
      String sharedSessionId, String capturedLocalSessionId) {
    if (!matchesActive(sharedSessionId) || !localSessionId.equals(capturedLocalSessionId)) {
      return terminal("local publication does not match the admitted local trigger");
    }
    localPublished = true;
    return evaluateEvidence();
  }

  public synchronized Transition peerPublished(
      String sharedSessionId, String capturedPeerSessionId) {
    if (!matchesActive(sharedSessionId) || !peerSessionId.equals(capturedPeerSessionId)) {
      return terminal("peer publication does not match the admitted peer trigger");
    }
    peerPublished = true;
    return evaluateEvidence();
  }

  public synchronized Transition tick(long nowNanos) {
    if (nowNanos < 0) {
      throw new IllegalArgumentException("tick timestamp cannot be negative");
    }
    if (!hasActiveSwing() || nowNanos < evidenceDeadlineNanos) {
      return transition();
    }
    if (state == State.ADMITTING_PAIR
        || state == State.STORING_LOCAL_RECORD
        || state == State.REPLICATING_RECORD
        || state == State.REARMING
        || state == State.TERMINAL_FAILURE) {
      return transition();
    }
    return completePartialEvidence("evidence deadline elapsed");
  }

  public synchronized Transition pairAdmitted(String sharedSessionId) {
    if (!matchesActive(sharedSessionId)) {
      return terminal("pair admission belongs to another shared session");
    }
    if (state != State.ADMITTING_PAIR) {
      return terminal("pair admission arrived in " + state);
    }
    state = State.STORING_LOCAL_RECORD;
    diagnostic = "storing immutable leader coordination record";
    return transition(Action.STORE_LOCAL_RECORD);
  }

  public synchronized Transition pairAdmissionFailed(String sharedSessionId, String reason) {
    requireReason(reason);
    if (!matchesActive(sharedSessionId)) {
      return terminal("pair-admission failure belongs to another shared session");
    }
    if (state != State.ADMITTING_PAIR) {
      return terminal("pair-admission failure arrived in " + state);
    }
    return terminal("pair admission failed: " + reason);
  }

  public synchronized Transition localRecordStored(
      String sharedSessionId, ImmutableStoreOutcome outcome) {
    Objects.requireNonNull(outcome, "outcome");
    if (!matchesActive(sharedSessionId)) {
      return terminal("local record result belongs to another shared session");
    }
    if (state != State.STORING_LOCAL_RECORD) {
      return terminal("local record result arrived in " + state);
    }
    if (outcome == ImmutableStoreOutcome.CONFLICT) {
      lastOutcome = LastOutcome.CONFLICT;
      return terminal("leader retains conflicting immutable coordination evidence");
    }
    if (outcome == ImmutableStoreOutcome.UNAVAILABLE) {
      return completePartialEvidence("leader coordination storage unavailable");
    }
    state = State.REPLICATING_RECORD;
    diagnostic = "replicating immutable coordination record to peer";
    return transition(Action.REPLICATE_RECORD_TO_PEER);
  }

  public synchronized Transition peerRecordStored(
      String sharedSessionId, ImmutableStoreOutcome outcome) {
    Objects.requireNonNull(outcome, "outcome");
    if (!matchesActive(sharedSessionId)) {
      return terminal("peer record result belongs to another shared session");
    }
    if (state != State.REPLICATING_RECORD) {
      return terminal("peer record result arrived in " + state);
    }
    if (outcome == ImmutableStoreOutcome.CONFLICT) {
      lastOutcome = LastOutcome.CONFLICT;
      return terminal("peer retains conflicting immutable coordination evidence");
    }
    if (outcome == ImmutableStoreOutcome.UNAVAILABLE) {
      peerAvailable = false;
      return finishAndRearm(LastOutcome.LOCAL_ONLY, "pair stored locally; peer replication pending");
    }
    return finishAndRearm(LastOutcome.PAIRED, "paired coordination evidence replicated");
  }

  public synchronized Transition rearmed(boolean peerReady) {
    requireState(State.REARMING, "rearm acknowledgement arrived outside rearming");
    peerAvailable = peerReady;
    peerArmed = false;
    peerArmUnconfirmed = false;
    clearActiveSwing();
    state = peerReady ? State.MONITORING : State.DEGRADED_MONITORING;
    diagnostic = peerReady ? "station monitoring" : "local monitoring; peer unavailable";
    return transition();
  }

  public synchronized Transition stopStation() {
    if (state == State.STOPPED) {
      return transition();
    }
    state = State.STOPPING;
    diagnostic = "stopping station";
    return transition(Action.STOP_PEER_STANDBY);
  }

  public synchronized Transition stopped() {
    requireState(State.STOPPING, "station stop acknowledgement arrived outside stopping");
    clearActiveSwing();
    peerAvailable = false;
    peerArmed = false;
    peerArmUnconfirmed = false;
    state = State.STOPPED;
    diagnostic = "station stopped";
    return transition();
  }

  public synchronized Snapshot snapshot() {
    return new Snapshot(
        state,
        activeSessionId,
        peerAvailable,
        peerArmed,
        localTriggered,
        peerTriggered,
        localPublished,
        peerPublished,
        clockFresh,
        lastOutcome,
        lastCompletedSessionId,
        diagnostic);
  }

  public synchronized Checkpoint checkpoint() {
    return new Checkpoint(
        Checkpoint.SCHEMA_VERSION,
        state,
        activeSessionId,
        localSessionId,
        peerSessionId,
        peerAvailable,
        peerArmed,
        peerArmUnconfirmed,
        localTriggered,
        peerTriggered,
        localPublished,
        peerPublished,
        clockFresh,
        evidenceDeadlineNanos,
        lastOutcome,
        lastCompletedSessionId,
        diagnostic);
  }

  private Transition evaluateEvidence() {
    if (localTriggered && localPublished && !peerArmed && !peerArmUnconfirmed) {
      return finishAndRearm(LastOutcome.LOCAL_ONLY, "local evidence retained without peer arm");
    }
    if (localTriggered
        && peerTriggered
        && localPublished
        && peerPublished) {
      if (!clockFresh) {
        return finishAndRearm(
            LastOutcome.LOCAL_ONLY,
            "both clips retained but stale peer clock evidence prevents pair admission");
      }
      state = State.ADMITTING_PAIR;
      diagnostic = "validating dual trigger association";
      return transition(Action.ADMIT_PAIR);
    }
    return transition();
  }

  private Transition completePartialEvidence(String reason) {
    LastOutcome outcome;
    if (localPublished) {
      outcome = LastOutcome.LOCAL_ONLY;
    } else if (peerPublished) {
      outcome = LastOutcome.PEER_ONLY;
    } else {
      outcome = LastOutcome.NO_EVIDENCE;
    }
    return finishAndRearm(outcome, reason);
  }

  private Transition finishAndRearm(LastOutcome outcome, String reason) {
    lastOutcome = outcome;
    lastCompletedSessionId = activeSessionId;
    diagnostic = reason;
    state = State.REARMING;
    EnumSet<Action> actions = EnumSet.of(Action.REARM_LOCAL);
    if (peerAvailable) {
      actions.add(Action.REARM_PEER);
    }
    return transition(actions);
  }

  private Transition terminal(String reason) {
    lastOutcome = LastOutcome.CONFLICT;
    diagnostic = reason;
    state = State.TERMINAL_FAILURE;
    return transition();
  }

  private boolean hasActiveSwing() {
    return !activeSessionId.isEmpty();
  }

  private boolean matchesActive(String sharedSessionId) {
    return sharedSessionId != null && sharedSessionId.equals(activeSessionId);
  }

  private void clearActiveSwing() {
    activeSessionId = "";
    localSessionId = "";
    peerSessionId = "";
    localTriggered = false;
    peerTriggered = false;
    peerArmUnconfirmed = false;
    localPublished = false;
    peerPublished = false;
    clockFresh = false;
    evidenceDeadlineNanos = 0;
  }

  private void requireState(State expected, String message) {
    if (state != expected) {
      throw new IllegalStateException(message + ": " + state);
    }
  }

  private static String requireOptionalIdentifier(String value, String name) {
    Objects.requireNonNull(value, name);
    if (!value.isEmpty()) {
      requireIdentifier(value, name);
    }
    return value;
  }

  private static void requireIdentifier(String value, String name) {
    if (value == null || !value.matches("[A-Za-z0-9._-]{1,128}")) {
      throw new IllegalArgumentException(name + " is invalid");
    }
  }

  private static void requireReason(String reason) {
    if (reason == null || reason.isBlank()) {
      throw new IllegalArgumentException("reason is required");
    }
  }

  private Transition transition(Action... actions) {
    return transition(actions.length == 0 ? EnumSet.noneOf(Action.class) : EnumSet.of(actions[0], actions));
  }

  private Transition transition(Set<Action> actions) {
    return new Transition(snapshot(), checkpoint(), actions);
  }
}
