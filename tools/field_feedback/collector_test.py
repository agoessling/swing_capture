"""Hermetic HTTP and archive tests for the field feedback collector."""

# BaseHTTPRequestHandler's override contract names this parameter after the built-in.
# ruff: noqa: A002

from __future__ import annotations

import datetime
import hashlib
import io
import json
import os
import stat
import tempfile
import threading
import unittest
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import TYPE_CHECKING, Self, cast, final
from unittest import mock

from tools.field_feedback import collector

if TYPE_CHECKING:
    from collections.abc import Mapping


TOKEN_A = "A" * 32
TOKEN_B = "B" * 32
CREATED_AT = "2026-08-17T12:34:56Z"
COLLECTED_AT = datetime.datetime(2026, 8, 17, 13, 0, tzinfo=datetime.UTC)
validate_peer_impact_mapping = (
    collector._validate_peer_impact_mapping  # pyright: ignore[reportPrivateUsage]  # noqa: SLF001
)


@final
class FakePhone:
    """Small real HTTP fixture that records archive authentication."""

    def __init__(  # noqa: C901
        self,
        node_id: str,
        role: str,
        token: str,
        archives: Mapping[str, bytes],
        *,
        redirect_archives: bool = False,
    ) -> None:
        """Configure but do not yet start a loopback fixture."""
        self.node_id = node_id
        self.role = role
        self.token = token
        self.archives = dict(archives)
        self.redirect_archives = redirect_archives
        self.archive_requests: list[tuple[str, str | None]] = []
        self.metadata_requests: list[tuple[str, str | None]] = []
        self.redirect_sink_authorizations: list[str | None] = []
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:  # noqa: PLR0911
                if self.path == "/api/v1/node":
                    authorization = self.headers.get("Authorization")
                    fixture.metadata_requests.append((self.path, authorization))
                    if authorization != f"Bearer {fixture.token}":
                        self.send_error(401)
                        return
                    self._json(
                        {
                            "schema_version": 1,
                            "node_id": fixture.node_id,
                            "role": fixture.role,
                            "capture_profile": "high_speed_720p_240fps",
                        }
                    )
                    return
                if self.path == "/api/v1/sessions":
                    authorization = self.headers.get("Authorization")
                    fixture.metadata_requests.append((self.path, authorization))
                    if authorization != f"Bearer {fixture.token}":
                        self.send_error(401)
                        return
                    self._json(
                        {
                            "schema_version": 1,
                            "sessions": [
                                {
                                    "session_id": session_id,
                                    "state": "ready",
                                    "created_at_utc": CREATED_AT,
                                    "error": "",
                                }
                                for session_id in sorted(fixture.archives)
                            ],
                        }
                    )
                    return
                if self.path == "/credential-sink":
                    fixture.redirect_sink_authorizations.append(self.headers.get("Authorization"))
                    self.send_error(500)
                    return
                prefix = "/api/v1/sessions/"
                suffix = "/diagnostics.zip"
                if self.path.startswith(prefix) and self.path.endswith(suffix):
                    session_id = self.path[len(prefix) : -len(suffix)]
                    authorization = self.headers.get("Authorization")
                    fixture.archive_requests.append((session_id, authorization))
                    if authorization != f"Bearer {fixture.token}":
                        self.send_error(401)
                        return
                    if fixture.redirect_archives:
                        self.send_response(302)
                        self.send_header("Location", fixture.base_url + "/credential-sink")
                        self.end_headers()
                        return
                    archive = fixture.archives.get(session_id)
                    if archive is None:
                        self.send_error(404)
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", "application/zip")
                    self.send_header("Content-Length", str(len(archive)))
                    self.end_headers()
                    self.wfile.write(archive)
                    return
                self.send_error(404)

            def log_message(  # pyright: ignore[reportImplicitOverride]
                self, format: str, *arguments: object
            ) -> None:
                del format, arguments

            def _json(self, value: object) -> None:
                body = json.dumps(value, separators=(",", ":")).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        # shutdown() waits for the serve loop's next poll. The 500 ms default
        # accumulated once for each of this test matrix's many fixtures.
        self._thread = threading.Thread(
            target=self._server.serve_forever,
            kwargs={"poll_interval": 0.01},
            daemon=True,
        )

    @property
    def base_url(self) -> str:
        """Return the loopback origin selected by the kernel."""
        address = self._server.server_address
        host, port = str(address[0]), int(address[1])
        return f"http://{host}:{port}"

    def __enter__(self) -> Self:
        """Start the fixture."""
        self._thread.start()
        return self

    def __exit__(self, *_arguments: object) -> None:
        """Stop the fixture and join its worker."""
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=5)


