"""Extract stable iteration-performance metrics from Bazel BEP JSON output."""

from __future__ import annotations

import argparse
import json
import os
from collections.abc import Iterable, Mapping
from pathlib import Path
from typing import cast


def _mapping(value: object) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        return {}
    return cast("Mapping[str, object]", value)


def _array(value: object) -> list[object]:
    if not isinstance(value, list):
        return []
    return cast("list[object]", value)


def _integer(value: object) -> int:
    if value is None or value == "":
        return 0
    if isinstance(value, (str, bytes, bytearray, bool, int, float)):
        return int(value)
    message = f"expected a BEP integer, got {type(value).__name__}"
    raise TypeError(message)


def _duration_seconds(value: object) -> float:
    if value is None:
        return 0.0
    duration = _mapping(value)
    if duration:
        return _integer(duration.get("seconds")) + _integer(duration.get("nanos")) / 1_000_000_000
    if isinstance(value, Mapping):
        return 0.0
    text = str(value).removesuffix("s")
    return float(text)


def _structured_command(
    event: Mapping[str, object],
) -> tuple[str, list[str], dict[str, list[str]]]:
    command_line = _mapping(event.get("structuredCommandLine"))
    label = str(command_line.get("commandLineLabel", ""))
    if label not in {"original", "canonical"}:
        return "", [], {}
    targets: list[str] = []
    options: dict[str, list[str]] = {}
    for section_value in _array(command_line.get("sections")):
        section = _mapping(section_value)
        if section.get("sectionLabel") == "residual":
            chunk_list = _mapping(section.get("chunkList"))
            targets = [str(target) for target in _array(chunk_list.get("chunk"))]
        option_list = _mapping(section.get("optionList"))
        for option_value in _array(option_list.get("option")):
            option = _mapping(option_value)
            name = option.get("optionName")
            if name:
                options.setdefault(str(name), []).append(str(option.get("optionValue", "")))
    return label, targets, options


def summarize(path: Path) -> dict[str, object]:
    """Returns one bounded, credential-free summary from a BEP JSON-lines file."""
    started: Mapping[str, object] = {}
    finished: Mapping[str, object] = {}
    metrics: Mapping[str, object] = {}
    targets: list[str] = []
    options: dict[str, list[str]] = {}
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, start=1):
            try:
                event = _mapping(cast("object", json.loads(line)))
            except json.JSONDecodeError as error:
                message = f"{path}:{line_number}: invalid BEP JSON"
                raise ValueError(message) from error
            if event.get("started") is not None:
                started = _mapping(event["started"])
            if event.get("finished") is not None:
                finished = _mapping(event["finished"])
            if event.get("buildMetrics") is not None:
                metrics = _mapping(event["buildMetrics"])
            label, command_targets, command_options = _structured_command(event)
            if label == "original":
                targets = command_targets
            elif label == "canonical":
                options = command_options

    if not started or not metrics or not finished:
        message = f"{path}: incomplete BEP; started, buildMetrics, and finished are required"
        raise ValueError(message)

    action_summary = _mapping(metrics.get("actionSummary"))
    cache = _mapping(action_summary.get("actionCacheStatistics"))
    timing = _mapping(metrics.get("timingMetrics"))
    graph = _mapping(metrics.get("buildGraphMetrics"))
    target_metrics = _mapping(metrics.get("targetMetrics"))
    package_metrics = _mapping(metrics.get("packageMetrics"))
    graph_actions = _integer(graph.get("actionCount"))
    graph_actions_without_aspects = _integer(graph.get("actionCountNotIncludingAspects"))
    configured_targets = _integer(target_metrics.get("targetsConfigured"))
    configured_targets_without_aspects = _integer(
        target_metrics.get("targetsConfiguredNotIncludingAspects")
    )
    action_rows = [_mapping(row) for row in _array(action_summary.get("actionData"))]
    executed_by_mnemonic = {
        str(row.get("mnemonic", "unknown")): _integer(row.get("actionsExecuted"))
        for row in action_rows
        if _integer(row.get("actionsExecuted")) > 0
    }
    runner_rows = [_mapping(row) for row in _array(action_summary.get("runnerCount"))]
    runners = {str(row.get("name", "unknown")): _integer(row.get("count")) for row in runner_rows}
    exit_code = _mapping(finished.get("exitCode"))
    return {
        "name": path.name.removesuffix(".bep.json"),
        "bazel_version": started.get("buildToolVersion", ""),
        "command": started.get("command", ""),
        "targets": targets,
        "exit_code": _integer(exit_code.get("code")),
        "wall_ms": _integer(timing.get("wallTimeInMs")),
        "cpu_ms": _integer(timing.get("cpuTimeInMs")),
        "analysis_ms": _integer(timing.get("analysisPhaseTimeInMs")),
        "execution_ms": _integer(timing.get("executionPhaseTimeInMs")),
        "critical_path_ms": round(_duration_seconds(timing.get("criticalPathTime")) * 1000, 3),
        "actions_executed": _integer(action_summary.get("actionsExecuted")),
        "action_cache_hits": _integer(cache.get("hits")),
        "action_cache_misses": _integer(cache.get("misses")),
        "targets_configured": configured_targets,
        "aspect_configured_targets": max(
            0, configured_targets - configured_targets_without_aspects
        ),
        "packages_loaded": _integer(package_metrics.get("packagesLoaded")),
        "action_graph_actions": graph_actions,
        "aspect_action_graph_actions": max(0, graph_actions - graph_actions_without_aspects),
        "aspects_enabled": len(options.get("aspects", [])),
        "build_manual_tests": options.get("build_manual_tests", ["false"])[-1]
        in {"1", "true", "yes"},
        "runners": dict(sorted(runners.items())),
        "executed_by_mnemonic": dict(sorted(executed_by_mnemonic.items())),
    }


def summarize_all(paths: Iterable[Path]) -> list[dict[str, object]]:
    """Summarizes BEP streams in command-line order."""
    return [summarize(path) for path in paths]


def _resolve_cli_path(path: Path) -> Path:
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if path.is_absolute() or workspace is None:
        return path
    return Path(workspace) / path


class _Arguments(argparse.Namespace):
    """Typed command-line values produced by the summary parser."""

    def __init__(self) -> None:
        """Initialize defaults that argparse replaces while parsing."""
        super().__init__()
        self.bep: list[Path] = []


def main() -> None:
    """Prints JSON summaries for the requested BEP streams."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bep", nargs="+", type=Path, help="Bazel --build_event_json_file output")
    arguments = parser.parse_args(namespace=_Arguments())
    paths = (_resolve_cli_path(path) for path in arguments.bep)
    print(json.dumps(summarize_all(paths), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
