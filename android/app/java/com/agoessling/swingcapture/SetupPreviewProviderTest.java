package com.agoessling.swingcapture;

import com.agoessling.swingcapture.SetupPreviewProvider.OfferDisposition;
import com.agoessling.swingcapture.SetupPreviewProvider.Reason;
import com.agoessling.swingcapture.SetupPreviewProvider.State;
import com.agoessling.swingcapture.diagnostics.PreviewEvidence;
import java.util.ArrayDeque;
import java.util.concurrent.Executor;

/** Deterministic cadence, latest-only, byte-bound, staleness, and lifecycle coverage. */
public final class SetupPreviewProviderTest {
  private static final SetupPreviewProvider.Config CONFIG =
      new SetupPreviewProvider.Config(1_000, 3_000, 64, 16, 70);

  private SetupPreviewProviderTest() {}

  public static void main(String[] arguments) {
    latestPendingFrameWinsAndCadenceBoundsCopiesAndJpegWork();
    snapshotsAreImmutableAndBecomeStaleAtTheExactBoundary();
    sourceTransitionsInvalidateDetachedOldGenerationWork();
    evidenceFramesReuseExistingJpegWithoutEncoding();
    asynchronousEncodingCannotRegressAnEvidenceFrame();
    failuresAndMalformedInputsAreExplicitAndNonDestructive();
    byteAndTimestampBoundsFailBeforeUnboundedWork();
    closeIsTerminalAndReleasesPendingMemory();
  }

  private static void latestPendingFrameWinsAndCadenceBoundsCopiesAndJpegWork() {
    ManualExecutor executor = new ManualExecutor();
    RecordingEncoder encoder = new RecordingEncoder();
    SetupPreviewProvider provider = new SetupPreviewProvider(CONFIG, encoder, executor);
    check(
        provider.snapshot(0).reason() == Reason.STARTING,
        "initial preview is explicitly starting");
    provider.markSourceAvailable();

    byte[] first = nv21(2, 2, 1);
    check(
        provider.offerNv21(100, 2, 2, first).disposition() == OfferDisposition.SCHEDULED,
        "first frame schedules one worker");
    first[0] = 99;
    check(
        provider.offerNv21(200, 2, 2, nv21(2, 2, 2)).disposition()
            == OfferDisposition.SKIPPED_CADENCE,
        "sub-cadence frame is skipped before copying or encoding");
    check(
        provider.offerNv21(1_100, 2, 2, nv21(2, 2, 3)).disposition()
            == OfferDisposition.QUEUED_LATEST,
        "one pending frame is retained");
    check(
        provider.offerNv21(2_100, 2, 2, nv21(2, 2, 4)).disposition()
            == OfferDisposition.REPLACED_PENDING,
        "newest pending frame replaces the older pending frame");
    check(executor.size() == 1, "exactly one serial drain is scheduled");
    check(provider.snapshot(2_100).metrics().retainedWorkCount() == 2, "work bound is two");

    executor.runNext();
    check(encoder.calls == 2, "only current and latest pending frames are encoded");
    check(encoder.firstInputByte == 1, "provider detached the current input from its caller");
    check(encoder.lastInputByte == 4, "latest pending input wins");
    SetupPreviewProvider.Snapshot snapshot = provider.snapshot(2_100);
    check(snapshot.state() == State.AVAILABLE, "latest encoded frame is available");
    check(snapshot.frameTimestampBoottimeNanos() == 2_100, "latest timestamp is published");
    check(snapshot.metrics().offeredFrames() == 4, "all offers counted");
    check(snapshot.metrics().cadenceSkippedFrames() == 1, "cadence skip counted");
    check(snapshot.metrics().replacedPendingFrames() == 1, "replacement counted");
    check(snapshot.metrics().encodingAttempts() == 2, "bounded attempts counted");
    check(snapshot.metrics().retainedWorkCount() == 0, "drain releases all raw input memory");
    provider.close();
  }

