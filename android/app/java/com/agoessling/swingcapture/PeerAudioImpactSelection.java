package com.agoessling.swingcapture;

import java.util.List;
import java.util.Objects;
import java.util.OptionalInt;

/** Pure timing policy for choosing a shadow phone's local audio evidence for a peer impact. */
final class PeerAudioImpactSelection {
  record Candidate(long strikeNanos, long uncertaintyNanos) {
    Candidate {
      if (strikeNanos < 0 || uncertaintyNanos < 0) {
        throw new IllegalArgumentException("candidate timing cannot be negative");
      }
    }
  }

  private PeerAudioImpactSelection() {}

  static boolean isFreshLocalCandidate(
      long peerArrivalNanos, long localStrikeNanos, long maximumCandidateAgeNanos) {
    if (peerArrivalNanos < 0 || localStrikeNanos < 0) {
      throw new IllegalArgumentException("timestamps cannot be negative");
    }
    if (maximumCandidateAgeNanos < 0) {
      throw new IllegalArgumentException("maximumCandidateAgeNanos cannot be negative");
    }
    return localStrikeNanos <= peerArrivalNanos
        && peerArrivalNanos - localStrikeNanos <= maximumCandidateAgeNanos;
  }

  /**
   * Chooses the candidate closest to a clock-mapped peer strike. Candidate and clock uncertainty
   * widen only that candidate's fixed base tolerance; newest evidence wins an exact tie.
   */
  static OptionalInt closestCandidateIndex(
      List<Candidate> candidates,
      long mappedPeerStrikeNanos,
      long baseToleranceNanos,
      long clockUncertaintyNanos) {
    Objects.requireNonNull(candidates, "candidates");
    if (mappedPeerStrikeNanos < 0 || baseToleranceNanos < 0 || clockUncertaintyNanos < 0) {
      throw new IllegalArgumentException("selection timing cannot be negative");
    }
    int selected = -1;
    long selectedDistance = Long.MAX_VALUE;
    for (int index = 0; index < candidates.size(); ++index) {
      Candidate candidate = Objects.requireNonNull(candidates.get(index), "candidate");
      long distance = absoluteDistance(candidate.strikeNanos(), mappedPeerStrikeNanos);
      long tolerance =
          saturatingAdd(
              baseToleranceNanos,
              saturatingAdd(clockUncertaintyNanos, candidate.uncertaintyNanos()));
      if (distance <= tolerance && distance <= selectedDistance) {
        selected = index;
        selectedDistance = distance;
      }
    }
    return selected < 0 ? OptionalInt.empty() : OptionalInt.of(selected);
  }

  private static long saturatingAdd(long first, long second) {
    if (Long.MAX_VALUE - first < second) {
      return Long.MAX_VALUE;
    }
    return first + second;
  }

  private static long absoluteDistance(long first, long second) {
    // Both inputs are nonnegative, so the larger-minus-smaller form cannot overflow even at the
    // full long range. Avoid subtract-then-abs, where Long.MIN_VALUE would remain negative.
    return first >= second ? first - second : second - first;
  }
}
