"""Hermetic fake-ADB/fake-HTTP coverage for the hitting-session sidecar."""
# Ruff's public-docstring rules add noise to unittest methods and intentionally small fakes.
# ruff: noqa: D101, D102, D107

from __future__ import annotations

import datetime
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, cast, final

from tools.session_sidecar.collector import (
    PACKAGE_NAME,
    CommandResult,
    Configuration,
    HttpResult,
    LogcatResult,
    Node,
    SidecarError,
    collect,
    parse_node,
)

if TYPE_CHECKING:
    from collections.abc import Sequence


TOKEN_ONE = "A" * 32
TOKEN_TWO = "B" * 32
NODES = (
    Node("pixel-6:37123", "http://192.0.2.10:8088"),
    Node("pixel-5a:42491", "http://192.0.2.11:8088"),
)


@final
class FakeClock:
    def __init__(self) -> None:
        self.elapsed: float = 0.0
        self.origin: datetime.datetime = datetime.datetime(2026, 8, 24, 1, 0, tzinfo=datetime.UTC)

    def monotonic(self) -> float:
        return self.elapsed

    def utc_now(self) -> datetime.datetime:
        return self.origin + datetime.timedelta(seconds=self.elapsed)

    def sleep(self, seconds: float) -> None:
        self.elapsed += seconds


@final
class FakeLogcat:
    def __init__(self, contents: bytes, maximum_bytes: int) -> None:
        self.contents: bytes = contents
        self.maximum_bytes: int = maximum_bytes

    def stop(self) -> LogcatResult:
        return LogcatResult(
            contents=self.contents[-self.maximum_bytes :],
            truncated_prefix=len(self.contents) > self.maximum_bytes,
        )


class FakeBackend:
    """Records every requested operation and supplies deterministic fake evidence."""

    def __init__(self) -> None:
        self.adb_commands: list[tuple[str, tuple[str, ...]]] = []
        self.http_requests: list[tuple[str, str, str]] = []
        self.logcat_starts: list[str] = []

    def adb(self, serial: str, arguments: Sequence[str], maximum_bytes: int) -> CommandResult:
        del maximum_bytes
        command = tuple(arguments)
        self.adb_commands.append((serial, command))
        if command == (
            "exec-out",
            "run-as",
            PACKAGE_NAME,
            "cat",
            "shared_prefs/node_configuration.xml",
        ):
            token = TOKEN_ONE if serial == NODES[0].serial else TOKEN_TWO
            return CommandResult(0, f'<string name="control_token">{token}</string>'.encode())
        label = " ".join(command)
        return CommandResult(0, f"{serial} {label} {TOKEN_ONE}\n".encode())

    def http_get(self, origin: str, path: str, token: str) -> HttpResult:
        self.http_requests.append((origin, path, token))
        body = {
            "schema_version": 1,
            "origin": origin,
            "path": path,
            "sample": len(self.http_requests),
        }
        return HttpResult(200, json.dumps(body, separators=(",", ":")).encode())

    def start_logcat(self, serial: str, maximum_bytes: int) -> FakeLogcat:
        self.logcat_starts.append(serial)
        return FakeLogcat((f"old\nnew bearer {TOKEN_TWO}\n" * 8).encode(), maximum_bytes)


