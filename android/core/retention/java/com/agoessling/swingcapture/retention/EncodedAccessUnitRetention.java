package com.agoessling.swingcapture.retention;

import java.nio.BufferOverflowException;
import java.nio.ByteBuffer;
import java.util.Objects;
import java.util.Optional;

/**
 * Fixed-memory retention for encoded video access units.
 *
 * <p>The store owns one byte-block pool, a bounded metadata pool, a bounded live ring, and a
 * bounded set of snapshot ownership tables. Appending an access unit copies its payload into the
 * byte pool without retaining or modifying the caller's {@link ByteBuffer}. A snapshot increments
 * metadata-slot ownership rather than copying encoded bytes, so the live ring can continue to wrap
 * while a completed clip is being consumed.
 */
public final class EncodedAccessUnitRetention {
  public static final long SWING_PRE_ROLL_NS = 1_400_000_000L;
  public static final long SWING_POST_ROLL_NS = 500_000_000L;

  /** All storage and timing bounds are explicit construction-time policy. */
  public record Limits(
      int maxBytes,
      int blockBytes,
      long maxDurationNs,
      int maxAccessUnits,
      int maxSnapshots,
      long preRollNs,
      long postRollNs,
      long cooldownNs,
      long maxInterAccessUnitGapNs,
      long maxTriggerLagNs) {
    public Limits {
      if (maxBytes <= 0 || blockBytes <= 0 || maxBytes % blockBytes != 0) {
        throw new IllegalArgumentException("maxBytes must be a positive multiple of blockBytes");
      }
      if (maxAccessUnits <= 0 || maxSnapshots <= 0) {
        throw new IllegalArgumentException("access-unit and snapshot limits must be positive");
      }
      if (maxDurationNs <= 0 || preRollNs < 0 || postRollNs < 0 || cooldownNs < 0) {
        throw new IllegalArgumentException("duration limits must be nonnegative and retention positive");
      }
      if (preRollNs > maxDurationNs - postRollNs) {
        throw new IllegalArgumentException("retention duration must cover pre-roll plus post-roll");
      }
      if (maxInterAccessUnitGapNs <= 0) {
        throw new IllegalArgumentException("maximum access-unit gap must be positive");
      }
      if (maxTriggerLagNs < maxInterAccessUnitGapNs) {
        throw new IllegalArgumentException(
            "maximum trigger lag must cover at least one access-unit gap");
      }
      Math.multiplyExact(maxAccessUnits, Math.addExact(maxSnapshots, 1));
    }
  }

  /** Stable metadata copied out of a retained slot. */
  public record AccessUnitMetadata(
      long ordinal,
      long presentationTimeUs,
      long sensorTimestampNs,
      int flags,
      boolean idr,
      int payloadBytes) {}

  /** Stable timing policy and achieved history for one completed snapshot. */
  public record SnapshotMetadata(
      long triggerSensorTimestampNs,
      long firstAccessUnitSensorTimestampNs,
      long configuredPreRollNs,
      long minimumRequiredPreRollNs,
      long actualPreRollNs,
      boolean startupPreRollTruncated) {}

  /** Trigger admission result; failures do not mutate an existing capture. */
  public enum TriggerStatus {
    ACCEPTED,
    CAPTURE_ACTIVE,
    COOLDOWN,
    NO_FRAME_AT_TRIGGER,
    INSUFFICIENT_PRE_ROLL,
    NO_PRECEDING_IDR,
    GAP_IN_RETAINED_WINDOW,
    SNAPSHOT_POOL_EXHAUSTED
  }

  /** Current trigger state, advanced by access-unit sensor timestamps. */
  public enum CaptureState {
    IDLE,
    CAPTURING,
    COOLDOWN
  }

  /** Explicit append/reset failures. */
  public enum Failure {
    ORDINAL_GAP,
    PRESENTATION_TIMESTAMP_DISCONTINUITY,
    SENSOR_TIMESTAMP_DISCONTINUITY,
    SENSOR_TIMESTAMP_GAP,
    ACCESS_UNIT_TOO_LARGE,
    BYTE_POOL_EXHAUSTED,
    METADATA_POOL_EXHAUSTED,
    SNAPSHOT_CAPACITY_EXHAUSTED,
    RESET_DURING_CAPTURE
  }

  /** One access unit's position in both the encoded and sensor timestamp domains. */
  public record AccessUnitTiming(
      long ordinal, long presentationTimeUs, long sensorTimestampNs) {
    public AccessUnitTiming {
      if (ordinal < 0 || presentationTimeUs < 0 || sensorTimestampNs < 0) {
        throw new IllegalArgumentException("access-unit timing must be nonnegative");
      }
    }
  }

