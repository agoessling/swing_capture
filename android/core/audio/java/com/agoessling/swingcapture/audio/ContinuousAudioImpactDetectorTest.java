package com.agoessling.swingcapture.audio;

import java.util.ArrayList;
import java.util.List;

/** Golden integration coverage for the AudioRecord-facing facade. */
public final class ContinuousAudioImpactDetectorTest {
  private ContinuousAudioImpactDetectorTest() {}

  public static void main(String[] arguments) {
    emitsValidatedStrikeAndConfirmationTimes();
    retainsImpactUntilRejectedStartupTimestampRecovers();
    neverDeliversWithoutValidatedTiming();
    flushesPendingImpactBeforeANewerEvent();
    boundsPendingImpactsAndRetainsTheirOrder();
    resetClearsDetectionAndClockState();
  }

  private static ContinuousAudioImpactDetector detector() {
    return new ContinuousAudioImpactDetector(
        new ImpactDetector.Config(8.0f, 0.05f, 0.005f, 4.0f, 0.5, 24, 4_800),
        new AudioTimestampMapper.Config(8, 1_000_000L, 1_000.0, 750_000_000L));
  }

  private static void emitsValidatedStrikeAndConfirmationTimes() {
    ContinuousAudioImpactDetector detector = detector();
    detector.observeAudioTimestamp(0, 5_000_000_000L, 1_000L);
    detector.observeAudioTimestamp(48_000, 6_000_000_000L, 1_000L);
    short[] pcm = new short[200];
    pcm[50] = pcm(0.90f);
    List<ContinuousAudioImpactDetector.TimedImpact> events = new ArrayList<>();

    detector.processPcm16(pcm, 0, pcm.length, 24_000, events::add);

    check(events.size() == 1, "timed event count");
    ContinuousAudioImpactDetector.TimedImpact event = events.get(0);
    check(event.hasValidatedTiming(), "timed event readiness");
    check(event.impact().strikeFramePosition() == 24_050, "timed strike frame");
    check(event.impact().confirmationFramePosition() == 24_074, "timed confirmation frame");
    check(
        event.strikeTime().orElseThrow().boottimeNanos() == 5_501_041_667L,
        "timed strike boottime");
    check(
        event.confirmationTime().orElseThrow().boottimeNanos() == 5_501_541_667L,
        "timed confirmation boottime");
  }

  private static void retainsImpactUntilRejectedStartupTimestampRecovers() {
    ContinuousAudioImpactDetector detector = detector();
    check(
        detector.observeAudioTimestamp(0, 5_000_000_000L, 1_000L).accepted(),
        "startup timestamp anchor");
    AudioTimestampMapper.ObservationResult rejected =
        detector.observeAudioTimestamp(4_800, 5_200_000_000L, 1_000L);
    check(!rejected.accepted(), "inconsistent startup timestamp rejected");
    check(
        rejected.rejectionReason() == AudioTimestampMapper.RejectionReason.CLOCK_RATE_OUT_OF_RANGE,
        "startup timestamp rejection reason");
    short[] pcm = new short[200];
    pcm[50] = pcm(-0.80f);
    List<ContinuousAudioImpactDetector.TimedImpact> events = new ArrayList<>();

    ImpactDetector.ProcessResult detection =
        detector.processPcm16(pcm, 0, pcm.length, 4_000, events::add);

    check(detection.eventsDetected() == 1, "startup detector reports the event immediately");
    check(
        detector.detector().cooldownUntilFramePosition() == 8_850,
        "pending delivery does not defer detector cooldown");
    check(events.isEmpty(), "unmapped startup event is not delivered");
    check(detector.pendingImpactCount() == 1, "startup event retained once");
    check(
        detector.observeAudioTimestamp(9_600, 5_200_000_000L, 1_000L).accepted(),
        "later consistent timestamp validates the clock");

    detector.processPcm16(new short[1], 0, 1, 4_200, events::add);

    check(events.size() == 1, "recovered startup event count");
    ContinuousAudioImpactDetector.TimedImpact recovered = events.get(0);
    check(recovered.hasValidatedTiming(), "recovered event timing readiness");
    check(recovered.impact().strikeFramePosition() == 4_050, "recovered strike frame");
    check(recovered.impact().confirmationFramePosition() == 4_074, "recovered confirmation frame");
    check(
        recovered.strikeTime().orElseThrow().boottimeNanos() == 5_084_375_000L,
        "recovered strike boottime");
    check(
        recovered.confirmationTime().orElseThrow().boottimeNanos() == 5_084_875_000L,
        "recovered confirmation boottime");
    check(detector.pendingImpactCount() == 0, "recovered event removed from pending queue");
  }

