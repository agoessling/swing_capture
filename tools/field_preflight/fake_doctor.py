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


def main(arguments: Sequence[str]) -> int:
    """Validate the contract received from the preflight and emit a passing report."""
    parser = argparse.ArgumentParser()
    parser.add_argument("adb", type=Path)
    parser.add_argument("--expected-apk", required=True, type=Path)
    parser.add_argument("--node", action="append", required=True)
    parser.add_argument("--json", required=True, type=Path)
    parser.add_argument("--require-wireless-adb", action="store_true")
    parser.add_argument("--require-monitoring", action="store_true")
    options = parser.parse_args(arguments, namespace=_Arguments())
    if not options.adb.is_file() or not options.expected_apk.is_file():
        print("strict doctor dependencies are unavailable", file=sys.stderr)
        return 64
    if tuple(options.node) != EXPECTED_NODES:
        print(f"unexpected nodes: {options.node!r}", file=sys.stderr)
        return 64
    if not options.require_wireless_adb or not options.require_monitoring:
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
            },
            {
                "serial": "10.168.168.241:42491",
                "role": "down_the_line",
                "installed_apk_sha256": apk_digest,
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