  /** Exact neighboring access units used to reject a continuity boundary. */
  public record ContinuityDiagnostic(
      AccessUnitTiming previous,
      AccessUnitTiming current,
      long maximumSensorTimestampGapNs) {
    public ContinuityDiagnostic {
      Objects.requireNonNull(previous, "previous");
      Objects.requireNonNull(current, "current");
      if (maximumSensorTimestampGapNs <= 0) {
        throw new IllegalArgumentException("maximum sensor timestamp gap must be positive");
      }
    }

    public long ordinalDelta() {
      return current.ordinal() - previous.ordinal();
    }

    public long presentationTimestampGapUs() {
      return current.presentationTimeUs() - previous.presentationTimeUs();
    }

    public long sensorTimestampGapNs() {
      return current.sensorTimestampNs() - previous.sensorTimestampNs();
    }
  }

  /** Checked failure from a rejected append, including the affected capture when one existed. */
  public static final class RetentionException extends Exception {
    private final Failure failure;
    private final long captureId;
    private final ContinuityDiagnostic continuityDiagnostic;

    private RetentionException(
        Failure failure,
        long captureId,
        String message,
        ContinuityDiagnostic continuityDiagnostic) {
      super(message);
      this.failure = failure;
      this.captureId = captureId;
      this.continuityDiagnostic = continuityDiagnostic;
    }

    public Failure failure() {
      return failure;
    }

    /** Returns zero if no capture was active when the failure occurred. */
    public long captureId() {
      return captureId;
    }

    /**
     * Neighboring timestamp evidence for continuity failures; empty for capacity and reset
     * failures.
     */
    public Optional<ContinuityDiagnostic> continuityDiagnostic() {
      return Optional.ofNullable(continuityDiagnostic);
    }
  }

  /** Trigger outcome with the capture identifier or next eligible trigger timestamp. */
  public record TriggerResult(TriggerStatus status, long captureId, long eligibleAtSensorTimestampNs) {
    public boolean accepted() {
      return status == TriggerStatus.ACCEPTED;
    }
  }

  /**
   * Ownership handle for a completed snapshot.
   *
   * <p>Close the handle after muxing or serving the clip. Every access validates both the snapshot
   * record's generation and lease state, preventing a stale handle from reading a reused record.
   */
  public static final class Snapshot implements AutoCloseable {
    private final EncodedAccessUnitRetention owner;
    private final int recordIndex;
    private final long generation;
    private final long captureId;
    private volatile boolean closed;

    private Snapshot(
        EncodedAccessUnitRetention owner, int recordIndex, long generation, long captureId) {
      this.owner = owner;
      this.recordIndex = recordIndex;
      this.generation = generation;
      this.captureId = captureId;
    }

    public long captureId() {
      return captureId;
    }

    public int accessUnitCount() {
      return owner.snapshotAccessUnitCount(recordIndex, generation, closed);
    }

    public long triggerSensorTimestampNs() {
      return owner.snapshotTriggerTimestamp(recordIndex, generation, closed);
    }

    /**
     * Reports the actual IDR-backed history retained before the trigger.
     *
     * <p>{@link SnapshotMetadata#startupPreRollTruncated()} is true only for an explicitly opted-in
     * startup admission whose history was shorter than the configured normal pre-roll.
     */
    public SnapshotMetadata snapshotMetadata() {
      return owner.snapshotMetadata(recordIndex, generation, closed);
    }

    public AccessUnitMetadata metadata(int accessUnitIndex) {
      return owner.snapshotMetadata(recordIndex, generation, closed, accessUnitIndex);
    }

    /** Copies one encoded access unit into {@code destination} and advances its position. */
    public int copyPayload(int accessUnitIndex, ByteBuffer destination) {
      return owner.copySnapshotPayload(
          recordIndex, generation, closed, accessUnitIndex, destination);
    }

    @Override
    public synchronized void close() {
      if (!closed) {
        owner.releaseSnapshot(recordIndex, generation);
        closed = true;
      }
    }
  }

  private enum SnapshotRecordState {
    FREE,
    CAPTURING,
    READY,
    LEASED
  }

  private static final int NO_INDEX = -1;

  private static final class Slot {
    private long ordinal;
    private long presentationTimeUs;
    private long sensorTimestampNs;
    private int flags;
    private boolean idr;
    private int payloadBytes;
    private int firstBlock = NO_INDEX;
    private int blockCount;
    private int owners;
  }

  private static final class SnapshotRecord {
    private final int[] slotIds;
    private SnapshotRecordState state = SnapshotRecordState.FREE;
    private long generation;
    private long captureId;
    private long triggerSensorTimestampNs;
    private long postEndSensorTimestampNs;
    private long firstAccessUnitSensorTimestampNs;
    private long minimumRequiredPreRollNs;
    private long actualPreRollNs;
    private boolean startupPreRollTruncated;
    private int count;

    private SnapshotRecord(int maxAccessUnits) {
      slotIds = new int[maxAccessUnits];
    }
  }

