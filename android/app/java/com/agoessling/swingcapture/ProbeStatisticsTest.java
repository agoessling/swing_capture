package com.agoessling.swingcapture;

/** Hermetic executable test for capture timing summaries. */
public final class ProbeStatisticsTest {
  private ProbeStatisticsTest() {}

  public static void main(String[] arguments) {
    nominal240Fps();
    anomaliesAreCounted();
    emptySeriesIsSafe();
  }

  private static void nominal240Fps() {
    ProbeStatistics statistics = new ProbeStatistics();
    for (int index = 0; index < 241; ++index) {
      statistics.add(index * 4_166_667L);
    }
    check(statistics.count() == 241, "nominal count");
    check(statistics.distinctCount() == 241, "nominal distinct count");
    check(statistics.span() == 1_000_000_080L, "nominal span");
    check(statistics.minimumPositiveDelta() == 4_166_667L, "nominal minimum delta");
    check(statistics.maximumPositiveDelta() == 4_166_667L, "nominal maximum delta");
    check(Math.abs(statistics.measuredRate(1_000_000_000.0) - 240.0) < 0.001,
        "nominal measured rate");
  }

  private static void anomaliesAreCounted() {
    ProbeStatistics statistics = new ProbeStatistics();
    statistics.add(10);
    statistics.add(20);
    statistics.add(20);
    statistics.add(15);
    check(statistics.count() == 4, "anomaly count");
    check(statistics.distinctCount() == 3, "anomaly distinct count");
    check(statistics.duplicateCount() == 1, "duplicate count");
    check(statistics.nonmonotonicCount() == 1, "nonmonotonic count");
    statistics.add(10);
    check(statistics.distinctCount() == 3, "true distinct value count");
    check(statistics.measuredRate(1.0) == 0.0, "invalid rate suppression");
  }

  private static void emptySeriesIsSafe() {
    ProbeStatistics statistics = new ProbeStatistics();
    check(statistics.first() == 0, "empty first");
    check(statistics.last() == 0, "empty last");
    check(statistics.span() == 0, "empty span");
    check(statistics.minimumPositiveDelta() == 0, "empty minimum delta");
    check(statistics.measuredRate(1.0) == 0.0, "empty rate");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