  private static void snapshotsAreImmutableAndBecomeStaleAtTheExactBoundary() {
    ManualExecutor executor = new ManualExecutor();
    SetupPreviewProvider provider =
        new SetupPreviewProvider(CONFIG, new RecordingEncoder(), executor);
    provider.markSourceAvailable();
    provider.offerNv21(10_000, 2, 2, nv21(2, 2, 7));
    executor.runNext();

    SetupPreviewProvider.Snapshot boundary = provider.snapshot(13_000);
    check(boundary.state() == State.AVAILABLE, "maximum age boundary is inclusive");
    byte[] exported = boundary.jpeg();
    exported[0] = 0;
    check((boundary.jpeg()[0] & 0xff) == 0xff, "snapshot JPEG accessor is detached");
    SetupPreviewProvider.Snapshot stale = provider.snapshot(13_001);
    check(stale.state() == State.STALE && stale.reason() == Reason.STALE, "stale is explicit");
    check(stale.hasFrame(), "stale snapshot retains bounded diagnostic pixels");
    provider.close();
  }

  private static void sourceTransitionsInvalidateDetachedOldGenerationWork() {
    ManualExecutor executor = new ManualExecutor();
    SetupPreviewProvider provider =
        new SetupPreviewProvider(CONFIG, new RecordingEncoder(), executor);
    provider.markSourceAvailable();
    long firstGeneration = provider.offerNv21(1_000, 2, 2, nv21(2, 2, 1)).generation();
    provider.markSourceUnavailable(Reason.HIGH_SPEED_CAPTURE);
    check(
        provider.snapshot(1_000).reason() == Reason.HIGH_SPEED_CAPTURE,
        "high-speed ownership is explicit");
    executor.runNext();
    check(!provider.snapshot(1_000).hasFrame(), "old detached work cannot publish after transfer");

    provider.markSourceAvailable();
    SetupPreviewProvider.OfferResult resumed =
        provider.offerNv21(1_001, 2, 2, nv21(2, 2, 2));
    check(resumed.generation() > firstGeneration, "source resume advances generation");
    executor.runNext();
    check(provider.snapshot(1_001).state() == State.AVAILABLE, "new source generation publishes");
    provider.markSourceUnavailable(Reason.POSE_DISABLED);
    check(provider.snapshot(1_001).reason() == Reason.POSE_DISABLED, "disabled pose is explicit");
    check(!provider.snapshot(1_001).hasFrame(), "source stop releases published JPEG memory");
    provider.close();
  }

  private static void evidenceFramesReuseExistingJpegWithoutEncoding() {
    RecordingEncoder encoder = new RecordingEncoder();
    SetupPreviewProvider provider = new SetupPreviewProvider(CONFIG, encoder, Runnable::run);
    provider.markSourceAvailable();
    PreviewEvidence withoutJpeg = evidence(1_000, new byte[0]);
    check(
        provider.offerEvidence(withoutJpeg).disposition()
            == OfferDisposition.REJECTED_NO_ENCODED_FRAME,
        "trace-only evidence is not advertised as a preview");
    PreviewEvidence withJpeg = evidence(2_000, jpeg(8));
    check(provider.offerEvidence(withJpeg).accepted(), "existing evidence JPEG is published");
    check(encoder.calls == 0, "evidence reuse performs no duplicate JPEG work");
    check(provider.snapshot(2_000).jpeg()[2] == 8, "exact evidence JPEG is retained");
    provider.close();
  }