  private final Limits limits;
  private final byte[] bytePool;
  private final int[] nextBlock;
  private final int[] freeBlocks;
  private int freeBlockCount;

  private final Slot[] slots;
  private final int[] freeSlots;
  private int freeSlotCount;

  private final int[] ringSlotIds;
  private int ringHead;
  private int ringCount;
  private int ringBytes;

  private final SnapshotRecord[] snapshotRecords;
  private final int[] readySnapshotRecords;
  private int readyHead;
  private int readyCount;
  private int activeSnapshotRecord = NO_INDEX;

  private boolean hasLastAccessUnit;
  private long lastOrdinal;
  private long lastPresentationTimeUs;
  private long lastSensorTimestampNs;
  private boolean startupHistoryIntact = true;
  private boolean startupContinuityCompromised;
  private long cooldownUntilSensorTimestampNs = Long.MIN_VALUE;
  private long nextCaptureId = 1;

  public EncodedAccessUnitRetention(Limits limits) {
    this.limits = limits;
    bytePool = new byte[limits.maxBytes()];
    int blockCount = limits.maxBytes() / limits.blockBytes();
    nextBlock = new int[blockCount];
    freeBlocks = new int[blockCount];
    for (int block = blockCount - 1; block >= 0; --block) {
      freeBlocks[freeBlockCount++] = block;
    }

    int metadataCapacity =
        Math.multiplyExact(limits.maxAccessUnits(), Math.addExact(limits.maxSnapshots(), 1));
    slots = new Slot[metadataCapacity];
    freeSlots = new int[metadataCapacity];
    for (int slot = metadataCapacity - 1; slot >= 0; --slot) {
      slots[slot] = new Slot();
      freeSlots[freeSlotCount++] = slot;
    }

    ringSlotIds = new int[limits.maxAccessUnits()];
    snapshotRecords = new SnapshotRecord[limits.maxSnapshots()];
    readySnapshotRecords = new int[limits.maxSnapshots()];
    for (int index = 0; index < snapshotRecords.length; ++index) {
      snapshotRecords[index] = new SnapshotRecord(limits.maxAccessUnits());
    }
  }

  public Limits limits() {
    return limits;
  }

  /**
   * Copies and appends one complete encoded access unit.
   *
   * <p>Ordinals, presentation timestamps, and sensor timestamps must be strictly increasing. A
   * failure never partially retains the new payload. A continuity or resource failure aborts the
   * active capture and identifies it through {@link RetentionException#captureId()}.
   */
  public synchronized void append(
      long ordinal,
      long presentationTimeUs,
      long sensorTimestampNs,
      int flags,
      boolean idr,
      ByteBuffer encodedAccessUnit)
      throws RetentionException {
    if (ordinal < 0 || presentationTimeUs < 0 || sensorTimestampNs < 0) {
      throw new IllegalArgumentException("access-unit timestamps and ordinal must be nonnegative");
    }
    if (encodedAccessUnit == null) {
      throw new NullPointerException("encodedAccessUnit");
    }
    int payloadBytes = encodedAccessUnit.remaining();
    if (payloadBytes <= 0) {
      throw new IllegalArgumentException("encoded access unit must not be empty");
    }
    validateContinuity(ordinal, presentationTimeUs, sensorTimestampNs);
    validateActiveSnapshotCapacity();

    int requiredBlocks = blocksForBytes(payloadBytes);
    if (requiredBlocks > freeBlocks.length) {
      throw appendFailure(
          Failure.ACCESS_UNIT_TOO_LARGE,
          "encoded access unit exceeds the configured byte-pool capacity");
    }
    makeAppendCapacity(requiredBlocks);
    if (freeBlockCount < requiredBlocks) {
      throw appendFailure(
          Failure.BYTE_POOL_EXHAUSTED,
          "snapshot ownership prevents enough byte blocks from being reclaimed");
    }
    if (freeSlotCount == 0) {
      throw appendFailure(
          Failure.METADATA_POOL_EXHAUSTED,
          "snapshot ownership prevents a metadata slot from being reclaimed");
    }

    int slotId = allocateSlot();
    Slot slot = slots[slotId];
    slot.ordinal = ordinal;
    slot.presentationTimeUs = presentationTimeUs;
    slot.sensorTimestampNs = sensorTimestampNs;
    slot.flags = flags;
    slot.idr = idr;
    slot.payloadBytes = payloadBytes;
    slot.blockCount = requiredBlocks;
    slot.owners = 1;
    allocateAndCopyBlocks(slot, encodedAccessUnit);

    ringSlotIds[(ringHead + ringCount) % ringSlotIds.length] = slotId;
    ++ringCount;
    ringBytes += payloadBytes;
    hasLastAccessUnit = true;
    lastOrdinal = ordinal;
    lastPresentationTimeUs = presentationTimeUs;
    lastSensorTimestampNs = sensorTimestampNs;

    evictExpiredRingEntries(sensorTimestampNs);
    updateActiveCaptureForAppend(slotId, sensorTimestampNs);
  }

