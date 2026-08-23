"""Tests for the credential-free Bazel BEP performance summary."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path

from tools.bazel_bep_summary import summarize

EXPECTED_WALL_MS = 100
EXPECTED_CRITICAL_PATH_MS = 55.0
EXPECTED_ACTIONS_EXECUTED = 12
EXPECTED_CACHE_HITS = 5
EXPECTED_ASPECT_ACTIONS = 29
EXPECTED_ASPECTS = 2
EXPECTED_ASPECT_CONFIGURED_TARGETS = 4
EXPECTED_PACKAGES = 9


def _write_bep(path: Path) -> None:
    events = [
        {
            "started": {
                "buildToolVersion": "9.2.0",
                "command": "test",
            }
        },
        {
            "structuredCommandLine": {
                "commandLineLabel": "canonical",
                "sections": [
                    {
                        "sectionLabel": "command options",
                        "optionList": {
                            "option": [
                                {"optionName": "aspects", "optionValue": "one"},
                                {"optionName": "aspects", "optionValue": "two"},
                                {"optionName": "build_manual_tests", "optionValue": "true"},
                            ]
                        },
                    },
                    {"sectionLabel": "residual", "chunkList": {"chunk": ["//..."]}},
                ],
            }
        },
        {
            "structuredCommandLine": {
                "commandLineLabel": "original",
                "sections": [{"sectionLabel": "residual", "chunkList": {"chunk": ["//..."]}}],
            }
        },
        {
            "buildMetrics": {
                "actionSummary": {
                    "actionsExecuted": "12",
                    "actionData": [
                        {"mnemonic": "Javac", "actionsExecuted": "3"},
                        {"mnemonic": "Unused", "actionsCreated": "4"},
                    ],
                    "runnerCount": [{"name": "local", "count": "8"}],
                    "actionCacheStatistics": {"hits": "5", "misses": "7"},
                },
                "timingMetrics": {
                    "wallTimeInMs": "100",
                    "cpuTimeInMs": "250",
                    "analysisPhaseTimeInMs": "30",
                    "executionPhaseTimeInMs": "60",
                    "criticalPathTime": "0.055s",
                },
                "targetMetrics": {
                    "targetsConfigured": "44",
                    "targetsConfiguredNotIncludingAspects": "40",
                },
                "packageMetrics": {"packagesLoaded": "9"},
                "buildGraphMetrics": {
                    "actionCount": "120",
                    "actionCountNotIncludingAspects": "91",
                },
            }
        },
        {"finished": {"exitCode": {"code": 0}}},
    ]
    path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")


def main() -> None:
    """Exercise a representative complete BEP stream."""
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "cold.bep.json"
        _write_bep(path)
        result = summarize(path)
    assert result["name"] == "cold"
    assert result["targets"] == ["//..."]
    assert result["wall_ms"] == EXPECTED_WALL_MS
    assert result["critical_path_ms"] == EXPECTED_CRITICAL_PATH_MS
    assert result["actions_executed"] == EXPECTED_ACTIONS_EXECUTED
    assert result["action_cache_hits"] == EXPECTED_CACHE_HITS
    assert result["aspect_action_graph_actions"] == EXPECTED_ASPECT_ACTIONS
    assert result["aspect_configured_targets"] == EXPECTED_ASPECT_CONFIGURED_TARGETS
    assert result["packages_loaded"] == EXPECTED_PACKAGES
    assert result["aspects_enabled"] == EXPECTED_ASPECTS
    assert result["build_manual_tests"] is True
    assert result["executed_by_mnemonic"] == {"Javac": 3}


if __name__ == "__main__":
    main()
