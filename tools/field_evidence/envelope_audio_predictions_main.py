"""Command line for robust_hp120_x12 holdout prediction evidence."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import final

from python.runfiles import runfiles

from tools.field_evidence import envelope_audio_predictions, manifest, output

KEY_VALUE_FIELD_COUNT = 2


@final
class _Arguments(argparse.Namespace):
    """Typed command-line values populated by argparse."""

    def __init__(self) -> None:
        """Initialize defaults that argparse and its selected subcommand replace."""
        super().__init__()
        self.replay_binary: Path | None = None
        self.implementation_source: list[str] = []
        self.command = ""
        self.device_label: list[str] = []
        self.output = Path()
        self.manifest = Path()
        self.media_root = Path()
        self.policy_lock = Path()


def _arguments() -> _Arguments:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replay-binary", type=Path)
    parser.add_argument(
        "--implementation-source",
        action="append",
        metavar="LOGICAL_NAME=PATH",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    describe = commands.add_parser("describe", help="emit the exact candidate detector object")
    describe.add_argument("--device-label", action="append", required=True)
    describe.add_argument("--output", type=Path, required=True)
    predict = commands.add_parser("predict", help="emit predictions for a frozen candidate policy")
    predict.add_argument("--manifest", type=Path, required=True)
    predict.add_argument("--media-root", type=Path, required=True)
    predict.add_argument("--policy-lock", type=Path, required=True)
    predict.add_argument("--output", type=Path, required=True)
    return parser.parse_args(namespace=_Arguments())


def main() -> int:
    """Describe the candidate or publish predictions for frozen evidence."""
    arguments = _arguments()
    output.require_absent(arguments.output)
    replay_binary = arguments.replay_binary or _runfile(
        "capture/offline/experiments/envelope/streaming_envelope_replay"
    )
    sources = (
        _keyed_paths(arguments.implementation_source)
        if arguments.implementation_source
        else {
            "detector_header": _runfile(
                "capture/offline/experiments/envelope/streaming_envelope_detector.h"
            ),
            "detector_source": _runfile(
                "capture/offline/experiments/envelope/streaming_envelope_detector.cc"
            ),
            "detector_types": _runfile(
                "capture/offline/experiments/envelope/envelope_experiment.h"
            ),
            "replay_adapter": _runfile(
                "capture/offline/experiments/envelope/streaming_envelope_replay_main.cc"
            ),
        }
    )
    if arguments.command == "describe":
        report = envelope_audio_predictions.describe_detector(
            replay_binary, sources, arguments.device_label
        )
    else:
        corpus = manifest.parse_manifest(arguments.manifest.read_text(encoding="utf-8"))
        manifest.validate_assets(corpus, arguments.media_root)
        report = envelope_audio_predictions.generate_predictions(
            corpus,
            arguments.media_root,
            arguments.policy_lock.read_text(encoding="utf-8"),
            replay_binary,
            sources,
        )
    output.publish_json_exclusive(arguments.output, report)
    return 0


def _keyed_paths(encoded: list[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for item in encoded:
        fields = item.split("=", maxsplit=1)
        if len(fields) != KEY_VALUE_FIELD_COUNT or not all(fields) or fields[0] in result:
            message = "implementation source must be unique LOGICAL_NAME=PATH"
            raise ValueError(message)
        result[fields[0]] = Path(fields[1])
    return result


def _runfile(logical_path: str) -> Path:
    resolver = runfiles.Create()
    if resolver is None:
        message = "Bazel runfiles resolver is unavailable"
        raise RuntimeError(message)
    resolved = resolver.Rlocation(f"_main/{logical_path}")
    if resolved is None:
        message = f"runfile is unavailable: {logical_path}"
        raise RuntimeError(message)
    return Path(resolved)


if __name__ == "__main__":
    raise SystemExit(main())