  /** Starts a full-pre-roll snapshot according to the supplied limits. */
  public synchronized TriggerResult trigger(long triggerSensorTimestampNs) {
    return triggerInternal(triggerSensorTimestampNs, false, limits.preRollNs());
  }

  /**
   * Experimental opt-in admission for the encoder-startup interval.
   *
   * <p>Normal callers should use {@link #trigger(long)}. This path may admit an IDR-backed,
   * continuous snapshot before the configured pre-roll has accumulated, but only while the
   * beginning of the current continuity epoch is still retained and the actual history reaches
   * {@code minimumStartupHistoryNs}. Once full history exists, this method is exactly equivalent
   * to {@code trigger}; it does not continue using a shorter rolling window. Eviction and an
   * unreset continuity failure disable startup truncation. The tradeoff is earlier trigger
   * readiness at the cost of losing the portion of pre-trigger motion before encoder startup; it
   * is intended for an experiment where Camera2 starts before the backswing, not as the production
   * default.
   */
  public synchronized TriggerResult triggerAllowingStartupTruncatedPreRoll(
      long triggerSensorTimestampNs, long minimumStartupHistoryNs) {
    if (minimumStartupHistoryNs < 0 || minimumStartupHistoryNs > limits.preRollNs()) {
      throw new IllegalArgumentException(
          "minimum startup history must be between zero and configured pre-roll");
    }
    return triggerInternal(triggerSensorTimestampNs, true, minimumStartupHistoryNs);
  }

  private TriggerResult triggerInternal(
      long triggerSensorTimestampNs,
      boolean allowStartupTruncatedPreRoll,
      long minimumStartupHistoryNs) {
    if (triggerSensorTimestampNs < 0) {
      throw new IllegalArgumentException("trigger timestamp must be nonnegative");
    }
    if (activeSnapshotRecord != NO_INDEX) {
      return triggerResult(TriggerStatus.CAPTURE_ACTIVE, 0, cooldownUntilSensorTimestampNs);
    }
    if (triggerSensorTimestampNs < cooldownUntilSensorTimestampNs) {
      return triggerResult(TriggerStatus.COOLDOWN, 0, cooldownUntilSensorTimestampNs);
    }
    int lastAtTrigger = lastRingIndexAtOrBefore(triggerSensorTimestampNs);
    if (lastAtTrigger == NO_INDEX) {
      return triggerResult(TriggerStatus.NO_FRAME_AT_TRIGGER, 0, 0);
    }
    if (triggerSensorTimestampNs - slots[ringSlotId(lastAtTrigger)].sensorTimestampNs
        > limits.maxTriggerLagNs()) {
      return triggerResult(TriggerStatus.NO_FRAME_AT_TRIGGER, 0, 0);
    }

    long preStart = saturatingSubtract(triggerSensorTimestampNs, limits.preRollNs());
    Slot oldest = slots[ringSlotId(0)];
    boolean fullHistoryAvailable =
        triggerSensorTimestampNs >= limits.preRollNs() && oldest.sensorTimestampNs <= preStart;
    int idrIndex;
    boolean startupPreRollTruncated = false;
    long minimumRequiredPreRollNs = limits.preRollNs();
    if (fullHistoryAvailable) {
      int firstAtOrAfterPreStart = firstRingIndexAtOrAfter(preStart);
      idrIndex = precedingIdrIndex(firstAtOrAfterPreStart);
      if (idrIndex == NO_INDEX) {
        return triggerResult(TriggerStatus.NO_PRECEDING_IDR, 0, 0);
      }
    } else {
      if (!allowStartupTruncatedPreRoll || !startupHistoryIntact) {
        return triggerResult(TriggerStatus.INSUFFICIENT_PRE_ROLL, 0, 0);
      }
      if (startupContinuityCompromised) {
        return triggerResult(TriggerStatus.GAP_IN_RETAINED_WINDOW, 0, 0);
      }
      idrIndex = firstIdrIndexAtOrBefore(lastAtTrigger);
      if (idrIndex == NO_INDEX) {
        return triggerResult(TriggerStatus.NO_PRECEDING_IDR, 0, 0);
      }
      long startupHistoryNs =
          triggerSensorTimestampNs - slots[ringSlotId(idrIndex)].sensorTimestampNs;
      if (startupHistoryNs < minimumStartupHistoryNs) {
        return triggerResult(TriggerStatus.INSUFFICIENT_PRE_ROLL, 0, 0);
      }
      startupPreRollTruncated = true;
      minimumRequiredPreRollNs = minimumStartupHistoryNs;
    }

    long postEnd = saturatingAdd(triggerSensorTimestampNs, limits.postRollNs());
    int firstAtOrAfterPostEnd = firstRingIndexAtOrAfter(postEnd);
    int snapshotEnd =
        firstAtOrAfterPostEnd == NO_INDEX
            ? lastRingIndexAtOrBefore(postEnd)
            : firstAtOrAfterPostEnd;
    if (!ringRangeIsContinuous(idrIndex, snapshotEnd)) {
      return triggerResult(TriggerStatus.GAP_IN_RETAINED_WINDOW, 0, 0);
    }
    int recordIndex = freeSnapshotRecord();
    if (recordIndex == NO_INDEX) {
      return triggerResult(TriggerStatus.SNAPSHOT_POOL_EXHAUSTED, 0, 0);
    }

    SnapshotRecord record = snapshotRecords[recordIndex];
    record.state = SnapshotRecordState.CAPTURING;
    record.generation = nextGeneration(record.generation);
    record.captureId = allocateCaptureId();
    record.triggerSensorTimestampNs = triggerSensorTimestampNs;
    record.postEndSensorTimestampNs = postEnd;
    record.firstAccessUnitSensorTimestampNs = slots[ringSlotId(idrIndex)].sensorTimestampNs;
    record.minimumRequiredPreRollNs = minimumRequiredPreRollNs;
    record.actualPreRollNs =
        triggerSensorTimestampNs - record.firstAccessUnitSensorTimestampNs;
    record.startupPreRollTruncated = startupPreRollTruncated;
    record.count = 0;
    for (int ringIndex = idrIndex; ringIndex <= snapshotEnd; ++ringIndex) {
      retainForSnapshot(record, ringSlotId(ringIndex));
    }

    activeSnapshotRecord = recordIndex;
    if (hasLastAccessUnit && lastSensorTimestampNs >= postEnd) {
      finalizeActiveCapture();
    }
    return triggerResult(TriggerStatus.ACCEPTED, record.captureId, 0);
  }

