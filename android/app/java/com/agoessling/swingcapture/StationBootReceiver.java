package com.agoessling.swingcapture;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import android.util.Log;

/** Records boot/unlock evidence without starting camera, microphone, or a foreground service. */
public final class StationBootReceiver extends BroadcastReceiver {
  private static final String TAG = "SwingCaptureBoot";

  @Override
  public void onReceive(Context context, Intent intent) {
    UnattendedRecoveryPolicy.bootEvent(intent == null ? null : intent.getAction())
        .ifPresent(
            event -> {
              try {
                BootRecoveryStateStore.record(
                    context, event, System.currentTimeMillis(), SystemClock.elapsedRealtimeNanos());
              } catch (RuntimeException failure) {
                Log.e(TAG, "Unable to persist boot recovery marker", failure);
              }
            });
  }
}
