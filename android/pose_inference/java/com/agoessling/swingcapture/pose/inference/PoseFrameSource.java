package com.agoessling.swingcapture.pose.inference;

import java.io.IOException;

/**
 * Pull-based five-Hz frame source shared by deterministic replay and future live-camera adapters.
 * Returned frames transfer ownership to the caller.
 */
public interface PoseFrameSource extends AutoCloseable {
  String sourceId();

  /** Returns the next frame, or {@code null} at end of stream. */
  RgbFrame nextFrame() throws IOException;

  @Override
  void close();
}
