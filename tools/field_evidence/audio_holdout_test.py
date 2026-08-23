"""Deterministic tests for the audio holdout policy and scoring gate."""

# Scenario names document the cases, and tests intentionally exercise private scoring seams.
# ruff: noqa: D101, D102, SLF001
# pyright: reportPrivateUsage=false

from __future__ import annotations

import copy
import datetime
import json
import unittest
from typing import cast

from tools.field_evidence import audio_holdout, manifest


def complete_corpus_value() -> dict[str, object]:
    """Return the complete synthetic corpus shared by executable-boundary tests."""
    return AudioHoldoutTest._corpus_value()


def source_capture_times_utc() -> dict[str, datetime.datetime]:
    """Return capture provenance for every session in the shared v2 corpus."""
    return {
        "holdout-002": datetime.datetime(2026, 8, 23, tzinfo=datetime.UTC),
        "holdout-004": datetime.datetime(2026, 8, 24, tzinfo=datetime.UTC),
    }


def policy_lock_value() -> dict[str, object]:
    """Return the synthetic policy lock shared by executable-boundary tests."""
    return AudioHoldoutTest._policy_lock()


def prediction_value(
    corpus_value: dict[str, object], lock_value: dict[str, object]
) -> dict[str, object]:
    """Return complete synthetic predictions shared by executable-boundary tests."""
    return AudioHoldoutTest._predictions(corpus_value, lock_value)


