package com.agoessling.swingcapture.node;

/** Thread-safe externally visible state for a continuously armed capture node. */
public final class CaptureRuntime {
  public enum State {
    STOPPED("stopped"),
    STARTING("starting"),
    ARMED("armed"),
    POSTROLL("postroll"),
    PUBLISHING("publishing"),
    ERROR("error");

    private final String wireName;

    State(String wireName) {
      this.wireName = wireName;
    }

    public String wireName() {
      return wireName;
    }
  }

  public static final class Snapshot {
    private final State state;
    private final boolean armed;
    private final String activeSessionId;
    private final String error;
    private final long videoFrames;
    private final long audioFrames;
    private final long ringBytes;
    private final long ringDurationUs;
    private final long lastTriggerElapsedRealtimeNanos;

    private Snapshot(
        State state,
        boolean armed,
        String activeSessionId,
        String error,
        long videoFrames,
        long audioFrames,
        long ringBytes,
        long ringDurationUs,
        long lastTriggerElapsedRealtimeNanos) {
      this.state = state;
      this.armed = armed;
      this.activeSessionId = activeSessionId;
      this.error = error;
      this.videoFrames = videoFrames;
      this.audioFrames = audioFrames;
      this.ringBytes = ringBytes;
      this.ringDurationUs = ringDurationUs;
      this.lastTriggerElapsedRealtimeNanos = lastTriggerElapsedRealtimeNanos;
    }

    public State state() {
      return state;
    }

    public boolean armed() {
      return armed;
    }

    public String activeSessionId() {
      return activeSessionId;
    }

    public String error() {
      return error;
    }

    public long videoFrames() {
      return videoFrames;
    }

    public long audioFrames() {
      return audioFrames;
    }

    public long ringBytes() {
      return ringBytes;
    }

    public long ringDurationUs() {
      return ringDurationUs;
    }

    public long lastTriggerElapsedRealtimeNanos() {
      return lastTriggerElapsedRealtimeNanos;
    }
  }

  private State state = State.STOPPED;
  private boolean armed;
  private String activeSessionId;
  private String error = "";
  private long videoFrames;
  private long audioFrames;
  private long ringBytes;
  private long ringDurationUs;
  private long lastTriggerElapsedRealtimeNanos;

  public synchronized void starting() {
    require(state == State.STOPPED || state == State.ERROR, "capture is already running");
    state = State.STARTING;
    armed = false;
    activeSessionId = null;
    error = "";
    videoFrames = 0;
    audioFrames = 0;
    ringBytes = 0;
    ringDurationUs = 0;
  }

  public synchronized void armed() {
    require(state == State.STARTING || state == State.PUBLISHING, "capture is not starting");
    state = State.ARMED;
    armed = true;
    activeSessionId = null;
    error = "";
  }

  public synchronized void updateRing(
      long videoFrames, long audioFrames, long ringBytes, long ringDurationUs) {
    require(videoFrames >= 0, "videoFrames must be nonnegative");
    require(audioFrames >= 0, "audioFrames must be nonnegative");
    require(ringBytes >= 0, "ringBytes must be nonnegative");
    require(ringDurationUs >= 0, "ringDurationUs must be nonnegative");
    this.videoFrames = videoFrames;
    this.audioFrames = audioFrames;
    this.ringBytes = ringBytes;
    this.ringDurationUs = ringDurationUs;
  }

  public synchronized void triggered(String sessionId, long elapsedRealtimeNanos) {
    require(state == State.ARMED || state == State.PUBLISHING, "capture is not armed");
    require(sessionId != null && !sessionId.isBlank(), "sessionId is required");
    require(elapsedRealtimeNanos > 0, "trigger timestamp must be positive");
    state = State.POSTROLL;
    activeSessionId = sessionId;
    lastTriggerElapsedRealtimeNanos = elapsedRealtimeNanos;
  }

  public synchronized void publishing() {
    require(state == State.POSTROLL, "capture is not collecting post-roll");
    state = State.PUBLISHING;
  }

  /** Advances only when this callback still describes the externally active capture. */
  public synchronized boolean publishing(String sessionId) {
    if (state != State.POSTROLL || !java.util.Objects.equals(activeSessionId, sessionId)) {
      return false;
    }
    state = State.PUBLISHING;
    return true;
  }

  /** Completes only the currently advertised capture; an overlapping newer capture wins. */
  public synchronized boolean published(String sessionId) {
    if (state != State.PUBLISHING || !java.util.Objects.equals(activeSessionId, sessionId)) {
      return false;
    }
    state = State.ARMED;
    armed = true;
    activeSessionId = null;
    error = "";
    return true;
  }

  public synchronized void failed(Throwable failure) {
    state = State.ERROR;
    armed = false;
    activeSessionId = null;
    error = failure == null ? "unknown capture failure" : String.valueOf(failure.getMessage());
  }

  public synchronized void stopped() {
    state = State.STOPPED;
    armed = false;
    activeSessionId = null;
    error = "";
    videoFrames = 0;
    audioFrames = 0;
    ringBytes = 0;
    ringDurationUs = 0;
  }

  public synchronized Snapshot snapshot() {
    return new Snapshot(
        state,
        armed,
        activeSessionId,
        error,
        videoFrames,
        audioFrames,
        ringBytes,
        ringDurationUs,
        lastTriggerElapsedRealtimeNanos);
  }

  private static void require(boolean condition, String message) {
    if (!condition) {
      throw new IllegalStateException(message);
    }
  }
}
