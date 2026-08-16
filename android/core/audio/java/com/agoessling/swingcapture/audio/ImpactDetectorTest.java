package com.agoessling.swingcapture.audio;

import java.util.ArrayList;
import java.util.List;

/** Deterministic synthetic/golden coverage for the 48 kHz PCM16 detector. */
public final class ImpactDetectorTest {
  private ImpactDetectorTest() {}

  public static void main(String[] arguments) {
    quietDoesNotTrigger();
    reportsGoldenPeakAndConfirmation();
    handlesBothPcmClippingLimits();
    adaptsToSteadyBackgroundNoise();
    suppressesEventsDuringCooldown();
    confirmsAcrossBlockBoundary();
    blockGapSeparatesDistinctImpacts();
    rejectsOverlappingBlocks();
  }

  private static void quietDoesNotTrigger() {
    ImpactDetector detector = new ImpactDetector();
    short[] quiet = new short[ImpactDetector.SAMPLE_RATE_HZ * 2];
    List<ImpactDetector.Impact> events = new ArrayList<>();

    ImpactDetector.ProcessResult result =
        detector.processBlock(quiet, 0, quiet.length, 0, events::add);

    check(result.eventsDetected() == 0, "quiet event count");
    check(result.blockPeakAmplitude() == 0.0f, "quiet block peak");
    checkNear(result.noiseFloorAfterBlock(), 0.0, 0.001, "quiet final noise floor");
    checkNear(result.thresholdAfterBlock(), 0.015, 0.001, "quiet final threshold");
    check(events.isEmpty(), "quiet event list");
    check(!detector.confirmationPending(), "quiet pending confirmation");
  }

  private static void reportsGoldenPeakAndConfirmation() {
    ImpactDetector detector =
        new ImpactDetector(new ImpactDetector.Config(8.0f, 0.05f, 0.005f, 4.0f, 0.5, 24, 4_800));
    short[] samples = new short[240];
    samples[122] = pcm(0.45f);
    samples[123] = pcm(-0.90f);
    samples[124] = pcm(0.60f);
    List<ImpactDetector.Impact> events = new ArrayList<>();

    ImpactDetector.ProcessResult result =
        detector.processBlock(samples, 0, samples.length, 100_000, events::add);

    check(events.size() == 1, "golden impact count");
    ImpactDetector.Impact event = events.get(0);
    check(event.strikeFramePosition() == 100_123, "golden strike frame");
    check(event.confirmationFramePosition() == 100_147, "golden confirmation frame");
    check(event.sourceBlockStartFramePosition() == 100_000, "golden source block");
    check(event.sampleIndexInBlock() == 123, "golden source index");
    checkNear(event.peakAmplitude(), 0.90, 0.001, "golden peak amplitude");
    checkNear(event.noiseFloorAtDetection(), 0.005, 0.001, "golden noise floor");
    checkNear(event.thresholdAtDetection(), 0.05, 0.001, "golden threshold");
    checkNear(result.blockPeakAmplitude(), 0.90, 0.001, "golden block peak");
  }

  private static void handlesBothPcmClippingLimits() {
    ImpactDetector detector =
        new ImpactDetector(new ImpactDetector.Config(2.0f, 0.05f, 0.005f, 4.0f, 0.5, 0, 0));
    short[] clipping = {Short.MIN_VALUE, Short.MAX_VALUE};
    List<ImpactDetector.Impact> events = new ArrayList<>();

    detector.processBlock(clipping, 0, clipping.length, 0, events::add);

    check(events.size() == 2, "clipping event count");
    check(events.get(0).peakAmplitude() == 1.0f, "negative full scale normalization");
    checkNear(events.get(1).peakAmplitude(), 32767.0 / 32768.0, 0.000001,
        "positive full scale normalization");
  }

  private static void adaptsToSteadyBackgroundNoise() {
    ImpactDetector detector =
        new ImpactDetector(new ImpactDetector.Config(5.0f, 0.04f, 0.002f, 4.0f, 0.1, 24, 4_800));
    short[] background = new short[ImpactDetector.SAMPLE_RATE_HZ];
    for (int index = 0; index < background.length; ++index) {
      background[index] = index % 2 == 0 ? pcm(0.03f) : pcm(-0.03f);
    }
    List<ImpactDetector.Impact> events = new ArrayList<>();

    detector.processBlock(background, 0, background.length, 0, events::add);

    check(events.isEmpty(), "background should not trigger");
    check(detector.noiseFloor() > 0.028f, "adapted noise floor");
    check(detector.detectionThreshold() > 0.14f, "adapted threshold");

    short[] impact = new short[128];
    impact[40] = pcm(0.75f);
    detector.processBlock(
        impact, 0, impact.length, background.length, events::add);
    check(events.size() == 1, "impact above adapted threshold");
    check(events.get(0).strikeFramePosition() == background.length + 40L,
        "adapted impact position");
  }

