package com.agoessling.swingcapture.core.coordination;

/** Terminal or not-yet-terminal outcome for a one-session dual-node decision. */
public enum AssociationStatus {
  NOT_READY,
  PAIRED,
  MISSING_OR_LATE,
  DUPLICATE_ROLE,
  SAME_NODE_FOR_BOTH_ROLES,
  UNRELATED_TRIGGERS,
  AMBIGUOUS_TIMING,
  EXCESSIVE_UNCERTAINTY
}
