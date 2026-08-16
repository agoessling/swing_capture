package com.agoessling.swingcapture.core.coordination;

import java.util.Optional;

/** Admission result for one node trigger report. */
public record SubmissionResult(
    SubmissionStatus status, Optional<ObservedNodeTrigger> observed, String detail) {
  public SubmissionResult {
    if ((status == SubmissionStatus.ACCEPTED) != observed.isPresent()) {
      throw new IllegalArgumentException("only an accepted submission may expose an observation");
    }
  }
}