class FieldFeedbackCollectorTest(unittest.TestCase):
    """Exercise discovery, authentication, validation, and durable deduplication."""

    def test_collects_and_pairs_two_nodes_without_redownload(self) -> None:
        """Store each authenticated archive once and build a paired deterministic index."""
        coordination = coordination_record("shared-42", "dtl-local", "atl-local")
        dtl_archive = diagnostic_archive(
            "dtl-local",
            "dtl-node",
            "down_the_line",
            "shared-42",
            coordination,
            include_preview=True,
            peer_impact_mapping=peer_impact_mapping("dtl-node"),
        )
        atl_archive = diagnostic_archive(
            "atl-local",
            "atl-node",
            "face_on",
            "shared-42",
            coordination,
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"dtl-local": dtl_archive}) as dtl,
            FakePhone("atl-node", "face_on", TOKEN_B, {"atl-local": atl_archive}) as atl,
        ):
            output = Path(temporary) / "feedback"
            subject = collector.FieldFeedbackCollector(
                [
                    collector.NodeSpec(dtl.base_url, TOKEN_A),
                    collector.NodeSpec(atl.base_url, TOKEN_B),
                ],
                output,
                clock=lambda: COLLECTED_AT,
                extract_previews=True,
            )
            first = subject.collect_once()
            self.assertEqual((2, 0, ()), (first.downloaded, first.skipped, first.errors))
            self.assertEqual([("dtl-local", f"Bearer {TOKEN_A}")], dtl.archive_requests)
            self.assertEqual([("atl-local", f"Bearer {TOKEN_B}")], atl.archive_requests)

            index_bytes = subject.index_path.read_bytes()
            index = read_index(subject.index_path)
            artifacts = index_objects(index, "artifacts")
            shared_sessions = index_objects(index, "shared_sessions")
            self.assertNotIn(TOKEN_A, index_bytes.decode())
            self.assertNotIn(TOKEN_B, index_bytes.decode())
            self.assertEqual("paired", shared_sessions[0]["status"])
            self.assertEqual("validated_pair", shared_sessions[0]["coordination_status"])
            shared_timing = collector.require_object(
                shared_sessions[0].get("coordination_timing"), "shared coordination timing"
            )
            self.assertEqual("1000", shared_timing["combined_mapped_uncertainty_ns"])
            self.assertEqual(
                ["atl-node", "dtl-node"],
                [collector.required_string(item, "node_id") for item in artifacts],
            )
            for artifact in artifacts:
                self.assertEqual("passed", artifact["checksum_validation"])
                self.assertEqual("validated_coordination", artifact["linkage_status"])
                timing = collector.require_object(
                    artifact.get("coordination_timing"), "coordination timing"
                )
                self.assertEqual("1000", timing["combined_mapped_uncertainty_ns"])
                self.assertEqual("1010", timing["maximum_trigger_separation_ns"])
                archive_path = collector.required_string(artifact, "archive_path")
                self.assertTrue(archive_path.startswith("artifacts/20260817T130000"))
                retained = output / archive_path
                self.assertEqual(
                    collector.required_string(artifact, "archive_sha256"),
                    sha256(retained.read_bytes()),
                )
            dtl_artifact = next(
                item
                for item in artifacts
                if collector.required_string(item, "node_id") == "dtl-node"
            )
            preview = collector.require_object(dtl_artifact.get("preview_bundle"), "preview bundle")
            preview_directory = output / collector.required_string(preview, "directory")
            self.assertEqual(
                b"\xff\xd8A\xff\xd9\xff\xd8BC\xff\xd9\xff\xd8DEF\xff\xd9",
                (preview_directory / "preview_frames.mjpeg").read_bytes(),
            )
            preview_index = read_index(preview_directory / "frame_index.json")
            self.assertEqual(3, len(index_objects(preview_index, "frames")))
            self.assertEqual("available", dtl_artifact["pose_diagnostics_status"])
            retained_mapping = collector.require_object(
                dtl_artifact.get("peer_impact_mapping"), "peer impact mapping"
            )
            self.assertEqual("30", retained_mapping["mapping_age_at_send_ns"])
            self.assertEqual("5", retained_mapping["selected_to_mapped_residual_ns"])

            second = subject.collect_once()
            self.assertEqual((0, 2, ()), (second.downloaded, second.skipped, second.errors))
            self.assertEqual(index_bytes, subject.index_path.read_bytes())
            self.assertEqual(1, len(dtl.archive_requests))
            self.assertEqual(1, len(atl.archive_requests))

    def test_recovers_archive_and_preview_published_before_index(self) -> None:
        """A durable intent indexes published evidence without another phone download."""
        archive = diagnostic_archive(
            "interrupted",
            "dtl-node",
            "down_the_line",
            None,
            None,
            include_preview=True,
        )

        def interrupt(checkpoint: str) -> None:
            if checkpoint == "preview_durable":
                message = "injected interruption after preview publish"
                raise collector.CollectionError(message)

        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"interrupted": archive}) as phone,
        ):
            output = Path(temporary) / "feedback"
            interrupted = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                output,
                clock=lambda: COLLECTED_AT,
                extract_previews=True,
                failure_injector=interrupt,
            )
            first = interrupted.collect_once()
            self.assertEqual(0, first.downloaded)
            self.assertRegex(first.errors[0], r"injected interruption")
            transactions = list((output / ".transactions").glob("*.json"))
            self.assertEqual(1, len(transactions))
            self.assertNotIn(TOKEN_A, transactions[0].read_text())
            self.assertEqual(1, len(list((output / "artifacts").rglob("*.zip"))))
            self.assertEqual(1, len(list((output / "artifacts").rglob("*-preview"))))
            self.assertEqual([], index_objects(read_index(interrupted.index_path), "artifacts"))

            recovered = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                output,
                clock=lambda: COLLECTED_AT,
                extract_previews=True,
            )
            second = recovered.collect_once()
            self.assertEqual((0, 1, ()), (second.downloaded, second.skipped, second.errors))
            self.assertEqual(1, len(phone.archive_requests), "recovery must reuse the durable ZIP")
            artifact = index_objects(read_index(recovered.index_path), "artifacts")[0]
            preview = collector.require_object(artifact.get("preview_bundle"), "preview bundle")
            preview_directory = output / collector.required_string(preview, "directory")
            self.assertTrue((preview_directory / "frame_index.json").is_file())
            self.assertEqual([], list((output / ".transactions").iterdir()))

    def test_retries_transaction_interrupted_before_archive_publication(self) -> None:
        """An intent with no final evidence is retired and downloaded again."""
        archive = diagnostic_archive("not-published", "node", "down_the_line", None, None)

        def interrupt(checkpoint: str) -> None:
            if checkpoint == "transaction_durable":
                message = "injected interruption before archive publish"
                raise collector.CollectionError(message)

        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("node", "down_the_line", TOKEN_A, {"not-published": archive}) as phone,
        ):
            output = Path(temporary) / "feedback"
            interrupted = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                output,
                clock=lambda: COLLECTED_AT,
                failure_injector=interrupt,
            )
            first = interrupted.collect_once()
            self.assertEqual(0, first.downloaded)
            self.assertEqual([], list((output / "artifacts").rglob("*.zip")))
            self.assertEqual(1, len(list((output / ".transactions").glob("*.json"))))

            recovered = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                output,
                clock=lambda: COLLECTED_AT,
            )
            second = recovered.collect_once()
            self.assertEqual((1, 0, ()), (second.downloaded, second.skipped, second.errors))
            self.assertEqual(2, len(phone.archive_requests))
            self.assertEqual([], list((output / ".transactions").iterdir()))

    def test_recovers_index_published_before_transaction_cleanup(self) -> None:
        """An indexed transaction is validated and retired after an interrupted cleanup."""
        archive = diagnostic_archive("indexed", "dtl-node", "down_the_line", None, None)

        def interrupt(checkpoint: str) -> None:
            if checkpoint == "index_durable":
                message = "injected interruption after index publish"
                raise collector.CollectionError(message)

        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"indexed": archive}) as phone,
        ):
            output = Path(temporary) / "feedback"
            interrupted = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                output,
                clock=lambda: COLLECTED_AT,
                failure_injector=interrupt,
            )
            first = interrupted.collect_once()
            self.assertEqual(0, first.downloaded)
            self.assertEqual(1, len(index_objects(read_index(interrupted.index_path), "artifacts")))
            self.assertEqual(1, len(list((output / ".transactions").glob("*.json"))))

            recovered = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                output,
                clock=lambda: COLLECTED_AT,
            )
            second = recovered.collect_once()
            self.assertEqual((0, 1, ()), (second.downloaded, second.skipped, second.errors))
            self.assertEqual(1, len(phone.archive_requests))
            self.assertEqual([], list((output / ".transactions").iterdir()))

    def test_published_files_and_directories_are_fsynced(self) -> None:
        """Durable writes sync both file contents and containing directory entries."""
        archive = diagnostic_archive("durable", "node", "down_the_line", None, None)
        synced_directory = False
        real_fsync = os.fsync

        def record_fsync(descriptor: int) -> None:
            nonlocal synced_directory
            synced_directory = synced_directory or stat.S_ISDIR(os.fstat(descriptor).st_mode)
            real_fsync(descriptor)

        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("node", "down_the_line", TOKEN_A, {"durable": archive}) as phone,
            mock.patch("tools.field_feedback.collector.os.fsync", side_effect=record_fsync),
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual((1, 0, ()), (result.downloaded, result.skipped, result.errors))
        self.assertTrue(synced_directory)

    def test_rejects_capture_manifest_frame_index_that_disagrees_with_trace(self) -> None:
        """The embedded pose index cannot disagree with checksummed trace/MJPEG byte spans."""
        archive = diagnostic_archive(
            "bad-frame-index",
            "dtl-node",
            "down_the_line",
            None,
            None,
            include_preview=True,
            corrupt_pose_frame_index=True,
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"bad-frame-index": archive}) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(0, result.downloaded)
            self.assertRegex(result.errors[0], r"frame index contradicts its trace")

    def test_rejects_boolean_capture_frame_index_integer(self) -> None:
        """JSON booleans cannot impersonate integer fields in the exact Android contract."""
        archive = diagnostic_archive(
            "bad-frame-type",
            "dtl-node",
            "down_the_line",
            None,
            None,
            include_preview=True,
            boolean_pose_frame_index=True,
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"bad-frame-type": archive}) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(0, result.downloaded)
            self.assertRegex(result.errors[0], r"frame index contradicts its trace")

    def test_collects_standby_audio_and_optional_pose_evidence_without_video(self) -> None:
        """Accept diagnostic-only sessions and index their explicit evidence contract."""
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone(
                "dtl-node",
                "down_the_line",
                TOKEN_A,
                {
                    "standby-audio": standby_diagnostic_archive(
                        "standby-audio", "dtl-node", include_preview=False
                    ),
                    "standby-preview": standby_diagnostic_archive(
                        "standby-preview", "dtl-node", include_preview=True
                    ),
                },
            ) as phone,
        ):
            output = Path(temporary) / "feedback"
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                output,
                clock=lambda: COLLECTED_AT,
                extract_previews=True,
            )
            result = subject.collect_once()
            self.assertEqual((2, 0, ()), (result.downloaded, result.skipped, result.errors))
            artifacts = index_objects(read_index(subject.index_path), "artifacts")
            self.assertEqual(
                ["standby_diagnostic", "standby_diagnostic"],
                [collector.required_string(item, "session_kind") for item in artifacts],
            )
            self.assertEqual(
                ["available", "not_available"],
                sorted(
                    collector.required_string(item, "pose_diagnostics_status") for item in artifacts
                ),
            )
            for artifact in artifacts:
                self.assertEqual("operator_tag", artifact["trigger_source"])
                audio = collector.require_object(artifact.get("diagnostic_audio"), "audio")
                self.assertEqual("diagnostic_audio.wav", audio["path"])
                self.assertEqual(48_000, audio["sample_rate_hz"])
            preview_artifact = next(
                item for item in artifacts if item["pose_diagnostics_status"] == "available"
            )
            preview = collector.require_object(
                preview_artifact.get("preview_bundle"), "preview bundle"
            )
            preview_directory = output / collector.required_string(preview, "directory")
            self.assertEqual(
                b"\xff\xd8A\xff\xd9\xff\xd8BC\xff\xd9\xff\xd8DEF\xff\xd9",
                (preview_directory / "preview_frames.mjpeg").read_bytes(),
            )
            preview_index = read_index(preview_directory / "frame_index.json")
            frames = index_objects(preview_index, "frames")
            self.assertEqual(6, preview_index["observation_count"])
            self.assertEqual([1, 4, 6], [frame["trace_line"] for frame in frames])
            self.assertEqual([0, 3, 5], [frame["sequence_index"] for frame in frames])

    def test_rejects_inconsistent_current_android_pose_trace(self) -> None:
        """Trace-only rows cannot smuggle spans and every MJPEG byte must be indexed."""
        archive = standby_diagnostic_archive(
            "standby-bad-preview",
            "dtl-node",
            include_preview=True,
            corrupt_preview_trace=True,
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone(
                "dtl-node",
                "down_the_line",
                TOKEN_A,
                {"standby-bad-preview": archive},
            ) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(0, result.downloaded)
            self.assertRegex(result.errors[0], r"trace-only pose observation")

    def test_rejects_standby_archive_with_noncanonical_wav(self) -> None:
        """Checksummed bytes must still satisfy the declared PCM16 WAV contract."""
        archive = standby_diagnostic_archive(
            "standby-bad-wav", "dtl-node", include_preview=False, corrupt_wav=True
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"standby-bad-wav": archive}) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(0, result.downloaded)
            self.assertRegex(result.errors[0], r"canonical mono PCM16")
            self.assertEqual([], index_objects(read_index(subject.index_path), "artifacts"))

    def test_validates_detected_impact_and_rejects_corrupt_cross_field_evidence(self) -> None:
        """Detector and incident semantics must agree with the retained audio marker."""
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone(
                "dtl-node",
                "down_the_line",
                TOKEN_A,
                {
                    "detected-valid": standby_diagnostic_archive(
                        "detected-valid", "dtl-node", include_preview=False, detected_impact=True
                    ),
                    "detector-corrupt": standby_diagnostic_archive(
                        "detector-corrupt",
                        "dtl-node",
                        include_preview=False,
                        detected_impact=True,
                        corrupt_detector=True,
                    ),
                    "timing-corrupt": standby_diagnostic_archive(
                        "timing-corrupt",
                        "dtl-node",
                        include_preview=False,
                        detected_impact=True,
                        corrupt_timing_mark=True,
                    ),
                },
            ) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(1, result.downloaded)
            self.assertEqual(2, len(result.errors))
            self.assertTrue(any("frame positions contradict" in error for error in result.errors))
            self.assertTrue(any("outside retained evidence" in error for error in result.errors))
            artifacts = index_objects(read_index(subject.index_path), "artifacts")
            self.assertEqual(["detected-valid"], [item["session_id"] for item in artifacts])
            self.assertEqual("detected_impact", artifacts[0]["trigger_source"])

    def test_rejects_export_checksum_mismatch_without_retaining_zip(self) -> None:
        """Fail closed when an otherwise valid ZIP disagrees with its export manifest."""
        archive = diagnostic_archive(
            "local-1",
            "node-1",
            "down_the_line",
            None,
            None,
            corrupt_manifest_checksum=True,
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("node-1", "down_the_line", TOKEN_A, {"local-1": archive}) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(0, result.downloaded)
            self.assertRegex(result.errors[0], r"checksum differs")
            index = read_index(subject.index_path)
            self.assertEqual([], index_objects(index, "artifacts"))
            self.assertEqual([], list((subject.index_path.parent / "artifacts").rglob("*.zip")))

    def test_rejects_conflicting_cross_node_coordination(self) -> None:
        """Retain the first valid node but reject a peer with different pairing evidence."""
        dtl_coordination = coordination_record("shared-9", "dtl-9", "atl-9")
        atl_coordination = {**dtl_coordination, "recorded_at_epoch_ms": "1770000000001"}
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone(
                "dtl-node",
                "down_the_line",
                TOKEN_A,
                {
                    "dtl-9": diagnostic_archive(
                        "dtl-9",
                        "dtl-node",
                        "down_the_line",
                        "shared-9",
                        dtl_coordination,
                    )
                },
            ) as dtl,
            FakePhone(
                "atl-node",
                "face_on",
                TOKEN_B,
                {
                    "atl-9": diagnostic_archive(
                        "atl-9",
                        "atl-node",
                        "face_on",
                        "shared-9",
                        atl_coordination,
                    )
                },
            ) as atl,
        ):
            subject = collector.FieldFeedbackCollector(
                [
                    collector.NodeSpec(dtl.base_url, TOKEN_A),
                    collector.NodeSpec(atl.base_url, TOKEN_B),
                ],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(1, result.downloaded)
            self.assertRegex(result.errors[0], r"conflicting coordination")
            index = read_index(subject.index_path)
            self.assertEqual("partial", index_objects(index, "shared_sessions")[0]["status"])
            self.assertEqual(
                ["dtl-node"],
                [
                    collector.required_string(item, "node_id")
                    for item in index_objects(index, "artifacts")
                ],
            )

    def test_rejects_checksum_valid_but_internally_corrupt_coordination_timing(self) -> None:
        """An exported record cannot lie about composed uncertainty or pair separation."""
        coordination = coordination_record("shared-bad", "dtl-bad", "atl-bad")
        down = coordination["down_the_line"]
        self.assertIsInstance(down, dict)
        cast("dict[str, object]", down)["mapped_coordinator_uncertainty_ns"] = "501"
        archive = diagnostic_archive(
            "dtl-bad",
            "dtl-node",
            "down_the_line",
            "shared-bad",
            coordination,
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"dtl-bad": archive}) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(0, result.downloaded)
            self.assertRegex(result.errors[0], r"mapped uncertainty is not composed")
            self.assertEqual([], index_objects(read_index(subject.index_path), "artifacts"))

    def test_does_not_publish_one_sided_coordination_as_paired_timing(self) -> None:
        """Keep paired timing unqualified when only one manifest has coordination evidence."""
        coordination = coordination_record("shared-one-sided", "dtl-one-sided", "atl-one-sided")
        dtl_archive = diagnostic_archive(
            "dtl-one-sided",
            "dtl-node",
            "down_the_line",
            "shared-one-sided",
            coordination,
        )
        atl_archive = diagnostic_archive(
            "atl-one-sided",
            "atl-node",
            "face_on",
            "shared-one-sided",
            None,
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"dtl-one-sided": dtl_archive}) as dtl,
            FakePhone("atl-node", "face_on", TOKEN_B, {"atl-one-sided": atl_archive}) as atl,
        ):
            subject = collector.FieldFeedbackCollector(
                [
                    collector.NodeSpec(dtl.base_url, TOKEN_A),
                    collector.NodeSpec(atl.base_url, TOKEN_B),
                ],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual((2, ()), (result.downloaded, result.errors))
            shared = index_objects(read_index(subject.index_path), "shared_sessions")[0]
            self.assertEqual("paired", shared["status"])
            self.assertEqual("incomplete", shared["coordination_status"])
            self.assertIsNone(shared["coordination_timing"])

    def test_rejects_coordination_that_places_local_session_under_peer_role(self) -> None:
        """A matching node/session tuple must occur under the manifest's advertised role."""
        coordination = coordination_record("shared-wrong-role", "peer-session", "dtl-wrong-role")
        down = collector.require_object(coordination["down_the_line"], "down timing")
        face = collector.require_object(coordination["face_on"], "face timing")
        down["node_id"] = "other-node"
        face["node_id"] = "dtl-node"
        archive = diagnostic_archive(
            "dtl-wrong-role",
            "dtl-node",
            "down_the_line",
            "shared-wrong-role",
            coordination,
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"dtl-wrong-role": archive}) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(0, result.downloaded)
            self.assertRegex(result.errors[0], r"recorded under another role")

    def test_rejects_tampered_derived_index_metadata_even_when_archive_is_unchanged(self) -> None:
        """The retained ZIP, not editable index fields, remains timing evidence authority."""
        coordination = coordination_record("shared-index", "dtl-index", "atl-index")
        archive = diagnostic_archive(
            "dtl-index", "dtl-node", "down_the_line", "shared-index", coordination
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"dtl-index": archive}) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            self.assertEqual(1, subject.collect_once().downloaded)
            index = read_index(subject.index_path)
            artifact = index_objects(index, "artifacts")[0]
            timing = collector.require_object(artifact["coordination_timing"], "timing")
            timing["maximum_trigger_separation_ns"] = "999999"
            subject.index_path.write_bytes(canonical_json(index))
            with self.assertRaisesRegex(
                collector.CollectionError, "metadata contradicts its archive"
            ):
                subject.collect_once()

    def test_rejects_peer_impact_mapping_with_false_candidate_residual(self) -> None:
        """Field indexing independently recomputes the shadow candidate-to-mapped residual."""
        mapping = peer_impact_mapping("dtl-node")
        mapping["selected_to_mapped_residual_ns"] = "6"
        coordination = coordination_record("shared-map", "dtl-map", "atl-map")
        archive = diagnostic_archive(
            "dtl-map",
            "dtl-node",
            "down_the_line",
            "shared-map",
            coordination,
            peer_impact_mapping=mapping,
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("dtl-node", "down_the_line", TOKEN_A, {"dtl-map": archive}) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(0, result.downloaded)
            self.assertRegex(result.errors[0], r"selected residual is invalid")

    def test_peer_impact_mapping_enforces_source_specific_policy(self) -> None:
        """Validated evidence must be producible by the mapped/local/arrival selection paths."""
        implausible_clock_candidate = peer_impact_mapping("dtl-node")
        implausible_clock_candidate["selected_local_trigger_elapsed_realtime_ns"] = "200000000"
        implausible_clock_candidate["selected_to_mapped_residual_ns"] = "199998000"
        with self.assertRaisesRegex(collector.CollectionError, "exceeds its selection window"):
            validate_peer_impact_mapping(
                {"peer_impact_mapping": implausible_clock_candidate},
                "dtl-node",
                "peer_audio_clock_candidate",
            )

        false_acceptance = peer_impact_mapping("dtl-node")
        false_acceptance["mapping_age_at_send_ns"] = "10000000001"
        with self.assertRaisesRegex(collector.CollectionError, "policy contradicts"):
            validate_peer_impact_mapping(
                {"peer_impact_mapping": false_acceptance},
                "dtl-node",
                "peer_audio_clock_candidate",
            )

        false_arrival = peer_impact_mapping("dtl-node")
        false_arrival.update(
            {
                "selection_source": "peer_audio_arrival",
                "selected_local_trigger_elapsed_realtime_ns": "2099",
                "selected_local_uncertainty_ns": "0",
                "selected_to_mapped_residual_ns": "99",
            }
        )
        with self.assertRaisesRegex(collector.CollectionError, "does not match request arrival"):
            validate_peer_impact_mapping(
                {"peer_impact_mapping": false_arrival},
                "dtl-node",
                "peer_audio_arrival",
            )

        rejected_mapping = peer_impact_mapping("dtl-node")
        rejected_mapping.update(
            {
                "mapping_age_at_send_ns": "10000000001",
                "mapping_policy": "rejected",
                "effective_request_schema_version": 1,
                "fallback_semantics": "fresh_local_candidate_else_arrival",
                "request_arrival_elapsed_realtime_ns": "300000000",
                "selection_source": "peer_audio_local_candidate",
                "selected_local_trigger_elapsed_realtime_ns": "100000000",
                "selected_local_uncertainty_ns": "7",
                "selected_to_mapped_residual_ns": "99998000",
            }
        )
        retained = validate_peer_impact_mapping(
            {"peer_impact_mapping": rejected_mapping},
            "dtl-node",
            "peer_audio_local_candidate",
        )
        self.assertEqual("rejected", retained["mapping_policy"] if retained else None)

    def test_rejects_wrong_token_and_changed_local_evidence(self) -> None:
        """Require bearer auth and refuse silent replacement after local checksum damage."""
        archive = diagnostic_archive("local", "node", "down_the_line", None, None)
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone("node", "down_the_line", TOKEN_A, {"local": archive}) as phone,
        ):
            output = Path(temporary) / "feedback"
            wrong = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_B)],
                output,
                clock=lambda: COLLECTED_AT,
            )
            rejected = wrong.collect_once()
            self.assertEqual(0, rejected.downloaded)
            self.assertRegex(rejected.errors[0], r"HTTP 401")

            accepted = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                output,
                clock=lambda: COLLECTED_AT,
            )
            self.assertEqual(1, accepted.collect_once().downloaded)
            self.assertIn(("/api/v1/node", f"Bearer {TOKEN_A}"), phone.metadata_requests)
            self.assertIn(("/api/v1/sessions", f"Bearer {TOKEN_A}"), phone.metadata_requests)
            index = read_index(accepted.index_path)
            artifact = index_objects(index, "artifacts")[0]
            retained = output / collector.required_string(artifact, "archive_path")
            retained.write_bytes(b"changed")
            with self.assertRaisesRegex(collector.CollectionError, "checksum changed"):
                accepted.collect_once()

    def test_does_not_forward_bearer_token_through_http_redirect(self) -> None:
        """A phone response cannot redirect its collector credential to another request target."""
        archive = diagnostic_archive("local", "node", "down_the_line", None, None)
        with (
            tempfile.TemporaryDirectory() as temporary,
            FakePhone(
                "node",
                "down_the_line",
                TOKEN_A,
                {"local": archive},
                redirect_archives=True,
            ) as phone,
        ):
            subject = collector.FieldFeedbackCollector(
                [collector.NodeSpec(phone.base_url, TOKEN_A)],
                Path(temporary) / "feedback",
                clock=lambda: COLLECTED_AT,
            )
            result = subject.collect_once()
            self.assertEqual(0, result.downloaded)
            self.assertRegex(result.errors[0], r"HTTP 302")
            self.assertEqual([], phone.redirect_sink_authorizations)

    def test_validates_explicit_node_configuration(self) -> None:
        """Reject credentials, origins, and collector cardinalities outside the contract."""
        with self.assertRaisesRegex(ValueError, "32 URL-safe"):
            collector.NodeSpec("http://phone.test:4315", "short")
        with self.assertRaisesRegex(ValueError, "bare HTTP"):
            collector.NodeSpec("http://phone.test:4315/path", TOKEN_A)
        with self.assertRaisesRegex(ValueError, "one or two"):
            collector.FieldFeedbackCollector([], Path("unused"))