  private static void neverDeliversWithoutValidatedTiming() {
    ContinuousAudioImpactDetector detector = detector();
    short[] impact = new short[100];
    impact[10] = pcm(-0.80f);
    List<ContinuousAudioImpactDetector.TimedImpact> events = new ArrayList<>();

    detector.processPcm16(impact, 0, impact.length, 0, events::add);
    detector.processPcm16(new short[100], 0, 100, 100, events::add);
    detector.observeAudioTimestamp(0, 5_000_000_000L, 1_000L);
    detector.processPcm16(new short[100], 0, 100, 200, events::add);

    check(events.isEmpty(), "zero or one timestamp never reaches the sink");
    check(detector.pendingImpactCount() == 1, "untimed impact remains bounded pending state");
  }

  private static void flushesPendingImpactBeforeANewerEvent() {
    ContinuousAudioImpactDetector detector = detector();
    short[] first = new short[100];
    first[10] = pcm(0.80f);
    List<ContinuousAudioImpactDetector.TimedImpact> events = new ArrayList<>();
    detector.processPcm16(first, 0, first.length, 0, events::add);
    check(events.isEmpty(), "first event awaits clock validation");

    detector.observeAudioTimestamp(0, 7_000_000_000L, 1_000L);
    detector.observeAudioTimestamp(4_800, 7_100_000_000L, 1_000L);
    short[] second = new short[5_000];
    second[4_900] = pcm(-0.90f);
    detector.processPcm16(second, 0, second.length, 100, events::add);

    check(events.size() == 2, "ordered recovered and current event count");
    check(events.get(0).impact().strikeFramePosition() == 10, "recovered event delivered first");
    check(events.get(1).impact().strikeFramePosition() == 5_000, "newer event delivered second");
    check(events.get(0).hasValidatedTiming(), "recovered event has timing");
    check(events.get(1).hasValidatedTiming(), "new event has timing");
  }

  private static void boundsPendingImpactsAndRetainsTheirOrder() {
    ContinuousAudioImpactDetector detector =
        new ContinuousAudioImpactDetector(
            new ImpactDetector.Config(2.0f, 0.05f, 0.005f, 4.0f, 0.5, 0, 0),
            new AudioTimestampMapper.Config(8, 1_000_000L, 1_000.0, 750_000_000L));
    short[] impacts = new short[10];
    for (int index = 0; index < impacts.length; index += 2) {
      impacts[index] = pcm(0.80f);
    }
    List<ContinuousAudioImpactDetector.TimedImpact> events = new ArrayList<>();

    detector.processPcm16(impacts, 0, impacts.length, 0, events::add);
    check(events.isEmpty(), "unmapped burst is withheld");
    check(detector.pendingImpactCount() == 4, "pending impact queue has a fixed bound");

    detector.observeAudioTimestamp(0, 9_000_000_000L, 1_000L);
    detector.observeAudioTimestamp(48_000, 10_000_000_000L, 1_000L);
    detector.processPcm16(new short[1], 0, 1, 10, events::add);

    check(events.size() == 4, "bounded retained events flush after validation");
    for (int index = 0; index < events.size(); ++index) {
      check(
          events.get(index).impact().strikeFramePosition() == 2L + index * 2L,
          "bounded pending FIFO order " + index);
      check(events.get(index).hasValidatedTiming(), "bounded pending event timing " + index);
    }
  }

  private static void resetClearsDetectionAndClockState() {
    ContinuousAudioImpactDetector detector = detector();
    detector.observeAudioTimestamp(0, 1_000_000_000L, 1_000L);
    detector.observeAudioTimestamp(48_000, 2_000_000_000L, 1_000L);
    short[] impact = new short[100];
    impact[10] = pcm(0.80f);
    ContinuousAudioImpactDetector pendingDetector = detector();
    pendingDetector.processPcm16(impact, 0, impact.length, 0, ignored -> {});
    check(pendingDetector.pendingImpactCount() == 1, "reset fixture pending event");
    pendingDetector.reset();
    check(pendingDetector.pendingImpactCount() == 0, "reset pending event queue");
    detector.reset();

    check(!detector.detector().confirmationPending(), "reset pending detector state");
    check(detector.timestampMapper().retainedEvidenceCount() == 0, "reset clock evidence");
    check(!detector.timestampMapper().clockValidated(), "reset clock validation");
  }

  private static short pcm(float normalizedAmplitude) {
    return (short) (normalizedAmplitude * 32767.0f);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
