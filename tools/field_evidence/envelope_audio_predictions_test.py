"""Hermetic holdout replay tests for the robust_hp120_x12 candidate."""

# unittest's legacy setUp hook predates the override decorator used by the type checker.
# pyright: reportImplicitOverride=false, reportUninitializedInstanceVariable=false
# Scenario names document this end-to-end test fixture.
# ruff: noqa: D101, D102

from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import tempfile
import unittest
import wave
from pathlib import Path
from typing import cast, final

from python.runfiles import runfiles

from tools.field_evidence import audio_holdout, envelope_audio_predictions, manifest
from tools.field_evidence.audio_holdout_test import (
    complete_corpus_value,
    source_capture_times_utc,
)


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


@final
class EnvelopeAudioPredictionsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.replay = _runfile("capture/offline/experiments/envelope/streaming_envelope_replay")
        self.sources = {
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

    def test_exact_config_and_end_to_end_holdout_predictions(self) -> None:
        detector = envelope_audio_predictions.describe_detector(
            self.replay, self.sources, ["pixel5a", "pixel6"]
        )
        configs = cast("dict[str, dict[str, object]]", detector["config_by_device"])
        self.assertEqual(12.0, configs["pixel5a"]["background_multiplier"])
        self.assertEqual(configs["pixel5a"], configs["pixel6"])
        self.assertEqual(
            envelope_audio_predictions.implementation_sha256(self.sources),
            detector["implementation_sha256"],
        )

        corpus_value = complete_corpus_value()
        lock = self._policy_lock(detector)
        with tempfile.TemporaryDirectory() as temporary:
            media_root = Path(temporary)
            for raw_session in cast("list[object]", corpus_value["sessions"]):
                session_value = cast("dict[str, object]", raw_session)
                for role in manifest.ROLES:
                    stream_value = cast(
                        "dict[str, object]",
                        cast("dict[str, object]", session_value["streams"])[role],
                    )
                    self._write_audio(
                        media_root
                        / cast(
                            "str",
                            cast("dict[str, object]", stream_value["audio"])["path"],
                        ),
                        duration_ms=cast("int", stream_value["duration_ms"]),
                        impact_ms=1_500,
                    )
                    self._complete_stream_assets(media_root, session_value, role, stream_value)
            manifest_path = media_root / "evidence_manifest.json"
            policy_path = media_root / "policy.json"
            predictions_path = media_root / "predictions.json"
            manifest_path.write_text(json.dumps(corpus_value), encoding="utf-8")
            policy_path.write_text(json.dumps(lock), encoding="utf-8")
            completed = subprocess.run(
                [
                    str(_runfile("tools/field_evidence/envelope_audio_predictions")),
                    "predict",
                    "--manifest",
                    str(manifest_path),
                    "--media-root",
                    str(media_root),
                    "--policy-lock",
                    str(policy_path),
                    "--output",
                    str(predictions_path),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            predictions = cast(
                "dict[str, object]",
                json.loads(predictions_path.read_text(encoding="utf-8")),
            )

        session = cast("dict[str, object]", cast("list[object]", predictions["sessions"])[0])
        streams = cast("dict[str, dict[str, object]]", session["streams"])
        for role in manifest.ROLES:
            continuous = cast("list[dict[str, int]]", streams[role]["continuous_candidates"])
            self.assertEqual(1_500, continuous[0]["strike_ms"])
            self.assertGreater(continuous[0]["decision_ms"], continuous[0]["strike_ms"])
            replays = cast("list[dict[str, object]]", streams[role]["armed_replays"])
            swing = next(replay for replay in replays if replay["attempt_id"] == "S001")
            self.assertEqual(
                1_500,
                cast("list[dict[str, int]]", swing["candidates"])[0]["strike_ms"],
            )

        report = audio_holdout.evaluate_audio_holdout(
            manifest.parse_manifest(json.dumps(corpus_value)),
            json.dumps(lock),
            json.dumps(predictions),
            assets_verified=True,
            source_capture_times_utc=source_capture_times_utc(),
        )
        self.assertTrue(report["passed"])

    def test_stale_implementation_digest_is_rejected_before_replay(self) -> None:
        detector = envelope_audio_predictions.describe_detector(
            self.replay, self.sources, ["pixel5a", "pixel6"]
        )
        detector["implementation_sha256"] = "0" * 64
        corpus = manifest.parse_manifest(json.dumps(complete_corpus_value()))
        with self.assertRaisesRegex(ValueError, "implementation SHA-256 is stale"):
            envelope_audio_predictions.generate_predictions(
                corpus,
                Path("unused"),
                json.dumps(self._policy_lock(detector)),
                self.replay,
                self.sources,
            )

    def test_describe_executable_refuses_to_overwrite(self) -> None:
        executable = _runfile("tools/field_evidence/envelope_audio_predictions")
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "detector.json"
            command = [
                str(executable),
                "describe",
                "--device-label",
                "pixel5a",
                "--device-label",
                "pixel6",
                "--output",
                str(output),
            ]
            completed = subprocess.run(
                command, check=False, capture_output=True, text=True, timeout=20
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            repeated = subprocess.run(
                command, check=False, capture_output=True, text=True, timeout=20
            )
            self.assertNotEqual(0, repeated.returncode)
            self.assertIn("refusing to overwrite", repeated.stderr)

    @staticmethod
    def _policy_lock(detector: dict[str, object]) -> dict[str, object]:
        return {
            "schema_version": 1,
            "report_type": "audio_detector_policy_lock",
            "policy_id": "robust-envelope-hp120-x12-candidate-v1",
            "frozen_at_utc": "2026-08-22T00:00:00Z",
            "development_recording_ids": ["development-001"],
            "detector": detector,
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

    @staticmethod
    def _write_audio(path: Path, *, duration_ms: int, impact_ms: int) -> None:
        frame_count = duration_ms * 48
        samples = bytearray(frame_count * 2)
        struct.pack_into("<hhhh", samples, impact_ms * 48 * 2, 24_000, -16_000, 8_000, -4_000)
        path.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(48_000)
            output.writeframes(samples)

    @staticmethod
    def _complete_stream_assets(
        media_root: Path,
        session: dict[str, object],
        role: str,
        stream: dict[str, object],
    ) -> None:
        audio_asset = cast("dict[str, object]", stream["audio"])
        video_asset = cast("dict[str, object]", stream["video"])
        source_asset = cast("dict[str, object]", stream["source_manifest"])
        audio_path = media_root / cast("str", audio_asset["path"])
        video_path = media_root / cast("str", video_asset["path"])
        source_path = media_root / cast("str", source_asset["path"])
        video_path.parent.mkdir(parents=True, exist_ok=True)
        source_path.parent.mkdir(parents=True, exist_ok=True)
        video_path.write_bytes(f"video-{role}".encode())
        source_path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "session_kind": "field_recording",
                    "shared_recording_id": session["recording_id"],
                    "node_id": stream["node_id"],
                    "role": role,
                    "created_at_utc": "2026-08-23T00:00:00Z",
                    "duration_us": "27000000",
                    "video": {"bytes": str(video_path.stat().st_size)},
                    "audio": {"bytes": str(audio_path.stat().st_size)},
                }
            ),
            encoding="utf-8",
        )
        audio_asset["sha256"] = hashlib.sha256(audio_path.read_bytes()).hexdigest()
        video_asset["sha256"] = hashlib.sha256(video_path.read_bytes()).hexdigest()
        source_asset["sha256"] = hashlib.sha256(source_path.read_bytes()).hexdigest()


if __name__ == "__main__":
    unittest.main()