  /** Returns the oldest completed snapshot, or {@code null} when none is ready. */
  public synchronized Snapshot pollCompletedSnapshot() {
    if (readyCount == 0) {
      return null;
    }
    int recordIndex = readySnapshotRecords[readyHead];
    readyHead = (readyHead + 1) % readySnapshotRecords.length;
    --readyCount;
    SnapshotRecord record = snapshotRecords[recordIndex];
    if (record.state != SnapshotRecordState.READY) {
      throw new IllegalStateException("ready snapshot queue is corrupt");
    }
    record.state = SnapshotRecordState.LEASED;
    return new Snapshot(this, recordIndex, record.generation, record.captureId);
  }

  /**
   * Clears live continuity after a reported gap. Completed snapshot leases remain valid.
   *
   * @throws RetentionException when an active capture had to be aborted
   */
  public synchronized void resetContinuity() throws RetentionException {
    long abortedCapture = abortActiveCapture();
    clearRing();
    hasLastAccessUnit = false;
    startupHistoryIntact = true;
    startupContinuityCompromised = false;
    cooldownUntilSensorTimestampNs = Long.MIN_VALUE;
    if (abortedCapture != 0) {
      throw new RetentionException(
          Failure.RESET_DURING_CAPTURE,
          abortedCapture,
          "continuity reset aborted active capture",
          null);
    }
  }

  public synchronized CaptureState captureState() {
    if (activeSnapshotRecord != NO_INDEX) {
      return CaptureState.CAPTURING;
    }
    if (hasLastAccessUnit && lastSensorTimestampNs < cooldownUntilSensorTimestampNs) {
      return CaptureState.COOLDOWN;
    }
    return CaptureState.IDLE;
  }

  public synchronized int retainedAccessUnitCount() {
    return ringCount;
  }

  public synchronized int retainedBytes() {
    return ringBytes;
  }

  public synchronized int freeBytes() {
    return freeBlockCount * limits.blockBytes();
  }

  public synchronized long oldestRetainedOrdinal() {
    return ringCount == 0 ? -1 : slots[ringSlotId(0)].ordinal;
  }

  public synchronized long oldestRetainedSensorTimestampNs() {
    return ringCount == 0 ? -1 : slots[ringSlotId(0)].sensorTimestampNs;
  }

  public synchronized long newestRetainedSensorTimestampNs() {
    return ringCount == 0 ? -1 : slots[ringSlotId(ringCount - 1)].sensorTimestampNs;
  }

