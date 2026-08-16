package com.agoessling.swingcapture.core.coordination;

/** Explicit availability state for a repeated clock-exchange estimate. */
public enum ClockExchangeEstimateStatus {
  READY,
  INSUFFICIENT_SAMPLES,
  INCONSISTENT_BOUNDS
}
