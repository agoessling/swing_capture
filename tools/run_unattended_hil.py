"""Operational supervision for the Daheng hardware-in-the-loop tests."""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import datetime
import fcntl
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import TYPE_CHECKING, cast, final

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping, Sequence
    from types import FrameType
    from typing import IO, TypeAlias

    SignalHandler: TypeAlias = signal.Handlers | Callable[[int, FrameType | None], object]


@dataclasses.dataclass(frozen=True)
class Stage:
    """Describe one supported HIL qualification stage."""

    duration_seconds: int
    test_target: str
    test_name: str


STAGES: dict[str, Stage] = {
    "smoke": Stage(
        duration_seconds=15,
        test_target="//capture/daheng:dual_camera_smoke_hil_test",
        test_name="dual_camera_smoke_hil_test",
    ),
    "qualify": Stage(
        duration_seconds=300,
        test_target="//capture/daheng:dual_camera_qualify_hil_test",
        test_name="dual_camera_qualify_hil_test",
    ),
    "soak": Stage(
        duration_seconds=1800,
        test_target="//capture/daheng:dual_camera_soak_hil_test",
        test_name="dual_camera_soak_hil_test",
    ),
}

ACTIVE_STATES = frozenset(("building", "running", "isolating"))
SERIAL_PATTERN = re.compile(r"^USB:.* serial=([^ ]+)", re.MULTILINE)


def utc_now() -> datetime.datetime:
    """Return a stable, second-resolution UTC timestamp."""
    return datetime.datetime.now(datetime.UTC).replace(microsecond=0)


def format_time(value: datetime.datetime) -> str:
    """Format a timestamp as UTC ISO-8601 using the conventional Z suffix."""
    return value.isoformat().replace("+00:00", "Z")


def positive_integer_setting(environment: Mapping[str, str], name: str, default: int) -> int:
    """Read and validate a positive integer environment setting."""
    value = environment.get(name, str(default))
    if not value.isdigit() or int(value) <= 0:
        message = f"{name} must be a positive integer"
        raise ValueError(message)
    return int(value)


def repository_root(environment: Mapping[str, str], current_directory: Path) -> Path:
    """Locate the Bazel workspace used to launch the unattended run."""
    workspace = environment.get("BUILD_WORKSPACE_DIRECTORY")
    if workspace:
        return Path(workspace).resolve()

    candidate = current_directory.resolve()
    for directory in (candidate, *candidate.parents):
        if (directory / "MODULE.bazel").is_file():
            return directory
    invocation = "'bazel run //tools:run_unattended_hil -- <stage>'"
    message = f"cannot locate the repository; invoke with {invocation}"
    raise RuntimeError(message)


def atomic_copy(source: Path, destination: Path) -> None:
    """Copy a file and atomically publish it at the destination."""
    temporary = destination.with_name(f"{destination.name}.tmp.{os.getpid()}")
    shutil.copyfile(source, temporary)
    temporary.replace(destination)


def atomic_write_json(destination: Path, value: Mapping[str, object]) -> None:
    """Durably write a JSON object and atomically publish it."""
    temporary = destination.with_name(f"{destination.name}.tmp.{os.getpid()}")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(destination)


@dataclasses.dataclass(frozen=True)
class RunPaths:
    """Collect durable artifact paths for one unattended invocation."""

    run_directory: Path
    report: Path
    status: Path
    log: Path
    latest_directory: Path

    @classmethod
    def create(
        cls,
        root: Path,
        stage_name: str,
        now: datetime.datetime,
        process_id: int,
    ) -> RunPaths:
        """Create the per-run and latest-result artifact directories."""
        run_id = f"{now.strftime('%Y%m%dT%H%M%SZ')}-{stage_name}-{process_id}"
        run_directory = root / "artifacts" / "hil" / "runs" / run_id
        latest_directory = root / "artifacts" / "hil" / "latest"
        run_directory.mkdir(parents=True)
        latest_directory.mkdir(parents=True, exist_ok=True)
        return cls(
            run_directory=run_directory,
            report=run_directory / "report.json",
            status=run_directory / "status.json",
            log=run_directory / "run.log",
            latest_directory=latest_directory,
        )


