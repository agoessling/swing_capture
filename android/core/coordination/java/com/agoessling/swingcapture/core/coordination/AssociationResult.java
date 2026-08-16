package com.agoessling.swingcapture.core.coordination;

import java.util.Optional;

/** Pairing decision for one shared capture session. */
public record AssociationResult(
    AssociationStatus status, Optional<AssociatedSwing> swing, String detail) {
  public AssociationResult {
    if ((status == AssociationStatus.PAIRED) != swing.isPresent()) {
      throw new IllegalArgumentException("only a paired result may contain a swing");
    }
  }
}
