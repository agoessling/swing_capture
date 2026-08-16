package com.agoessling.swingcapture.core.coordination;

import java.util.Optional;

/** Result of intersecting repeated four-timestamp offset bounds. */
public record ClockExchangeEstimateResult(
    ClockExchangeEstimateStatus status, Optional<ClockOffsetEstimate> estimate, String detail) {
  public ClockExchangeEstimateResult {
    if ((status == ClockExchangeEstimateStatus.READY) != estimate.isPresent()) {
      throw new IllegalArgumentException("only a ready clock result may contain an estimate");
    }
  }
}
