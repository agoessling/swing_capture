package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.AutonomousPairLifecycle;
import com.agoessling.swingcapture.core.coordination.CaptureRole;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Optional;

/** Filesystem-level restart and immutable-backlog coverage. */
public final class AutonomousPairDurableStoreTest {
  private AutonomousPairDurableStoreTest() {}

  public static void main(String[] arguments) throws Exception {
    checkpointRoundTripsArmCaptureAndPublication();
    durablePendingRecordSurvivesProcessRestart();
    backlogRetriesIdempotentlyAcrossStoreInstances();
    conflictingBacklogRecordNeverOverwritesWinner();
    corruptCheckpointFailsClosed();
  }

  private static void checkpointRoundTripsArmCaptureAndPublication() throws Exception {
    Path temporary = Files.createTempDirectory("autonomous-pair-checkpoint-");
    AutonomousPairDurableStore store = store(temporary);
    AutonomousPairLifecycle lifecycle = started();
    lifecycle.claimSwing("restart-arm", 100);
    store.saveCheckpoint(lifecycle.checkpoint(), Optional.empty());
    check(
        reload(temporary).checkpoint().activeSessionId().equals("restart-arm"),
        "arm shared session survives restart");

    lifecycle.peerArmAccepted("restart-arm");
    store.saveCheckpoint(lifecycle.checkpoint(), Optional.empty());
    check(
        reload(temporary).checkpoint().state()
            == AutonomousPairLifecycle.State.CAPTURING,
        "capture phase survives restart");

    lifecycle.localTriggered("restart-arm", "local-restart");
    lifecycle.localPublished("restart-arm", "local-restart");
    store.saveCheckpoint(lifecycle.checkpoint(), Optional.empty());
    AutonomousPairDurableStore.Recovery publication = reload(temporary);
    check(publication.checkpoint().localPublished(), "publication flag survives restart");
    check(
        publication.checkpoint().localSessionId().equals("local-restart"),
        "published local clip ID survives restart");
  }

  private static void durablePendingRecordSurvivesProcessRestart() throws Exception {
    Path temporary = Files.createTempDirectory("autonomous-pair-pending-");
    AutonomousPairLifecycle lifecycle = readyToStore("restart-record");
    PairedCoordinationRecord record = record("restart-record", 1_700_000_000_000L);
    store(temporary).saveCheckpoint(lifecycle.checkpoint(), Optional.of(record));
    AutonomousPairDurableStore.Recovery recovered = reload(temporary);
    check(recovered.pendingRecord().orElseThrow().equals(record), "pending record round trip");
    AutonomousPairLifecycle restored =
        AutonomousPairLifecycle.restore(config(), recovered.checkpoint());
    check(
        restored.recover(true, 200).requests(
            AutonomousPairLifecycle.Action.STORE_LOCAL_RECORD),
        "restart replays idempotent local store");
  }

  private static void backlogRetriesIdempotentlyAcrossStoreInstances() throws Exception {
    Path temporary = Files.createTempDirectory("autonomous-pair-backlog-");
    PairedCoordinationRecord record = record("backlog-retry", 1_700_000_000_000L);
    AutonomousPairDurableStore first = store(temporary);
    check(
        first.enqueue(record) == AutonomousPairDurableStore.EnqueueStatus.STORED,
        "first enqueue stores record");
    AutonomousPairDurableStore restarted = store(temporary);
    check(restarted.backlog().equals(java.util.List.of(record)), "backlog survives restart");
    check(
        restarted.enqueue(record) == AutonomousPairDurableStore.EnqueueStatus.ALREADY_PRESENT,
        "same immutable record is idempotent");
    restarted.markReplicated(record);
    check(store(temporary).backlog().isEmpty(), "replication acknowledgement is durable");
    // A lost local acknowledgement can replay the same completion safely.
    store(temporary).markReplicated(record);
  }

  private static void conflictingBacklogRecordNeverOverwritesWinner() throws Exception {
    Path temporary = Files.createTempDirectory("autonomous-pair-conflict-");
    AutonomousPairDurableStore store = store(temporary);
    PairedCoordinationRecord winner = record("backlog-conflict", 1_700_000_000_000L);
    PairedCoordinationRecord conflicting = record("backlog-conflict", 1_700_000_000_001L);
    store.enqueue(winner);
    check(
        store.enqueue(conflicting) == AutonomousPairDurableStore.EnqueueStatus.CONFLICT,
        "conflicting enqueue fails closed");
    check(store.backlog().equals(java.util.List.of(winner)), "winner is never overwritten");
    expectFailure(() -> store.markReplicated(conflicting), "conflicting completion rejected");
    check(store.backlog().equals(java.util.List.of(winner)), "conflict retains local evidence");
  }

  private static void corruptCheckpointFailsClosed() throws Exception {
    Path temporary = Files.createTempDirectory("autonomous-pair-corrupt-");
    File root = temporary.resolve("state").toFile();
    check(root.mkdir(), "create corrupt root");
    Files.write(
        new File(root, "checkpoint.bin").toPath(),
        "not a checkpoint".getBytes(java.nio.charset.StandardCharsets.UTF_8));
    expectFailure(() -> store(temporary).loadCheckpoint(), "corrupt checkpoint rejected");
  }

  private static AutonomousPairDurableStore.Recovery reload(Path base) throws Exception {
    return store(base).loadCheckpoint().orElseThrow();
  }

  private static AutonomousPairDurableStore store(Path base) throws Exception {
    return new AutonomousPairDurableStore(base.resolve("state").toFile(), ignored -> {});
  }

  private static AutonomousPairLifecycle started() {
    AutonomousPairLifecycle lifecycle = new AutonomousPairLifecycle(config());
    lifecycle.startStation();
    lifecycle.peerStandbyStarted();
    return lifecycle;
  }

  private static AutonomousPairLifecycle readyToStore(String sharedSessionId) {
    AutonomousPairLifecycle lifecycle = started();
    lifecycle.claimSwing(sharedSessionId, 100);
    lifecycle.peerArmAccepted(sharedSessionId);
    lifecycle.localTriggered(sharedSessionId, "local");
    lifecycle.peerTriggered(sharedSessionId, "peer", true);
    lifecycle.localPublished(sharedSessionId, "local");
    lifecycle.peerPublished(sharedSessionId, "peer");
    lifecycle.pairAdmitted(sharedSessionId);
    return lifecycle;
  }

  private static AutonomousPairLifecycle.Config config() {
    return new AutonomousPairLifecycle.Config(10_000);
  }

  private static PairedCoordinationRecord record(String sessionId, long recordedAt) {
    PairedCoordinationRecord.NodeEvidence downTheLine =
        new PairedCoordinationRecord.NodeEvidence(
            CaptureRole.DOWN_THE_LINE,
            "dtl-node",
            "dtl-clip",
            1_000_000,
            100,
            1_000_000,
            100,
            0,
            0,
            10,
            20,
            3,
            "local_audio");
    PairedCoordinationRecord.NodeEvidence faceOn =
        new PairedCoordinationRecord.NodeEvidence(
            CaptureRole.FACE_ON,
            "face-node",
            "face-clip",
            1_001_000,
            100,
            1_001_000,
            100,
            0,
            0,
            10,
            20,
            3,
            "peer_audio");
    return PairedCoordinationRecord.create(sessionId, recordedAt, downTheLine, faceOn);
  }

  private static void expectFailure(ThrowingRunnable action, String label) throws Exception {
    try {
      action.run();
      throw new AssertionError(label);
    } catch (java.io.IOException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }

  @FunctionalInterface
  private interface ThrowingRunnable {
    void run() throws Exception;
  }
}
