"""Exact production-Android audio-detector prediction generation."""

from __future__ import annotations

import hashlib
import json
import subprocess
from typing import TYPE_CHECKING, cast

from tools.field_evidence import audio_holdout, manifest

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence
    from pathlib import Path


ALGORITHM_ID = "android-impact-detector-v1"
IMPLEMENTATION_TARGET = "//tools/field_evidence:production_audio_detector_replay"


def implementation_sha256(sources: Mapping[str, Path]) -> str:
    """Digest the exact production logic, configuration, and replay adapter sources."""
    if not sources:
        message = "at least one implementation source is required"
        raise ValueError(message)
    digest = hashlib.sha256(b"swing-capture-production-audio-replay-v1\0")
    for logical_name in sorted(sources):
        if not logical_name or "\0" in logical_name:
            message = "implementation source names must be nonempty and NUL-free"
            raise ValueError(message)
        digest.update(logical_name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(sources[logical_name].read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def describe_detector(
    replay_binary: Path,
    sources: Mapping[str, Path],
    devices: Mapping[str, tuple[str, str]],
) -> dict[str, object]:
    """Describe the frozen implementation and device-specific production policies."""
    if not devices:
        message = "at least one device description is required"
        raise ValueError(message)
    configs = {
        label: _describe_config(replay_binary, manufacturer, model)
        for label, (manufacturer, model) in sorted(devices.items())
    }
    return {
        "algorithm_id": ALGORITHM_ID,
        "implementation_target": IMPLEMENTATION_TARGET,
        "implementation_sha256": implementation_sha256(sources),
        "config_by_device": configs,
    }


def generate_predictions(
    corpus: manifest.Corpus,
    media_root: Path,
    policy_lock_text: str,
    replay_binary: Path,
    sources: Mapping[str, Path],
) -> dict[str, object]:
    """Replay every qualifying holdout through the exact checked production detector."""
    policy = audio_holdout.parse_policy_lock(policy_lock_text)
    detector = policy.detector
    if detector["algorithm_id"] != ALGORITHM_ID:
        message = "policy lock does not select the production Android detector"
        raise ValueError(message)
    if detector["implementation_target"] != IMPLEMENTATION_TARGET:
        message = "policy lock does not select the production replay target"
        raise ValueError(message)
    current_digest = implementation_sha256(sources)
    if detector["implementation_sha256"] != current_digest:
        message = "frozen detector implementation SHA-256 is stale"
        raise ValueError(message)

    readiness = manifest.readiness_report(corpus, assets_verified=True)
    holdout_ids = set(cast("list[str]", readiness["phone_view_swapped_holdout_ids"]))
    if not readiness["audio_detector_selection_ready"] or not holdout_ids:
        message = "corpus is not ready for production audio holdout replay"
        raise ValueError(message)
    configs = cast("Mapping[str, object]", detector["config_by_device"])
    sessions_json: list[object] = []
    for session in corpus.sessions:
        if session.recording_id not in holdout_ids:
            continue
        streams_json: dict[str, object] = {}
        for role in manifest.ROLES:
            stream = session.streams[role]
            raw_config = configs.get(stream.device_label)
            if not isinstance(raw_config, dict):
                message = f"missing production config for device {stream.device_label!r}"
                raise TypeError(message)
            config = cast("dict[str, object]", raw_config)
            manufacturer = _config_text(config, "manufacturer", stream.device_label)
            model = _config_text(config, "model", stream.device_label)
            windows = audio_holdout.armed_attempt_windows(session, role, policy)
            replay = _predict_stream(
                replay_binary,
                media_root / stream.audio.path,
                manufacturer,
                model,
                windows,
            )
            if replay.get("config") != config:
                message = (
                    f"frozen production config for {stream.device_label!r} "
                    "disagrees with Java policy"
                )
                raise ValueError(message)
            streams_json[role] = {
                "continuous_candidates": replay["continuous_candidates"],
                "armed_replays": replay["armed_replays"],
            }
        sessions_json.append({"recording_id": session.recording_id, "streams": streams_json})
    return {
        "schema_version": audio_holdout.SCHEMA_VERSION,
        "report_type": audio_holdout.REPORT_TYPE,
        "policy_id": policy.policy_id,
        "policy_lock_sha256": policy.canonical_sha256,
        "detector": detector,
        "sessions": sessions_json,
    }


def _describe_config(replay_binary: Path, manufacturer: str, model: str) -> object:
    completed = subprocess.run(
        [str(replay_binary), "describe", manufacturer, model],
        check=True,
        capture_output=True,
        text=True,
        timeout=10,
    )
    return cast("object", json.loads(completed.stdout))


def _predict_stream(
    replay_binary: Path,
    wav_path: Path,
    manufacturer: str,
    model: str,
    windows: Sequence[tuple[str, int, int]],
) -> dict[str, object]:
    attempt_input = "".join(
        f"{attempt_id}\t{arm_ms}\t{end_ms}\n" for attempt_id, arm_ms, end_ms in windows
    )
    completed = subprocess.run(
        [str(replay_binary), "predict", str(wav_path), manufacturer, model],
        input=attempt_input,
        check=True,
        capture_output=True,
        text=True,
        timeout=120,
    )
    decoded = cast("object", json.loads(completed.stdout))
    if not isinstance(decoded, dict):
        message = "production detector replay output must be an object"
        raise TypeError(message)
    result = cast("dict[str, object]", decoded)
    if set(result) != {"config", "continuous_candidates", "armed_replays"}:
        message = "production detector replay output keys differ"
        raise ValueError(message)
    return result


def _config_text(config: Mapping[str, object], key: str, label: str) -> str:
    value = config.get(key)
    if not isinstance(value, str) or not value:
        message = f"production config for {label!r} has invalid {key}"
        raise ValueError(message)
    return value
