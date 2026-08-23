package com.agoessling.swingcapture.standby;

import com.agoessling.swingcapture.audio.AudioTimestampMapper;
import com.agoessling.swingcapture.audio.ImpactDetector;
import com.agoessling.swingcapture.diagnostics.PreviewEvidence;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import java.util.concurrent.atomic.AtomicInteger;

/** Deterministic event lifecycle, bounds, discontinuity, and timing coverage. */
public final class StandbyDiagnosticCoordinatorTest {
  private StandbyDiagnosticCoordinatorTest() {}

  public static void main(String[] arguments) {
    defaultsAreFixedSixtySecondAudioAndTenTwoWindow();
    detectedImpactWaitsForPostRollAndFreezesExactEvidence();
    detectedImpactWaitsForValidatedClockAndThenFreezes();
    operatorTagUsesLatestReceiptFrameWithoutGuessingImpact();
    livePreviewSupplierRunsOnlyForConfirmedImpact();
    pendingEventsAreBoundedAtFour();
    discontinuityDropsPendingEvidenceAndRecoversAtNewPosition();
    malformedInputsAreRejected();
  }

  private static void defaultsAreFixedSixtySecondAudioAndTenTwoWindow() {
    StandbyDiagnosticCoordinator coordinator = new StandbyDiagnosticCoordinator();
    check(
        StandbyDiagnosticCoordinator.RETENTION_FRAMES == 60 * 48_000,
        "fixed 60-second capacity");
    check(
        coordinator.config().preRollFrames() == 10 * 48_000,
        "default ten-second pre-roll");
    check(
        coordinator.config().postRollFrames() == 2 * 48_000,
        "default two-second post-roll");
    check(
        coordinator.config().maximumPendingEvents() == 4,
        "default four pending events");
  }

  private static void detectedImpactWaitsForPostRollAndFreezesExactEvidence() {
    StandbyDiagnosticCoordinator coordinator = coordinator(10, 5, 4);
    coordinator.observeAudioTimestamp(0, 1_000_000_000L, 1_000);
    coordinator.observeAudioTimestamp(48_000, 2_000_000_000L, 1_000);
    PreviewEvidenceRing.Snapshot preview = previewSnapshot(100);
    short[] first = new short[8];
    first[5] = pcm(0.8f);

    StandbyDiagnosticCoordinator.AppendResult detected =
        coordinator.appendPcm16(first, 0, first.length, 0, preview);

    check(detected.detectorMetrics().eventsDetected() == 1, "detector event count");
    check(detected.registeredEvents().size() == 1, "registered impact count");
    check(detected.frozenEvidence().isEmpty(), "impact waits for post-roll");
    StandbyDiagnosticCoordinator.EventMarker event = detected.registeredEvents().get(0);
    check(event.sequence() == 1, "first event sequence");
    check(
        event.kind() == StandbyDiagnosticCoordinator.EventKind.DETECTED_IMPACT,
        "detected event kind");
    check(event.markerFramePosition() == 5, "detected strike marker");
    check(event.previewSnapshot() == preview, "preview snapshot frozen at registration");
    check(
        event.audioClockStatus() == StandbyDiagnosticCoordinator.AudioClockStatus.VALIDATED,
        "validated audio clock");
    check(
        event.markerTime().orElseThrow().boottimeNanos() == 1_000_104_167L,
        "mapped strike BOOTTIME");

    StandbyDiagnosticCoordinator.AppendResult completed =
        coordinator.appendPcm16(new short[2], 0, 2, 8, previewSnapshot(200));

    check(completed.registeredEvents().isEmpty(), "no second impact");
    check(completed.frozenEvidence().size() == 1, "post-roll completes evidence");
    StandbyDiagnosticCoordinator.FrozenEvidence frozen = completed.frozenEvidence().get(0);
    check(frozen.event().equals(event), "same registered event freezes");
    check(frozen.audioSnapshot().firstFramePosition() == 0, "startup pre-roll clipped to zero");
    check(frozen.audioSnapshot().endFramePosition() == 10, "exact post-roll end");
    check(frozen.audioSnapshot().frameCount() == 10, "exact frozen audio count");
    check(frozen.audioSnapshot().sampleAt(5) == first[5], "impact PCM retained exactly");
    check(coordinator.pendingEventCount() == 0, "completed event removed");
  }

