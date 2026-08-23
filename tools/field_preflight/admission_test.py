"""Hermetic regression tests for the bounded Android field preflight."""
# Ruff's public-docstring rules add noise to unittest methods whose names are the specification.
# ruff: noqa: D101, D102, D107

from __future__ import annotations

import hashlib
import json
import os
import tempfile
import threading
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, cast
from unittest import mock

from tools.field_preflight import admission as admission_module
from tools.field_preflight.admission import (
    AdmissionError,
    CommandResult,
    ExpectedRole,
    Node,
    execute,
    parse_expected_role,
    parse_mdns_connect_services,
    parse_node,
    resolve_wireless_nodes,
    validate_expected_roles,
    validate_pair,
)

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence


ADB = Path("/runfiles/adb")
DOCTOR = Path("/runfiles/android_pair_doctor_current_apk")
NODES = (
    Node("10.168.168.111:37123", "http://10.168.168.111:8088"),
    Node("10.168.168.241:42491", "http://10.168.168.241:8088"),
)
EXPECTED_ROLES = (
    ExpectedRole("10.168.168.111", "face_on"),
    ExpectedRole("10.168.168.241", "down_the_line"),
)


def _host(node: Node) -> str:
    """Return the fixed host portion of a test node endpoint."""
    return node.serial.partition(":")[0]


def _lines(*values: str) -> str:
    return "\n".join(values)


class FakeRunner:
    """Production-shaped command boundary with programmable doctor outcomes."""

    def __init__(  # noqa: PLR0913
        self,
        doctor_outcomes: Sequence[bool],
        *,
        locked_serial: str | None = None,
        power_state: str = "Asleep",
        mdns_output: str = "",
        doctor_roles: dict[str, str] | None = None,
        doctor_checks: Sequence[Mapping[str, object]] | None = None,
        doctor_stdout: str = "doctor output\n",
        doctor_stderr: str = "",
        omit_apk_evidence: bool = False,
        omit_lan_evidence: bool = False,
    ) -> None:
        self.doctor_outcomes: list[bool] = list(doctor_outcomes)
        self.locked_serial: str | None = locked_serial
        self.power_state: str = power_state
        self.mdns_output: str = mdns_output
        self.doctor_roles: dict[str, str] = doctor_roles or {
            "10.168.168.111": "face_on",
            "10.168.168.241": "down_the_line",
        }
        self.doctor_checks: list[dict[str, object]] | None = (
            [dict(check) for check in doctor_checks] if doctor_checks is not None else None
        )
        self.doctor_stdout: str = doctor_stdout
        self.doctor_stderr: str = doctor_stderr
        self.omit_apk_evidence: bool = omit_apk_evidence
        self.omit_lan_evidence: bool = omit_lan_evidence
        self.commands: list[tuple[str, ...]] = []

    def __call__(self, arguments: Sequence[str]) -> CommandResult:  # noqa: C901, PLR0911
        command = tuple(arguments)
        self.commands.append(command)
        if command[0] == str(DOCTOR):
            outcome = self.doctor_outcomes.pop(0)
            output_path = Path(command[command.index("--json") + 1])
            node_values = [
                command[index + 1] for index, argument in enumerate(command) if argument == "--node"
            ]
            doctor_report: dict[str, object] = {
                "schema_version": 1,
                "report_type": "android_pair_preflight",
                "passed": outcome,
                "credentials_redacted": True,
                "nodes": [
                    {
                        "serial": value.partition("=")[0],
                        "role": self.doctor_roles.get(value.partition("=")[0].partition(":")[0]),
                    }
                    for value in node_values
                ],
            }
            if not self.omit_lan_evidence:
                parsed_nodes = [
                    (value.partition("=")[0], value.partition("=")[2]) for value in node_values
                ]
                doctor_report["lan_diagnostics"] = {
                    "schema_version": 1,
                    "nodes": [
                        {
                            "serial": serial,
                            "origin": origin,
                            "bssid": f"44:d9:e7:aa:bb:{index + 1:02x}",
                        }
                        for index, (serial, origin) in enumerate(parsed_nodes)
                    ],
                    "reachability_matrix": [
                        edge
                        for index, (_, origin) in enumerate(parsed_nodes)
                        for edge in (
                            {
                                "source": "field_host",
                                "target": origin,
                                "transport": "direct_http_clock",
                                "reachable": True,
                                "http_status": 200,
                            },
                            {
                                "source": origin,
                                "target": parsed_nodes[1 - index][1],
                                "transport": "adb_shell_wifi_icmp",
                                "reachable": True,
                                "http_status": None,
                            },
                        )
                    ],
                }
            if self.doctor_checks is not None:
                doctor_report["checks"] = self.doctor_checks
            if "--expected-apk" in command and not self.omit_apk_evidence:
                apk_path = Path(command[command.index("--expected-apk") + 1])
                digest = hashlib.sha256(apk_path.read_bytes()).hexdigest()
                doctor_report["expected_apk_sha256"] = digest
                for raw_node in cast("list[dict[str, object]]", doctor_report["nodes"]):
                    raw_node["installed_apk_sha256"] = digest
            output_path.write_text(
                json.dumps(doctor_report),
                encoding="utf-8",
            )
            return CommandResult(0 if outcome else 1, self.doctor_stdout, self.doctor_stderr)
        if command[1:] == ("mdns", "services"):
            return CommandResult(0, self.mdns_output)
        if command[1] == "connect":
            return CommandResult(0, f"connected to {command[2]}\n", "")
        serial = command[2]
        if command[-1] == "get-state":
            return CommandResult(0, "device\n", "")
        if command[-2:] == ("getprop", "sys.user.0.ce_available"):
            return CommandResult(0, "false\n" if serial == self.locked_serial else "true\n", "")
        if "am" in command:
            return CommandResult(0, "Starting: Intent\nStatus: ok\nComplete\n", "")
        if command[-3:] == ("input", "keyevent", "KEYCODE_SLEEP"):
            return CommandResult(0)
        if command[-2:] == ("dumpsys", "power"):
            return CommandResult(0, f"mWakefulness={self.power_state}\n")
        message = f"unexpected command: {command!r}"
        raise AssertionError(message)


