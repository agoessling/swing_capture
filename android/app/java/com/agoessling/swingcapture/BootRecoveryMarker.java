package com.agoessling.swingcapture;

/** Pure transition for the non-secret marker retained across Android OS boot boundaries. */
final class BootRecoveryMarker {
  record Observation(
      long observationCount,
      String lastEvent,
      long lastObservedEpochMillis,
      long lastObservedElapsedRealtimeNanos) {
    Observation {
      if (observationCount <= 0) {
        throw new IllegalArgumentException("boot recovery observation is invalid");
      }
      validateStored(
          observationCount,
          lastEvent,
          lastObservedEpochMillis,
          lastObservedElapsedRealtimeNanos);
    }
  }

  private BootRecoveryMarker() {}

  static void validateStored(
      long observationCount,
      String lastEvent,
      long lastObservedEpochMillis,
      long lastObservedElapsedRealtimeNanos) {
    boolean pristine =
        observationCount == 0
            && lastEvent != null
            && lastEvent.isEmpty()
            && lastObservedEpochMillis == 0
            && lastObservedElapsedRealtimeNanos == 0;
    boolean observed =
        observationCount > 0
            && lastEvent != null
            && !lastEvent.isBlank()
            && lastObservedEpochMillis > 0
            && lastObservedElapsedRealtimeNanos >= 0;
    if (!pristine && !observed) {
      throw new IllegalArgumentException("stored boot recovery marker is inconsistent");
    }
  }

  static Observation recordedAfter(
      long previousObservationCount,
      String event,
      long observedEpochMillis,
      long observedElapsedRealtimeNanos) {
    if (previousObservationCount < 0) {
      throw new IllegalStateException("boot observation counter is invalid");
    }
    if (previousObservationCount == Long.MAX_VALUE) {
      throw new IllegalStateException("boot observation counter is exhausted");
    }
    return new Observation(
        previousObservationCount + 1,
        event,
        observedEpochMillis,
        observedElapsedRealtimeNanos);
  }
}