def coordination_record(
    shared_session_id: str,
    dtl_session_id: str,
    atl_session_id: str,
) -> dict[str, object]:
    """Build the linkage fields used by archive validation."""
    return {
        "schema_version": 1,
        "shared_session_id": shared_session_id,
        "status": "paired",
        "recorded_at_epoch_ms": "1770000000000",
        "down_the_line": {
            "role": "down_the_line",
            "node_id": "dtl-node",
            "local_session_id": dtl_session_id,
            "trigger_timestamp_ns": "10000",
            "trigger_uncertainty_ns": "200",
            "mapped_coordinator_timestamp_ns": "9000",
            "mapped_coordinator_uncertainty_ns": "500",
            "clock_offset_ns": "1000",
            "clock_uncertainty_ns": "300",
            "minimum_round_trip_ns": "400",
            "maximum_round_trip_ns": "800",
            "clock_sample_count": 3,
            "source": "peer_audio_clock_candidate",
        },
        "face_on": {
            "role": "face_on",
            "node_id": "atl-node",
            "local_session_id": atl_session_id,
            "trigger_timestamp_ns": "20010",
            "trigger_uncertainty_ns": "250",
            "mapped_coordinator_timestamp_ns": "9010",
            "mapped_coordinator_uncertainty_ns": "500",
            "clock_offset_ns": "11000",
            "clock_uncertainty_ns": "250",
            "minimum_round_trip_ns": "500",
            "maximum_round_trip_ns": "900",
            "clock_sample_count": 3,
            "source": "local_audio",
        },
        "minimum_trigger_separation_ns": "0",
        "maximum_trigger_separation_ns": "1010",
    }


