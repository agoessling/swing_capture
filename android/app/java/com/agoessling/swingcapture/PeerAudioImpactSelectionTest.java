package com.agoessling.swingcapture;

import java.util.List;
import java.util.OptionalInt;

public final class PeerAudioImpactSelectionTest {
  private static final long MAXIMUM_AGE_NANOS = 250_000_000L;

  public static void main(String[] args) {
    acceptsRecentAndBoundaryCandidates();
    rejectsStaleAndFutureCandidates();
    choosesClockMatchedCandidateInsteadOfLatestPrecursor();
    accountsForBoundedUncertaintyAndTieBreaking();
    handlesExtremeTimestampsWithoutOverflow();
    returnsEmptyWithoutCandidateInTolerance();
    rejectsInvalidInputs();
  }

  private static void acceptsRecentAndBoundaryCandidates() {
    check(
        PeerAudioImpactSelection.isFreshLocalCandidate(
            1_000_000_000L, 900_000_000L, MAXIMUM_AGE_NANOS),
        "recent candidate should be used");
    check(
        PeerAudioImpactSelection.isFreshLocalCandidate(
            1_000_000_000L, 750_000_000L, MAXIMUM_AGE_NANOS),
        "candidate at age boundary should be used");
  }

  private static void rejectsStaleAndFutureCandidates() {
    check(
        !PeerAudioImpactSelection.isFreshLocalCandidate(
            1_000_000_000L, 749_999_999L, MAXIMUM_AGE_NANOS),
        "stale candidate should not be used");
    check(
        !PeerAudioImpactSelection.isFreshLocalCandidate(
            1_000_000_000L, 1_000_000_001L, MAXIMUM_AGE_NANOS),
        "future candidate should not be used");
  }

  private static void choosesClockMatchedCandidateInsteadOfLatestPrecursor() {
    List<PeerAudioImpactSelection.Candidate> candidates =
        List.of(
            new PeerAudioImpactSelection.Candidate(800_000_000L, 1_000_000L),
            new PeerAudioImpactSelection.Candidate(950_000_000L, 1_000_000L),
            new PeerAudioImpactSelection.Candidate(1_040_000_000L, 1_000_000L));
    OptionalInt selected =
        PeerAudioImpactSelection.closestCandidateIndex(
            candidates, 1_000_000_000L, 80_000_000L, 2_000_000L);
    check(selected.orElseThrow() == 2, "closest candidate should beat latest precursor");
  }

  private static void accountsForBoundedUncertaintyAndTieBreaking() {
    List<PeerAudioImpactSelection.Candidate> candidates =
        List.of(
            new PeerAudioImpactSelection.Candidate(900, 0),
            new PeerAudioImpactSelection.Candidate(1_100, 5));
    check(
        PeerAudioImpactSelection.closestCandidateIndex(candidates, 1_000, 90, 5)
                .orElseThrow()
            == 1,
        "newest candidate wins an exact-distance tie when its uncertainty admits it");
  }

  private static void returnsEmptyWithoutCandidateInTolerance() {
    check(
        PeerAudioImpactSelection.closestCandidateIndex(
                List.of(new PeerAudioImpactSelection.Candidate(100, 0)), 1_000, 80, 0)
            .isEmpty(),
        "out-of-window candidate must not be selected");
  }

  private static void handlesExtremeTimestampsWithoutOverflow() {
    check(
        PeerAudioImpactSelection.closestCandidateIndex(
                    List.of(new PeerAudioImpactSelection.Candidate(0, Long.MAX_VALUE)),
                    Long.MAX_VALUE,
                    1,
                    Long.MAX_VALUE)
                .orElseThrow()
            == 0,
        "extreme valid timestamps and saturated uncertainty remain selectable");
  }

  private static void rejectsInvalidInputs() {
    expectIllegalArgument(() -> PeerAudioImpactSelection.isFreshLocalCandidate(-1, 0, 0));
    expectIllegalArgument(() -> PeerAudioImpactSelection.isFreshLocalCandidate(0, -1, 0));
    expectIllegalArgument(() -> PeerAudioImpactSelection.isFreshLocalCandidate(0, 0, -1));
    expectIllegalArgument(
        () ->
            PeerAudioImpactSelection.closestCandidateIndex(
                List.of(), -1, 0, 0));
    expectIllegalArgument(
        () ->
            PeerAudioImpactSelection.closestCandidateIndex(
                List.of(new PeerAudioImpactSelection.Candidate(0, 0)), 0, -1, 0));
  }

  private static void expectIllegalArgument(Runnable action) {
    try {
      action.run();
      throw new AssertionError("expected IllegalArgumentException");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
