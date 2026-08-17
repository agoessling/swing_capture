package com.agoessling.swingcapture;

import com.agoessling.swingcapture.audio.Pcm16EvidenceRing;

/** Pure allocation and publication policy that keeps exact PCM evidence exclusive to audio HIL. */
public final class AudioEvidencePolicy {
  public static final int RETENTION_SECONDS = 4;
  public static final int RETENTION_FRAMES =
      RETENTION_SECONDS * Pcm16EvidenceRing.SAMPLE_RATE_HZ;

  private AudioEvidencePolicy() {}

  public static Pcm16EvidenceRing ringForCapture(boolean audioHilMode) {
    return audioHilMode ? new Pcm16EvidenceRing(RETENTION_FRAMES) : null;
  }

  public static boolean requiresPublishedEvidence(
      Pcm16EvidenceRing allocatedRing, String triggerSource) {
    return allocatedRing != null && "local_audio".equals(triggerSource);
  }
}