def peer_impact_mapping(target_node_id: str) -> dict[str, object]:
    """Build the retained schema emitted for a mapped shadow audio candidate."""
    return {
        "schema_version": 2,
        "request_schema_version": 2,
        "leader_node_id": "atl-node",
        "leader_trigger_elapsed_realtime_ns": "1000",
        "target_peer_node_id": target_node_id,
        "mapped_peer_trigger_elapsed_realtime_ns": "2000",
        "mapping_uncertainty_ns": "20",
        "mapping_age_at_send_ns": "30",
        "minimum_round_trip_ns": "40",
        "maximum_round_trip_ns": "50",
        "clock_sample_count": 3,
        "request_arrival_elapsed_realtime_ns": "2100",
        "selection_source": "peer_audio_clock_candidate",
        "selected_local_trigger_elapsed_realtime_ns": "2005",
        "selected_local_uncertainty_ns": "7",
        "selected_to_mapped_residual_ns": "5",
        "mapping_policy": "accepted",
        "effective_request_schema_version": 2,
        "fallback_semantics": "mapped_candidate_else_arrival",
    }


def current_pose_preview_bundle(
    *, corrupt_trace_only_row: bool = False
) -> tuple[bytes, bytes, dict[str, object]]:
    """Build the current Android 5 Hz trace plus lower-cadence JPEG contract."""
    encoded_frames = (b"\xff\xd8A\xff\xd9", b"\xff\xd8BC\xff\xd9", b"\xff\xd8DEF\xff\xd9")
    frames = b"".join(encoded_frames)
    trace_rows: list[bytes] = []
    frame_by_sequence = {0: (0, 5), 3: (5, 6), 5: (11, 7)}
    for sequence_index in range(6):
        frame_span = frame_by_sequence.get(sequence_index)
        frame_available = frame_span is not None
        frame_offset = None if frame_span is None else str(frame_span[0])
        frame_length = 0 if frame_span is None else frame_span[1]
        if corrupt_trace_only_row and sequence_index == 1:
            frame_offset = "5"
        trace_rows.append(
            canonical_json(
                {
                    "schema_version": 1,
                    "sequence_index": sequence_index,
                    "timestamp_boottime_ns": str(10 + sequence_index * 10),
                    "frame_available": frame_available,
                    "frame_content_type": "image/jpeg" if frame_available else None,
                    "frame_byte_offset": frame_offset,
                    "frame_byte_length": frame_length,
                    "model_id": "models/pose_landmarker_lite.task:gpu",
                    "inference_duration_ns": "12000000",
                    "person_confidence": 0.8,
                    "address_confidence": 0.7,
                    "motion_magnitude": 0.12,
                    "hitting_region_occupied": True,
                    "controller_state": "qualifying",
                    "decision_reason": "address candidate",
                }
            )
        )
    trace = b"".join(trace_rows)
    metadata: dict[str, object] = {
        "frames_path": "pose_diagnostics/preview_frames.mjpeg",
        "frames_content_type": "image/jpeg",
        "frames_bytes": str(len(frames)),
        "trace_path": "pose_diagnostics/pose_trace.ndjson",
        "trace_content_type": "application/x-ndjson",
        "trace_bytes": len(trace),
        "first_timestamp_boottime_ns": "10",
        "end_timestamp_boottime_ns_exclusive": "61",
        "observation_count": 6,
        "jpeg_frame_count": 3,
        "frame_count": 3,
    }
    return frames, trace, metadata


