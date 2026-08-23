"""Summarize retained direct-LAN pair-clock operational evidence."""

# Evidence errors intentionally retain the failing field or report path at each check.
# ruff: noqa: EM101, EM102, TRY003

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from typing import TYPE_CHECKING, cast

if TYPE_CHECKING:
    from collections.abc import Iterable, Mapping


class EvidenceError(ValueError):
    """Raised when a retained aggregate report contradicts its pair-clock evidence."""


MAXIMUM_MAPPING_AGE_NS = 10_000_000_000
PROVISIONAL_MAPPING_UNCERTAINTY_NS = 25_000_000
MAPPING_SCHEMA_VERSION = 2
MINIMUM_MAPPING_SAMPLE_COUNT = 3


def _object(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, dict):
        raise EvidenceError(f"{label} must be an object")
    return cast("dict[str, object]", value)


def _canonical_nonnegative_decimal(value: object, label: str) -> int:
    if (
        not isinstance(value, str)
        or not value
        or (len(value) > 1 and value.startswith("0"))
        or not value.isascii()
        or not value.isdecimal()
    ):
        raise EvidenceError(f"{label} must be a canonical nonnegative decimal string")
    return int(value)


def _integer(value: object, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise EvidenceError(f"{label} must be an integer")
    return value


def _required_text(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or value.isspace():
        raise EvidenceError(f"{label} must be a nonempty string")
    return value


def _successful_direct_lan_trial(
    report: Mapping[str, object], report_path: str
) -> dict[str, object]:
    if report.get("report_type") != "android_dual_phone_paired_pose_arm_lan_hil":
        raise EvidenceError(f"{report_path}: direct-LAN report type is unsupported")
    if report.get("schema_version") != 1 or report.get("passed") is not True:
        raise EvidenceError(f"{report_path}: explicit direct-LAN aggregate did not pass")

    transition = _object(report.get("pose_transition"), "pose_transition")
    if transition.get("passed") is not True or transition.get("adb_reverse_used") is not False:
        raise EvidenceError(f"{report_path}: direct-LAN transition is not independently valid")
    leader_role = _required_text(transition.get("leader_role"), "leader_role")
    mapping = _object(transition.get("mapped_impact_evidence"), "mapped_impact_evidence")
    uncertainty = _canonical_nonnegative_decimal(
        mapping.get("mapping_uncertainty_ns"), "mapping_uncertainty_ns"
    )
    age = _canonical_nonnegative_decimal(
        mapping.get("mapping_age_at_send_ns"), "mapping_age_at_send_ns"
    )
    minimum_round_trip = _canonical_nonnegative_decimal(
        mapping.get("minimum_round_trip_ns"), "minimum_round_trip_ns"
    )
    maximum_round_trip = _canonical_nonnegative_decimal(
        mapping.get("maximum_round_trip_ns"), "maximum_round_trip_ns"
    )
    sample_count = _integer(mapping.get("sample_count"), "sample_count")
    if (
        mapping.get("schema_version") != MAPPING_SCHEMA_VERSION
        or mapping.get("mapping_within_policy") is not True
        or mapping.get("selected_source") != "peer_audio_clock_candidate"
        or uncertainty > PROVISIONAL_MAPPING_UNCERTAINTY_NS
        or age > MAXIMUM_MAPPING_AGE_NS
        or maximum_round_trip < minimum_round_trip
        or sample_count < MINIMUM_MAPPING_SAMPLE_COUNT
    ):
        raise EvidenceError(f"{report_path}: mapped impact contradicts direct-LAN policy")

    coordination = _object(report.get("coordination"), "coordination")
    if coordination.get("passed") is not True:
        raise EvidenceError(f"{report_path}: host coordination evidence did not pass")
    coordination_record = _object(coordination.get("record"), "coordination.record")
    leader_trigger = _object(
        coordination_record.get(leader_role), f"coordination.record.{leader_role}"
    )
    trigger_uncertainty = _canonical_nonnegative_decimal(
        leader_trigger.get("trigger_uncertainty_ns"), "trigger_uncertainty_ns"
    )
    if trigger_uncertainty > uncertainty:
        raise EvidenceError(f"{report_path}: composed mapping uncertainty is impossible")
    measured = _object(coordination.get("measured"), "coordination.measured")
    combined_uncertainty = _integer(
        measured.get("combined_pair_uncertainty_ns"), "combined_pair_uncertainty_ns"
    )
    maximum_separation = _integer(
        measured.get("maximum_trigger_separation_ns"), "maximum_trigger_separation_ns"
    )
    if combined_uncertainty < 0 or maximum_separation < 0:
        raise EvidenceError(f"{report_path}: host coordination bounds cannot be negative")

    return {
        "report_path": report_path,
        "peer_snapshot_uncertainty_ns": str(uncertainty - trigger_uncertainty),
        "mapped_impact_uncertainty_ns": str(uncertainty),
        "mapping_age_at_send_ns": str(age),
        "minimum_round_trip_ns": str(minimum_round_trip),
        "maximum_round_trip_ns": str(maximum_round_trip),
        "combined_hil_uncertainty_ns": str(combined_uncertainty),
        "maximum_mapped_separation_ns": str(maximum_separation),
        "leader_device_model": _required_text(
            transition.get("leader_device_model"), "leader_device_model"
        ),
        "shadow_device_model": _required_text(
            transition.get("shadow_device_model"), "shadow_device_model"
        ),
        "leader_role": leader_role,
        "shadow_role": _required_text(transition.get("shadow_role"), "shadow_role"),
    }


def summarize(
    report_paths: Iterable[Path], *, display_root: Path | None = None
) -> dict[str, object]:
    """Validate and summarize aggregate reports with an explicit direct-LAN transition."""
    paths = sorted(report_paths)
    trials: list[dict[str, object]] = []
    for path in paths:
        try:
            parsed = cast("object", json.loads(path.read_text(encoding="utf-8")))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise EvidenceError(f"cannot read retained report {path}: {error}") from error
        report = _object(parsed, str(path))
        transition_value = report.get("pose_transition")
        if not isinstance(transition_value, dict):
            continue
        transition = cast("dict[str, object]", transition_value)
        if transition.get("peer_transport") != "wifi_lan_direct":
            continue
        report_path = path.as_posix()
        if display_root is not None:
            try:
                report_path = path.relative_to(display_root).as_posix()
            except ValueError as error:
                raise EvidenceError(
                    f"report {path} is outside the declared artifact root"
                ) from error
        trials.append(_successful_direct_lan_trial(report, report_path))

    if not trials:
        raise EvidenceError("no explicit successful direct-LAN aggregate reports were found")
    uncertainties = [int(str(trial["mapped_impact_uncertainty_ns"])) for trial in trials]
    ages = [int(str(trial["mapping_age_at_send_ns"])) for trial in trials]
    return {
        "schema_version": 1,
        "report_type": "pair_clock_operational_evidence_summary",
        "scanned_aggregate_report_count": len(paths),
        "successful_direct_lan_count": len(trials),
        "provisional_mapping_uncertainty_ns": str(PROVISIONAL_MAPPING_UNCERTAINTY_NS),
        "minimum_observed_mapping_uncertainty_ns": str(min(uncertainties)),
        "maximum_observed_mapping_uncertainty_ns": str(max(uncertainties)),
        "maximum_observed_mapping_age_ns": str(max(ages)),
        "physical_alignment_reference_present": False,
        "threshold_selection_eligible": False,
        "trials": trials,
    }


def main() -> int:
    """Print the retained operational evidence summary."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_root", type=Path)
    arguments = parser.parse_args()
    root_argument = cast("Path", arguments.artifact_root)
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not root_argument.is_absolute() and workspace:
        root_argument = Path(workspace) / root_argument
    if root_argument.is_symlink():
        raise EvidenceError("artifact root must be a non-symlink directory")
    root = root_argument.resolve(strict=True)
    if not root.is_dir():
        raise EvidenceError("artifact root must be a non-symlink directory")
    report = summarize(root.glob("*/report.json"), display_root=root)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
