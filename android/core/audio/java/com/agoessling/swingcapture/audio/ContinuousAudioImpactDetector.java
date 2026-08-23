package com.agoessling.swingcapture.audio;

import java.util.ArrayDeque;
import java.util.Objects;
import java.util.Optional;

/** Android-independent facade intended to be owned by one continuous AudioRecord read loop. */
public final class ContinuousAudioImpactDetector {
  private static final int MAXIMUM_PENDING_IMPACTS = 4;

  /** A confirmed detector event together with the best currently validated BOOTTIME estimates. */
  public record TimedImpact(
      ImpactDetector.Impact impact,
      Optional<AudioTimestampMapper.Estimate> strikeTime,
      Optional<AudioTimestampMapper.Estimate> confirmationTime) {
    public TimedImpact {
      Objects.requireNonNull(impact, "impact");
      Objects.requireNonNull(strikeTime, "strikeTime");
      Objects.requireNonNull(confirmationTime, "confirmationTime");
    }

    public boolean hasValidatedTiming() {
      return strikeTime.isPresent() && confirmationTime.isPresent();
    }
  }

  @FunctionalInterface
  public interface ImpactSink {
    void onImpact(TimedImpact impact);
  }

  private final ImpactDetector detector;
  private final AudioTimestampMapper timestampMapper;
  private final ArrayDeque<ImpactDetector.Impact> pendingImpacts = new ArrayDeque<>();

  public ContinuousAudioImpactDetector() {
    this(ImpactDetector.Config.defaults(), AudioTimestampMapper.Config.defaults());
  }

  public ContinuousAudioImpactDetector(
      ImpactDetector.Config detectorConfig, AudioTimestampMapper.Config timestampConfig) {
    detector = new ImpactDetector(detectorConfig);
    timestampMapper = new AudioTimestampMapper(timestampConfig);
  }

  /** Pass through each successful AudioRecord BOOTTIME timestamp observation. */
  public AudioTimestampMapper.ObservationResult observeAudioTimestamp(
      long framePosition, long boottimeNanos, long uncertaintyNanos) {
    return timestampMapper.observe(framePosition, boottimeNanos, uncertaintyNanos);
  }

  /**
   * Processes one AudioRecord PCM16 read.
   *
   * <p>The owner maintains {@code firstFramePosition}: initialize it to zero when AudioRecord is
   * started and advance it by the number of complete mono frames returned by each successful read.
   */
  public ImpactDetector.ProcessResult processPcm16(
      short[] pcm,
      int offset,
      int frameCount,
      long firstFramePosition,
      ImpactSink sink) {
    Objects.requireNonNull(sink, "sink");
    flushPendingImpacts(sink);
    return detector.processBlock(
        pcm,
        offset,
        frameCount,
        firstFramePosition,
        impact -> {
          pendingImpacts.addLast(impact);
          if (pendingImpacts.size() > MAXIMUM_PENDING_IMPACTS) {
            // Prefer the most recent still-recoverable window after a prolonged timestamp outage.
            // Events retained in the deque and all events delivered to the sink remain ordered.
            pendingImpacts.removeFirst();
          }
          flushPendingImpacts(sink);
        });
  }

  /** Number of detected impacts waiting for a validated BOOTTIME mapping. */
  public int pendingImpactCount() {
    return pendingImpacts.size();
  }

  public ImpactDetector detector() {
    return detector;
  }

  public AudioTimestampMapper timestampMapper() {
    return timestampMapper;
  }

  public void reset() {
    detector.reset();
    timestampMapper.reset();
    pendingImpacts.clear();
  }

  private void flushPendingImpacts(ImpactSink sink) {
    while (!pendingImpacts.isEmpty()) {
      ImpactDetector.Impact impact = pendingImpacts.getFirst();
      Optional<AudioTimestampMapper.Estimate> strikeTime =
          timestampMapper.estimateBoottime(impact.strikeFramePosition());
      Optional<AudioTimestampMapper.Estimate> confirmationTime =
          timestampMapper.estimateBoottime(impact.confirmationFramePosition());
      if (strikeTime.isEmpty() || confirmationTime.isEmpty()) {
        return;
      }
      pendingImpacts.removeFirst();
      sink.onImpact(new TimedImpact(impact, strikeTime, confirmationTime));
    }
  }
}
