package com.agoessling.swingcapture.core.coordination;

/** Per-report admission result, independent of the final pair decision. */
public enum SubmissionStatus {
  ACCEPTED,
  DUPLICATE_REPORT,
  UNRELATED_SESSION,
  STALE_REPORT,
  LATE_REPORT,
  EXCESSIVE_UNCERTAINTY
}