@final
class StatusWriter:
    """Publish durable status snapshots for an unattended invocation."""

    def __init__(
        self,
        paths: RunPaths,
        stage_name: str,
        stage: Stage,
        watchdog_grace_seconds: int,
        started_at: datetime.datetime,
    ) -> None:
        """Initialize a writer for one stage invocation."""
        self._paths = paths
        self._stage_name = stage_name
        self._stage = stage
        self._watchdog_grace_seconds = watchdog_grace_seconds
        self._started_at = started_at

    def write(
        self,
        state: str,
        *,
        exit_code: int | None = None,
        timed_out: bool = False,
        message: str = "",
        now: datetime.datetime | None = None,
    ) -> None:
        """Publish the current state to both run-specific and latest paths."""
        timestamp = now or utc_now()
        value = {
            "schema_version": 2,
            "stage": self._stage_name,
            "state": state,
            "started_at": format_time(self._started_at),
            "heartbeat_at": format_time(timestamp),
            "completed_at": None if state in ACTIVE_STATES else format_time(timestamp),
            "requested_duration_seconds": self._stage.duration_seconds,
            "watchdog_grace_seconds": self._watchdog_grace_seconds,
            "timed_out": timed_out,
            "command_exit_code": exit_code,
            "report_present": self._paths.report.is_file(),
            "report_path": str(self._paths.report),
            "log_path": str(self._paths.log),
            "message": message,
        }
        atomic_write_json(self._paths.status, value)
        atomic_copy(self._paths.status, self._paths.latest_directory / "status.json")


@dataclasses.dataclass(frozen=True)
class CommandResult:
    """Describe the termination of one supervised command."""

    exit_code: int
    timed_out: bool = False


@final
class InterruptedRunError(Exception):
    """Indicate that a host signal interrupted the supervised run."""

    def __init__(self, signal_name: str) -> None:
        """Record the name of the signal that interrupted the run."""
        super().__init__(signal_name)
        self.signal_name = signal_name


@final
class CommandSupervisor:
    """Stream command output while enforcing heartbeat and watchdog behavior."""

    def __init__(
        self,
        root: Path,
        log: Path,
        statuses: StatusWriter,
        heartbeat_seconds: int,
    ) -> None:
        """Initialize command supervision for one run."""
        self._root = root
        self._log = log
        self._statuses = statuses
        self._heartbeat_seconds = heartbeat_seconds
        self._process: subprocess.Popen[str] | None = None
        self._received_signal: str | None = None

    def handle_signal(self, signal_number: int, _frame: object) -> None:
        """Record an interrupting signal and terminate the active process group."""
        self._received_signal = signal.Signals(signal_number).name
        self._terminate_process_group()

    def _terminate_process_group(self) -> None:
        if self._process is None or self._process.poll() is not None:
            return
        try:
            os.killpg(self._process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return

    def _copy_output(self, pipe: IO[str]) -> None:
        with self._log.open("a", encoding="utf-8") as log_output:
            for line in pipe:
                sys.stdout.write(line)
                sys.stdout.flush()
                log_output.write(line)
                log_output.flush()

    def run(
        self,
        command: Sequence[str],
        *,
        state: str,
        watchdog_seconds: float | None = None,
    ) -> CommandResult:
        """Run a command while streaming output and enforcing its watchdog."""
        self._received_signal = None
        self._process = subprocess.Popen(
            list(command),
            cwd=self._root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            start_new_session=True,
        )
        assert self._process.stdout is not None
        output_thread = threading.Thread(
            target=self._copy_output,
            args=(self._process.stdout,),
            daemon=True,
        )
        output_thread.start()

        deadline = None if watchdog_seconds is None else time.monotonic() + watchdog_seconds
        next_heartbeat = 0.0
        timed_out = False
        while self._process.poll() is None:
            now = time.monotonic()
            if self._received_signal is not None:
                self._wait_after_terminate()
                break
            if deadline is not None and now >= deadline:
                timed_out = True
                self._terminate_process_group()
                self._wait_after_terminate()
                break
            if now >= next_heartbeat:
                watchdog_message = "none" if watchdog_seconds is None else f"{watchdog_seconds}s"
                self._statuses.write(
                    state,
                    message=f"hardware command active; watchdog={watchdog_message}",
                )
                next_heartbeat = now + self._heartbeat_seconds
            time.sleep(0.1)

        received_signal = self._received_signal
        exit_code = self._process.wait()
        output_thread.join(timeout=5)
        self._process.stdout.close()
        self._process = None
        if received_signal is not None:
            raise InterruptedRunError(received_signal)
        if timed_out:
            return CommandResult(exit_code=124, timed_out=True)
        return CommandResult(exit_code=exit_code)

    def _wait_after_terminate(self) -> None:
        assert self._process is not None
        try:
            self._process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self._process.pid, signal.SIGKILL)
            self._process.wait()


