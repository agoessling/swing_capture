package com.agoessling.swingcapture;

import java.util.Objects;

/** A small thread-safe value that cannot be observed outside its owning coordinated session. */
final class SessionBoundValue<T> {
  private record Entry<T>(String sessionId, T value) {}

  private volatile Entry<T> entry;

  void set(String sessionId, T value) {
    if (sessionId == null || sessionId.isBlank()) {
      throw new IllegalArgumentException("sessionId cannot be blank");
    }
    entry = new Entry<>(sessionId, Objects.requireNonNull(value, "value"));
  }

  T current(String sessionId) {
    Entry<T> observed = entry;
    return observed != null && observed.sessionId().equals(sessionId) ? observed.value() : null;
  }

  void clear() {
    entry = null;
  }
}
