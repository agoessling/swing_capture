package com.agoessling.swingcapture;

public final class SessionBoundValueTest {
  public static void main(String[] args) {
    hiddenAfterSessionChanges();
    clearedForMonitoringRestart();
  }

  private static void hiddenAfterSessionChanges() {
    SessionBoundValue<String> value = new SessionBoundValue<>();
    value.set("session-a", "mapping-a");
    assert "mapping-a".equals(value.current("session-a"));
    assert value.current("session-b") == null;
    assert value.current(null) == null;
  }

  private static void clearedForMonitoringRestart() {
    SessionBoundValue<String> value = new SessionBoundValue<>();
    value.set("session-a", "mapping-a");
    value.clear();
    assert value.current("session-a") == null;
  }
}