  private void validateContinuity(long ordinal, long presentationTimeUs, long sensorTimestampNs)
      throws RetentionException {
    if (!hasLastAccessUnit) {
      return;
    }
    ContinuityDiagnostic diagnostic =
        new ContinuityDiagnostic(
            new AccessUnitTiming(lastOrdinal, lastPresentationTimeUs, lastSensorTimestampNs),
            new AccessUnitTiming(ordinal, presentationTimeUs, sensorTimestampNs),
            limits.maxInterAccessUnitGapNs());
    if (lastOrdinal == Long.MAX_VALUE || ordinal != lastOrdinal + 1) {
      throw appendContinuityFailure(
          Failure.ORDINAL_GAP,
          "expected access-unit ordinal "
              + (lastOrdinal == Long.MAX_VALUE ? "after Long.MAX_VALUE" : lastOrdinal + 1)
              + " but received "
              + ordinal,
          diagnostic);
    }
    if (presentationTimeUs <= lastPresentationTimeUs) {
      throw appendContinuityFailure(
          Failure.PRESENTATION_TIMESTAMP_DISCONTINUITY,
          "presentation timestamps must be strictly increasing",
          diagnostic);
    }
    if (sensorTimestampNs <= lastSensorTimestampNs) {
      throw appendContinuityFailure(
          Failure.SENSOR_TIMESTAMP_DISCONTINUITY,
          "sensor timestamps must be strictly increasing",
          diagnostic);
    }
    if (diagnostic.sensorTimestampGapNs() > limits.maxInterAccessUnitGapNs()) {
      throw appendContinuityFailure(
          Failure.SENSOR_TIMESTAMP_GAP,
          "sensor timestamp gap exceeds configured maximum",
          diagnostic);
    }
  }

  private RetentionException appendFailure(Failure failure, String message) {
    long captureId = abortActiveCapture();
    return new RetentionException(failure, captureId, message, null);
  }

  private RetentionException appendContinuityFailure(
      Failure failure, String message, ContinuityDiagnostic diagnostic) {
    startupContinuityCompromised = true;
    long captureId = abortActiveCapture();
    return new RetentionException(
        failure,
        captureId,
        message
            + " (previous_ordinal="
            + diagnostic.previous().ordinal()
            + ", current_ordinal="
            + diagnostic.current().ordinal()
            + ", ordinal_delta="
            + diagnostic.ordinalDelta()
            + ", previous_presentation_time_us="
            + diagnostic.previous().presentationTimeUs()
            + ", current_presentation_time_us="
            + diagnostic.current().presentationTimeUs()
            + ", presentation_timestamp_gap_us="
            + diagnostic.presentationTimestampGapUs()
            + ", previous_sensor_timestamp_ns="
            + diagnostic.previous().sensorTimestampNs()
            + ", current_sensor_timestamp_ns="
            + diagnostic.current().sensorTimestampNs()
            + ", sensor_timestamp_gap_ns="
            + diagnostic.sensorTimestampGapNs()
            + ", maximum_sensor_timestamp_gap_ns="
            + diagnostic.maximumSensorTimestampGapNs()
            + ")",
        diagnostic);
  }

  private void validateActiveSnapshotCapacity() throws RetentionException {
    if (activeSnapshotRecord == NO_INDEX) {
      return;
    }
    SnapshotRecord record = snapshotRecords[activeSnapshotRecord];
    if (record.count >= record.slotIds.length) {
      throw appendFailure(
          Failure.SNAPSHOT_CAPACITY_EXHAUSTED,
          "active snapshot exceeds the fixed access-unit ownership capacity");
    }
  }

  private void makeAppendCapacity(int requiredBlocks) {
    while (ringCount > 0
        && (ringCount >= ringSlotIds.length
            || freeBlockCount < requiredBlocks
            || freeSlotCount == 0)) {
      evictOldestRingEntry();
    }
  }

  private int allocateSlot() {
    return freeSlots[--freeSlotCount];
  }

  private void allocateAndCopyBlocks(Slot slot, ByteBuffer source) {
    int originalPosition = source.position();
    try {
      int bytesRemaining = slot.payloadBytes;
      int previousBlock = NO_INDEX;
      for (int index = 0; index < slot.blockCount; ++index) {
        int block = freeBlocks[--freeBlockCount];
        if (previousBlock == NO_INDEX) {
          slot.firstBlock = block;
        } else {
          nextBlock[previousBlock] = block;
        }
        int bytesInBlock = Math.min(bytesRemaining, limits.blockBytes());
        source.get(bytePool, block * limits.blockBytes(), bytesInBlock);
        bytesRemaining -= bytesInBlock;
        previousBlock = block;
      }
      nextBlock[previousBlock] = NO_INDEX;
    } finally {
      source.position(originalPosition);
    }
  }

  private void evictExpiredRingEntries(long newestSensorTimestampNs) {
    while (ringCount > 1
        && newestSensorTimestampNs - slots[ringSlotId(0)].sensorTimestampNs
            > limits.maxDurationNs()) {
      evictOldestRingEntry();
    }
  }

  private void evictOldestRingEntry() {
    startupHistoryIntact = false;
    int slotId = ringSlotIds[ringHead];
    ringHead = (ringHead + 1) % ringSlotIds.length;
    --ringCount;
    ringBytes -= slots[slotId].payloadBytes;
    releaseSlot(slotId);
  }

  private void clearRing() {
    while (ringCount > 0) {
      evictOldestRingEntry();
    }
    ringHead = 0;
    ringBytes = 0;
  }

