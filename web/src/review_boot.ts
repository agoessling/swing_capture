/** Returns true when the review app is controlling one or two Android capture nodes. */
export function isAndroidReviewMode(parameters: URLSearchParams): boolean {
  return (
    parameters.has("node") ||
    parameters.has("node_token") ||
    parameters.has("dtl_node") ||
    parameters.has("face_node")
  );
}

/** Android nodes use bounded authenticated status polling; the host retains its event stream. */
export function reviewEventsSupported(parameters: URLSearchParams): boolean {
  return !isAndroidReviewMode(parameters);
}
