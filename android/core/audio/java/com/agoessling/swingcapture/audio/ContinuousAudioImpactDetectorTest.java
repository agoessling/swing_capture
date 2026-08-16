package com.agoessling.swingcapture.audio;

import java.util.ArrayList;
import java.util.List;

/** Golden integration coverage for the AudioRecord-facing facade. */
public final class ContinuousAudioImpactDetectorTest {
  private ContinuousAudioImpactDetectorTest() {}

  public static void main(String[] arguments) {
    emitsValidatedStrikeAndConfirmationTimes();
    preservesUntimedImpactWhenClockIsNotReady();
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

  private static void preservesUntimedImpactWhenClockIsNotReady() {
    ContinuousAudioImpactDetector detector = detector();
    short[] pcm = new short[100];
    pcm[10] = pcm(-0.80f);
    List<ContinuousAudioImpactDetector.TimedImpact> events = new ArrayList<>();

    detector.processPcm16(pcm, 0, pcm.length, 0, events::add);

    check(events.size() == 1, "untimed event count");
    check(!events.get(0).hasValidatedTiming(), "untimed event marked unvalidated");
    check(events.get(0).strikeTime().isEmpty(), "untimed strike estimate absent");
    check(events.get(0).confirmationTime().isEmpty(), "untimed confirmation estimate absent");
  }

  private static void resetClearsDetectionAndClockState() {
    ContinuousAudioImpactDetector detector = detector();
    detector.observeAudioTimestamp(0, 1_000_000_000L, 1_000L);
    detector.observeAudioTimestamp(48_000, 2_000_000_000L, 1_000L);
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