class AudioHoldoutTest(unittest.TestCase):
    def test_continuous_candidate_cannot_recall_two_nearby_impacts(self) -> None:
        corpus_value = self._corpus_value()
        first_session = cast("dict[str, object]", cast("list[object]", corpus_value["sessions"])[0])
        episodes = cast("list[object]", first_session["episodes"])
        for episode_id, start_ms, end_ms, reference_ms in (
            ("S002", 25_200, 26_100, 26_050),
            ("S003", 26_100, 27_000, 26_600),
        ):
            views = {
                role: {
                    "start_ms": start_ms,
                    "end_ms": end_ms,
                    "reference_ms": reference_ms,
                    "safe_arm_start_ms": start_ms,
                    "preferred_arm_ms": start_ms,
                    "takeaway_ms": start_ms + 100,
                    "clear_ms": None,
                    "rearm_safe_start_ms": None,
                    "rearm_preferred_ms": None,
                }
                for role in manifest.ROLES
            }
            episodes.append(
                {
                    "id": episode_id,
                    "category": "real_swing",
                    "pose_expectation": "must_arm",
                    "audio_expectation": "must_trigger",
                    "audio_character": "quiet",
                    "views": views,
                    "notes": "Adjacent synthetic impacts exercise one-to-one recall matching.",
                }
            )
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        cast("dict[str, object]", lock_value["evaluation"])["target_tolerance_ms"] = 600
        predictions = self._predictions(corpus_value, lock_value)
        prediction_session = cast(
            "dict[str, object]", cast("list[object]", predictions["sessions"])[0]
        )
        streams = cast("dict[str, dict[str, object]]", prediction_session["streams"])
        face_candidates = cast("list[dict[str, int]]", streams["face_on"]["continuous_candidates"])
        face_candidates[:] = [
            candidate
            for candidate in face_candidates
            if candidate["strike_ms"] not in {26_050, 26_600}
        ]
        face_candidates.append({"strike_ms": 26_100, "decision_ms": 26_102})
        face_candidates.sort(key=lambda candidate: candidate["decision_ms"])

        report = audio_holdout.evaluate_audio_holdout(
            corpus,
            json.dumps(lock_value),
            json.dumps(predictions),
            assets_verified=True,
            source_capture_times_utc=source_capture_times_utc(),
        )

        generation = cast("dict[str, object]", report["candidate_generation"])
        self.assertEqual(7, generation["target_recalled"])
        self.assertEqual(8, generation["target_count"])
        face_stream = next(
            row
            for row in cast("list[dict[str, object]]", generation["streams"])
            if row["recording_id"] == "holdout-002" and row["role"] == "face_on"
        )
        adjacent_rows = [
            row
            for row in cast("list[dict[str, object]]", face_stream["targets"])
            if row["episode_id"] in {"S002", "S003"}
        ]
        self.assertEqual(1, sum(cast("bool", row["recalled"]) for row in adjacent_rows))
        self.assertFalse(cast("dict[str, bool]", report["checks"])["target_recall"])

    def test_duplicate_candidate_near_one_impact_is_counted_as_non_target(self) -> None:
        matches, unmatched = audio_holdout._one_to_one_target_matches(
            [1_000],
            (
                audio_holdout.Candidate(strike_ms=995, decision_ms=1_005),
                audio_holdout.Candidate(strike_ms=1_005, decision_ms=1_006),
            ),
            10,
        )

        self.assertEqual(1, len(matches))
        self.assertEqual(1, len(unmatched))

    def test_complete_locked_holdout_passes_and_exercises_clear_rearm(self) -> None:
        corpus_value = self._corpus_value()
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        predictions = self._predictions(corpus_value, lock_value)

        report = audio_holdout.evaluate_audio_holdout(
            corpus,
            json.dumps(lock_value),
            json.dumps(predictions),
            assets_verified=True,
            source_capture_times_utc=source_capture_times_utc(),
        )

        self.assertTrue(report["passed"])
        self.assertEqual(
            "one_to_one_maximum_cardinality_v1",
            cast("dict[str, object]", report["evaluation_policy"])["continuous_candidate_matching"],
        )
        self.assertEqual(
            {
                "target_recall": True,
                "quiet_target_recall": True,
                "negative_continuous_candidates": True,
                "lifecycle_capture": True,
                "lifecycle_attempt_coverage": True,
                "false_terminal_attempts": True,
                "high_speed_duty": True,
            },
            report["checks"],
        )
        lifecycle = cast("dict[str, object]", report["lifecycle"])
        attempts = cast("list[dict[str, object]]", lifecycle["attempts"])
        initial = next(row for row in attempts if row["attempt_id"] == "N009:initial")
        self.assertEqual("pose_clear", initial["outcome"])
        rearm = next(row for row in attempts if row["attempt_id"] == "N009:rearm")
        self.assertEqual("no_impact_timeout", rearm["outcome"])
        timeout = next(row for row in attempts if row["attempt_id"] == "N007")
        self.assertEqual("no_impact_timeout", timeout["outcome"])
        self.assertEqual(lifecycle["required_attempt_count"], lifecycle["evaluated_attempt_count"])
        self.assertEqual([], lifecycle["ignored_attempt_ids"])
        self.assertEqual(1_000_000, lifecycle["target_capture_ppm"])

    def test_ignored_must_arm_attempt_fails_lifecycle_coverage(self) -> None:
        corpus_value = self._corpus_value()
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        evaluation = cast("dict[str, object]", lock_value["evaluation"])
        evaluation["rearm_delay_ms"] = 2_500
        predictions = self._predictions(corpus_value, lock_value)

        report = audio_holdout.evaluate_audio_holdout(
            corpus,
            json.dumps(lock_value),
            json.dumps(predictions),
            assets_verified=True,
            source_capture_times_utc=source_capture_times_utc(),
        )

        self.assertFalse(report["passed"])
        self.assertFalse(cast("dict[str, bool]", report["checks"])["lifecycle_attempt_coverage"])
        lifecycle = cast("dict[str, object]", report["lifecycle"])
        self.assertGreater(cast("int", lifecycle["ignored_attempt_count"]), 0)
        ignored_attempt_ids = cast("list[str]", lifecycle["ignored_attempt_ids"])
        self.assertIn("holdout-002/N009:rearm", ignored_attempt_ids)

    def test_reviewed_pose_clear_ends_the_initial_attempt_before_late_audio(self) -> None:
        corpus_value = self._corpus_value()
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        predictions = self._predictions(corpus_value, lock_value)
        session = cast("dict[str, object]", cast("list[object]", predictions["sessions"])[0])
        streams = cast("dict[str, dict[str, object]]", session["streams"])
        replays = cast("list[dict[str, object]]", streams["face_on"]["armed_replays"])
        initial = next(replay for replay in replays if replay["attempt_id"] == "N009:initial")
        episode = next(
            episode
            for episode in cast(
                "list[dict[str, object]]",
                cast("dict[str, object]", cast("list[object]", corpus_value["sessions"])[0])[
                    "episodes"
                ],
            )
            if episode["id"] == "N009"
        )
        clear_ms = cast(
            "int",
            cast("dict[str, object]", cast("dict[str, object]", episode["views"])["face_on"])[
                "clear_ms"
            ],
        )
        initial["candidates"] = [{"strike_ms": clear_ms + 1, "decision_ms": clear_ms + 2}]

        report = audio_holdout.evaluate_audio_holdout(
            corpus,
            json.dumps(lock_value),
            json.dumps(predictions),
            assets_verified=True,
            source_capture_times_utc=source_capture_times_utc(),
        )

        lifecycle = cast("dict[str, object]", report["lifecycle"])
        attempt = next(
            row
            for row in cast("list[dict[str, object]]", lifecycle["attempts"])
            if row["attempt_id"] == "N009:initial"
        )
        self.assertEqual("pose_clear", attempt["outcome"])
        self.assertEqual(clear_ms, attempt["completion_ms"])
        self.assertEqual(0, lifecycle["false_terminal_attempt_count"])

    def test_first_post_ready_false_candidate_is_the_terminal_and_loses_the_swing(self) -> None:
        corpus_value = self._corpus_value()
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        predictions = self._predictions(corpus_value, lock_value)
        session = cast("dict[str, object]", cast("list[object]", predictions["sessions"])[0])
        streams = cast("dict[str, dict[str, object]]", session["streams"])
        replays = cast("list[dict[str, object]]", streams["face_on"]["armed_replays"])
        swing = next(replay for replay in replays if replay["attempt_id"] == "S001")
        cast("list[dict[str, int]]", swing["candidates"]).insert(
            0, {"strike_ms": 950, "decision_ms": 952}
        )

        report = audio_holdout.evaluate_audio_holdout(
            corpus,
            json.dumps(lock_value),
            json.dumps(predictions),
            assets_verified=True,
            source_capture_times_utc=source_capture_times_utc(),
        )

        self.assertFalse(report["passed"])
        lifecycle = cast("dict[str, object]", report["lifecycle"])
        self.assertEqual(1, lifecycle["false_terminal_attempt_count"])
        self.assertEqual(1, lifecycle["captured_target_count"])
        attempt = next(
            row
            for row in cast("list[dict[str, object]]", lifecycle["attempts"])
            if row["attempt_id"] == "S001"
        )
        self.assertEqual("false_terminal", attempt["outcome"])

    def test_missing_quiet_impact_fails_candidate_and_lifecycle_recall(self) -> None:
        corpus_value = self._corpus_value()
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        predictions = self._predictions(corpus_value, lock_value)
        session = cast("dict[str, object]", cast("list[object]", predictions["sessions"])[0])
        streams = cast("dict[str, dict[str, object]]", session["streams"])
        streams["face_on"]["continuous_candidates"] = []
        replays = cast("list[dict[str, object]]", streams["face_on"]["armed_replays"])
        swing = next(replay for replay in replays if replay["attempt_id"] == "S001")
        swing["candidates"] = []

        report = audio_holdout.evaluate_audio_holdout(
            corpus,
            json.dumps(lock_value),
            json.dumps(predictions),
            assets_verified=True,
            source_capture_times_utc=source_capture_times_utc(),
        )

        self.assertFalse(report["passed"])
        generation = cast("dict[str, object]", report["candidate_generation"])
        lifecycle = cast("dict[str, object]", report["lifecycle"])
        self.assertEqual(750_000, generation["quiet_target_recall_ppm"])
        # The maximum-armed timeout still retains this early synthetic swing, while the
        # independent second session captures normally. Candidate recall, rather than lifecycle
        # capture, is what makes the missed impact fail qualification.
        self.assertEqual(2, lifecycle["captured_target_count"])
        swing = next(
            row
            for row in cast("list[dict[str, object]]", lifecycle["attempts"])
            if row["attempt_id"] == "S001"
        )
        self.assertEqual("no_impact_timeout", swing["outcome"])

    def test_policy_must_predate_source_capture(self) -> None:
        corpus_value = self._corpus_value()
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        lock_value["frozen_at_utc"] = "2026-08-24T00:00:00Z"
        predictions = self._predictions(corpus_value, lock_value)

        with self.assertRaisesRegex(ValueError, "not frozen before every holdout capture"):
            audio_holdout.evaluate_audio_holdout(
                corpus,
                json.dumps(lock_value),
                json.dumps(predictions),
                assets_verified=True,
                source_capture_times_utc=source_capture_times_utc(),
            )

    def test_selection_provenance_and_all_holdouts_are_not_cherry_picked(self) -> None:
        corpus_value = self._corpus_value()
        second = copy.deepcopy(cast("list[object]", corpus_value["sessions"])[0])
        second = cast("dict[str, object]", second)
        second["recording_id"] = "holdout-003"
        for role in manifest.ROLES:
            stream = cast("dict[str, object]", cast("dict[str, object]", second["streams"])[role])
            for kind in ("source_manifest", "video", "audio"):
                asset = cast("dict[str, object]", stream[kind])
                asset["path"] = cast("str", asset["path"]).replace("holdout/", "second/")
        cast("list[object]", corpus_value["sessions"]).append(second)
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        predictions = self._predictions(corpus_value, lock_value)
        cast("list[object]", predictions["sessions"]).pop()

        with self.assertRaisesRegex(ValueError, "every qualifying holdout"):
            audio_holdout.evaluate_audio_holdout(
                corpus,
                json.dumps(lock_value),
                json.dumps(predictions),
                assets_verified=True,
                source_capture_times_utc={
                    "holdout-002": datetime.datetime(2026, 8, 23, tzinfo=datetime.UTC),
                    "holdout-003": datetime.datetime(2026, 8, 24, tzinfo=datetime.UTC),
                    "holdout-004": datetime.datetime(2026, 8, 24, tzinfo=datetime.UTC),
                },
            )

        lock_value["development_recording_ids"] = ["other-development"]
        predictions = self._predictions(corpus_value, lock_value)
        with self.assertRaisesRegex(ValueError, "selection IDs do not exactly match"):
            audio_holdout.evaluate_audio_holdout(
                corpus,
                json.dumps(lock_value),
                json.dumps(predictions),
                assets_verified=True,
                source_capture_times_utc={
                    "holdout-002": datetime.datetime(2026, 8, 23, tzinfo=datetime.UTC),
                    "holdout-003": datetime.datetime(2026, 8, 24, tzinfo=datetime.UTC),
                    "holdout-004": datetime.datetime(2026, 8, 24, tzinfo=datetime.UTC),
                },
            )

    def test_missing_category_and_incomplete_armed_replay_are_rejected(self) -> None:
        corpus_value = self._corpus_value()
        session = cast("dict[str, object]", cast("list[object]", corpus_value["sessions"])[0])
        episodes = cast("list[dict[str, object]]", session["episodes"])
        episodes.remove(next(episode for episode in episodes if episode["category"] == "club_drop"))
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        predictions = self._predictions(corpus_value, lock_value)
        with self.assertRaisesRegex(ValueError, "audio holdout evidence is incomplete"):
            audio_holdout.evaluate_audio_holdout(
                corpus,
                json.dumps(lock_value),
                json.dumps(predictions),
                assets_verified=True,
                source_capture_times_utc=source_capture_times_utc(),
            )

        corpus_value = self._corpus_value()
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        predictions = self._predictions(corpus_value, lock_value)
        streams = cast(
            "dict[str, object]",
            cast("dict[str, object]", cast("list[object]", predictions["sessions"])[0])["streams"],
        )
        replays = cast(
            "list[dict[str, object]]",
            cast("dict[str, object]", streams["face_on"])["armed_replays"],
        )
        replays.pop()
        with self.assertRaisesRegex(ValueError, "armed replay coverage is incomplete"):
            audio_holdout.evaluate_audio_holdout(
                corpus,
                json.dumps(lock_value),
                json.dumps(predictions),
                assets_verified=True,
                source_capture_times_utc=source_capture_times_utc(),
            )

    def test_prediction_must_match_the_locked_detector(self) -> None:
        corpus_value = self._corpus_value()
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        predictions = self._predictions(corpus_value, lock_value)
        detector = cast("dict[str, object]", predictions["detector"])
        detector["algorithm_id"] = "tuned-on-holdout"

        with self.assertRaisesRegex(ValueError, "configuration disagrees"):
            audio_holdout.evaluate_audio_holdout(
                corpus,
                json.dumps(lock_value),
                json.dumps(predictions),
                assets_verified=True,
                source_capture_times_utc=source_capture_times_utc(),
            )

    def test_unlabeled_reviewed_background_candidate_fails_the_gate(self) -> None:
        corpus_value = self._corpus_value()
        corpus = manifest.parse_manifest(json.dumps(corpus_value))
        lock_value = self._policy_lock()
        predictions = self._predictions(corpus_value, lock_value)
        session = cast("dict[str, object]", cast("list[object]", predictions["sessions"])[0])
        streams = cast("dict[str, object]", session["streams"])
        for role in manifest.ROLES:
            candidates = cast(
                "list[dict[str, int]]",
                cast("dict[str, object]", streams[role])["continuous_candidates"],
            )
            candidates.append({"strike_ms": 26_000, "decision_ms": 26_002})

        report = audio_holdout.evaluate_audio_holdout(
            corpus,
            json.dumps(lock_value),
            json.dumps(predictions),
            assets_verified=True,
            source_capture_times_utc=source_capture_times_utc(),
        )

        self.assertFalse(report["passed"])
        candidate_generation = cast("dict[str, object]", report["candidate_generation"])
        self.assertEqual(2, candidate_generation["non_target_candidate_count"])
        self.assertEqual(0, candidate_generation["labeled_negative_candidate_count"])

    @staticmethod
    def _policy_lock() -> dict[str, object]:
        return {
            "schema_version": 1,
            "report_type": "audio_detector_policy_lock",
            "policy_id": "conservative-envelope-locked-v1",
            "frozen_at_utc": "2026-08-22T00:00:00Z",
            "development_recording_ids": ["development-001"],
            "detector": {
                "algorithm_id": "adaptive-envelope-v1",
                "implementation_target": (
                    "//capture/offline/experiments/envelope:envelope_experiment"
                ),
                "implementation_sha256": "d" * 64,
                "config_by_device": {
                    "pixel5a": {"background_multiplier": 12.0},
                    "pixel6": {"background_multiplier": 12.0},
                },
            },
            "evaluation": {
                "leader_role": "face_on",
                "target_tolerance_ms": 100,
                "audio_ready_delay_ms": 500,
                "video_ready_delay_ms": 300,
                "maximum_armed_ms": 1_200,
                "post_terminal_ms": 500,
                "rearm_delay_ms": 500,
                "retained_history_ms": 1_000,
                "minimum_target_recall_ppm": 1_000_000,
                "minimum_quiet_target_recall_ppm": 1_000_000,
                "maximum_negative_continuous_candidates": 0,
                "maximum_false_terminal_attempts": 0,
                "minimum_lifecycle_capture_ppm": 1_000_000,
                "maximum_high_speed_duty_ppm": 500_000,
            },
        }

    @classmethod
    def _predictions(
        cls, corpus_value: dict[str, object], lock_value: dict[str, object]
    ) -> dict[str, object]:
        policy = audio_holdout.parse_policy_lock(json.dumps(lock_value))
        sessions: list[object] = []
        for raw_session in cast("list[object]", corpus_value["sessions"]):
            session = cast("dict[str, object]", raw_session)
            streams: dict[str, object] = {}
            for role in manifest.ROLES:
                raw_stream = cast(
                    "dict[str, object]", cast("dict[str, object]", session["streams"])[role]
                )
                duration = cast("int", raw_stream["duration_ms"])
                continuous: list[object] = []
                replays: list[object] = []
                for episode in cast("list[dict[str, object]]", session["episodes"]):
                    view = cast(
                        "dict[str, object]", cast("dict[str, object]", episode["views"])[role]
                    )
                    if episode["audio_expectation"] == "must_trigger":
                        reference = cast("int", view["reference_ms"])
                        continuous.append({"strike_ms": reference, "decision_ms": reference + 2})
                    if episode["pose_expectation"] != "must_arm":
                        continue
                    arms = [(cast("str", episode["id"]), cast("int", view["preferred_arm_ms"]))]
                    if episode["category"] == "clear_and_rearm":
                        arms = [
                            (f"{episode['id']}:initial", cast("int", view["preferred_arm_ms"])),
                            (f"{episode['id']}:rearm", cast("int", view["rearm_preferred_ms"])),
                        ]
                    for attempt_id, arm in arms:
                        candidates: list[object] = []
                        if episode["audio_expectation"] == "must_trigger":
                            reference = cast("int", view["reference_ms"])
                            candidates.append(
                                {"strike_ms": reference, "decision_ms": reference + 2}
                            )
                        replays.append(
                            {
                                "attempt_id": attempt_id,
                                "arm_ms": arm,
                                "evaluation_end_ms": min(
                                    arm
                                    + cast(
                                        "int",
                                        cast("dict[str, object]", lock_value["evaluation"])[
                                            "maximum_armed_ms"
                                        ],
                                    ),
                                    duration,
                                ),
                                "candidates": candidates,
                            }
                        )
                streams[role] = {
                    "continuous_candidates": sorted(
                        continuous, key=lambda value: cast("dict[str, int]", value)["decision_ms"]
                    ),
                    "armed_replays": replays,
                }
            sessions.append({"recording_id": session["recording_id"], "streams": streams})
        return {
            "schema_version": 1,
            "report_type": "audio_detector_holdout_predictions",
            "policy_id": lock_value["policy_id"],
            "policy_lock_sha256": policy.canonical_sha256,
            "detector": copy.deepcopy(lock_value["detector"]),
            "sessions": sessions,
        }

    @staticmethod
    def _corpus_value() -> dict[str, object]:
        categories = [
            ("S001", "real_swing", "must_arm", "must_trigger", "quiet"),
            ("N001", "practice_swing", "must_not_arm", "must_not_trigger", "nominal"),
            ("N007", "aborted_address", "must_arm", "must_not_trigger", "not_applicable"),
            ("N002", "mat_strike", "must_not_arm", "must_not_trigger", "loud"),
            ("N003", "waggle", "must_not_arm", "must_not_trigger", "quiet"),
            ("N008", "address_no_swing", "must_arm", "must_not_trigger", "not_applicable"),
            ("N004", "speech", "must_not_arm", "must_not_trigger", "nominal"),
            ("N005", "footsteps", "must_not_arm", "must_not_trigger", "quiet"),
            ("N006", "club_drop", "must_not_arm", "must_not_trigger", "loud"),
            ("N009", "clear_and_rearm", "must_arm", "diagnostic", "not_applicable"),
            ("N010", "empty_scene", "must_not_arm", "diagnostic", "not_applicable"),
            ("N011", "walk_through", "must_not_arm", "diagnostic", "not_applicable"),
            ("N012", "post_shot_finish", "must_not_arm", "diagnostic", "not_applicable"),
            ("N013", "repeated_setup", "must_not_arm", "diagnostic", "not_applicable"),
        ]
        episodes: list[object] = []
        for index, (episode_id, category, pose, audio, character) in enumerate(categories):
            start = index * 1_800
            end = start + 1_800
            views: dict[str, object] = {}
            for role in manifest.ROLES:
                must_arm = pose == "must_arm"
                clear_rearm = category == "clear_and_rearm"
                views[role] = {
                    "start_ms": start,
                    "end_ms": end,
                    "reference_ms": start + 1_500 if audio == "must_trigger" else None,
                    "safe_arm_start_ms": start + (100 if clear_rearm else 200)
                    if must_arm
                    else None,
                    "preferred_arm_ms": start + (200 if clear_rearm else 400) if must_arm else None,
                    "takeaway_ms": start + (1_500 if clear_rearm else 1_000) if must_arm else None,
                    "clear_ms": start + 700 if clear_rearm else None,
                    "rearm_safe_start_ms": start + 1_200 if clear_rearm else None,
                    "rearm_preferred_ms": start + 1_300 if clear_rearm else None,
                }
            episodes.append(
                {
                    "id": episode_id,
                    "category": category,
                    "pose_expectation": pose,
                    "audio_expectation": audio,
                    "audio_character": character,
                    "views": views,
                    "notes": "Synthetic label used only to exercise the holdout gate.",
                }
            )
        base_stream = {
            "node_id": "node-placeholder",
            "device_label": "device-placeholder",
            "duration_ms": 27_000,
            "source_manifest": {"path": "holdout/dtl_manifest.json", "sha256": "a" * 64},
            "video": {"path": "holdout/dtl_video.mp4", "sha256": "b" * 64},
            "audio": {"path": "holdout/dtl_audio.wav", "sha256": "c" * 64},
            "reviewed_intervals": [{"start_ms": 0, "end_ms": 27_000, "note": "complete review"}],
        }
        down = copy.deepcopy(base_stream)
        down.update({"node_id": "node-pixel6", "device_label": "pixel6"})
        face = copy.deepcopy(base_stream)
        face.update({"node_id": "node-pixel5a", "device_label": "pixel5a"})
        for kind in ("source_manifest", "video", "audio"):
            asset = cast("dict[str, object]", face[kind])
            asset["path"] = cast("str", asset["path"]).replace("dtl_", "atl_")
        first_session: dict[str, object] = {
            "recording_id": "holdout-002",
            "split": "validation",
            "scene": {
                "golfer_label": "golfer-a",
                "environment_label": "studio-a",
                "lighting_label": "artificial-a",
                "framing_label": "portrait-a",
            },
            "streams": {"down_the_line": down, "face_on": face},
            "review": {
                "state": "complete",
                "reviewer": "human-reviewer",
                "reviewed_at_utc": "2026-08-24T00:00:00Z",
            },
            "episodes": episodes,
            "notes": "Synthetic complete holdout used only by deterministic tests.",
        }
        second_session = copy.deepcopy(first_session)
        second_session["recording_id"] = "holdout-004"
        second_session["split"] = "challenge"
        second_session["scene"] = {
            "golfer_label": "golfer-b",
            "environment_label": "studio-b",
            "lighting_label": "daylight-b",
            "framing_label": "portrait-b",
        }
        for role in manifest.ROLES:
            second_stream = cast(
                "dict[str, object]",
                cast("dict[str, object]", second_session["streams"])[role],
            )
            for kind in ("source_manifest", "video", "audio"):
                asset = cast("dict[str, object]", second_stream[kind])
                asset["path"] = cast("str", asset["path"]).replace("holdout/", "holdout-second/")
        return {
            "schema_version": 1,
            "corpus_id": "audio-holdout-corpus",
            "development_recording_ids": ["development-001"],
            "development_device_assignment": {
                "down_the_line": "pixel5a",
                "face_on": "pixel6",
            },
            "sessions": [first_session, second_session],
        }


if __name__ == "__main__":
    unittest.main()