  private void releaseSlot(int slotId) {
    Slot slot = slots[slotId];
    if (--slot.owners != 0) {
      return;
    }
    int block = slot.firstBlock;
    for (int index = 0; index < slot.blockCount; ++index) {
      int next = nextBlock[block];
      freeBlocks[freeBlockCount++] = block;
      block = next;
    }
    slot.firstBlock = NO_INDEX;
    slot.blockCount = 0;
    slot.payloadBytes = 0;
    freeSlots[freeSlotCount++] = slotId;
  }

  private int blocksForBytes(int bytes) {
    return (bytes - 1) / limits.blockBytes() + 1;
  }

  private int ringSlotId(int orderedIndex) {
    return ringSlotIds[(ringHead + orderedIndex) % ringSlotIds.length];
  }

  private int lastRingIndexAtOrBefore(long sensorTimestampNs) {
    for (int index = ringCount - 1; index >= 0; --index) {
      if (slots[ringSlotId(index)].sensorTimestampNs <= sensorTimestampNs) {
        return index;
      }
    }
    return NO_INDEX;
  }

  private int firstRingIndexAtOrAfter(long sensorTimestampNs) {
    for (int index = 0; index < ringCount; ++index) {
      if (slots[ringSlotId(index)].sensorTimestampNs >= sensorTimestampNs) {
        return index;
      }
    }
    return NO_INDEX;
  }

  private int precedingIdrIndex(int fromIndex) {
    for (int index = fromIndex; index >= 0; --index) {
      if (slots[ringSlotId(index)].idr) {
        return index;
      }
    }
    return NO_INDEX;
  }

  private int firstIdrIndexAtOrBefore(int lastIndex) {
    for (int index = 0; index <= lastIndex; ++index) {
      if (slots[ringSlotId(index)].idr) {
        return index;
      }
    }
    return NO_INDEX;
  }

  private boolean ringRangeIsContinuous(int firstIndex, int lastIndex) {
    if (firstIndex < 0 || lastIndex < firstIndex) {
      return false;
    }
    Slot previous = slots[ringSlotId(firstIndex)];
    for (int index = firstIndex + 1; index <= lastIndex; ++index) {
      Slot current = slots[ringSlotId(index)];
      if (previous.ordinal == Long.MAX_VALUE
          || current.ordinal != previous.ordinal + 1
          || current.presentationTimeUs <= previous.presentationTimeUs
          || current.sensorTimestampNs <= previous.sensorTimestampNs
          || current.sensorTimestampNs - previous.sensorTimestampNs
              > limits.maxInterAccessUnitGapNs()) {
        return false;
      }
      previous = current;
    }
    return true;
  }

  private int freeSnapshotRecord() {
    for (int index = 0; index < snapshotRecords.length; ++index) {
      if (snapshotRecords[index].state == SnapshotRecordState.FREE) {
        return index;
      }
    }
    return NO_INDEX;
  }

  private void retainForSnapshot(SnapshotRecord record, int slotId) {
    if (record.count >= record.slotIds.length) {
      throw new IllegalStateException("initial snapshot exceeds fixed ownership capacity");
    }
    record.slotIds[record.count++] = slotId;
    ++slots[slotId].owners;
  }

  private void updateActiveCaptureForAppend(int appendedSlotId, long sensorTimestampNs) {
    if (activeSnapshotRecord == NO_INDEX) {
      return;
    }
    SnapshotRecord record = snapshotRecords[activeSnapshotRecord];
    // Preserve the first encoded frame at or after the desired endpoint. Impacts normally occur
    // between video frames, so stopping at the final frame before the boundary would silently
    // shorten post-roll by almost one frame period.
    retainForSnapshot(record, appendedSlotId);
    if (sensorTimestampNs >= record.postEndSensorTimestampNs) {
      finalizeActiveCapture();
    }
  }

  private void finalizeActiveCapture() {
    int recordIndex = activeSnapshotRecord;
    SnapshotRecord record = snapshotRecords[recordIndex];
    record.state = SnapshotRecordState.READY;
    readySnapshotRecords[(readyHead + readyCount) % readySnapshotRecords.length] = recordIndex;
    ++readyCount;
    activeSnapshotRecord = NO_INDEX;
    cooldownUntilSensorTimestampNs =
        saturatingAdd(record.postEndSensorTimestampNs, limits.cooldownNs());
  }

  private long abortActiveCapture() {
    if (activeSnapshotRecord == NO_INDEX) {
      return 0;
    }
    SnapshotRecord record = snapshotRecords[activeSnapshotRecord];
    long captureId = record.captureId;
    for (int index = 0; index < record.count; ++index) {
      releaseSlot(record.slotIds[index]);
    }
    resetSnapshotRecord(record);
    activeSnapshotRecord = NO_INDEX;
    return captureId;
  }