  private static void detectedImpactWaitsForValidatedClockAndThenFreezes() {
    StandbyDiagnosticCoordinator coordinator = coordinator(2, 2, 4);
    short[] pcm = new short[8];
    pcm[3] = pcm(0.9f);

    StandbyDiagnosticCoordinator.AppendResult detected =
        coordinator.appendPcm16(pcm, 0, pcm.length, 50, previewSnapshot(10));

    check(detected.detectorMetrics().eventsDetected() == 1, "untimed detector event observed");
    check(detected.registeredEvents().isEmpty(), "untimed event is withheld from consumers");
    check(detected.frozenEvidence().isEmpty(), "untimed event cannot freeze evidence early");

    coordinator.observeAudioTimestamp(0, 1_000_000_000L, 1_000);
    coordinator.observeAudioTimestamp(48_000, 2_000_000_000L, 1_000);
    StandbyDiagnosticCoordinator.AppendResult recovered =
        coordinator.appendPcm16(new short[1], 0, 1, 58, previewSnapshot(20));

    check(recovered.registeredEvents().size() == 1, "timed event registered after recovery");
    StandbyDiagnosticCoordinator.EventMarker event = recovered.registeredEvents().get(0);
    check(event.markerTime().isPresent(), "recovered marker estimate present");
    check(
        event.audioClockStatus() == StandbyDiagnosticCoordinator.AudioClockStatus.VALIDATED,
        "recovered explicit clock status");
    check(recovered.frozenEvidence().size() == 1, "recovered evidence freezes after validation");
  }

  private static void operatorTagUsesLatestReceiptFrameWithoutGuessingImpact() {
    StandbyDiagnosticCoordinator coordinator = coordinator(10, 5, 4);
    PreviewEvidenceRing.Snapshot preview = previewSnapshot(500);
    coordinator.appendPcm16(new short[20], 0, 20, 100, previewSnapshot(100));

    StandbyDiagnosticCoordinator.OperatorTagResult tagged =
        coordinator.tagOperator(9_000_000_000L, preview);

    check(tagged.accepted(), "operator tag accepted");
    check(tagged.event().kind() == StandbyDiagnosticCoordinator.EventKind.OPERATOR_TAG,
        "operator event kind");
    check(tagged.event().markerFramePosition() == 119, "button uses latest recorded frame");
    check(
        tagged.event().operatorReceivedBoottimeNanos().orElseThrow() == 9_000_000_000L,
        "operator receipt BOOTTIME");
    check(tagged.event().detectedImpact().isEmpty(), "operator marker is not guessed impact");
    check(tagged.event().previewSnapshot() == preview, "operator preview frozen");

    StandbyDiagnosticCoordinator.AppendResult completed =
        coordinator.appendPcm16(new short[4], 0, 4, 120, previewSnapshot(600));
    check(completed.frozenEvidence().size() == 1, "operator post-roll completes");
    check(
        completed.frozenEvidence().get(0).audioSnapshot().firstFramePosition() == 109,
        "operator exact pre-roll");
    check(
        completed.frozenEvidence().get(0).audioSnapshot().endFramePosition() == 124,
        "operator exact post-roll");
  }

  private static void pendingEventsAreBoundedAtFour() {
    StandbyDiagnosticCoordinator coordinator = coordinator(10, 100, 4);
    coordinator.appendPcm16(new short[20], 0, 20, 0, previewSnapshot(1));
    for (int index = 0; index < 4; ++index) {
      StandbyDiagnosticCoordinator.OperatorTagResult accepted =
          coordinator.tagOperator(1_000L + index, previewSnapshot(10 + index));
      check(accepted.accepted(), "pending tag accepted " + index);
    }

    StandbyDiagnosticCoordinator.OperatorTagResult rejected =
        coordinator.tagOperator(2_000L, previewSnapshot(20));

    check(!rejected.accepted(), "fifth pending tag rejected");
    check(rejected.droppedEvents().size() == 1, "pending-limit drop emitted");
    check(
        rejected.droppedEvents().get(0).reason()
            == StandbyDiagnosticCoordinator.DropReason.PENDING_LIMIT,
        "pending-limit reason");
    check(coordinator.pendingEventCount() == 4, "pending count stays bounded");
  }

  private static void livePreviewSupplierRunsOnlyForConfirmedImpact() {
    StandbyDiagnosticCoordinator coordinator = coordinator(2, 2, 4);
    coordinator.observeAudioTimestamp(0, 1_000_000_000L, 1_000);
    coordinator.observeAudioTimestamp(48_000, 2_000_000_000L, 1_000);
    AtomicInteger snapshots = new AtomicInteger();
    coordinator.appendPcm16(
        new short[4],
        0,
        4,
        0,
        () -> {
          snapshots.incrementAndGet();
          return previewSnapshot(1);
        });
    check(snapshots.get() == 0, "quiet read does not snapshot preview");
    short[] impact = new short[8];
    impact[1] = pcm(0.9f);
    coordinator.appendPcm16(
        impact,
        0,
        impact.length,
        4,
        () -> {
          snapshots.incrementAndGet();
          return previewSnapshot(2);
        });
    check(snapshots.get() == 1, "confirmed impact snapshots preview once");
  }

