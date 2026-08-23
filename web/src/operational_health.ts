export interface OperationalHealth {
  ready_for_capture: boolean;
  thermal: {
    status: number | null;
    headroom: number | null;
    ready: boolean;
    power_save_mode: boolean;
  };
  storage: {
    usable_bytes: number;
    minimum_free_bytes: number;
    ready: boolean;
  };
  issues: string[];
}

export function parseOperationalHealth(value: unknown): OperationalHealth | undefined {
  if (value === undefined) {
    return undefined;
  }
  const object = record(value, "operational_health");
  const thermal = record(object.thermal, "operational_health.thermal");
  const storage = record(object.storage, "operational_health.storage");
  const status = nullableInteger(thermal.status, "operational_health.thermal.status", 0, 6);
  const headroom = nullableNonnegativeNumber(
    thermal.headroom,
    "operational_health.thermal.headroom",
  );
  if (!Array.isArray(object.issues) || !object.issues.every((issue) => typeof issue === "string")) {
    throw new Error("operational_health.issues must be an array of strings");
  }
  const result: OperationalHealth = {
    ready_for_capture: boolean(object.ready_for_capture, "operational_health.ready_for_capture"),
    thermal: {
      status,
      headroom,
      ready: boolean(thermal.ready, "operational_health.thermal.ready"),
      power_save_mode: boolean(
        thermal.power_save_mode,
        "operational_health.thermal.power_save_mode",
      ),
    },
    storage: {
      usable_bytes: nonnegativeSafeInteger(
        storage.usable_bytes,
        "operational_health.storage.usable_bytes",
      ),
      minimum_free_bytes: positiveSafeInteger(
        storage.minimum_free_bytes,
        "operational_health.storage.minimum_free_bytes",
      ),
      ready: boolean(storage.ready, "operational_health.storage.ready"),
    },
    issues: [...object.issues],
  };
  if (result.ready_for_capture !== (result.thermal.ready && result.storage.ready)) {
    throw new Error("operational_health readiness disagrees with thermal or storage readiness");
  }
  return result;
}

export function operationalHealthDescription(health: OperationalHealth): string {
  const status =
    health.thermal.status === null
      ? "thermal status unavailable"
      : thermalStatus(health.thermal.status);
  const headroom =
    health.thermal.headroom === null
      ? "headroom unavailable"
      : `${health.thermal.headroom.toFixed(2)} thermal headroom`;
  const power = health.thermal.power_save_mode ? "power saver on" : "power saver off";
  const free = `${(health.storage.usable_bytes / 2 ** 30).toFixed(1)} GiB free`;
  return `${status} · ${headroom} · ${power} · ${free}`;
}

function thermalStatus(status: number): string {
  const labels = [
    "no thermal throttling",
    "light thermal load",
    "moderate thermal load",
    "severe thermal load",
    "critical thermal load",
    "emergency thermal load",
    "shutdown thermal load",
  ];
  return labels[status] ?? `thermal status ${String(status)}`;
}

function record(value: unknown, label: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

function boolean(value: unknown, label: string): boolean {
  if (typeof value !== "boolean") {
    throw new Error(`${label} must be a boolean`);
  }
  return value;
}

function nullableInteger(
  value: unknown,
  label: string,
  minimum: number,
  maximum: number,
): number | null {
  if (value === null) {
    return null;
  }
  if (!Number.isInteger(value) || (value as number) < minimum || (value as number) > maximum) {
    throw new Error(`${label} must be null or an integer from ${minimum} through ${maximum}`);
  }
  return value as number;
}

function nullableNonnegativeNumber(value: unknown, label: string): number | null {
  if (value === null) {
    return null;
  }
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) {
    throw new Error(`${label} must be null or a nonnegative finite number`);
  }
  return value;
}

function nonnegativeSafeInteger(value: unknown, label: string): number {
  if (!Number.isSafeInteger(value) || (value as number) < 0) {
    throw new Error(`${label} must be a nonnegative safe integer`);
  }
  return value as number;
}

function positiveSafeInteger(value: unknown, label: string): number {
  const result = nonnegativeSafeInteger(value, label);
  if (result === 0) {
    throw new Error(`${label} must be positive`);
  }
  return result;
}