def diagnostic_archive(  # noqa: PLR0913
    session_id: str,
    node_id: str,
    role: str,
    shared_session_id: str | None,
    coordination: Mapping[str, object] | None,
    *,
    corrupt_manifest_checksum: bool = False,
    include_preview: bool = False,
    corrupt_pose_frame_index: bool = False,
    boolean_pose_frame_index: bool = False,
    peer_impact_mapping: Mapping[str, object] | None = None,
) -> bytes:
    """Create a deterministic Android-shaped diagnostic ZIP fixture."""
    manifest: dict[str, object] = {
        "schema_version": 1,
        "session_id": session_id,
        "created_at_utc": CREATED_AT,
        "trigger": {"source": "local_audio"},
        "views": [{"role": role}],
        "android_capture": {
            "node_id": node_id,
            "shared_session_id": shared_session_id,
            "diagnostic_evidence": {
                "schema_version": 1,
                "audio": None,
                "audio_status": "not_available",
                "incident_status": "available",
                "preview": None,
                "preview_status": "not_available",
            },
        },
    }
    incident: dict[str, object] = {
        "schema_version": 1,
        "incident_id": session_id,
        "classification": "successful_capture",
        "created_at_epoch_ms": "1786970096000",
        "source_node_id": node_id,
        "user_feedback": {"classification": "good_capture", "note": "clean strike"},
        "timing_marks": [],
    }
    if peer_impact_mapping is not None:
        android_capture = collector.require_object(manifest["android_capture"], "android capture")
        android_capture["peer_impact_mapping"] = dict(peer_impact_mapping)
        trigger = collector.require_object(manifest["trigger"], "manifest trigger")
        trigger["source"] = collector.required_string(peer_impact_mapping, "selection_source")
    sources: dict[str, bytes] = {
        f"{session_id}/diagnostic_incident.json": canonical_json(incident),
        f"{session_id}/{role}.mp4": b"small encoded fixture",
    }
    if coordination is not None:
        sources[f"{session_id}/coordination_record.json"] = canonical_json(coordination)
    if include_preview:
        frames, trace, preview = current_pose_preview_bundle()
        preview["schema_version"] = 1
        preview["frame_index"] = [
            {
                "sequence_index": 0,
                "timestamp_boottime_ns": "10",
                "byte_offset": "0",
                "byte_length": True if boolean_pose_frame_index else 5,
                "content_type": "image/jpeg",
            },
            {
                "sequence_index": 3,
                "timestamp_boottime_ns": "40",
                "byte_offset": "5",
                "byte_length": 6,
                "content_type": "image/jpeg",
            },
            {
                "sequence_index": 5,
                "timestamp_boottime_ns": "60",
                "byte_offset": "12" if corrupt_pose_frame_index else "11",
                "byte_length": 7,
                "content_type": "image/jpeg",
            },
        ]
        diagnostic_evidence = collector.require_object(
            collector.require_object(manifest["android_capture"], "android capture")[
                "diagnostic_evidence"
            ],
            "diagnostic evidence",
        )
        diagnostic_evidence["preview"] = preview
        diagnostic_evidence["preview_status"] = "available"
        sources[f"{session_id}/pose_diagnostics/preview_frames.mjpeg"] = frames
        sources[f"{session_id}/pose_diagnostics/pose_trace.ndjson"] = trace
    sources[f"{session_id}/manifest.json"] = canonical_json(manifest)
    exported_files: list[dict[str, object]] = [
        {"path": path, "bytes": len(contents), "sha256": sha256(contents)}
        for path, contents in sorted(sources.items())
    ]
    if corrupt_manifest_checksum:
        for item in exported_files:
            if collector.required_string(item, "path").endswith("/manifest.json"):
                item["sha256"] = "0" * 64
    export = canonical_json(
        {
            "schema_version": 1,
            "session_id": session_id,
            "created_at_utc": "2026-08-17T13:00:00Z",
            "files": exported_files,
        }
    )
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as archive:
        write_zip_entry(archive, "diagnostic_export.json", export)
        for path, contents in sorted(sources.items()):
            write_zip_entry(archive, path, contents)
    return output.getvalue()


