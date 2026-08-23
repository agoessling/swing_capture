"""Hermetic tests for field-evidence acquisition and readiness contracts."""

# Test names describe each contract, and literal counts intentionally mirror the fixture schema.
# ruff: noqa: D102, PLR2004
# These tests deliberately mutate nested, schema-validated JSON fixture objects.
# pyright: reportIndexIssue=false, reportArgumentType=false, reportOperatorIssue=false

from __future__ import annotations

import copy
import json
import os
import tempfile
import unittest
from pathlib import Path
from typing import cast
from unittest import mock

from tools.field_evidence import manifest


class FieldEvidenceManifestTest(unittest.TestCase):
    """Protect holdout independence, ground-truth provenance, and media identity."""

    def test_complete_swapped_negative_rich_holdout_is_ready(self) -> None:
        value = self._complete_manifest()
        parsed = manifest.parse_manifest(json.dumps(value))
        report = manifest.readiness_report(parsed, assets_verified=True)

        self.assertTrue(report["pose_release_gate_ready"])
        self.assertTrue(report["audio_detector_selection_ready"])
        self.assertEqual(["holdout-002", "holdout-004"], report["phone_view_swapped_holdout_ids"])
        self.assertEqual(
            {
                "version": "field_readiness_v2",
                "minimum_phone_view_swapped_holdout_sessions": 2,
                "minimum_episodes_per_required_category": 2,
                "minimum_distinct_sessions_per_required_category": 2,
                "minimum_quiet_real_impact_episodes": 2,
                "minimum_distinct_quiet_real_impact_sessions": 2,
            },
            report["readiness_policy"],
        )
        self.assertTrue(
            all(
                count == 2
                for count in cast("dict[str, int]", report["pose_category_episode_counts"]).values()
            )
        )
        self.assertTrue(
            all(
                count == 2
                for count in cast(
                    "dict[str, int]", report["audio_negative_category_episode_counts"]
                ).values()
            )
        )
        self.assertEqual(2, report["quiet_real_impact_episode_count"])
        self.assertEqual(2, report["quiet_real_impact_distinct_session_count"])
        self.assertEqual([], report["missing_pose_categories"])
        self.assertEqual([], report["missing_audio_negative_categories"])
        self.assertEqual([], report["insufficient_pose_categories"])
        self.assertEqual([], report["insufficient_audio_negative_categories"])
        self.assertEqual([], report["gaps"])
        self.assertEqual(
            {"down_the_line": "pixel6", "face_on": "pixel5a"},
            report["next_collection_plan"]["required_phone_view_assignment"],
        )
        self.assertEqual([], report["next_collection_plan"]["required_pose_categories"])
        self.assertEqual([], report["next_collection_plan"]["required_audio_negative_categories"])
        self.assertFalse(report["next_collection_plan"]["quiet_real_impact_required"])
        self.assertEqual(
            0,
            report["next_collection_plan"][
                "additional_phone_view_swapped_holdout_sessions_required"
            ],
        )
        self.assertEqual(
            {"golfer": 0, "lighting": 0, "framing": 0},
            report["next_collection_plan"]["additional_distinct_scene_labels_required"],
        )
        self.assertEqual([], report["next_collection_plan"]["scenario_sequence"])

    def test_initial_collection_plan_reverses_phones_and_requires_complete_evidence(self) -> None:
        plan = manifest.initial_collection_plan({"down_the_line": "pixel5a", "face_on": "pixel6"})

        self.assertEqual(
            {"down_the_line": "pixel6", "face_on": "pixel5a"},
            plan["required_phone_view_assignment"],
        )
        self.assertEqual(
            sorted(manifest.REQUIRED_POSE_CATEGORIES), plan["required_pose_categories"]
        )
        self.assertEqual(
            sorted(manifest.REQUIRED_AUDIO_NEGATIVE_CATEGORIES),
            plan["required_audio_negative_categories"],
        )
        self.assertEqual(
            {"golfer": 2, "lighting": 2, "framing": 2},
            plan["additional_distinct_scene_labels_required"],
        )
        self.assertTrue(plan["quiet_real_impact_required"])
        self.assertEqual(
            manifest.READINESS_POLICY_VERSION,
            cast("dict[str, object]", plan["readiness_policy"])["version"],
        )
        self.assertEqual(2, plan["additional_phone_view_swapped_holdout_sessions_required"])
        scenarios = cast("list[dict[str, object]]", plan["scenario_sequence"])
        self.assertEqual(
            list(manifest.COLLECTION_SCENARIO_ORDER),
            [scenario["category"] for scenario in scenarios],
        )
        self.assertEqual(
            [f"F{index:02d}" for index in range(1, len(scenarios) + 1)],
            [scenario["scenario_id"] for scenario in scenarios],
        )
        self.assertTrue(
            all(scenario["additional_reviewed_occurrences_required"] == 2 for scenario in scenarios)
        )
        self.assertTrue(
            all(scenario["additional_distinct_sessions_required"] == 2 for scenario in scenarios)
        )
        self.assertEqual(
            ["audio", "pose"],
            next(
                cast("list[str]", scenario["collect_for"])
                for scenario in scenarios
                if scenario["category"] == "practice_swing"
            ),
        )
        self.assertEqual(
            ["pose", "audio_quiet_impact"],
            next(
                cast("list[str]", scenario["collect_for"])
                for scenario in scenarios
                if scenario["category"] == "real_swing"
            ),
        )
        self.assertEqual(4, len(cast("list[str]", plan["operator_protocol"])))

        with self.assertRaisesRegex(ValueError, "two distinct phones"):
            manifest.initial_collection_plan({"down_the_line": "pixel6", "face_on": "pixel6"})

    def test_unreviewed_or_incomplete_evidence_cannot_pass(self) -> None:
        value = self._complete_manifest()
        session = cast("dict[str, object]", cast("list[object]", value["sessions"])[0])
        review = cast("dict[str, object]", session["review"])
        review.update({"state": "needs_review", "reviewer": None, "reviewed_at_utc": None})
        streams = cast("dict[str, object]", session["streams"])
        dtl = cast("dict[str, object]", streams["down_the_line"])
        dtl["reviewed_intervals"] = [{"start_ms": 0, "end_ms": 12_000, "note": "first half only"}]

        report = manifest.readiness_report(
            manifest.parse_manifest(json.dumps(value)), assets_verified=True
        )

        self.assertFalse(report["pose_release_gate_ready"])
        self.assertFalse(report["audio_detector_selection_ready"])
        self.assertIn("holdout-002: review state is needs_review", report["gaps"])
        self.assertIn("holdout-002/down_the_line: timeline review is incomplete", report["gaps"])

    def test_next_collection_plan_is_actionable_before_any_holdout_is_reviewed(self) -> None:
        value = self._complete_manifest()
        for raw_session in cast("list[object]", value["sessions"]):
            session = cast("dict[str, object]", raw_session)
            review = cast("dict[str, object]", session["review"])
            review.update({"state": "needs_review", "reviewer": None, "reviewed_at_utc": None})
            session["episodes"] = []
            for stream in cast("dict[str, dict[str, object]]", session["streams"]).values():
                stream["reviewed_intervals"] = []

        report = manifest.readiness_report(
            manifest.parse_manifest(json.dumps(value)), assets_verified=True
        )
        plan = cast("dict[str, object]", report["next_collection_plan"])

        self.assertEqual(
            {"down_the_line": "pixel6", "face_on": "pixel5a"},
            plan["required_phone_view_assignment"],
        )
        self.assertEqual(
            sorted(manifest.REQUIRED_POSE_CATEGORIES), plan["required_pose_categories"]
        )
        self.assertEqual(
            sorted(manifest.REQUIRED_AUDIO_NEGATIVE_CATEGORIES),
            plan["required_audio_negative_categories"],
        )
        self.assertTrue(plan["quiet_real_impact_required"])
        self.assertEqual(
            {"golfer": 2, "lighting": 2, "framing": 2},
            plan["additional_distinct_scene_labels_required"],
        )
        self.assertTrue(plan["complete_timeline_review_required"])
        self.assertTrue(plan["hash_pinned_media_required"])
        self.assertEqual(
            list(manifest.COLLECTION_SCENARIO_ORDER),
            [
                scenario["category"]
                for scenario in cast("list[dict[str, object]]", plan["scenario_sequence"])
            ],
        )

    def test_missing_negative_categories_cannot_satisfy_audio_readiness(self) -> None:
        value = self._complete_manifest()
        for raw_session in cast("list[object]", value["sessions"]):
            session = cast("dict[str, object]", raw_session)
            episodes = cast("list[dict[str, object]]", session["episodes"])
            episodes.remove(next(episode for episode in episodes if episode["id"] == "N002"))
        parsed = manifest.parse_manifest(json.dumps(value))

        report = manifest.readiness_report(parsed, assets_verified=True)

        self.assertFalse(report["audio_detector_selection_ready"])
        self.assertIn("mat_strike", report["missing_audio_negative_categories"])
        mat_strike = next(
            scenario
            for scenario in cast(
                "list[dict[str, object]]", report["next_collection_plan"]["scenario_sequence"]
            )
            if scenario["category"] == "mat_strike"
        )
        self.assertEqual("F06", mat_strike["scenario_id"])

    def test_one_swapped_session_cannot_satisfy_either_detector_gate(self) -> None:
        value = self._complete_manifest()
        value["sessions"] = cast("list[object]", value["sessions"])[:1]

        report = manifest.readiness_report(
            manifest.parse_manifest(json.dumps(value)), assets_verified=True
        )

        self.assertFalse(report["pose_release_gate_ready"])
        self.assertFalse(report["audio_detector_selection_ready"])
        self.assertEqual(["framing", "golfer", "lighting"], report["missing_pose_diversity"])
        self.assertEqual(1, report["quiet_real_impact_episode_count"])
        self.assertEqual(
            sorted(manifest.REQUIRED_AUDIO_NEGATIVE_CATEGORIES),
            report["insufficient_audio_negative_categories"],
        )
        expected_gap = "insufficient fully reviewed phone/view-swapped holdout sessions: observed 1, requires 2"  # noqa: E501
        self.assertIn(expected_gap, report["gaps"])

    def test_singleton_negative_and_quiet_impact_fail_closed(self) -> None:
        value = self._complete_manifest()
        sessions = cast("list[dict[str, object]]", value["sessions"])
        second_episodes = cast("list[dict[str, object]]", sessions[1]["episodes"])
        second_episodes[:] = [
            episode for episode in second_episodes if episode["id"] not in {"S001", "N002"}
        ]

        report = manifest.readiness_report(
            manifest.parse_manifest(json.dumps(value)), assets_verified=True
        )

        self.assertFalse(report["pose_release_gate_ready"])
        self.assertFalse(report["audio_detector_selection_ready"])
        self.assertEqual([], report["missing_audio_negative_categories"])
        self.assertEqual(["mat_strike"], report["insufficient_audio_negative_categories"])
        self.assertEqual(1, report["quiet_real_impact_episode_count"])
        self.assertEqual(1, report["quiet_real_impact_distinct_session_count"])
        plan = cast("dict[str, object]", report["next_collection_plan"])
        self.assertEqual(["mat_strike"], plan["required_audio_negative_categories"])
        self.assertTrue(plan["quiet_real_impact_required"])
        self.assertEqual(1, plan["quiet_real_impact_additional_occurrences_required"])
        self.assertEqual(1, plan["quiet_real_impact_additional_distinct_sessions_required"])

    def test_repeated_negative_in_one_session_does_not_manufacture_session_diversity(self) -> None:
        value = self._complete_manifest()
        sessions = cast("list[dict[str, object]]", value["sessions"])
        first_episodes = cast("list[dict[str, object]]", sessions[0]["episodes"])
        second_episodes = cast("list[dict[str, object]]", sessions[1]["episodes"])
        second_episodes.remove(next(row for row in second_episodes if row["id"] == "N002"))
        self._append_episode_copy(first_episodes, "N002", "N014")

        report = manifest.readiness_report(
            manifest.parse_manifest(json.dumps(value)), assets_verified=True
        )

        self.assertEqual(2, report["audio_negative_category_episode_counts"]["mat_strike"])
        self.assertEqual(
            1,
            report["audio_negative_category_distinct_session_counts"]["mat_strike"],
        )
        self.assertFalse(report["audio_detector_selection_ready"])
        self.assertEqual(["mat_strike"], report["insufficient_audio_negative_categories"])
        plan = cast("dict[str, object]", report["next_collection_plan"])
        self.assertEqual(
            0,
            cast("dict[str, int]", plan["audio_negative_additional_occurrences_required"])[
                "mat_strike"
            ],
        )
        self.assertEqual(
            1,
            cast("dict[str, int]", plan["audio_negative_additional_distinct_sessions_required"])[
                "mat_strike"
            ],
        )

    def test_unrelated_holdouts_cannot_supply_swapped_coverage(self) -> None:
        value = self._complete_manifest()
        sessions = cast("list[dict[str, object]]", value["sessions"])
        original_assignment = cast("dict[str, object]", sessions[0]["streams"])
        first_dtl = cast("dict[str, object]", original_assignment["down_the_line"])
        first_face_on = cast("dict[str, object]", original_assignment["face_on"])
        first_dtl.update({"node_id": "node-pixel5a", "device_label": "pixel5a"})
        first_face_on.update({"node_id": "node-pixel6", "device_label": "pixel6"})
        sessions[1]["episodes"] = []

        report = manifest.readiness_report(
            manifest.parse_manifest(json.dumps(value)), assets_verified=True
        )

        self.assertEqual(["holdout-004"], report["phone_view_swapped_holdout_ids"])
        self.assertFalse(report["pose_release_gate_ready"])
        self.assertFalse(report["audio_detector_selection_ready"])
        self.assertIn("real_swing", report["missing_pose_categories"])
        self.assertIn("practice_swing", report["missing_audio_negative_categories"])

    def test_device_labels_cannot_move_between_durable_nodes_across_sessions(self) -> None:
        value = self._complete_manifest()
        sessions = cast("list[dict[str, object]]", value["sessions"])
        second_streams = cast("dict[str, dict[str, object]]", sessions[1]["streams"])
        second_streams["down_the_line"]["node_id"] = "replacement-node"

        with self.assertRaisesRegex(ValueError, "stable one-to-one binding"):
            manifest.parse_manifest(json.dumps(value))

        value = self._complete_manifest()
        sessions = cast("list[dict[str, object]]", value["sessions"])
        second_streams = cast("dict[str, dict[str, object]]", sessions[1]["streams"])
        second_streams["down_the_line"]["device_label"] = "pixel5a"
        second_streams["face_on"]["device_label"] = "pixel6"
        with self.assertRaisesRegex(ValueError, "stable one-to-one binding"):
            manifest.parse_manifest(json.dumps(value))

    def test_category_expectations_and_rearm_lifecycle_are_strict(self) -> None:
        value = self._complete_manifest()
        empty_scene = self._episode(value, "N010")
        empty_scene["pose_expectation"] = "must_arm"
        with self.assertRaisesRegex(
            ValueError, "empty_scene pose expectation must be must_not_arm"
        ):
            manifest.parse_manifest(json.dumps(value))

        value = self._complete_manifest()
        post_shot_finish = self._episode(value, "N012")
        post_shot_finish["pose_expectation"] = "must_arm"
        with self.assertRaisesRegex(
            ValueError, "post_shot_finish pose expectation must be must_not_arm"
        ):
            manifest.parse_manifest(json.dumps(value))

        value = self._complete_manifest()
        practice = self._episode(value, "N001")
        practice["audio_expectation"] = "diagnostic"
        with self.assertRaisesRegex(
            ValueError, "practice_swing audio expectation must be must_not_trigger"
        ):
            manifest.parse_manifest(json.dumps(value))

        value = self._complete_manifest()
        rearm = self._episode(value, "N009")
        views = cast("dict[str, object]", rearm["views"])
        dtl = cast("dict[str, object]", views["down_the_line"])
        dtl["rearm_safe_start_ms"] = None
        with self.assertRaisesRegex(ValueError, "requires clear, rearm, and takeaway labels"):
            manifest.parse_manifest(json.dumps(value))

        value = self._complete_manifest()
        rearm = self._episode(value, "N009")
        views = cast("dict[str, object]", rearm["views"])
        dtl = cast("dict[str, object]", views["down_the_line"])
        dtl["clear_ms"] = cast("int", dtl["rearm_safe_start_ms"])
        with self.assertRaisesRegex(ValueError, "clear_and_rearm lifecycle is out of order"):
            manifest.parse_manifest(json.dumps(value))

    def test_episode_labels_cannot_overlap_or_move_backwards_on_either_timeline(self) -> None:
        value = self._complete_manifest()
        first = self._episode(value, "S001")
        second = self._episode(value, "N001")
        first_views = cast("dict[str, dict[str, object]]", first["views"])
        second_views = cast("dict[str, dict[str, object]]", second["views"])
        second_views["face_on"]["start_ms"] = first_views["face_on"]["end_ms"] - 1

        with self.assertRaisesRegex(
            ValueError, "chronological and nonoverlapping on the face_on timeline"
        ):
            manifest.parse_manifest(json.dumps(value))

        value = self._complete_manifest()
        first = self._episode(value, "S001")
        second = self._episode(value, "N001")
        first_views = cast("dict[str, dict[str, object]]", first["views"])
        second_views = cast("dict[str, dict[str, object]]", second["views"])
        second_views["down_the_line"]["start_ms"] = first_views["down_the_line"]["start_ms"]
        second_views["down_the_line"]["end_ms"] = first_views["down_the_line"]["end_ms"]

        with self.assertRaisesRegex(
            ValueError, "chronological and nonoverlapping on the down_the_line timeline"
        ):
            manifest.parse_manifest(json.dumps(value))

    def test_review_timestamp_is_validated_and_normalized_to_utc(self) -> None:
        value = self._complete_manifest()
        session = cast("dict[str, object]", cast("list[object]", value["sessions"])[0])
        review = cast("dict[str, object]", session["review"])
        review["reviewed_at_utc"] = "2026-08-22T17:30:00-07:00"

        parsed = manifest.parse_manifest(json.dumps(value))

        self.assertEqual("2026-08-23T00:30:00Z", parsed.sessions[0].review.reviewed_at_utc)

        review["reviewed_at_utc"] = "2026-08-22T17:30:00"
        with self.assertRaisesRegex(ValueError, "timezone-aware RFC 3339"):
            manifest.parse_manifest(json.dumps(value))

    def test_rejects_leakage_and_contradictory_lifecycle_labels(self) -> None:
        value = self._complete_manifest()
        session = cast("dict[str, object]", cast("list[object]", value["sessions"])[0])
        session["recording_id"] = "development-001"
        with self.assertRaisesRegex(ValueError, "leaks a development recording"):
            manifest.parse_manifest(json.dumps(value))

        value = self._complete_manifest()
        episode = self._episode(value, "S001")
        views = cast("dict[str, object]", episode["views"])
        dtl = cast("dict[str, object]", views["down_the_line"])
        dtl["takeaway_ms"] = 1_600
        dtl["reference_ms"] = 1_500
        with self.assertRaisesRegex(ValueError, "takeaway must precede impact"):
            manifest.parse_manifest(json.dumps(value))

        with self.assertRaisesRegex(ValueError, "duplicate JSON field 'schema_version'"):
            manifest.parse_manifest(
                '{"schema_version":1,"schema_version":1,"corpus_id":"duplicate"}'
            )

    def test_create_skeleton_pins_assets_but_never_invents_labels(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            paths = self._write_raw_pair(root)
            value = manifest.create_session_skeleton(
                corpus_id="second-field-corpus",
                development_recording_ids=("development-001",),
                development_assignment={
                    "down_the_line": "pixel5a",
                    "face_on": "pixel6",
                },
                split="validation",
                media_root=root,
                source_manifests={
                    role: role_paths["manifest"] for role, role_paths in paths.items()
                },
                videos={role: role_paths["video"] for role, role_paths in paths.items()},
                audio={role: role_paths["audio"] for role, role_paths in paths.items()},
                device_assignment={
                    "down_the_line": "pixel6",
                    "face_on": "pixel5a",
                },
                scene={
                    "golfer_label": "golfer-a",
                    "environment_label": "studio-a",
                    "lighting_label": "artificial-a",
                    "framing_label": "portrait-a",
                },
            )
            session = cast("dict[str, object]", cast("list[object]", value["sessions"])[0])
            self.assertEqual([], session["episodes"])
            self.assertEqual("needs_review", cast("dict[str, object]", session["review"])["state"])
            parsed = manifest.parse_manifest(json.dumps(value))
            verified = manifest.validate_assets(parsed, root)
            self.assertEqual(6, len(verified))
            self.assertFalse(manifest.readiness_report(parsed)["audio_detector_selection_ready"])

            paths["face_on"]["audio"].write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "audio SHA-256 mismatch"):
                manifest.validate_assets(parsed, root)

    def test_create_rejects_an_interrupted_media_copy(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            paths = self._write_raw_pair(root)
            paths["down_the_line"]["video"].write_bytes(b"partial")

            with self.assertRaisesRegex(ValueError, "video byte count disagrees"):
                manifest.create_session_skeleton(
                    corpus_id="second-field-corpus",
                    development_recording_ids=("development-001",),
                    development_assignment={
                        "down_the_line": "pixel5a",
                        "face_on": "pixel6",
                    },
                    split="validation",
                    media_root=root,
                    source_manifests={
                        role: role_paths["manifest"] for role, role_paths in paths.items()
                    },
                    videos={role: role_paths["video"] for role, role_paths in paths.items()},
                    audio={role: role_paths["audio"] for role, role_paths in paths.items()},
                    device_assignment={
                        "down_the_line": "pixel6",
                        "face_on": "pixel5a",
                    },
                    scene={
                        "golfer_label": "golfer-a",
                        "environment_label": "studio-a",
                        "lighting_label": "artificial-a",
                        "framing_label": "portrait-a",
                    },
                )

    def test_append_preserves_corpus_identity_and_rejects_duplicate_recordings(self) -> None:
        base = self._complete_manifest()
        candidate = self._complete_manifest()
        candidate_sessions = cast("list[object]", candidate["sessions"])
        candidate["sessions"] = [candidate_sessions[0]]
        candidate_session = cast("dict[str, object]", candidate_sessions[0])
        candidate_session["recording_id"] = "holdout-006"
        streams = cast("dict[str, object]", candidate_session["streams"])
        for role in manifest.ROLES:
            stream = cast("dict[str, object]", streams[role])
            for kind in ("source_manifest", "video", "audio"):
                asset = cast("dict[str, object]", stream[kind])
                asset["path"] = cast("str", asset["path"]).replace("holdout/", "holdout-third/")

        combined = manifest.append_session_skeleton(json.dumps(base), candidate)

        parsed = manifest.parse_manifest(json.dumps(combined))
        self.assertEqual(3, len(parsed.sessions))
        duplicate = self._complete_manifest()
        duplicate["sessions"] = [cast("list[object]", duplicate["sessions"])[0]]
        with self.assertRaisesRegex(ValueError, "session recording ids must be unique"):
            manifest.append_session_skeleton(json.dumps(base), duplicate)

        candidate["corpus_id"] = "another-corpus"
        with self.assertRaisesRegex(ValueError, "different corpus_id"):
            manifest.append_session_skeleton(json.dumps(base), candidate)

    def test_validate_refuses_to_replace_its_input_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "evidence.json"
            original = json.dumps(self._complete_manifest())
            path.write_text(original, encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "must not replace an input"):
                manifest.main(
                    [
                        "validate",
                        "--manifest",
                        str(path),
                        "--output",
                        str(path),
                    ]
                )

            self.assertEqual(path.read_text(encoding="utf-8"), original)

    def test_atomic_render_preserves_prior_output_when_publication_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "report.json"
            output.write_text("prior evidence\n", encoding="utf-8")
            with (
                mock.patch.object(os, "replace", side_effect=OSError("interrupted")),
                self.assertRaisesRegex(OSError, "interrupted"),
            ):
                manifest._render(  # pyright: ignore[reportPrivateUsage]  # noqa: SLF001
                    {"passed": True}, output
                )

            self.assertEqual(output.read_text(encoding="utf-8"), "prior evidence\n")
            self.assertEqual(sorted(path.name for path in root.iterdir()), ["report.json"])

    @staticmethod
    def _episode(value: dict[str, object], episode_id: str) -> dict[str, object]:
        session = cast("dict[str, object]", cast("list[object]", value["sessions"])[0])
        episodes = cast("list[dict[str, object]]", session["episodes"])
        return next(episode for episode in episodes if episode["id"] == episode_id)

    @staticmethod
    def _append_episode_copy(
        episodes: list[dict[str, object]], episode_id: str, new_episode_id: str
    ) -> None:
        source = next(episode for episode in episodes if episode["id"] == episode_id)
        duplicate = copy.deepcopy(source)
        duplicate["id"] = new_episode_id
        views = cast("dict[str, dict[str, object]]", duplicate["views"])
        source_views = cast("dict[str, dict[str, object]]", source["views"])
        for role in manifest.ROLES:
            shift_ms = 25_200 - cast("int", source_views[role]["start_ms"])
            for field in (
                "start_ms",
                "end_ms",
                "reference_ms",
                "safe_arm_start_ms",
                "preferred_arm_ms",
                "takeaway_ms",
                "clear_ms",
                "rearm_safe_start_ms",
                "rearm_preferred_ms",
            ):
                value = views[role][field]
                if value is not None:
                    views[role][field] = cast("int", value) + shift_ms
        episodes.append(duplicate)

    @classmethod
    def _complete_manifest(cls) -> dict[str, object]:
        categories = [
            ("S001", "real_swing", "must_arm", "must_trigger", "quiet"),
            ("N001", "practice_swing", "must_not_arm", "must_not_trigger", "nominal"),
            ("N002", "mat_strike", "must_not_arm", "must_not_trigger", "loud"),
            ("N003", "waggle", "must_not_arm", "must_not_trigger", "quiet"),
            ("N004", "speech", "must_not_arm", "must_not_trigger", "nominal"),
            ("N005", "footsteps", "must_not_arm", "must_not_trigger", "quiet"),
            ("N006", "club_drop", "must_not_arm", "must_not_trigger", "loud"),
            ("N007", "aborted_address", "must_arm", "must_not_trigger", "not_applicable"),
            ("N008", "address_no_swing", "must_arm", "must_not_trigger", "not_applicable"),
            ("N009", "clear_and_rearm", "must_arm", "diagnostic", "not_applicable"),
            ("N010", "empty_scene", "must_not_arm", "diagnostic", "not_applicable"),
            ("N011", "walk_through", "must_not_arm", "diagnostic", "not_applicable"),
            ("N012", "post_shot_finish", "must_not_arm", "diagnostic", "not_applicable"),
            ("N013", "repeated_setup", "must_not_arm", "diagnostic", "not_applicable"),
        ]
        episodes: list[dict[str, object]] = []
        for index, (episode_id, category, pose, audio, character) in enumerate(categories):
            start = index * 1_800
            end = start + 1_800
            views: dict[str, object] = {}
            for role in manifest.ROLES:
                must_arm = pose == "must_arm"
                clear_and_rearm = category == "clear_and_rearm"
                views[role] = {
                    "start_ms": start,
                    "end_ms": end,
                    "reference_ms": start + 1_500 if audio == "must_trigger" else None,
                    "safe_arm_start_ms": start + (100 if clear_and_rearm else 200)
                    if must_arm
                    else None,
                    "preferred_arm_ms": start + (200 if clear_and_rearm else 400)
                    if must_arm
                    else None,
                    "takeaway_ms": start + (1_500 if clear_and_rearm else 1_000)
                    if must_arm
                    else None,
                    "clear_ms": start + 700 if clear_and_rearm else None,
                    "rearm_safe_start_ms": start + 900 if clear_and_rearm else None,
                    "rearm_preferred_ms": start + 1_100 if clear_and_rearm else None,
                }
            episodes.append(
                {
                    "id": episode_id,
                    "category": category,
                    "pose_expectation": pose,
                    "audio_expectation": audio,
                    "audio_character": character,
                    "views": views,
                    "notes": "Synthetic schema test label, not field ground truth.",
                }
            )
        stream = {
            "node_id": "node-placeholder",
            "device_label": "device-placeholder",
            "duration_ms": 27_000,
            "source_manifest": {"path": "holdout/dtl_manifest.json", "sha256": "a" * 64},
            "video": {"path": "holdout/dtl_video.mp4", "sha256": "b" * 64},
            "audio": {"path": "holdout/dtl_audio.wav", "sha256": "c" * 64},
            "reviewed_intervals": [
                {"start_ms": 0, "end_ms": 27_000, "note": "complete human review"}
            ],
        }
        dtl = copy.deepcopy(stream)
        dtl.update({"node_id": "node-pixel6", "device_label": "pixel6"})
        atl = copy.deepcopy(stream)
        atl.update({"node_id": "node-pixel5a", "device_label": "pixel5a"})
        for kind in ("source_manifest", "video", "audio"):
            atl_asset = cast("dict[str, object]", atl[kind])
            atl_asset["path"] = cast("str", atl_asset["path"]).replace("/dtl_", "/atl_")
        first_session: dict[str, object] = {
            "recording_id": "holdout-002",
            "split": "validation",
            "scene": {
                "golfer_label": "golfer-a",
                "environment_label": "studio-a",
                "lighting_label": "artificial-a",
                "framing_label": "portrait-a",
            },
            "streams": {"down_the_line": dtl, "face_on": atl},
            "review": {
                "state": "complete",
                "reviewer": "human-reviewer",
                "reviewed_at_utc": "2026-08-23T00:00:00Z",
            },
            "episodes": episodes,
            "notes": "Synthetic complete contract example used only by tests.",
        }
        second_session = copy.deepcopy(first_session)
        second_session["recording_id"] = "holdout-004"
        second_session["scene"] = {
            "golfer_label": "golfer-b",
            "environment_label": "studio-a",
            "lighting_label": "daylight-b",
            "framing_label": "portrait-b",
        }
        second_streams = cast("dict[str, object]", second_session["streams"])
        for role in manifest.ROLES:
            second_stream = cast("dict[str, object]", second_streams[role])
            for kind in ("source_manifest", "video", "audio"):
                second_asset = cast("dict[str, object]", second_stream[kind])
                second_asset["path"] = cast("str", second_asset["path"]).replace(
                    "holdout/", "holdout-second/"
                )
        return {
            "schema_version": 1,
            "corpus_id": "field-holdout-corpus",
            "development_recording_ids": ["development-001"],
            "development_device_assignment": {
                "down_the_line": "pixel5a",
                "face_on": "pixel6",
            },
            "sessions": [first_session, second_session],
        }

    @staticmethod
    def _write_raw_pair(root: Path) -> dict[str, dict[str, Path]]:
        results: dict[str, dict[str, Path]] = {}
        for index, role in enumerate(manifest.ROLES):
            directory = root / role
            directory.mkdir()
            source = directory / "manifest.json"
            video = directory / "video.mp4"
            audio = directory / "audio.wav"
            source.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "session_kind": "field_recording",
                        "shared_recording_id": "holdout-raw-003",
                        "node_id": f"node-{index}",
                        "role": role,
                        "duration_us": "24000000",
                        "video": {"bytes": str(len(f"video-{role}".encode()))},
                        "audio": {"bytes": str(len(f"audio-{role}".encode()))},
                    }
                ),
                encoding="utf-8",
            )
            video.write_bytes(f"video-{role}".encode())
            audio.write_bytes(f"audio-{role}".encode())
            results[role] = {"manifest": source, "video": video, "audio": audio}
        return results


if __name__ == "__main__":
    unittest.main()
