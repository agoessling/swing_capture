"""Creation contract for immutable pre-capture audio-detector policy locks."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, cast

from tools.field_evidence import audio_holdout, manifest

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

_ASSIGNMENT_ERROR = "development device assignment must cover both roles exactly"
_RECORDING_IDS_ERROR = "development recording IDs must be nonempty and unique"
_DETECTOR_ERROR = "detector descriptor must be an object"
_CONFIG_TYPE_ERROR = "detector config_by_device must be an object"
_CONFIG_DEVICES_ERROR = "detector configurations must exactly match development-assignment devices"


def create_policy_lock(
    *,
    policy_id: str,
    frozen_at_utc: str,
    development_recording_ids: Sequence[str],
    development_device_assignment: Mapping[str, str],
    detector: object,
) -> dict[str, object]:
    """Build and validate one lock without inferring selection provenance."""
    if set(development_device_assignment) != set(manifest.ROLES):
        raise ValueError(_ASSIGNMENT_ERROR)
    if not development_recording_ids or len(development_recording_ids) != len(
        set(development_recording_ids)
    ):
        raise ValueError(_RECORDING_IDS_ERROR)
    if not isinstance(detector, dict):
        raise TypeError(_DETECTOR_ERROR)
    encoded_detector = cast("dict[str, object]", detector)
    raw_configs = encoded_detector.get("config_by_device")
    if not isinstance(raw_configs, dict):
        raise TypeError(_CONFIG_TYPE_ERROR)
    configs = cast("dict[str, object]", raw_configs)
    expected_devices = set(development_device_assignment.values())
    if set(configs) != expected_devices:
        raise ValueError(_CONFIG_DEVICES_ERROR)
    value: dict[str, object] = {
        "schema_version": audio_holdout.SCHEMA_VERSION,
        "report_type": audio_holdout.LOCK_TYPE,
        "policy_id": policy_id,
        "frozen_at_utc": frozen_at_utc,
        "development_recording_ids": list(development_recording_ids),
        "detector": encoded_detector,
        "evaluation": {
            "leader_role": "face_on",
            "target_tolerance_ms": 100,
            "audio_ready_delay_ms": 2_450,
            "video_ready_delay_ms": 800,
            "maximum_armed_ms": 15_000,
            "post_terminal_ms": 1_000,
            "rearm_delay_ms": 2_000,
            "retained_history_ms": 3_000,
            "minimum_target_recall_ppm": 1_000_000,
            "minimum_quiet_target_recall_ppm": 1_000_000,
            "maximum_negative_continuous_candidates": 0,
            "maximum_false_terminal_attempts": 0,
            "minimum_lifecycle_capture_ppm": 1_000_000,
            "maximum_high_speed_duty_ppm": 300_000,
        },
    }
    audio_holdout.parse_policy_lock(json.dumps(value))
    return value