  private static void asynchronousEncodingCannotRegressAnEvidenceFrame() {
    ManualExecutor executor = new ManualExecutor();
    SetupPreviewProvider provider =
        new SetupPreviewProvider(CONFIG, new RecordingEncoder(), executor);
    provider.markSourceAvailable();
    provider.offerNv21(1_000, 2, 2, nv21(2, 2, 1));
    check(
        provider.offerEvidence(evidence(2_000, jpeg(2))).disposition()
            == OfferDisposition.PUBLISHED_ENCODED,
        "a newer encoded evidence frame publishes without waiting for raw work");
    executor.runNext();
    check(
        provider.snapshot(2_000).frameTimestampBoottimeNanos() == 2_000,
        "older asynchronous work cannot replace a newer evidence frame");
    expectIllegalArgument(
        () -> provider.offerNv21(1_500, 2, 2, nv21(2, 2, 3)),
        "timestamp ordering is shared by raw and evidence inputs");
    provider.close();
  }

  private static void failuresAndMalformedInputsAreExplicitAndNonDestructive() {
    ManualExecutor executor = new ManualExecutor();
    RecordingEncoder encoder = new RecordingEncoder();
    SetupPreviewProvider provider = new SetupPreviewProvider(CONFIG, encoder, executor);
    provider.markSourceAvailable();
    expectIllegalArgument(() -> provider.offerNv21(-1, 2, 2, nv21(2, 2, 1)), "negative time");
    expectIllegalArgument(() -> provider.offerNv21(1, 3, 2, new byte[9]), "odd width");
    expectIllegalArgument(() -> provider.offerNv21(1, 2, 2, new byte[5]), "wrong NV21 bytes");

    encoder.fail = true;
    provider.offerNv21(1_000, 2, 2, nv21(2, 2, 1));
    executor.runNext();
    SetupPreviewProvider.Snapshot failed = provider.snapshot(1_000);
    check(failed.state() == State.UNAVAILABLE, "encoder failure leaves no fake preview");
    check(failed.reason() == Reason.ENCODING_FAILED, "encoder failure reason is explicit");
    check(failed.metrics().encodingFailures() == 1, "encoder failure counted");

    provider.markSourceAvailable();
    check(
        provider.offerEvidence(evidence(2_000, new byte[] {1, 2, 3, 4})).disposition()
            == OfferDisposition.REJECTED_INVALID_ENCODED_FRAME,
        "malformed evidence JPEG is rejected");
    check(provider.snapshot(2_000).reason() == Reason.ENCODING_FAILED, "malformed JPEG is visible");
    provider.close();
  }

  private static void byteAndTimestampBoundsFailBeforeUnboundedWork() {
    ManualExecutor executor = new ManualExecutor();
    RecordingEncoder encoder = new RecordingEncoder();
    SetupPreviewProvider provider = new SetupPreviewProvider(CONFIG, encoder, executor);
    provider.markSourceAvailable();
    expectIllegalArgument(
        () -> provider.offerNv21(1, 8, 8, nv21(8, 8, 1)), "oversize raw frame");
    provider.offerNv21(1_000, 2, 2, nv21(2, 2, 1));
    provider.offerNv21(1_100, 2, 2, nv21(2, 2, 2));
    expectIllegalArgument(
        () -> provider.offerNv21(1_050, 2, 2, nv21(2, 2, 3)),
        "timestamps remain monotonic across cadence skips");
    executor.runNext();

    encoder.oversize = true;
    provider.offerNv21(2_000, 2, 2, nv21(2, 2, 4));
    executor.runNext();
    SetupPreviewProvider.Snapshot retained = provider.snapshot(2_000);
    check(
        retained.frameTimestampBoottimeNanos() == 1_000,
        "oversize JPEG cannot replace good frame");
    check(retained.metrics().encodingFailures() == 1, "oversize JPEG failure is counted");

    provider.markSourceAvailable();
    byte[] oversizeJpeg = new byte[17];
    oversizeJpeg[0] = (byte) 0xff;
    oversizeJpeg[1] = (byte) 0xd8;
    oversizeJpeg[15] = (byte) 0xff;
    oversizeJpeg[16] = (byte) 0xd9;
    check(
        provider.offerEvidence(evidence(3_000, oversizeJpeg)).disposition()
            == OfferDisposition.REJECTED_INVALID_ENCODED_FRAME,
        "oversize evidence is rejected before publication");
    provider.close();

    expectIllegalArgument(
        () -> new SetupPreviewProvider.Config(0, 1, 1, 1, 1), "zero cadence bound");
    expectIllegalArgument(
        () -> new SetupPreviewProvider.Config(1, 1, 1, 1, 101), "invalid JPEG quality");
  }

