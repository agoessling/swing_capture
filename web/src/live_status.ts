export interface LiveStatusVersion {
  schema_version: 1;
  stream_id: string;
  revision: string;
  generated_elapsed_realtime_ns: string;
}

export interface StatusSubscriptionScheduler {
  setTimeout(callback: () => void, delayMs: number): unknown;
  clearTimeout(handle: unknown): void;
}

export interface StatusSubscriptionOptions {
  intervalMs?: number;
  maximumBackoffMs?: number;
  scheduler?: StatusSubscriptionScheduler;
}

export function parseLiveStatusVersion(value: unknown): LiveStatusVersion | undefined {
  if (value === undefined) {
    return undefined;
  }
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error("live_status must be an object");
  }
  const object = value as Record<string, unknown>;
  if (object.schema_version !== 1) {
    throw new Error(`Unsupported live status schema: ${String(object.schema_version)}`);
  }
  const streamId = nonemptyString(object.stream_id, "live_status.stream_id");
  if (!/^[A-Za-z0-9._-]{8,128}$/.test(streamId)) {
    throw new Error("live_status.stream_id has an invalid format");
  }
  return {
    schema_version: 1,
    stream_id: streamId,
    revision: decimalString(object.revision, "live_status.revision"),
    generated_elapsed_realtime_ns: decimalString(
      object.generated_elapsed_realtime_ns,
      "live_status.generated_elapsed_realtime_ns",
    ),
  };
}

export class LiveStatusCursor {
  #streamId: string | null = null;
  #revision = -1n;
  #generatedElapsedRealtimeNs = -1n;

  accept(version: LiveStatusVersion | undefined): boolean {
    if (version === undefined) {
      return true;
    }
    const revision = BigInt(version.revision);
    const generatedElapsedRealtimeNs = BigInt(version.generated_elapsed_realtime_ns);
    if (version.stream_id !== this.#streamId) {
      this.#streamId = version.stream_id;
      this.#revision = revision;
      this.#generatedElapsedRealtimeNs = generatedElapsedRealtimeNs;
      return true;
    }
    if (
      revision <= this.#revision ||
      generatedElapsedRealtimeNs < this.#generatedElapsedRealtimeNs
    ) {
      return false;
    }
    this.#revision = revision;
    this.#generatedElapsedRealtimeNs = generatedElapsedRealtimeNs;
    return true;
  }
}

export function subscribeToReconnectableStatus(
  probe: () => Promise<LiveStatusVersion | undefined>,
  onChange: () => void,
  options: StatusSubscriptionOptions = {},
): () => void {
  const intervalMs = positiveInteger(options.intervalMs ?? 1_000, "status interval");
  const maximumBackoffMs = positiveInteger(
    options.maximumBackoffMs ?? 15_000,
    "maximum status backoff",
  );
  if (maximumBackoffMs < intervalMs) {
    throw new Error("maximum status backoff must be at least the status interval");
  }
  const scheduler = options.scheduler ?? browserScheduler;
  const cursor = new LiveStatusCursor();
  let stopped = false;
  let timer: unknown;
  let consecutiveFailures = 0;
  let connected = true;

  const schedule = (delayMs: number) => {
    if (!stopped) {
      timer = scheduler.setTimeout(() => void poll(), delayMs);
    }
  };
  const poll = async () => {
    if (stopped) {
      return;
    }
    try {
      const accepted = cursor.accept(await probe());
      if (stopped) {
        return;
      }
      consecutiveFailures = 0;
      connected = true;
      if (accepted) {
        onChange();
      }
      schedule(intervalMs);
    } catch {
      if (stopped) {
        return;
      }
      ++consecutiveFailures;
      if (connected) {
        connected = false;
        onChange();
      }
      schedule(reconnectDelayMs(intervalMs, maximumBackoffMs, consecutiveFailures));
    }
  };

  schedule(intervalMs);
  return () => {
    stopped = true;
    if (timer !== undefined) {
      scheduler.clearTimeout(timer);
    }
  };
}

export function reconnectDelayMs(
  intervalMs: number,
  maximumBackoffMs: number,
  consecutiveFailures: number,
): number {
  const exponent = Math.max(0, Math.min(30, consecutiveFailures - 1));
  return Math.min(maximumBackoffMs, intervalMs * 2 ** exponent);
}

const browserScheduler: StatusSubscriptionScheduler = {
  setTimeout: (callback, delayMs) => globalThis.setTimeout(callback, delayMs),
  clearTimeout: (handle) => globalThis.clearTimeout(handle as ReturnType<typeof setTimeout>),
};

function nonemptyString(value: unknown, label: string): string {
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`${label} must be a nonempty string`);
  }
  return value;
}

function decimalString(value: unknown, label: string): string {
  const result = nonemptyString(value, label);
  if (!/^\d+$/.test(result)) {
    throw new Error(`${label} must be an unsigned decimal string`);
  }
  return result;
}

function positiveInteger(value: number, label: string): number {
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`${label} must be a positive integer`);
  }
  return value;
}
