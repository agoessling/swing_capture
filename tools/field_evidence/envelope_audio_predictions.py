"""Holdout prediction generation for the frozen robust_hp120_x12 candidate."""

from __future__ import annotations

import hashlib
import json
import subprocess
from typing import TYPE_CHECKING, cast

from tools.field_evidence import audio_holdout, manifest

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence
    from pathlib import Path


ALGORITHM_ID = "robust-envelope-hp120-x12-v1"
IMPLEMENTATION_TARGET = "//capture/offline/experiments/envelope:streaming_envelope_replay"
_SOURCES_REQUIRED_ERROR = "at least one implementation source is required"
_SOURCE_NAME_ERROR = "implementation source names must be nonempty and NUL-free"
_DEVICE_LABEL_ERROR = "device labels must be nonempty and unique"
_ALGORITHM_ERROR = "policy lock does not select the robust envelope candidate"
_TARGET_ERROR = "policy lock does not select the streaming envelope replay target"
_IMPLEMENTATION_DIGEST_ERROR = "frozen detector implementation SHA-256 is stale"
_CORPUS_READINESS_ERROR = "corpus is not ready for envelope audio holdout replay"
_REPLAY_TYPE_ERROR = "envelope replay output must be an object"
_REPLAY_KEYS_ERROR = "envelope replay output keys differ"


def implementation_sha256(sources: Mapping[str, Path]) -> str:
    """Digest the bounded detector, selected config, and replay adapter sources."""
    if not sources:
        raise ValueError(_SOURCES_REQUIRED_ERROR)
    digest = hashlib.sha256(b"swing-capture-streaming-envelope-replay-v1\0")
    for logical_name in sorted(sources):
        if not logical_name or "\0" in logical_name:
            raise ValueError(_SOURCE_NAME_ERROR)
        digest.update(logical_name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(sources[logical_name].read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def describe_detector(
    replay_binary: Path,
    sources: Mapping[str, Path],
    device_labels: Sequence[str],
) -> dict[str, object]:
    """Describe the exact replay implementation and device-specific configuration."""
    labels = tuple(sorted(device_labels))
    if not labels or len(labels) != len(set(labels)) or any(not label for label in labels):
        raise ValueError(_DEVICE_LABEL_ERROR)
    config = _describe_config(replay_binary)
    return {
        "algorithm_id": ALGORITHM_ID,
        "implementation_target": IMPLEMENTATION_TARGET,
        "implementation_sha256": implementation_sha256(sources),
        "config_by_device": dict.fromkeys(labels, config),
    }


def generate_predictions(
    corpus: manifest.Corpus,
    media_root: Path,
    policy_lock_text: str,
    replay_binary: Path,
    sources: Mapping[str, Path],
) -> dict[str, object]:
    """Replay every qualifying stream through the exact frozen candidate."""
    policy = audio_holdout.parse_policy_lock(policy_lock_text)
    detector = policy.detector
    if detector["algorithm_id"] != ALGORITHM_ID:
        raise ValueError(_ALGORITHM_ERROR)
    if detector["implementation_target"] != IMPLEMENTATION_TARGET:
        raise ValueError(_TARGET_ERROR)
    if detector["implementation_sha256"] != implementation_sha256(sources):
        raise ValueError(_IMPLEMENTATION_DIGEST_ERROR)

    readiness = manifest.readiness_report(corpus, assets_verified=True)
    holdout_ids = set(cast("list[str]", readiness["phone_view_swapped_holdout_ids"]))
    if not readiness["audio_detector_selection_ready"] or not holdout_ids:
        raise ValueError(_CORPUS_READINESS_ERROR)
    configs = cast("Mapping[str, object]", detector["config_by_device"])
    sessions_json: list[object] = []
    for session in corpus.sessions:
        if session.recording_id not in holdout_ids:
            continue
        streams_json: dict[str, object] = {}
        for role in manifest.ROLES:
            stream = session.streams[role]
            config = configs.get(stream.device_label)
            if not isinstance(config, dict):
                message = f"missing envelope config for device {stream.device_label!r}"
                raise TypeError(message)
            replay = _predict_stream(
                replay_binary,
                media_root / stream.audio.path,
                audio_holdout.armed_attempt_windows(session, role, policy),
            )
            if replay.get("config") != config:
                message = (
                    f"frozen envelope config for {stream.device_label!r} disagrees with replay"
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


def _describe_config(replay_binary: Path) -> object:
    completed = subprocess.run(
        [str(replay_binary), "describe"],
        check=True,
        capture_output=True,
        text=True,
        timeout=10,
    )
    return cast("object", json.loads(completed.stdout))


def _predict_stream(
    replay_binary: Path,
    wav_path: Path,
    windows: Sequence[tuple[str, int, int]],
) -> dict[str, object]:
    attempt_input = "".join(
        f"{attempt_id}\t{arm_ms}\t{end_ms}\n" for attempt_id, arm_ms, end_ms in windows
    )
    completed = subprocess.run(
        [str(replay_binary), "predict", str(wav_path)],
        input=attempt_input,
        check=True,
        capture_output=True,
        text=True,
        timeout=120,
    )
    decoded = cast("object", json.loads(completed.stdout))
    if not isinstance(decoded, dict):
        raise TypeError(_REPLAY_TYPE_ERROR)
    result = cast("dict[str, object]", decoded)
    if set(result) != {"config", "continuous_candidates", "armed_replays"}:
        raise ValueError(_REPLAY_KEYS_ERROR)
    return result
