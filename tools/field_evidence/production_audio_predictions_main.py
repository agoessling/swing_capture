"""Command line for exact production Android audio prediction evidence."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import final

from python.runfiles import runfiles

from tools.field_evidence import manifest, output, production_audio_predictions

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
        self.device: list[str] = []
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
    describe = commands.add_parser("describe", help="emit the current exact detector object")
    describe.add_argument(
        "--device", action="append", required=True, metavar="LABEL=MANUFACTURER|MODEL"
    )
    describe.add_argument("--output", type=Path, required=True)
    predict = commands.add_parser("predict", help="emit predictions for a frozen policy")
    predict.add_argument("--manifest", type=Path, required=True)
    predict.add_argument("--media-root", type=Path, required=True)
    predict.add_argument("--policy-lock", type=Path, required=True)
    predict.add_argument("--output", type=Path, required=True)
    return parser.parse_args(namespace=_Arguments())


def main() -> int:
    """Describe production or publish predictions for frozen evidence."""
    arguments = _arguments()
    output.require_absent(arguments.output)
    replay_binary = arguments.replay_binary or _runfile(
        "tools/field_evidence/production_audio_detector_replay"
    )
    sources = (
        _keyed_paths(arguments.implementation_source, "implementation source")
        if arguments.implementation_source
        else {
            "impact_detector": _runfile(
                "android/core/audio/java/com/agoessling/swingcapture/audio/ImpactDetector.java"
            ),
            "device_policy": _runfile(
                "android/app/java/com/agoessling/swingcapture/DeviceAudioDetectorPolicy.java"
            ),
            "replay_adapter": _runfile(
                "tools/field_evidence/java/com/agoessling/swingcapture/ProductionAudioDetectorReplayCli.java"
            ),
        }
    )
    if arguments.command == "describe":
        raw_devices = _keyed_values(arguments.device, "device")
        devices: dict[str, tuple[str, str]] = {}
        for label, description in raw_devices.items():
            fields = description.split("|", maxsplit=1)
            if len(fields) != KEY_VALUE_FIELD_COUNT or not all(fields):
                message = "device must be LABEL=MANUFACTURER|MODEL"
                raise ValueError(message)
            devices[label] = (fields[0], fields[1])
        report = production_audio_predictions.describe_detector(replay_binary, sources, devices)
    else:
        corpus = manifest.parse_manifest(arguments.manifest.read_text(encoding="utf-8"))
        manifest.validate_assets(corpus, arguments.media_root)
        report = production_audio_predictions.generate_predictions(
            corpus,
            arguments.media_root,
            arguments.policy_lock.read_text(encoding="utf-8"),
            replay_binary,
            sources,
        )
    output.publish_json_exclusive(arguments.output, report)
    return 0


def _keyed_paths(encoded: list[str], label: str) -> dict[str, Path]:
    return {key: Path(value) for key, value in _keyed_values(encoded, label).items()}


def _keyed_values(encoded: list[str], label: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in encoded:
        fields = item.split("=", maxsplit=1)
        if len(fields) != KEY_VALUE_FIELD_COUNT or not all(fields) or fields[0] in result:
            message = f"{label} must be unique NAME=VALUE"
            raise ValueError(message)
        result[fields[0]] = fields[1]
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