def _read_json_object(path: Path) -> dict[str, object] | None:
    if not path.is_file():
        return None
    try:
        parsed = cast("object", json.loads(path.read_text(encoding="utf-8")))
    except (json.JSONDecodeError, OSError):
        return None
    if not isinstance(parsed, dict):
        return None
    return cast("dict[str, object]", parsed)


def camera_serials(report: Path, log: Path) -> list[str]:
    """Read camera serials from the structured report or probe log fallback."""
    value = _read_json_object(report)
    if value is not None:
        cameras_value = value.get("cameras")
        serials: set[str] = set()
        if isinstance(cameras_value, list):
            for camera_value in cast("list[object]", cameras_value):
                if not isinstance(camera_value, dict):
                    continue
                camera = cast("dict[str, object]", camera_value)
                serial = camera.get("serial")
                if isinstance(serial, str) and serial:
                    serials.add(serial)
        if serials:
            return sorted(serials)

    if not log.is_file():
        return []
    return sorted(set(SERIAL_PATTERN.findall(log.read_text(encoding="utf-8"))))


def publish_test_artifacts(root: Path, stage: Stage, paths: RunPaths) -> None:
    """Copy Bazel test outputs into durable run and latest directories."""
    output_directory = (
        root / "bazel-testlogs" / "capture" / "daheng" / stage.test_name / "test.outputs"
    )
    bazel_report = output_directory / "report.json"
    if not bazel_report.is_file():
        return

    atomic_copy(bazel_report, paths.report)
    for image in output_directory.glob("frame-*.png"):
        atomic_copy(image, paths.run_directory / image.name)
        atomic_copy(image, paths.latest_directory / image.name)
    atomic_copy(paths.report, paths.latest_directory / "report.json")


def report_passed(report: Path) -> bool:
    """Return whether a readable HIL report records a passing result."""
    value = _read_json_object(report)
    return value is not None and value.get("passed") is True


def append_log(log: Path, message: str) -> None:
    """Write an operational message to both the terminal and durable log."""
    print(message, flush=True)
    with log.open("a", encoding="utf-8") as output:
        output.write(f"{message}\n")


@dataclasses.dataclass(frozen=True)
class _RunContext:
    root: Path
    stage: Stage
    watchdog_grace_seconds: int
    paths: RunPaths
    statuses: StatusWriter
    supervisor: CommandSupervisor


def _install_signal_handlers(
    supervisor: CommandSupervisor,
) -> dict[signal.Signals, SignalHandler]:
    previous_handlers: dict[signal.Signals, SignalHandler] = {}
    for handled_signal in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        previous_handlers[handled_signal] = cast(
            "SignalHandler",
            signal.signal(handled_signal, supervisor.handle_signal),
        )
    return previous_handlers


def _restore_signal_handlers(
    previous_handlers: Mapping[signal.Signals, SignalHandler],
) -> None:
    for handled_signal, previous_handler in previous_handlers.items():
        signal.signal(handled_signal, previous_handler)


def _isolate_failed_dual_run(context: _RunContext) -> None:
    append_log(
        context.paths.log,
        "Dual run failed; running automatic single-camera isolation.",
    )
    probe = context.root / "bazel-bin" / "capture" / "daheng" / "camera_probe"
    for serial in camera_serials(context.paths.report, context.paths.log):
        single_directory = context.paths.run_directory / f"isolation-{serial}"
        single_directory.mkdir()
        context.supervisor.run(
            [
                str(probe),
                "--duration-seconds",
                "15",
                "--minimum-fps-ratio",
                "0.98",
                "--maximum-host-frame-interval-multiple",
                "10",
                "--maximum-device-frame-interval-multiple",
                "4",
                "--ring-seconds",
                "2",
                "--ring-reserve-seconds",
                "2",
                "--exercise-frozen-ring",
                "--require-camera-count",
                "1",
                "--serial",
                serial,
                "--json",
                str(single_directory / "report.json"),
            ],
            state="isolating",
            watchdog_seconds=15 + context.watchdog_grace_seconds,
        )