class ConcurrentConnectRunner(FakeRunner):
    """Require both independent connect operations to overlap."""

    def __init__(self, doctor_outcomes: Sequence[bool]) -> None:
        super().__init__(doctor_outcomes)
        self.connect_barrier: threading.Barrier = threading.Barrier(2, timeout=5.0)

    def __call__(  # pyright: ignore[reportImplicitOverride]
        self, arguments: Sequence[str]
    ) -> CommandResult:
        if len(arguments) > 1 and arguments[1] == "connect":
            self.connect_barrier.wait()
        return super().__call__(arguments)


class AdmissionTest(unittest.TestCase):
    def test_node_parser_requires_matching_wireless_hosts(self) -> None:
        self.assertEqual(
            parse_node("10.0.0.4:37001=http://10.0.0.4:8088"),
            Node("10.0.0.4:37001", "http://10.0.0.4:8088"),
        )
        self.assertEqual(
            parse_node("10.0.0.4=http://10.0.0.4:8088"),
            Node("10.0.0.4", "http://10.0.0.4:8088", "mdns_pending"),
        )
        for malformed in (
            "USB123=http://10.0.0.4:8088",
            "10.0.0.4:37001=https://10.0.0.4:8088",
            "10.0.0.4:37001=http://10.0.0.5:8088",
            "10.0.0.4:99999=http://10.0.0.4:8088",
            "10.0.0.4:37001=http://token@10.0.0.4:8088",
        ):
            with self.subTest(malformed=malformed), self.assertRaises(ValueError):
                parse_node(malformed)

    def test_expected_role_parser_and_pair_binding_reject_ambiguity(self) -> None:
        self.assertEqual(parse_expected_role("10.168.168.111=face_on"), EXPECTED_ROLES[0])
        for malformed in (
            "10.168.168.111:37123=face_on",
            "10.168.168.111=atl",
            "bad host=face_on",
        ):
            with self.subTest(malformed=malformed), self.assertRaises(ValueError):
                parse_expected_role(malformed)
        validate_expected_roles(NODES, EXPECTED_ROLES)
        with self.assertRaisesRegex(ValueError, "exactly two"):
            validate_expected_roles(NODES, EXPECTED_ROLES[:1])
        with self.assertRaisesRegex(ValueError, "complementary"):
            validate_expected_roles(
                NODES,
                (
                    ExpectedRole("10.168.168.111", "face_on"),
                    ExpectedRole("10.168.168.241", "face_on"),
                ),
            )
        with self.assertRaisesRegex(ValueError, "exactly match"):
            validate_expected_roles(
                NODES,
                (
                    ExpectedRole("10.168.168.111", "face_on"),
                    ExpectedRole("10.168.168.242", "down_the_line"),
                ),
            )

    def test_pair_rejects_duplicate_endpoints_before_commands(self) -> None:
        with self.assertRaises(ValueError):
            validate_pair((NODES[0], NODES[0]))

    def test_mdns_parser_ignores_pairing_and_deduplicates_connect_advertisements(self) -> None:
        services = parse_mdns_connect_services(
            _lines(
                "List of discovered mdns services",
                "adb-pixel6 _adb-tls-pairing._tcp. 10.168.168.111:39001",
                "adb-pixel6._adb-tls-connect._tcp. 10.168.168.111:37123",
                "adb-pixel6-copy _adb-tls-connect._tcp 10.168.168.111:37123",
                "adb-pixel5a._adb-tls-connect._tcp. 10.168.168.241:42491",
            )
        )
        self.assertEqual(services["10.168.168.111"], ("10.168.168.111:37123",))
        self.assertEqual(services["10.168.168.241"], ("10.168.168.241:42491",))

    def test_host_only_nodes_resolve_current_ports_in_input_order(self) -> None:
        requested = tuple(parse_node(f"{_host(node)}={node.origin}") for node in NODES)
        runner = FakeRunner(
            (),
            mdns_output=_lines(
                "List of discovered mdns services",
                "adb-pixel5a _adb-tls-connect._tcp. 10.168.168.241:42491",
                "adb-pixel6 _adb-tls-connect._tcp. 10.168.168.111:37123",
            ),
        )

        resolved = resolve_wireless_nodes(ADB, requested, runner=runner)

        self.assertEqual([node.serial for node in resolved], [node.serial for node in NODES])
        self.assertTrue(all(node.adb_endpoint_source == "mdns" for node in resolved))
        self.assertEqual(runner.commands, [(str(ADB), "mdns", "services")])

    def test_host_only_resolution_fails_closed_on_ambiguous_service(self) -> None:
        requested = tuple(parse_node(f"{_host(node)}={node.origin}") for node in NODES)
        runner = FakeRunner(
            (),
            mdns_output=_lines(
                "adb-pixel6-a _adb-tls-connect._tcp. 10.168.168.111:37123",
                "adb-pixel6-b _adb-tls-connect._tcp. 10.168.168.111:37124",
                "adb-pixel5a _adb-tls-connect._tcp. 10.168.168.241:42491",
            ),
        )

        with self.assertRaisesRegex(AdmissionError, "expected exactly one"):
            resolve_wireless_nodes(ADB, requested, runner=runner)

    def test_host_only_resolution_fails_closed_when_service_is_missing(self) -> None:
        requested = tuple(parse_node(f"{_host(node)}={node.origin}") for node in NODES)
        runner = FakeRunner(
            (),
            mdns_output="adb-pixel5a _adb-tls-connect._tcp. 10.168.168.241:42491\n",
        )

        with self.assertRaisesRegex(AdmissionError, "found none"):
            resolve_wireless_nodes(ADB, requested, runner=runner)

    def test_resolution_failure_is_retained_before_any_phone_operation(self) -> None:
        requested = tuple(parse_node(f"{_host(node)}={node.origin}") for node in NODES)
        runner = FakeRunner((), mdns_output="")
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "field"
            with self.assertRaisesRegex(AdmissionError, "found none"):
                execute(
                    ADB,
                    DOCTOR,
                    requested,
                    evidence,
                    expected_roles=EXPECTED_ROLES,
                    runner=runner,
                )
            retained = cast(
                "dict[str, object]",
                json.loads((evidence / "report.json").read_text(encoding="utf-8")),
            )

        self.assertFalse(retained["passed"])
        self.assertIn("found none", cast("str", retained["preparation_failure"]))
        self.assertEqual(retained["nodes"], [])

    def test_host_only_preflight_passes_resolved_endpoints_to_strict_doctor(self) -> None:
        requested = tuple(parse_node(f"{_host(node)}={node.origin}") for node in NODES)
        runner = FakeRunner(
            (True,),
            mdns_output=_lines(
                "adb-pixel6 _adb-tls-connect._tcp. 10.168.168.111:37123",
                "adb-pixel5a _adb-tls-connect._tcp. 10.168.168.241:42491",
            ),
        )
        with tempfile.TemporaryDirectory() as temporary:
            report = execute(
                ADB,
                DOCTOR,
                requested,
                Path(temporary) / "field",
                expected_roles=EXPECTED_ROLES,
                runner=runner,
            )

        self.assertTrue(report["passed"])
        self.assertTrue(all(node["adb_endpoint_source"] == "mdns" for node in report["nodes"]))
        self.assertTrue(report["doctor_attempts"][0]["lan_diagnostics_passed"])
        diagnostics = cast("dict[str, object]", report["lan_diagnostics"])
        self.assertEqual(diagnostics["schema_version"], 1)
        self.assertEqual(len(cast("list[object]", diagnostics["reachability_matrix"])), 4)
        self.assertEqual(
            [node["bssid"] for node in cast("list[dict[str, object]]", diagnostics["nodes"])],
            ["44:d9:e7:aa:bb:01", "44:d9:e7:aa:bb:02"],
        )
        doctor_command = next(command for command in runner.commands if command[0] == str(DOCTOR))
        for node in NODES:
            self.assertIn(f"{node.serial}={node.origin}", doctor_command)

    def test_launches_both_and_retries_until_peer_monitoring_recovers(self) -> None:
        runner = ConcurrentConnectRunner((False, True))
        sleeps: list[float] = []
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "field"
            report = execute(
                ADB,
                DOCTOR,
                NODES,
                evidence,
                expected_roles=EXPECTED_ROLES,
                launch_after_unlock=True,
                sleep_screen_after_launch=True,
                require_monitoring=True,
                maximum_attempts=3,
                retry_seconds=0.25,
                runner=runner,
                sleeper=sleeps.append,
            )
            decoded = cast(
                "object",
                json.loads((evidence / "report.json").read_text(encoding="utf-8")),
            )
            self.assertIsInstance(decoded, dict)
            retained = cast("dict[str, object]", decoded)

        self.assertTrue(report["passed"])
        self.assertTrue(retained["passed"])
        self.assertEqual(len(report["doctor_attempts"]), 2)
        self.assertEqual(sleeps, [0.25])
        self.assertEqual(
            [node["serial"] for node in report["nodes"]],
            [node.serial for node in NODES],
        )
        launch_commands = [command for command in runner.commands if "am" in command]
        self.assertEqual(len(launch_commands), 2)
        sleep_commands = [command for command in runner.commands if "KEYCODE_SLEEP" in command]
        self.assertEqual(len(sleep_commands), 2)
        self.assertTrue(all(node["screen_noninteractive"] for node in report["nodes"]))
        doctor_commands = [command for command in runner.commands if command[0] == str(DOCTOR)]
        self.assertEqual(len(doctor_commands), 2)
        self.assertTrue(all("--require-wireless-adb" in command for command in doctor_commands))
        self.assertTrue(all("--require-monitoring" in command for command in doctor_commands))

    def test_retains_only_bounded_credential_redacted_doctor_failures(self) -> None:
        checks = [
            {"name": "node.1.exact_apk", "message": " installed APK\n differs ", "passed": False},
            {"name": "node.1.resources", "message": "healthy", "passed": True},
            {"name": 42, "message": "malformed", "passed": False},
        ]
        runner = FakeRunner((False,), doctor_checks=checks)
        with tempfile.TemporaryDirectory() as temporary:
            report = execute(
                ADB,
                DOCTOR,
                NODES,
                Path(temporary) / "field",
                expected_roles=EXPECTED_ROLES,
                maximum_attempts=1,
                runner=runner,
            )

        self.assertFalse(report["passed"])
        self.assertEqual(
            report["doctor_attempts"][0]["failures"],
            [{"name": "node.1.exact_apk", "message": "installed APK differs"}],
        )

    def test_redacts_child_logs_and_report_even_when_child_claims_redaction(self) -> None:
        secret = "A" * 32
        runner = FakeRunner(
            (False,),
            doctor_checks=[
                {
                    "name": "node.1.peer",
                    "message": f"Authorization: Bearer {secret}",
                    "passed": False,
                }
            ],
            doctor_stdout=f"request used Bearer {secret}\n",
            doctor_stderr=f'{{"control_token":"{secret}"}}\n',
        )
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "field"
            report = execute(
                ADB,
                DOCTOR,
                NODES,
                evidence,
                expected_roles=EXPECTED_ROLES,
                maximum_attempts=1,
                runner=runner,
            )
            retained = "\n".join(path.read_text(encoding="utf-8") for path in evidence.iterdir())

        self.assertNotIn(secret, retained)
        self.assertIn("[REDACTED]", retained)
        self.assertNotIn(secret, json.dumps(report))

    def test_expected_apk_requires_reproducible_child_identity_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            apk = root / "expected.apk"
            apk.write_bytes(b"exact APK")
            passed = execute(
                ADB,
                DOCTOR,
                NODES,
                root / "pass",
                expected_roles=EXPECTED_ROLES,
                expected_apk=apk,
                maximum_attempts=1,
                runner=FakeRunner((True,)),
            )
            missing = execute(
                ADB,
                DOCTOR,
                NODES,
                root / "missing",
                expected_roles=EXPECTED_ROLES,
                expected_apk=apk,
                maximum_attempts=1,
                runner=FakeRunner((True,), omit_apk_evidence=True),
            )

        expected_digest = hashlib.sha256(b"exact APK").hexdigest()
        self.assertTrue(passed["passed"])
        self.assertEqual(passed["expected_apk_sha256"], expected_digest)
        self.assertTrue(passed["doctor_attempts"][0]["exact_apk_evidence_passed"])
        self.assertFalse(missing["passed"])
        self.assertFalse(missing["doctor_attempts"][0]["exact_apk_evidence_passed"])

    def test_field_report_rejects_missing_child_lan_matrix(self) -> None:
        """A nominal child exit cannot hide missing direct-LAN proof from field admission."""
        with tempfile.TemporaryDirectory() as temporary:
            report = execute(
                ADB,
                DOCTOR,
                NODES,
                Path(temporary) / "field",
                expected_roles=EXPECTED_ROLES,
                maximum_attempts=1,
                runner=FakeRunner((True,), omit_lan_evidence=True),
            )

        self.assertFalse(report["passed"])
        self.assertIsNone(report["lan_diagnostics"])
        self.assertFalse(report["doctor_attempts"][0]["lan_diagnostics_passed"])

    def test_locked_phone_fails_without_launch_or_doctor_but_checks_its_peer(self) -> None:
        runner = FakeRunner((), locked_serial=NODES[0].serial)
        with tempfile.TemporaryDirectory() as temporary:
            report = execute(
                ADB,
                DOCTOR,
                NODES,
                Path(temporary) / "field",
                expected_roles=EXPECTED_ROLES,
                launch_after_unlock=True,
                runner=runner,
            )

        self.assertFalse(report["passed"])
        self.assertEqual(len(report["nodes"]), 2)
        self.assertFalse(report["nodes"][0]["user_unlocked"])
        self.assertTrue(report["nodes"][1]["passed"])
        self.assertFalse(any(command[0] == str(DOCTOR) for command in runner.commands))
        locked_launches = [
            command for command in runner.commands if "am" in command and NODES[0].serial in command
        ]
        self.assertEqual(locked_launches, [])

    def test_read_only_mode_does_not_launch_and_still_runs_strict_doctor(self) -> None:
        runner = FakeRunner((True,))
        with tempfile.TemporaryDirectory() as temporary:
            report = execute(
                ADB,
                DOCTOR,
                NODES,
                Path(temporary) / "field",
                expected_roles=EXPECTED_ROLES,
                runner=runner,
            )

        self.assertTrue(report["passed"])
        self.assertEqual(report["launch_mode"], "none")
        self.assertFalse(any("am" in command for command in runner.commands))

    def test_awake_power_state_fails_before_strict_doctor(self) -> None:
        runner = FakeRunner((), power_state="Awake")
        with tempfile.TemporaryDirectory() as temporary:
            report = execute(
                ADB,
                DOCTOR,
                NODES,
                Path(temporary) / "field",
                expected_roles=EXPECTED_ROLES,
                launch_after_unlock=True,
                sleep_screen_after_launch=True,
                runner=runner,
            )

        self.assertFalse(report["passed"])
        self.assertTrue(all(node["screen_sleep_succeeded"] for node in report["nodes"]))
        self.assertTrue(all(not node["screen_noninteractive"] for node in report["nodes"]))
        self.assertFalse(any(command[0] == str(DOCTOR) for command in runner.commands))

    def test_screen_sleep_requires_explicit_foreground_launch(self) -> None:
        runner = FakeRunner(())
        with tempfile.TemporaryDirectory() as temporary, self.assertRaises(ValueError):
            execute(
                ADB,
                DOCTOR,
                NODES,
                Path(temporary) / "field",
                expected_roles=EXPECTED_ROLES,
                sleep_screen_after_launch=True,
                runner=runner,
            )
        self.assertEqual(runner.commands, [])

    def test_refuses_to_replace_prior_evidence(self) -> None:
        runner = FakeRunner((True,))
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "field"
            evidence.mkdir()
            with self.assertRaises(AdmissionError):
                execute(
                    ADB,
                    DOCTOR,
                    NODES,
                    evidence,
                    expected_roles=EXPECTED_ROLES,
                    runner=runner,
                )
        self.assertEqual(runner.commands, [])

    def test_unexpected_preparation_interruption_leaves_failing_checkpoint(self) -> None:
        def interrupted(_arguments: Sequence[str]) -> CommandResult:
            message = "simulated interruption"
            raise RuntimeError(message)

        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "field"
            with self.assertRaisesRegex(RuntimeError, "simulated interruption"):
                execute(
                    ADB,
                    DOCTOR,
                    NODES,
                    evidence,
                    expected_roles=EXPECTED_ROLES,
                    runner=interrupted,
                )
            checkpoint = cast(
                "dict[str, object]",
                json.loads((evidence / "report.json").read_text(encoding="utf-8")),
            )

        self.assertFalse(checkpoint["passed"])
        self.assertEqual(checkpoint["nodes"], [])
        self.assertEqual(checkpoint["doctor_attempts"], [])
        self.assertEqual(checkpoint["preparation_failure"], "simulated interruption")

    def test_atomic_checkpoint_is_fsynced_and_ignores_stale_predictable_temp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "report.json"
            stale = root / "report.json.tmp"
            stale.write_text("stale partial\n", encoding="utf-8")
            with mock.patch.object(
                os,
                "fsync",
                wraps=os.fsync,
            ) as synchronize:
                admission_module._write_text_atomic(  # pyright: ignore[reportPrivateUsage]  # noqa: SLF001
                    output, "complete\n"
                )

            self.assertEqual(output.read_text(encoding="utf-8"), "complete\n")
            self.assertEqual(stale.read_text(encoding="utf-8"), "stale partial\n")
            self.assertGreaterEqual(synchronize.call_count, 2)

    def test_atomic_checkpoint_cleanup_preserves_prior_file_on_replace_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "report.json"
            output.write_text("prior\n", encoding="utf-8")
            with (
                mock.patch.object(os, "replace", side_effect=OSError("interrupted")),
                self.assertRaisesRegex(OSError, "interrupted"),
            ):
                admission_module._write_text_atomic(  # pyright: ignore[reportPrivateUsage]  # noqa: SLF001
                    output, "replacement\n"
                )

            self.assertEqual(output.read_text(encoding="utf-8"), "prior\n")
            self.assertEqual(sorted(path.name for path in root.iterdir()), ["report.json"])

    def test_swapped_camera_roles_fail_even_when_strict_doctor_otherwise_passes(self) -> None:
        runner = FakeRunner(
            (True,),
            doctor_roles={
                "10.168.168.111": "down_the_line",
                "10.168.168.241": "face_on",
            },
        )
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "field"
            report = execute(
                ADB,
                DOCTOR,
                NODES,
                evidence,
                expected_roles=EXPECTED_ROLES,
                maximum_attempts=1,
                runner=runner,
            )
            retained = cast(
                "dict[str, object]",
                json.loads((evidence / "report.json").read_text(encoding="utf-8")),
            )

        self.assertFalse(report["passed"])
        self.assertFalse(report["doctor_attempts"][0]["role_association_passed"])
        self.assertTrue(all(not value["passed"] for value in report["role_associations"]))
        self.assertEqual(retained["role_associations"], report["role_associations"])


if __name__ == "__main__":
    unittest.main()
