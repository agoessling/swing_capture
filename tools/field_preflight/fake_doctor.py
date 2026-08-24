"""Hermetic strict-doctor command boundary for the executable smoke test."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Sequence


EXPECTED_NODES = (
    "10.168.168.111:37123=http://10.168.168.111:8088",
    "10.168.168.241:42491=http://10.168.168.241:8088",
)


class _Arguments(argparse.Namespace):
    """Typed fake-doctor values populated by argparse."""

    def __init__(self) -> None:
        """Initialize defaults replaced by the test invocation."""
        super().__init__()
        self.node: list[str] = []
        self.json: Path = Path()
        self.adb: Path = Path()
        self.expected_apk: Path = Path()
        self.require_wireless_adb: bool = False
        self.require_monitoring: bool = False
        self.require_stopped_clean: bool = False


def _capture_status(*, monitoring: bool) -> dict[str, object]:
    """Return rich numeric telemetry shaped like the production doctor evidence."""
    return {
        "schema_version": 2,
        "state": "armed" if monitoring else "ready",
        "armed": monitoring,
        "active_session_id": None,
        "video_frames": 913,
        "audio_frames": 1_826,
        "ring_bytes": 12_345_678,
        "ring_duration_us": 9_000_000,
        "pose": {
            "metrics": {
                "recent_inference_sample_count": 150,
                "recent_inference_duration_p95_ns": "180000000",
            },
            "standby_audio": {
                "ready": monitoring,
                "peak_amplitude": 0.2,
                "noise_floor": 0.001,
                "threshold": 0.015,
            },
            "autonomous_pair": {
                "state": "monitoring" if monitoring else "stopped",
                "active_shared_session_id": None,
                "replication_backlog_size": 0,
                "local_triggered": False,
                "peer_triggered": False,
                "local_published": False,
                "peer_published": False,
            },
        },
        "operational_health": {
            "thermal": {"status": 0, "headroom": 0.5},
            "storage": {"usable_bytes": 8 * 1024**3, "ready": True},
        },
        "pair_network_health": {
            "schema_version": 1,
            "state": "good",
            "raw_state": "good",
            "local_to_peer": {"p95_round_trip_ns": "14000000"},
            "peer_to_local": {"p95_round_trip_ns": "15000000"},
        },
    }


def _field_recording_status() -> dict[str, object]:
    return {
        "schema_version": 1,
        "state": "idle",
        "active_recording_id": None,
        "shared_recording_id": None,
        "started_at_utc": None,
        "started_elapsed_realtime_ns": None,
        "elapsed_ms": 0,
        "video_bytes": "0",
        "audio_frames": "0",
        "max_duration_seconds": 600,
        "error": "",
        "hil_reject_next_start": False,
        "hil_fail_next_accepted_start": False,
        "hil_accepted_start_waiting": False,
    }


def main(arguments: Sequence[str]) -> int:
    """Validate the contract received from the preflight and emit a passing report."""
    parser = argparse.ArgumentParser()
    parser.add_argument("adb", type=Path)
    parser.add_argument("--expected-apk", required=True, type=Path)
    parser.add_argument("--node", action="append", required=True)
    parser.add_argument("--json", required=True, type=Path)
    parser.add_argument("--require-wireless-adb", action="store_true")
    state_group = parser.add_mutually_exclusive_group()
    state_group.add_argument("--require-monitoring", action="store_true")
    state_group.add_argument("--require-stopped-clean", action="store_true")
    options = parser.parse_args(arguments, namespace=_Arguments())
    if not options.adb.is_file() or not options.expected_apk.is_file():
        print("strict doctor dependencies are unavailable", file=sys.stderr)
        return 64
    if tuple(options.node) != EXPECTED_NODES:
        print(f"unexpected nodes: {options.node!r}", file=sys.stderr)
        return 64
    if not options.require_wireless_adb or not (
        options.require_monitoring or options.require_stopped_clean
    ):
        print("strict admission flags were not forwarded", file=sys.stderr)
        return 64
    forced_failure = os.environ.get("SWING_CAPTURE_FAKE_DOCTOR_FAIL") == "1"
    apk_digest = hashlib.sha256(options.expected_apk.read_bytes()).hexdigest()
    report: dict[str, object] = {
        "schema_version": 1,
        "report_type": "android_pair_preflight",
        "passed": not forced_failure,
        "credentials_redacted": True,
        "expected_apk_sha256": apk_digest,
        "nodes": [
            {
                "serial": "10.168.168.111:37123",
                "role": "face_on",
                "installed_apk_sha256": apk_digest,
                "capture_status": _capture_status(monitoring=options.require_monitoring),
                "field_recording_status": _field_recording_status(),
            },
            {
                "serial": "10.168.168.241:42491",
                "role": "down_the_line",
                "installed_apk_sha256": apk_digest,
                "capture_status": _capture_status(monitoring=options.require_monitoring),
                "field_recording_status": _field_recording_status(),
            },
        ],
        "lan_diagnostics": {
            "schema_version": 1,
            "nodes": [
                {
                    "serial": "10.168.168.111:37123",
                    "origin": "http://10.168.168.111:8088",
                    "bssid": "44:d9:e7:aa:bb:01",
                },
                {
                    "serial": "10.168.168.241:42491",
                    "origin": "http://10.168.168.241:8088",
                    "bssid": "44:d9:e7:aa:bb:02",
                },
            ],
            "reachability_matrix": [
                {
                    "source": "field_host",
                    "target": "http://10.168.168.111:8088",
                    "transport": "direct_http_clock",
                    "reachable": True,
                    "http_status": 200,
                },
                {
                    "source": "http://10.168.168.111:8088",
                    "target": "http://10.168.168.241:8088",
                    "transport": "adb_shell_wifi_icmp",
                    "reachable": True,
                    "http_status": None,
                },
                {
                    "source": "field_host",
                    "target": "http://10.168.168.241:8088",
                    "transport": "direct_http_clock",
                    "reachable": True,
                    "http_status": 200,
                },
                {
                    "source": "http://10.168.168.241:8088",
                    "target": "http://10.168.168.111:8088",
                    "transport": "adb_shell_wifi_icmp",
                    "reachable": True,
                    "http_status": None,
                },
            ],
        },
    }
    if forced_failure:
        report["checks"] = [
            {
                "name": "node.1.exact_apk",
                "message": "installed APK differs from the requested Bazel artifact",
                "passed": False,
            }
        ]
    options.json.write_text(json.dumps(report) + "\n", encoding="utf-8")
    message = (
        "hermetic strict pair admission failed"
        if forced_failure
        else "hermetic strict pair admission passed"
    )
    print(message)
    return 1 if forced_failure else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
