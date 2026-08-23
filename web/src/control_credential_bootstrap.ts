import { MutableControlCredential } from "./control_credential.js";

const STORAGE_PREFIX = "swing_capture.control_credential.v1";

export interface SessionCredentialStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}

export interface ControlCredentialBinding {
  parameter: string;
  scope: string;
}

export interface ConsumedControlCredentials {
  credentials: ReadonlyMap<string, MutableControlCredential>;
  removedFromUrl: boolean;
}

/** Retains a non-secret Android marker for the historical same-origin `?node_token=...` URL. */
export function retainSingleNodeAndroidMode(parameters: URLSearchParams): void {
  if (
    parameters.has("node_token") &&
    !parameters.has("node") &&
    !parameters.has("dtl_node") &&
    !parameters.has("face_node")
  ) {
    parameters.set("node", "");
  }
}

/**
 * Consumes bootstrap credentials from a mutable parameter set.
 *
 * A credential supplied by the phone URL wins and replaces the tab-scoped copy. Subsequent
 * reloads recover that copy without leaving a bearer token in the address bar, browser history,
 * bookmarks, or referrer URLs. The scope prevents a credential retained for one phone origin
 * from being offered to a different phone after the node URL changes.
 */
export function consumeControlCredentials(
  parameters: URLSearchParams,
  storage: SessionCredentialStorage,
  bindings: readonly ControlCredentialBinding[],
): ConsumedControlCredentials {
  const credentials = new Map<string, MutableControlCredential>();
  let removedFromUrl = false;
  for (const binding of bindings) {
    if (credentials.has(binding.parameter)) {
      throw new Error(`Duplicate control credential parameter: ${binding.parameter}`);
    }
    const storageKey = credentialStorageKey(binding);
    const supplied = parameters.get(binding.parameter);
    let initial = supplied ?? safeStorageRead(storage, storageKey) ?? "";
    if (supplied !== null) {
      parameters.delete(binding.parameter);
      removedFromUrl = true;
      safeStorageWrite(storage, storageKey, supplied);
      initial = supplied;
    }
    credentials.set(
      binding.parameter,
      new MutableControlCredential(initial, (replacement) => {
        safeStorageWrite(storage, storageKey, replacement);
      }),
    );
  }
  return { credentials, removedFromUrl };
}

export function credentialFreeUrl(currentUrl: string, parameters: URLSearchParams): string {
  const url = new URL(currentUrl);
  const search = parameters.toString();
  url.search = search.length === 0 ? "" : `?${search}`;
  return url.toString();
}

function credentialStorageKey(binding: ControlCredentialBinding): string {
  if (binding.parameter.length === 0 || binding.scope.length === 0) {
    throw new Error("Control credential storage binding must have a parameter and scope");
  }
  return `${STORAGE_PREFIX}.${encodeURIComponent(binding.parameter)}.${encodeURIComponent(binding.scope)}`;
}

function safeStorageRead(storage: SessionCredentialStorage, key: string): string | null {
  try {
    return storage.getItem(key);
  } catch {
    // A privacy policy may disable session storage. The supplied credential remains usable for
    // this page load, but is never put back into the URL as an insecure fallback.
    return null;
  }
}

function safeStorageWrite(storage: SessionCredentialStorage, key: string, value: string): void {
  try {
    storage.setItem(key, value);
  } catch {
    // Preserve current-page operation when storage is unavailable without leaking into the URL.
  }
}
