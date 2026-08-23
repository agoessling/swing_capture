"""Tests for the bounded Bazel iteration-profile session runner."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from tools import bazel_iteration_profile as profile

if TYPE_CHECKING:
    from collections.abc import Sequence


INCREMENTAL_CALL_NUMBER = 3


class BazelIterationProfileTest(unittest.TestCase):
    """Verify command policy and cleanup without starting Bazel."""

    def test_command_enforces_resource_and_evidence_policy(self) -> None:
        """Place caller arguments before bounds which they cannot override."""
        policy = profile.ResourcePolicy(
            jobs=3,
            memory_mib=6144,
            server_heap_mib=2048,
            total_worker_memory_mib=3072,
        )
        output_base = Path("/profile-output")
        evidence = profile.ProfilePaths(
            profile=Path("/evidence/cold.profile.gz"),
            bep=Path("/evidence/cold.bep.json"),
        )

        command = profile.bazel_profile_command(
            "bazel",
            output_base,
            evidence,
            ["test", "--config=android_inner", "//android/app:all"],
            policy,
        )

        self.assertEqual("bazel", command[0])
        self.assertIn("--output_base=/profile-output", command)
        self.assertIn("--max_idle_secs=5", command)
        self.assertIn("--host_jvm_args=-Xmx2048m", command)
        self.assertIn("--jobs=3", command)
        self.assertIn("--local_resources=memory=6144", command)
        self.assertIn("--worker_max_instances=3", command)
        self.assertIn(
            "--experimental_total_worker_memory_limit_mb=3072",
            command,
        )
        self.assertIn("--profile=/evidence/cold.profile.gz", command)
        self.assertIn("--build_event_json_file=/evidence/cold.bep.json", command)

    def test_caller_cannot_override_managed_options(self) -> None:
        """Reject options which could silently remove the memory safeguards."""
        policy = profile.ResourcePolicy()
        evidence = profile.ProfilePaths(Path("cold.profile.gz"), Path("cold.bep.json"))
        for option in (
            "--jobs=40",
            "--local_resources=memory=HOST_RAM",
            "--worker_max_instances=20",
            "--profile=elsewhere",
            "--build_event_json_file=elsewhere",
            "--experimental_total_worker_memory_limit_mb=99999",
        ):
            with self.subTest(option=option), self.assertRaises(ValueError):
                profile.bazel_profile_command(
                    "bazel",
                    Path("output"),
                    evidence,
                    ["test", "//...", option],
                    policy,
                )

    def test_complete_session_reuses_one_server_and_restores_source(self) -> None:
        """Run all cache states before shutting down and leave source byte-identical."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "MODULE.bazel").write_text('module(name = "test")\n', encoding="utf-8")
            source = root / "Example.java"
            original = b"final class Example {}\n"
            source.write_bytes(original)
            output_base = root / "output-base"
            evidence = root / "evidence"
            calls: list[list[str]] = []
            observed_sources: list[bytes] = []

            def run(command: Sequence[str], _root: Path) -> int:
                calls.append(list(command))
                observed_sources.append(source.read_bytes())
                output_base.mkdir(exist_ok=True)
                return 0

            profile.run_profile_session(
                profile.ProfileSession(
                    root=root,
                    bazel="bazel",
                    output_base=output_base,
                    evidence_directory=evidence,
                    name="android_unit",
                    incremental_source=source,
                    bazel_arguments=["test", "//android/app:all"],
                    policy=profile.ResourcePolicy(),
                ),
                run,
            )

            self.assertEqual(4, len(calls))
            self.assertEqual(
                ["test", "test", "test", "shutdown"],
                [call[4] for call in calls],
            )
            self.assertEqual(original, observed_sources[0])
            self.assertEqual(original, observed_sources[1])
            self.assertIn(
                b"final class BazelIterationProfileTemporaryMarker_",
                observed_sources[2],
            )
            self.assertNotIn(b"temporary incremental marker", observed_sources[2])
            self.assertEqual(original, observed_sources[3])
            self.assertEqual(original, source.read_bytes())
            self.assertEqual(
                [
                    evidence / "android_unit_cold.profile.gz",
                    evidence / "android_unit_warm.profile.gz",
                    evidence / "android_unit_incremental_java_bytecode.profile.gz",
                ],
                [Path(call[-3].removeprefix("--profile=")) for call in calls[:3]],
            )

    def test_failed_stage_still_restores_source_and_shuts_down(self) -> None:
        """Treat failure evidence as durable while guaranteeing server cleanup."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Example.java"
            original = b"final class Example {}\n"
            source.write_bytes(original)
            output_base = root / "output-base"
            calls: list[list[str]] = []

            def run(command: Sequence[str], _root: Path) -> int:
                calls.append(list(command))
                output_base.mkdir(exist_ok=True)
                if command[4] == "test" and len(calls) == INCREMENTAL_CALL_NUMBER:
                    return 7
                return 0

            with self.assertRaises(profile.ProfileCommandError) as raised:
                profile.run_profile_session(
                    profile.ProfileSession(
                        root=root,
                        bazel="bazel",
                        output_base=output_base,
                        evidence_directory=root / "evidence",
                        name="full",
                        incremental_source=source,
                        bazel_arguments=["test", "//..."],
                        policy=profile.ResourcePolicy(),
                    ),
                    run,
                )

            self.assertEqual(
                profile.JAVA_BYTECODE_STAGE,
                raised.exception.stage,
            )
            self.assertEqual(7, raised.exception.exit_code)
            self.assertEqual("shutdown", calls[-1][4])
            self.assertEqual(original, source.read_bytes())

    def test_existing_output_base_is_not_misreported_as_cold(self) -> None:
        """Refuse stale cache state before starting any client process."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Example.java"
            source.write_text("final class Example {}\n", encoding="utf-8")
            output_base = root / "output-base"
            output_base.mkdir()
            calls: list[list[str]] = []

            def run(command: Sequence[str], _root: Path) -> int:
                calls.append(list(command))
                return 0

            with self.assertRaises(FileExistsError):
                profile.run_profile_session(
                    profile.ProfileSession(
                        root=root,
                        bazel="bazel",
                        output_base=output_base,
                        evidence_directory=root / "evidence",
                        name="full",
                        incremental_source=source,
                        bazel_arguments=["test", "//..."],
                        policy=profile.ResourcePolicy(),
                    ),
                    run,
                )
            self.assertEqual([], calls)

    def test_concurrent_source_change_is_never_overwritten(self) -> None:
        """Preserve a concurrent edit instead of restoring stale source bytes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Example.java"
            source.write_text("final class Example {}\n", encoding="utf-8")
            with (
                self.assertRaisesRegex(RuntimeError, "changed concurrently"),
                profile.temporary_incremental_comment(root, source),
            ):
                source.write_text("concurrent edit\n", encoding="utf-8")
            self.assertEqual("concurrent edit\n", source.read_text(encoding="utf-8"))

    def test_java_bytecode_change_never_overwrites_concurrent_edit(self) -> None:
        """Apply the exact-restore guard to the bytecode-changing Java marker."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Example.java"
            source.write_text("final class Example {}\n", encoding="utf-8")
            with (
                self.assertRaisesRegex(RuntimeError, "changed concurrently"),
                profile.temporary_incremental_java_bytecode(root, source),
            ):
                marker_source = source.read_text(encoding="utf-8")
                self.assertIn("BazelIterationProfileTemporaryMarker_", marker_source)
                source.write_text("concurrent Java edit\n", encoding="utf-8")
            self.assertEqual(
                "concurrent Java edit\n",
                source.read_text(encoding="utf-8"),
            )

    def test_non_java_incremental_change_is_labeled_source_only(self) -> None:
        """Do not imply that a comment-only native edit changes link outputs."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "example.cc"
            original = b"int Answer() { return 42; }\n"
            source.write_bytes(original)

            self.assertEqual(profile.SOURCE_ONLY_STAGE, profile.incremental_stage(source))
            with profile.temporary_incremental_change(root, source):
                self.assertIn(b"temporary incremental marker", source.read_bytes())
            self.assertEqual(original, source.read_bytes())

    def test_java_metadata_sources_are_rejected_before_profiling(self) -> None:
        """Avoid appending a top-level class where Java grammar forbids one."""
        for name in ("module-info.java", "package-info.java"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                profile.incremental_stage(Path(name))


if __name__ == "__main__":
    unittest.main()