  private static void closeIsTerminalAndReleasesPendingMemory() {
    ManualExecutor executor = new ManualExecutor();
    SetupPreviewProvider provider =
        new SetupPreviewProvider(CONFIG, new RecordingEncoder(), executor);
    provider.markSourceAvailable();
    provider.offerNv21(1_000, 2, 2, nv21(2, 2, 1));
    provider.offerNv21(2_000, 2, 2, nv21(2, 2, 2));
    provider.close();
    SetupPreviewProvider.Snapshot stopped = provider.snapshot(2_000);
    check(stopped.reason() == Reason.STOPPED, "closed provider reports stopped");
    check(stopped.metrics().retainedWorkCount() == 1, "only already-running detached work remains");
    check(
        provider.offerNv21(3_000, 2, 2, nv21(2, 2, 3)).disposition()
            == OfferDisposition.REJECTED_STOPPED,
        "close rejects future input before copying");
    executor.runNext();
    check(provider.snapshot(3_000).metrics().retainedWorkCount() == 0, "terminal drain releases work");
    expectIllegalState(provider::markSourceAvailable, "closed source restart");
  }

  private static PreviewEvidence evidence(long timestamp, byte[] jpeg) {
    return new PreviewEvidence(
        timestamp,
        jpeg,
        "pose-model",
        1,
        0.9,
        0.8,
        0.1,
        true,
        PreviewEvidence.ControllerState.MONITORING,
        "monitoring");
  }

  private static byte[] nv21(int width, int height, int value) {
    byte[] bytes = new byte[width * height * 3 / 2];
    java.util.Arrays.fill(bytes, (byte) value);
    return bytes;
  }

  private static byte[] jpeg(int body) {
    return new byte[] {(byte) 0xff, (byte) 0xd8, (byte) body, (byte) 0xff, (byte) 0xd9};
  }

  private static final class RecordingEncoder implements SetupPreviewProvider.Encoder {
    private int calls;
    private int firstInputByte = -1;
    private int lastInputByte = -1;
    private boolean fail;
    private boolean oversize;

    @Override
    public byte[] encodeNv21(
        byte[] nv21, int width, int height, int quality, int maximumJpegBytes) {
      check(width == 2 && height == 2, "encoder geometry");
      check(quality == 70 && maximumJpegBytes == 16, "encoder bounds propagated");
      calls++;
      if (firstInputByte < 0) {
        firstInputByte = nv21[0] & 0xff;
      }
      lastInputByte = nv21[0] & 0xff;
      if (fail) {
        throw new IllegalStateException("synthetic encoder failure");
      }
      if (oversize) {
        byte[] bytes = new byte[17];
        bytes[0] = (byte) 0xff;
        bytes[1] = (byte) 0xd8;
        bytes[15] = (byte) 0xff;
        bytes[16] = (byte) 0xd9;
        return bytes;
      }
      return jpeg(lastInputByte);
    }
  }

  private static final class ManualExecutor implements Executor {
    private final ArrayDeque<Runnable> work = new ArrayDeque<>();

    @Override
    public void execute(Runnable command) {
      work.addLast(command);
    }

    private int size() {
      return work.size();
    }

    private void runNext() {
      Runnable next = work.pollFirst();
      check(next != null, "manual executor has work");
      next.run();
    }
  }

  private static void expectIllegalArgument(Runnable action, String label) {
    try {
      action.run();
      throw new AssertionError(label + " did not fail");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void expectIllegalState(Runnable action, String label) {
    try {
      action.run();
      throw new AssertionError(label + " did not fail");
    } catch (IllegalStateException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
