import type { PairNetworkDirectionHealth, PairNetworkHealth } from "./pair_network_health.js";

export function PairNetworkHealthNotice({
  health,
  label = "Pair network health",
}: {
  health: PairNetworkHealth;
  label?: string;
}) {
  if (!health.configured) {
    return null;
  }
  const tone =
    health.stale || health.state === "unusable"
      ? "error"
      : health.state === "degraded"
        ? "warning"
        : "ok";
  const visibleState = health.stale ? "Unusable · stale" : stateLabel(health.state);
  return (
    <section
      aria-label={label}
      className={`pair-network-health notice notice-${tone}`}
      role={tone === "ok" ? "status" : "alert"}
    >
      <header>
        <strong>Pair network: {visibleState}</strong>
        <span>
          {health.measured
            ? `Application-level bidirectional sample · ${formatAge(health.age_ns)} old`
            : "Awaiting application-level bidirectional measurement"}
        </span>
      </header>
      {health.local_to_peer === null || health.peer_to_local === null ? null : (
        <dl>
          <DirectionMetrics direction={health.local_to_peer} label="Leader → peer" />
          <DirectionMetrics direction={health.peer_to_local} label="Peer → leader" />
        </dl>
      )}
      {health.issues.length === 0 ? null : (
        <ul>
          {health.issues.map((issue) => (
            <li key={issue}>{issue}</li>
          ))}
        </ul>
      )}
    </section>
  );
}

export function UnknownPairNetworkHealthNotice() {
  return (
    <section
      aria-label="Pair network health"
      className="pair-network-health notice notice-error"
      role="alert"
    >
      <header>
        <strong>Pair network: Unknown</strong>
        <span>The configured leader did not report current pair-network health.</span>
      </header>
    </section>
  );
}

function DirectionMetrics({
  direction,
  label,
}: {
  direction: PairNetworkDirectionHealth;
  label: string;
}) {
  return (
    <div>
      <dt>{label}</dt>
      <dd>
        {direction.successes}/{direction.attempts} replies · {direction.timeouts} timeouts · p95{" "}
        {formatNanoseconds(direction.p95_round_trip_ns)} · jitter{" "}
        {formatNanoseconds(direction.jitter_ns)} ·{" "}
        {formatBitRate(direction.transfer_bits_per_second)}
        {direction.transfer_complete ? "" : " · transfer incomplete"}
      </dd>
    </div>
  );
}

function stateLabel(state: PairNetworkHealth["state"]): string {
  return `${state[0]?.toUpperCase() ?? ""}${state.slice(1)}`;
}

function formatNanoseconds(value: string): string {
  return `${(Number(BigInt(value)) / 1_000_000).toFixed(1)} ms`;
}

function formatAge(value: string): string {
  const seconds = Number(BigInt(value)) / 1_000_000_000;
  return seconds < 10 ? `${seconds.toFixed(1)} s` : `${Math.round(seconds)} s`;
}

function formatBitRate(value: number): string {
  if (value >= 1_000_000) {
    return `${(value / 1_000_000).toFixed(1)} Mbps`;
  }
  return `${(value / 1_000).toFixed(0)} kbps`;
}
