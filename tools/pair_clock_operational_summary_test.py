"""Tests for retained direct-LAN pair-clock operational aggregation."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from typing import cast

from tools import pair_clock_operational_summary as summary


def valid_report(uncertainty: str = "9625081") -> dict[str, object]:
    """Build a minimal valid direct-LAN aggregate report."""
    return {
        "schema_version": 1,
        "report_type": "android_dual_phone_paired_pose_arm_lan_hil",
        "passed": True,
        "pose_transition": {
            "passed": True,
            "peer_transport": "wifi_lan_direct",
            "adb_reverse_used": False,
            "leader_device_model": "Pixel 6",
            "shadow_device_model": "Pixel 5a",
            "leader_role": "face_on",
            "shadow_role": "down_the_line",
            "mapped_impact_evidence": {
                "schema_version": 2,
                "mapping_uncertainty_ns": uncertainty,
                "mapping_age_at_send_ns": "2245028810",
                "minimum_round_trip_ns": "25619510",
                "maximum_round_trip_ns": "36878180",
                "sample_count": 3,
                "mapping_within_policy": True,
                "selected_source": "peer_audio_clock_candidate",
            },
        },
        "coordination": {
            "passed": True,
            "record": {"face_on": {"trigger_uncertainty_ns": "293365"}},
            "measured": {
                "combined_pair_uncertainty_ns": 10092541,
                "maximum_trigger_separation_ns": 16805805,
            },
        },
    }


class PairClockOperationalSummaryTest(unittest.TestCase):
    """Exercise aggregate arithmetic and policy validation with synthetic reports."""

    def test_recomputes_snapshot_uncertainty_and_refuses_calibration_claim(self) -> None:
        """Operational transport evidence must remain distinct from physical calibration."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "pass-a" / "report.json"
            second = root / "pass-b" / "report.json"
            unrelated = root / "unrelated" / "report.json"
            for path in (first, second, unrelated):
                path.parent.mkdir()
            first.write_text(json.dumps(valid_report()), encoding="utf-8")
            second.write_text(json.dumps(valid_report("16455703")), encoding="utf-8")
            unrelated.write_text("{}", encoding="utf-8")
            report = summary.summarize([second, unrelated, first], display_root=root)

        self.assertEqual(3, report["scanned_aggregate_report_count"])
        self.assertEqual(2, report["successful_direct_lan_count"])
        trials = cast("list[dict[str, object]]", report["trials"])
        self.assertEqual("9331716", trials[0]["peer_snapshot_uncertainty_ns"])
        self.assertEqual("9625081", report["minimum_observed_mapping_uncertainty_ns"])
        self.assertEqual("16455703", report["maximum_observed_mapping_uncertainty_ns"])
        self.assertFalse(report["physical_alignment_reference_present"])
        self.assertFalse(report["threshold_selection_eligible"])

    def test_rejects_noncanonical_or_out_of_policy_mapping(self) -> None:
        """The aggregate cannot normalize bad encodings or policy failures."""
        for uncertainty in ("09625081", "25000001"):
            with self.subTest(uncertainty=uncertainty), tempfile.TemporaryDirectory() as temporary:
                path = Path(temporary) / "report.json"
                path.write_text(json.dumps(valid_report(uncertainty)), encoding="utf-8")
                with self.assertRaises(summary.EvidenceError):
                    summary.summarize([path])


if __name__ == "__main__":
    unittest.main()