def _finalize_hil_run(context: _RunContext, hil_result: CommandResult) -> int:
    if hil_result.exit_code == 0 and report_passed(context.paths.report):
        context.statuses.write("passed", exit_code=0, message="dual-camera HIL passed")
        print(f"HIL PASS report={context.paths.report} status={context.paths.status}")
        return 0

    if hil_result.timed_out:
        final_state = "timed_out"
        message = "Bazel HIL exceeded duration plus watchdog grace"
    elif not context.paths.report.is_file():
        final_state = "failed"
        message = "Bazel HIL failed before producing its report"
    else:
        final_state = "failed"
        message = "Bazel HIL or its checks failed"
    context.statuses.write(
        final_state,
        exit_code=hil_result.exit_code,
        timed_out=hil_result.timed_out,
        message=message,
    )
    print(f"HIL FAIL report={context.paths.report} status={context.paths.status}")
    return hil_result.exit_code or 1


def _run_hil_commands(context: _RunContext) -> int:
    context.statuses.write("building", message="building Bazel HIL target")
    build = context.supervisor.run(
        [
            "bazel",
            "build",
            context.stage.test_target,
            "//capture/daheng:camera_probe",
        ],
        state="building",
    )
    if build.exit_code != 0:
        context.statuses.write(
            "build_failed",
            exit_code=build.exit_code,
            message="Bazel build failed",
        )
        print(f"HIL BUILD FAILED status={context.paths.status}")
        return build.exit_code

    watchdog_seconds = context.stage.duration_seconds + context.watchdog_grace_seconds
    hil_result = context.supervisor.run(
        [
            "bazel",
            "test",
            context.stage.test_target,
            "--test_output=streamed",
            "--nocache_test_results",
        ],
        state="running",
        watchdog_seconds=watchdog_seconds,
    )
    publish_test_artifacts(context.root, context.stage, context.paths)

    if hil_result.exit_code != 0:
        _isolate_failed_dual_run(context)
    return _finalize_hil_run(context, hil_result)


def _run_with_hardware_lock(context: _RunContext) -> int:
    lock_path = context.root / "artifacts" / "hil" / "hardware.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("w", encoding="utf-8") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            context.statuses.write(
                "busy",
                exit_code=75,
                message="another unattended hardware run holds the host lock",
            )
            print(f"HIL BUSY status={context.paths.status}")
            return 75
        return _run_hil_commands(context)


def run(stage_name: str, environment: Mapping[str, str]) -> int:
    """Run one unattended HIL stage and publish durable status artifacts."""
    stage = STAGES[stage_name]
    watchdog_grace_seconds = positive_integer_setting(environment, "HIL_WATCHDOG_GRACE_SECONDS", 60)
    heartbeat_seconds = positive_integer_setting(environment, "HIL_HEARTBEAT_SECONDS", 10)
    root = repository_root(environment, Path.cwd())
    started_at = utc_now()
    paths = RunPaths.create(root, stage_name, started_at, os.getpid())
    statuses = StatusWriter(paths, stage_name, stage, watchdog_grace_seconds, started_at)
    supervisor = CommandSupervisor(root, paths.log, statuses, heartbeat_seconds)
    context = _RunContext(
        root=root,
        stage=stage,
        watchdog_grace_seconds=watchdog_grace_seconds,
        paths=paths,
        statuses=statuses,
        supervisor=supervisor,
    )
    previous_handlers = _install_signal_handlers(supervisor)
    try:
        return _run_with_hardware_lock(context)
    except InterruptedRunError as interrupted:
        context.statuses.write(
            "interrupted",
            exit_code=130,
            message=f"HIL runner received {interrupted.signal_name}",
        )
        return 130
    finally:
        _restore_signal_handlers(previous_handlers)


@dataclasses.dataclass(frozen=True)
class Arguments:
    """Hold validated unattended-run command-line arguments."""

    stage: str


def parse_arguments(arguments: Sequence[str]) -> Arguments:
    """Parse unattended-run command-line arguments."""
    parser = argparse.ArgumentParser(
        description="Run and supervise a Daheng hardware-in-the-loop stage."
    )
    parser.add_argument("stage", choices=tuple(STAGES), nargs="?", default="smoke")
    namespace = parser.parse_args(arguments)
    return Arguments(stage=cast("str", namespace.stage))


def main() -> None:
    """Run the requested stage and convert configuration failures to an exit code."""
    arguments = parse_arguments(sys.argv[1:])
    try:
        exit_code = run(arguments.stage, os.environ)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"HIL runner configuration error: {error}", file=sys.stderr)
        exit_code = 2
    raise SystemExit(exit_code)


if __name__ == "__main__":
    main()
