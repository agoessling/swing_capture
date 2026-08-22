package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.PoseTriggerController;
import java.util.Objects;
import java.util.function.LongSupplier;

/** Keeps pose hysteresis alive while Camera2 temporarily switches to a high-speed session. */
final class PoseTriggerControllerLease {
  private PoseTriggerController.Config config;
  private PoseTriggerController controller;

  synchronized PoseTriggerController acquire(PoseTriggerController.Config requestedConfig) {
    Objects.requireNonNull(requestedConfig, "requestedConfig");
    if (controller == null) {
      config = requestedConfig;
      controller = new PoseTriggerController(requestedConfig);
    } else if (!config.equals(requestedConfig)) {
      throw new IllegalStateException("pose controller configuration changed during one arm cycle");
    }
    return controller;
  }

  synchronized PoseTriggerController.Decision captureEnded(long timestampNs) {
    return captureEnded(() -> timestampNs);
  }

  synchronized PoseTriggerController.Decision captureEnded(LongSupplier timestampSource) {
    if (controller == null) {
      throw new IllegalStateException("pose controller is unavailable");
    }
    synchronized (controller) {
      return controller.captureEnded(timestampSource.getAsLong());
    }
  }

  synchronized PoseTriggerController.Decision externalCaptureStarted(long timestampNs) {
    return externalCaptureStarted(() -> timestampNs);
  }

  synchronized PoseTriggerController.Decision externalCaptureStarted(LongSupplier timestampSource) {
    if (controller == null) {
      throw new IllegalStateException("pose controller is unavailable");
    }
    synchronized (controller) {
      return controller.externalCaptureStarted(timestampSource.getAsLong());
    }
  }

  synchronized void release() {
    controller = null;
    config = null;
  }
}