def standby_diagnostic_archive(  # noqa: PLR0913
    session_id: str,
    node_id: str,
    *,
    include_preview: bool,
    corrupt_wav: bool = False,
    detected_impact: bool = False,
    corrupt_detector: bool = False,
    corrupt_timing_mark: bool = False,
    corrupt_preview_trace: bool = False,
) -> bytes:
    """Create a deterministic diagnostic-only ZIP with no video track."""
    samples = (100, -200, 300, -400)
    wav = pcm16_wav(samples)
    if corrupt_wav:
        wav = b"NOPE" + wav[4:]
    incident: dict[str, object] = {
        "schema_version": 1,
        "incident_id": session_id,
        "classification": "impact_while_not_armed" if detected_impact else "user_reported",
        "created_at_epoch_ms": "1786970096000",
        "source_node_id": node_id,
        "user_feedback": {
            "classification": "unreviewed" if detected_impact else "missed_shot",
            "note": "",
        },
        "timing_marks": (
            [
                {
                    "kind": "audio_impact_transient",
                    "stream_id": "standby_audio",
                    "offset_us": "42" if corrupt_timing_mark else "0",
                }
            ]
            if detected_impact
            else []
        ),
    }
    incident_bytes = canonical_json(incident)
    sources: dict[str, bytes] = {
        f"{session_id}/diagnostic_audio.wav": wav,
        f"{session_id}/diagnostic_incident.json": incident_bytes,
    }
    preview: dict[str, object] | None = None
    preview_status = "not_available"
    if include_preview:
        frames, trace, preview = current_pose_preview_bundle(
            corrupt_trace_only_row=corrupt_preview_trace
        )
        sources[f"{session_id}/pose_diagnostics/preview_frames.mjpeg"] = frames
        sources[f"{session_id}/pose_diagnostics/pose_trace.ndjson"] = trace
        preview_status = "available"
    detector: dict[str, object] | None = None
    if detected_impact:
        detector = {
            "strike_frame_position": "101" if corrupt_detector else "102",
            "confirmation_frame_position": "103",
            "peak_amplitude": "0.9",
            "noise_floor": "0.1",
            "threshold": "0.5",
            "strike_boottime_ns": None,
            "strike_uncertainty_ns": None,
            "confirmation_boottime_ns": None,
            "confirmation_uncertainty_ns": None,
        }
    manifest: dict[str, object] = {
        "schema_version": 1,
        "session_kind": "standby_diagnostic",
        "session_id": session_id,
        "created_at_epoch_ms": "1786970096000",
        "source_node_id": node_id,
        "event": {
            "sequence": "1",
            "kind": "detected_impact" if detected_impact else "operator_tag",
            "marker_audio_frame_position": "102",
            "audio_clock_status": "audio_clock_unvalidated",
            "marker_boottime_ns": None,
            "marker_uncertainty_ns": None,
            "operator_received_boottime_ns": None if detected_impact else "123000000",
            "detector": detector,
        },
        "evidence": {
            "audio": {
                "path": "diagnostic_audio.wav",
                "content_type": "audio/wav",
                "bytes": str(len(wav)),
                "sample_rate_hz": 48_000,
                "first_frame_position": "100",
                "end_frame_position": "104",
                "marker_frame_position": "102",
                "sample_count": len(samples),
                "marker_sample_index": 2,
            },
            "preview_status": preview_status,
            "preview": preview,
        },
        "incident": {"path": "diagnostic_incident.json", "bytes": len(incident_bytes)},
    }
    sources[f"{session_id}/manifest.json"] = canonical_json(manifest)
    exported_files = [
        {"path": path, "bytes": len(contents), "sha256": sha256(contents)}
        for path, contents in sorted(sources.items())
    ]
    export = canonical_json(
        {
            "schema_version": 1,
            "session_id": session_id,
            "created_at_utc": "2026-08-17T13:00:00Z",
            "files": exported_files,
        }
    )
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as archive:
        write_zip_entry(archive, "diagnostic_export.json", export)
        for path, contents in sorted(sources.items()):
            write_zip_entry(archive, path, contents)
    return output.getvalue()