  private int snapshotAccessUnitCount(int recordIndex, long generation, boolean handleClosed) {
    synchronized (this) {
      return checkedLeasedRecord(recordIndex, generation, handleClosed).count;
    }
  }

  private long snapshotTriggerTimestamp(int recordIndex, long generation, boolean handleClosed) {
    synchronized (this) {
      return checkedLeasedRecord(recordIndex, generation, handleClosed).triggerSensorTimestampNs;
    }
  }

  private SnapshotMetadata snapshotMetadata(
      int recordIndex, long generation, boolean handleClosed) {
    synchronized (this) {
      SnapshotRecord record = checkedLeasedRecord(recordIndex, generation, handleClosed);
      return new SnapshotMetadata(
          record.triggerSensorTimestampNs,
          record.firstAccessUnitSensorTimestampNs,
          limits.preRollNs(),
          record.minimumRequiredPreRollNs,
          record.actualPreRollNs,
          record.startupPreRollTruncated);
    }
  }

  private AccessUnitMetadata snapshotMetadata(
      int recordIndex, long generation, boolean handleClosed, int accessUnitIndex) {
    synchronized (this) {
      SnapshotRecord record = checkedLeasedRecord(recordIndex, generation, handleClosed);
      Slot slot = snapshotSlot(record, accessUnitIndex);
      return new AccessUnitMetadata(
          slot.ordinal,
          slot.presentationTimeUs,
          slot.sensorTimestampNs,
          slot.flags,
          slot.idr,
          slot.payloadBytes);
    }
  }

  private int copySnapshotPayload(
      int recordIndex,
      long generation,
      boolean handleClosed,
      int accessUnitIndex,
      ByteBuffer destination) {
    synchronized (this) {
      if (destination == null) {
        throw new NullPointerException("destination");
      }
      SnapshotRecord record = checkedLeasedRecord(recordIndex, generation, handleClosed);
      Slot slot = snapshotSlot(record, accessUnitIndex);
      if (destination.remaining() < slot.payloadBytes) {
        throw new BufferOverflowException();
      }
      int block = slot.firstBlock;
      int bytesRemaining = slot.payloadBytes;
      for (int index = 0; index < slot.blockCount; ++index) {
        int bytesInBlock = Math.min(bytesRemaining, limits.blockBytes());
        destination.put(bytePool, block * limits.blockBytes(), bytesInBlock);
        bytesRemaining -= bytesInBlock;
        block = nextBlock[block];
      }
      return slot.payloadBytes;
    }
  }

  private SnapshotRecord checkedLeasedRecord(
      int recordIndex, long generation, boolean handleClosed) {
    if (handleClosed || recordIndex < 0 || recordIndex >= snapshotRecords.length) {
      throw new IllegalStateException("snapshot lease is closed");
    }
    SnapshotRecord record = snapshotRecords[recordIndex];
    if (record.state != SnapshotRecordState.LEASED || record.generation != generation) {
      throw new IllegalStateException("snapshot lease is stale");
    }
    return record;
  }

  private Slot snapshotSlot(SnapshotRecord record, int accessUnitIndex) {
    if (accessUnitIndex < 0 || accessUnitIndex >= record.count) {
      throw new IndexOutOfBoundsException(accessUnitIndex);
    }
    return slots[record.slotIds[accessUnitIndex]];
  }

  private synchronized void releaseSnapshot(int recordIndex, long generation) {
    SnapshotRecord record = checkedLeasedRecord(recordIndex, generation, false);
    for (int index = 0; index < record.count; ++index) {
      releaseSlot(record.slotIds[index]);
    }
    resetSnapshotRecord(record);
  }

  private void resetSnapshotRecord(SnapshotRecord record) {
    record.state = SnapshotRecordState.FREE;
    record.captureId = 0;
    record.triggerSensorTimestampNs = 0;
    record.postEndSensorTimestampNs = 0;
    record.firstAccessUnitSensorTimestampNs = 0;
    record.minimumRequiredPreRollNs = 0;
    record.actualPreRollNs = 0;
    record.startupPreRollTruncated = false;
    record.count = 0;
  }

  private TriggerResult triggerResult(
      TriggerStatus status, long captureId, long eligibleAtSensorTimestampNs) {
    return new TriggerResult(status, captureId, eligibleAtSensorTimestampNs);
  }

  private long allocateCaptureId() {
    long allocated = nextCaptureId;
    ++nextCaptureId;
    if (nextCaptureId <= 0) {
      nextCaptureId = 1;
    }
    return allocated;
  }

  private static long nextGeneration(long generation) {
    return generation == Long.MAX_VALUE ? 1 : generation + 1;
  }

  private static long saturatingAdd(long value, long delta) {
    return value > Long.MAX_VALUE - delta ? Long.MAX_VALUE : value + delta;
  }

  private static long saturatingSubtract(long value, long delta) {
    return value < delta ? 0 : value - delta;
  }
}