  private static void suppressesEventsDuringCooldown() {
    ImpactDetector detector =
        new ImpactDetector(new ImpactDetector.Config(8.0f, 0.05f, 0.005f, 4.0f, 0.5, 12, 4_800));
    short[] samples = new short[7_000];
    samples[100] = pcm(0.90f);
    samples[1_000] = pcm(0.95f);
    samples[5_000] = pcm(0.85f);
    List<ImpactDetector.Impact> events = new ArrayList<>();

    detector.processBlock(samples, 0, samples.length, 0, events::add);

    check(events.size() == 2, "cooldown event count");
    check(events.get(0).strikeFramePosition() == 100, "first cooldown strike");
    check(events.get(1).strikeFramePosition() == 5_000, "post-cooldown strike");
    check(detector.cooldownUntilFramePosition() == 9_800, "cooldown endpoint");
  }

  private static void confirmsAcrossBlockBoundary() {
    ImpactDetector detector =
        new ImpactDetector(new ImpactDetector.Config(8.0f, 0.05f, 0.005f, 4.0f, 0.5, 48, 4_800));
    short[] first = new short[64];
    first[63] = pcm(-0.80f);
    short[] second = new short[96];
    List<ImpactDetector.Impact> events = new ArrayList<>();

    ImpactDetector.ProcessResult firstResult =
        detector.processBlock(first, 0, first.length, 2_000, events::add);
    check(firstResult.eventsDetected() == 0, "boundary first block count");
    check(detector.confirmationPending(), "boundary pending confirmation");

    detector.processBlock(second, 0, second.length, 2_064, events::add);
    check(events.size() == 1, "boundary event count");
    ImpactDetector.Impact event = events.get(0);
    check(event.strikeFramePosition() == 2_063, "boundary strike frame");
    check(event.confirmationFramePosition() == 2_111, "boundary confirmation frame");
    check(event.sourceBlockStartFramePosition() == 2_000, "boundary source block");
    check(event.sampleIndexInBlock() == 63, "boundary source index");
  }

  private static void blockGapSeparatesDistinctImpacts() {
    ImpactDetector detector =
        new ImpactDetector(new ImpactDetector.Config(8.0f, 0.05f, 0.005f, 4.0f, 0.5, 48, 100));
    short[] first = {pcm(0.60f)};
    short[] later = new short[64];
    later[0] = pcm(-0.95f);
    List<ImpactDetector.Impact> events = new ArrayList<>();

    detector.processBlock(first, 0, 1, 10_000, events::add);
    detector.processBlock(later, 0, later.length, 11_000, events::add);

    check(events.size() == 2, "gap event count");
    check(events.get(0).strikeFramePosition() == 10_000, "gap first strike");
    check(events.get(0).confirmationFramePosition() == 11_000, "gap first confirmation");
    check(events.get(1).strikeFramePosition() == 11_000, "gap second strike");
    check(events.get(1).confirmationFramePosition() == 11_048, "gap second confirmation");
  }

  private static void rejectsOverlappingBlocks() {
    ImpactDetector detector = new ImpactDetector();
    short[] samples = new short[16];
    detector.processBlock(samples, 0, samples.length, 100, ignored -> {});
    expectIllegalArgument(
        () -> detector.processBlock(samples, 0, samples.length, 115, ignored -> {}),
        "overlapping block");
  }

  private static short pcm(float normalizedAmplitude) {
    return (short) (normalizedAmplitude * 32767.0f);
  }

  private static void expectIllegalArgument(Runnable operation, String message) {
    try {
      operation.run();
      throw new AssertionError("Expected IllegalArgumentException: " + message);
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void checkNear(double actual, double expected, double tolerance, String message) {
    check(Math.abs(actual - expected) <= tolerance,
        message + ": expected " + expected + ", got " + actual);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