  private static void discontinuityDropsPendingEvidenceAndRecoversAtNewPosition() {
    StandbyDiagnosticCoordinator coordinator = coordinator(10, 100, 4);
    coordinator.observeAudioTimestamp(0, 1_000_000_000L, 1_000);
    coordinator.observeAudioTimestamp(48_000, 2_000_000_000L, 1_000);
    coordinator.appendPcm16(new short[10], 0, 10, 0, previewSnapshot(1));
    coordinator.tagOperator(1_100_000_000L, previewSnapshot(2));

    StandbyDiagnosticCoordinator.AppendResult recovered =
        coordinator.appendPcm16(new short[5], 0, 5, 20, previewSnapshot(3));

    check(recovered.discontinuity().isPresent(), "gap reported");
    check(
        recovered.discontinuity().orElseThrow().kind()
            == com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing.DiscontinuityKind.GAP,
        "typed gap kind");
    check(recovered.discontinuity().orElseThrow().expectedFirstFramePosition() == 10,
        "gap expected frame");
    check(recovered.discontinuity().orElseThrow().receivedFirstFramePosition() == 20,
        "gap received frame");
    check(recovered.droppedEvents().size() == 1, "pending event dropped on gap");
    check(
        recovered.droppedEvents().get(0).reason()
            == StandbyDiagnosticCoordinator.DropReason.AUDIO_DISCONTINUITY,
        "gap drop reason");
    check(coordinator.pendingEventCount() == 0, "gap clears pending events");
    check(coordinator.retainedAudioFirstFramePosition() == 20, "ring restarts at gap block");
    check(coordinator.retainedAudioEndFramePosition() == 25, "gap block retained");
    StandbyDiagnosticCoordinator.OperatorTagResult afterGap =
        coordinator.tagOperator(2_000_000_000L, previewSnapshot(4));
    check(
        afterGap.event().audioClockStatus()
            == StandbyDiagnosticCoordinator.AudioClockStatus.UNVALIDATED,
        "gap resets timestamp validation");
  }

  private static void malformedInputsAreRejected() {
    expectThrows(
        IllegalArgumentException.class,
        () -> new StandbyDiagnosticCoordinator.Config(-1, 1, 1),
        "negative pre-roll");
    expectThrows(
        IllegalArgumentException.class,
        () ->
            new StandbyDiagnosticCoordinator.Config(
                StandbyDiagnosticCoordinator.RETENTION_FRAMES, 1, 1),
        "window exceeds ring");
    expectThrows(
        IllegalArgumentException.class,
        () -> new StandbyDiagnosticCoordinator.Config(1, 1, 5),
        "pending count over hard cap");
    StandbyDiagnosticCoordinator coordinator = coordinator(1, 1, 1);
    expectThrows(
        IllegalArgumentException.class,
        () -> coordinator.appendPcm16(new short[1], 0, 0, 0, previewSnapshot(1)),
        "empty audio append");
    expectThrows(
        IllegalStateException.class,
        () -> coordinator.tagOperator(1, previewSnapshot(1)),
        "tag before audio");
  }

  private static StandbyDiagnosticCoordinator coordinator(
      int preRollFrames, int postRollFrames, int maximumPending) {
    return new StandbyDiagnosticCoordinator(
        new StandbyDiagnosticCoordinator.Config(preRollFrames, postRollFrames, maximumPending),
        new ImpactDetector.Config(8.0f, 0.05f, 0.005f, 4.0f, 0.5, 2, 100),
        new AudioTimestampMapper.Config(8, 1_000_000L, 1_000.0, 750_000_000L));
  }

  private static PreviewEvidenceRing.Snapshot previewSnapshot(long timestamp) {
    PreviewEvidenceRing ring = new PreviewEvidenceRing(1_000, 1, 100);
    ring.append(
        new PreviewEvidence(
            timestamp,
            new byte[] {1},
            "model",
            1,
            0.5,
            0.5,
            0.0,
            true,
            PreviewEvidence.ControllerState.MONITORING,
            "waiting"));
    return ring.snapshot();
  }

  private static short pcm(float amplitude) {
    return (short) (amplitude * 32_767.0f);
  }

  private static <T extends Throwable> void expectThrows(
      Class<T> expected, Runnable action, String message) {
    try {
      action.run();
    } catch (Throwable thrown) {
      if (expected.isInstance(thrown)) {
        return;
      }
      throw new AssertionError(message + " threw " + thrown, thrown);
    }
    throw new AssertionError(message + " did not throw " + expected.getSimpleName());
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