def pcm16_wav(samples: tuple[int, ...]) -> bytes:
    """Encode the canonical 48 kHz mono PCM16 shape used by Android diagnostics."""
    payload = b"".join(sample.to_bytes(2, "little", signed=True) for sample in samples)
    return (
        b"RIFF"
        + (36 + len(payload)).to_bytes(4, "little")
        + b"WAVEfmt "
        + (16).to_bytes(4, "little")
        + (1).to_bytes(2, "little")
        + (1).to_bytes(2, "little")
        + (48_000).to_bytes(4, "little")
        + (96_000).to_bytes(4, "little")
        + (2).to_bytes(2, "little")
        + (16).to_bytes(2, "little")
        + b"data"
        + len(payload).to_bytes(4, "little")
        + payload
    )


def write_zip_entry(archive: zipfile.ZipFile, path: str, contents: bytes) -> None:
    """Write one reproducibly timestamped fixture entry."""
    entry = zipfile.ZipInfo(path, date_time=(2026, 8, 17, 13, 0, 0))
    entry.compress_type = zipfile.ZIP_STORED
    archive.writestr(entry, contents)


def canonical_json(value: object) -> bytes:
    """Encode compact deterministic fixture JSON."""
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def read_index(path: Path) -> dict[str, object]:
    """Parse a collector index without introducing untyped json.loads results."""
    return collector.require_object(collector.parse_json(path.read_bytes(), "test index"), "index")


def index_objects(index: Mapping[str, object], name: str) -> list[dict[str, object]]:
    """Return a typed object array from a test index."""
    value = index.get(name)
    assert isinstance(value, list)
    return [collector.require_object(item, name) for item in cast("list[object]", value)]


def sha256(contents: bytes) -> str:
    """Return a lowercase fixture digest."""
    return hashlib.sha256(contents).hexdigest()


if __name__ == "__main__":
    unittest.main()