class SessionSidecarTest(unittest.TestCase):
    def test_collects_two_nodes_with_only_read_only_operations_and_no_credentials(self) -> None:
        backend = FakeBackend()
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "sidecar"
            report = collect(
                Configuration(
                    adb=Path("/fake/adb"),
                    nodes=NODES,
                    output_directory=output,
                    duration_seconds=2.1,
                    interval_seconds=1,
                    maximum_logcat_bytes=64,
                    maximum_artifact_bytes=16 * 1024 * 1024,
                ),
                backend=backend,
                utc_now=clock.utc_now,
                monotonic=clock.monotonic,
                sleep=clock.sleep,
            )
            self.assertTrue(report["passed"])
            self.assertEqual(report["snapshot_count"], 3)
            snapshots = cast(
                "list[dict[str, object]]",
                [
                    json.loads(line)
                    for line in (output / "status-snapshots.jsonl").read_text().splitlines()
                ],
            )
            self.assertEqual([item["sequence"] for item in snapshots], [0, 1, 2])
            self.assertEqual(len(backend.http_requests), 12)
            inventory = cast(
                "dict[str, object]", json.loads((output / "inventory.json").read_text())
            )
            self.assertTrue(inventory["token_free"])
            entries = cast("list[dict[str, object]]", inventory["files"])
            self.assertNotIn("inventory.json", {item["path"] for item in entries})
            for item in entries:
                path = output / cast("str", item["path"])
                self.assertEqual(item["bytes"], path.stat().st_size)
                self.assertEqual(item["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            combined = b"".join(path.read_bytes() for path in output.rglob("*") if path.is_file())
            self.assertNotIn(TOKEN_ONE.encode(), combined)
            self.assertNotIn(TOKEN_TWO.encode(), combined)
            self.assertIn(b"[REDACTED", combined)

        commands = [" ".join(arguments) for _, arguments in backend.adb_commands]
        self.assertTrue(all(" arm" not in command for command in commands))
        self.assertTrue(all(" stop" not in command for command in commands))
        self.assertTrue(
            all(" rm" not in command and "delete" not in command for command in commands)
        )
        self.assertEqual(
            {request[1] for request in backend.http_requests},
            {
                "/api/v1/capture/status",
                "/api/v1/field-recording/status",
            },
        )
        self.assertEqual(backend.logcat_starts, [NODES[0].serial, NODES[1].serial])

    def test_credential_bearing_http_body_is_omitted_but_collection_continues(self) -> None:
        class LeakyBackend(FakeBackend):
            def http_get(  # pyright: ignore[reportImplicitOverride]
                self, origin: str, path: str, token: str
            ) -> HttpResult:
                self.http_requests.append((origin, path, token))
                return HttpResult(200, json.dumps({"leak": token}).encode())

        backend = LeakyBackend()
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "sidecar"
            report = collect(
                Configuration(
                    adb=Path("/fake/adb"),
                    nodes=NODES,
                    output_directory=output,
                    duration_seconds=0.5,
                    maximum_logcat_bytes=64,
                    maximum_artifact_bytes=16 * 1024 * 1024,
                ),
                backend=backend,
                utc_now=clock.utc_now,
                monotonic=clock.monotonic,
                sleep=clock.sleep,
            )
            self.assertEqual(report["snapshot_count"], 1)
            self.assertEqual(report["endpoint_failure_count"], 4)
            snapshot = cast(
                "dict[str, object]",
                json.loads((output / "status-snapshots.jsonl").read_text().splitlines()[0]),
            )
            nodes = cast("list[dict[str, object]]", snapshot["nodes"])
            endpoint = cast("dict[str, object]", nodes[0]["capture_status"])
            self.assertIsNone(endpoint["body"])
            self.assertIn("omitted", cast("str", endpoint["error"]))
            self.assertNotIn(TOKEN_ONE, (output / "status-snapshots.jsonl").read_text())

    def test_http_backend_exception_is_retained_and_inventory_is_finalized(self) -> None:
        class ExplodingBackend(FakeBackend):
            def http_get(  # pyright: ignore[reportImplicitOverride]
                self, origin: str, path: str, token: str
            ) -> HttpResult:
                del origin, path, token
                message = "synthetic HTTP transport failure"
                raise RuntimeError(message)

        clock = FakeClock()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "sidecar"
            report = collect(
                Configuration(
                    adb=Path("/fake/adb"),
                    nodes=NODES,
                    output_directory=output,
                    duration_seconds=0.5,
                    maximum_logcat_bytes=64,
                    maximum_artifact_bytes=16 * 1024 * 1024,
                ),
                backend=ExplodingBackend(),
                utc_now=clock.utc_now,
                monotonic=clock.monotonic,
                sleep=clock.sleep,
            )
            self.assertEqual(report["snapshot_count"], 1)
            self.assertEqual(report["endpoint_failure_count"], 4)
            self.assertTrue((output / "inventory.json").is_file())
            retained = (output / "status-snapshots.jsonl").read_text()
            self.assertIn("synthetic HTTP transport failure", retained)

    def test_parser_and_configuration_reject_credential_or_unbounded_inputs(self) -> None:
        self.assertEqual(
            parse_node("pixel=http://192.0.2.1:8088"),
            Node("pixel", "http://192.0.2.1:8088"),
        )
        for malformed in (
            "pixel=http://token@192.0.2.1:8088",
            "pixel=http://127.0.0.1:8088",
            "pixel=http://192.0.2.1:8088/path",
            "pixel=http://192.0.2.1:8088?token=x",
        ):
            with self.assertRaises(ValueError, msg=malformed):
                parse_node(malformed)
        with tempfile.TemporaryDirectory() as temporary, self.assertRaises(ValueError):
            Configuration(
                adb=Path("adb"),
                nodes=NODES,
                output_directory=Path(temporary) / "out",
                duration_seconds=43201,
            )

    def test_existing_output_is_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "sidecar"
            output.mkdir()
            marker = output / "operator-note.txt"
            marker.write_text("keep")
            with self.assertRaises(SidecarError):
                collect(
                    Configuration(
                        adb=Path("/fake/adb"),
                        nodes=NODES,
                        output_directory=output,
                        duration_seconds=1,
                    ),
                    backend=FakeBackend(),
                )
            self.assertEqual(marker.read_text(), "keep")


if __name__ == "__main__":
    unittest.main()
