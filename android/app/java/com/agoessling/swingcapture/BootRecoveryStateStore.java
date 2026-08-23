package com.agoessling.swingcapture;

import android.content.Context;
import android.content.SharedPreferences;

/** Device-protected, non-secret evidence that the APK observed an OS boot boundary. */
final class BootRecoveryStateStore {
  private static final String PREFERENCES = "boot_recovery_state";
  private static final String OBSERVATION_COUNT = "observation_count";
  private static final String LAST_EVENT = "last_event";
  private static final String LAST_OBSERVED_EPOCH_MILLIS = "last_observed_epoch_millis";
  private static final String LAST_OBSERVED_ELAPSED_REALTIME_NANOS =
      "last_observed_elapsed_realtime_nanos";

  record Snapshot(
      long observationCount,
      String lastEvent,
      long lastObservedEpochMillis,
      long lastObservedElapsedRealtimeNanos) {
    Snapshot {
      BootRecoveryMarker.validateStored(
          observationCount,
          lastEvent,
          lastObservedEpochMillis,
          lastObservedElapsedRealtimeNanos);
    }

    boolean observed() {
      return observationCount > 0;
    }
  }

  private BootRecoveryStateStore() {}

  static synchronized void record(
      Context context, String event, long observedEpochMillis, long observedElapsedRealtimeNanos) {
    SharedPreferences preferences = preferences(context);
    long previous = preferences.getLong(OBSERVATION_COUNT, 0);
    BootRecoveryMarker.Observation observation =
        BootRecoveryMarker.recordedAfter(
            previous, event, observedEpochMillis, observedElapsedRealtimeNanos);
    if (!preferences
        .edit()
        .putLong(OBSERVATION_COUNT, observation.observationCount())
        .putString(LAST_EVENT, observation.lastEvent())
        .putLong(LAST_OBSERVED_EPOCH_MILLIS, observation.lastObservedEpochMillis())
        .putLong(
            LAST_OBSERVED_ELAPSED_REALTIME_NANOS,
            observation.lastObservedElapsedRealtimeNanos())
        .commit()) {
      throw new IllegalStateException("unable to persist boot recovery marker");
    }
  }

  static synchronized Snapshot snapshot(Context context) {
    SharedPreferences preferences = preferences(context);
    return new Snapshot(
        preferences.getLong(OBSERVATION_COUNT, 0),
        preferences.getString(LAST_EVENT, ""),
        preferences.getLong(LAST_OBSERVED_EPOCH_MILLIS, 0),
        preferences.getLong(LAST_OBSERVED_ELAPSED_REALTIME_NANOS, 0));
  }

  private static SharedPreferences preferences(Context context) {
    Context deviceProtected = context.createDeviceProtectedStorageContext();
    if (!deviceProtected.isDeviceProtectedStorage()) {
      throw new IllegalStateException("boot recovery marker requires device-protected storage");
    }
    return deviceProtected.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
  }
}
