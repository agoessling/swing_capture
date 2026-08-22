package com.agoessling.swingcapture;

import java.util.function.Supplier;

/** One process-wide critical section for all SharedPreferences setup generations. */
final class NodeConfigurationProcessLock {
  private static final Object LOCK = new Object();

  private NodeConfigurationProcessLock() {}

  static <T> T call(Supplier<T> action) {
    synchronized (LOCK) {
      return action.get();
    }
  }

  static void run(Runnable action) {
    synchronized (LOCK) {
      action.run();
    }
  }
}
