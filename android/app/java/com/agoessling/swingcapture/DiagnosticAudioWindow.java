package com.agoessling.swingcapture;

/** Pure selection of a clipped diagnostic PCM interval from a rolling retained range. */
public final class DiagnosticAudioWindow {
  public enum State {
    AVAILABLE,
    WAITING_FOR_POST_ROLL,
    MARKER_EVICTED,
  }

  public record Selection(State state, long firstFramePosition, long endFramePosition) {
    public Selection {
      if (state == State.AVAILABLE
          && (firstFramePosition < 0 || endFramePosition <= firstFramePosition)) {
        throw new IllegalArgumentException("available diagnostic audio range is invalid");
      }
      if (state != State.AVAILABLE
          && (firstFramePosition != -1 || endFramePosition != -1)) {
        throw new IllegalArgumentException("unavailable diagnostic audio range must be absent");
      }
    }
  }

  private DiagnosticAudioWindow() {}

  public static Selection select(
      long markerFramePosition,
      long retainedFirstFramePosition,
      long retainedEndFramePosition,
      int requestedPreRollFrames,
      int requestedPostRollFrames) {
    if (markerFramePosition < 0
        || requestedPreRollFrames < 0
        || requestedPostRollFrames <= 0
        || (retainedFirstFramePosition < 0) != (retainedEndFramePosition < 0)
        || (retainedFirstFramePosition >= 0
            && retainedEndFramePosition < retainedFirstFramePosition)) {
      throw new IllegalArgumentException("diagnostic audio selection input is invalid");
    }
    if (retainedFirstFramePosition < 0) {
      return unavailable(State.WAITING_FOR_POST_ROLL);
    }
    if (markerFramePosition < retainedFirstFramePosition) {
      return unavailable(State.MARKER_EVICTED);
    }
    long desiredEnd = Math.addExact(markerFramePosition, requestedPostRollFrames);
    if (retainedEndFramePosition < desiredEnd) {
      return unavailable(State.WAITING_FOR_POST_ROLL);
    }
    long desiredFirst = Math.max(0, markerFramePosition - (long) requestedPreRollFrames);
    return new Selection(
        State.AVAILABLE, Math.max(desiredFirst, retainedFirstFramePosition), desiredEnd);
  }

  private static Selection unavailable(State state) {
    return new Selection(state, -1, -1);
  }
}
