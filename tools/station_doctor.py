"""Read-only readiness checks for one machine-local capture station."""

from __future__ import annotations

import argparse
import dataclasses
import grp
import json
import os
import pwd
import re
import subprocess
import sys
from pathlib import Path
from typing import TYPE_CHECKING, cast

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping, Sequence


CONFIG_ENVIRONMENT = "SWING_CAPTURE_STATION_CONFIG"
DEFAULT_CONFIG_NAME = ".station.local.conf"
EXPECTED_CAMERA_COUNT = 2
SUPER_SPEED_MBPS = 5000
MAX_AUDIO_CHANNEL_COUNT = 65535
SYSFS_USB_DEVICES = Path("/sys/bus/usb/devices")
DEV_ROOT = Path("/dev")
SERIAL_DIRECTORY = Path("/dev/serial/by-id")
CONFIG_KEYS = frozenset(
    (
        "schema_version",
        "camera.roles_verified",
        "camera.down_the_line.serial",
        "camera.face_on.serial",
        "audio.alsa_device",
        "audio.channel_count",
        "audio.selected_channel",
        "feather.serial_path",
    )
)
AUDIO_DEVICE_PATTERN = re.compile(
    r"^hw:CARD=(?P<card>[A-Za-z0-9_-]*[A-Za-z_-][A-Za-z0-9_-]*),DEV=(?P<device>[0-9]+)$"
)
ALSA_CARD_PATTERN = re.compile(r"^\s*(?P<index>[0-9]+) \[(?P<card>[^]]+)\]: (?P<name>.+)$")


@dataclasses.dataclass(frozen=True)
class StationConfig:
    """Stable device selections and camera roles for one capture machine."""

    camera_roles_verified: bool
    down_the_line_camera_serial: str
    face_on_camera_serial: str
    audio_alsa_device: str
    audio_channel_count: int
    audio_selected_channel: int
    feather_serial_path: Path


@dataclasses.dataclass(frozen=True)
class Check:
    """One explicit station-readiness assertion."""

    name: str
    passed: bool
    message: str

    def as_json(self) -> dict[str, object]:
        """Return a machine-readable representation."""
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class UsbCamera:
    """One Daheng camera discovered through Linux sysfs."""

    serial: str
    product: str
    sysfs_name: str
    usb_path: str
    root_controller: str
    speed_mbps: int
    device_node: Path
    read_write: bool

    def as_json(self) -> dict[str, object]:
        """Return a machine-readable representation."""
        value = dataclasses.asdict(self)
        value["device_node"] = str(self.device_node)
        return value


@dataclasses.dataclass(frozen=True)
class AlsaCard:
    """One ALSA card parsed from procfs."""

    index: int
    card_id: str
    name: str

    def as_json(self) -> dict[str, object]:
        """Return a machine-readable representation."""
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class SerialLink:
    """One stable serial-device symlink."""

    path: Path
    target: Path
    read_write: bool

    def as_json(self) -> dict[str, object]:
        """Return a machine-readable representation."""
        return {
            "path": str(self.path),
            "target": str(self.target),
            "read_write": self.read_write,
        }


def _parse_config_values(contents: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(contents.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        key = key.strip()
        value = value.strip()
        if not separator:
            message = f"station config line {line_number} must contain '='"
            raise ValueError(message)
        if key not in CONFIG_KEYS:
            message = f"unknown station config key on line {line_number}: {key}"
            raise ValueError(message)
        if not value:
            message = f"empty station config value on line {line_number}: {key}"
            raise ValueError(message)
        if key in values:
            message = f"duplicate station config key on line {line_number}: {key}"
            raise ValueError(message)
        values[key] = value

    missing = sorted(CONFIG_KEYS.difference(values))
    if missing:
        message = f"missing station config key: {missing[0]}"
        raise ValueError(message)
    return values


def parse_station_config(contents: str) -> StationConfig:
    """Parse and validate the deliberately small station configuration format."""
    values = _parse_config_values(contents)
    if values["schema_version"] != "1":
        msg = "unsupported station config schema_version"
        raise ValueError(msg)
    verified_value = values["camera.roles_verified"]
    if verified_value not in ("true", "false"):
        msg = "camera.roles_verified must be true or false"
        raise ValueError(msg)
    down_the_line = values["camera.down_the_line.serial"]
    face_on = values["camera.face_on.serial"]
    if down_the_line == face_on:
        msg = "camera role serials must be distinct"
        raise ValueError(msg)
    audio_device = values["audio.alsa_device"]
    if AUDIO_DEVICE_PATTERN.fullmatch(audio_device) is None:
        msg = "audio.alsa_device must use a stable hw:CARD=<id>,DEV=<number> identifier"
        raise ValueError(msg)
    channel_count_value = values["audio.channel_count"]
    selected_channel_value = values["audio.selected_channel"]
    if (
        re.fullmatch(r"[0-9]+", channel_count_value) is None
        or re.fullmatch(r"[0-9]+", selected_channel_value) is None
    ):
        msg = "audio channel values must be unsigned integers"
        raise ValueError(msg)
    audio_channel_count = int(channel_count_value)
    audio_selected_channel = int(selected_channel_value)
    if (
        audio_channel_count <= 0
        or audio_channel_count > MAX_AUDIO_CHANNEL_COUNT
        or audio_selected_channel < 0
        or audio_selected_channel >= audio_channel_count
    ):
        msg = "audio.channel_count must be positive and audio.selected_channel must be smaller"
        raise ValueError(msg)
    feather_path = Path(values["feather.serial_path"])
    if (
        not feather_path.is_absolute()
        or not str(feather_path).startswith("/dev/serial/by-id/")
        or feather_path.name == ""
    ):
        msg = "feather.serial_path must be an absolute /dev/serial/by-id path"
        raise ValueError(msg)
    return StationConfig(
        camera_roles_verified=verified_value == "true",
        down_the_line_camera_serial=down_the_line,
        face_on_camera_serial=face_on,
        audio_alsa_device=audio_device,
        audio_channel_count=audio_channel_count,
        audio_selected_channel=audio_selected_channel,
        feather_serial_path=feather_path,
    )


def repository_root(environment: Mapping[str, str], current_directory: Path) -> Path:
    """Locate the repository used to launch the doctor."""
    workspace = environment.get("BUILD_WORKSPACE_DIRECTORY")
    if workspace:
        return Path(workspace).resolve()
    candidate = current_directory.resolve()
    for directory in (candidate, *candidate.parents):
        if (directory / "MODULE.bazel").is_file():
            return directory
    msg = "cannot locate the swing_capture repository"
    raise RuntimeError(msg)


def resolve_config_path(explicit: Path | None, environment: Mapping[str, str], root: Path) -> Path:
    """Resolve the explicit, environment, or conventional station path."""
    if explicit is not None:
        return explicit.expanduser().resolve()
    configured = environment.get(CONFIG_ENVIRONMENT)
    if configured:
        return Path(configured).expanduser().resolve()
    return root / DEFAULT_CONFIG_NAME


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError):
        return ""


def _integer(path: Path) -> int | None:
    try:
        return int(_read_text(path))
    except ValueError:
        return None


def _root_controller(path: Path) -> str:
    try:
        canonical = str(path.resolve(strict=True))
    except OSError:
        return ""
    marker = canonical.find("/usb")
    return canonical if marker < 0 else canonical[:marker]


def find_daheng_cameras(
    sysfs_devices: Path = SYSFS_USB_DEVICES,
    dev_root: Path = DEV_ROOT,
    access: Callable[[Path, int], bool] = os.access,
) -> list[UsbCamera]:
    """Inventory Daheng cameras without opening the vendor SDK."""
    cameras: list[UsbCamera] = []
    try:
        entries = tuple(sysfs_devices.iterdir())
    except OSError:
        return cameras
    for entry in entries:
        if _read_text(entry / "idVendor") != "2ba2":
            continue
        bus_number = _integer(entry / "busnum")
        device_number = _integer(entry / "devnum")
        speed = _integer(entry / "speed")
        if bus_number is None or device_number is None or speed is None:
            continue
        device_node = dev_root / "bus" / "usb" / f"{bus_number:03d}" / f"{device_number:03d}"
        cameras.append(
            UsbCamera(
                serial=_read_text(entry / "serial"),
                product=_read_text(entry / "product"),
                sysfs_name=entry.name,
                usb_path=_read_text(entry / "devpath"),
                root_controller=_root_controller(entry),
                speed_mbps=speed,
                device_node=device_node,
                read_write=access(device_node, os.R_OK | os.W_OK),
            )
        )
    return sorted(cameras, key=lambda camera: camera.serial)


def parse_alsa_cards(contents: str) -> list[AlsaCard]:
    """Parse stable ALSA card IDs and volatile indices from procfs."""
    cards: list[AlsaCard] = []
    for line in contents.splitlines():
        match = ALSA_CARD_PATTERN.match(line)
        if match is None:
            continue
        cards.append(
            AlsaCard(
                index=int(match.group("index")),
                card_id=match.group("card").strip(),
                name=match.group("name").strip(),
            )
        )
    return cards


def find_serial_links(
    serial_directory: Path = SERIAL_DIRECTORY,
    access: Callable[[Path, int], bool] = os.access,
) -> list[SerialLink]:
    """Inventory stable serial links and access to their resolved nodes."""
    try:
        entries = tuple(serial_directory.iterdir())
    except OSError:
        return []
    links: list[SerialLink] = []
    for entry in entries:
        try:
            target = entry.resolve(strict=True)
        except OSError:
            target = entry.resolve(strict=False)
        links.append(
            SerialLink(
                path=entry,
                target=target,
                read_write=access(target, os.R_OK | os.W_OK),
            )
        )
    return sorted(links, key=lambda link: str(link.path))


def current_user_in_group(group_name: str) -> bool:
    """Return whether the current process has the named group active."""
    try:
        group_id = grp.getgrnam(group_name).gr_gid
    except KeyError:
        return False
    return group_id == os.getgid() or group_id in os.getgroups()


def _camera_root_topology_check(selected: Sequence[UsbCamera]) -> Check:
    distinct_roots = (
        len(selected) == EXPECTED_CAMERA_COUNT
        and len({camera.root_controller for camera in selected}) == EXPECTED_CAMERA_COUNT
    )
    message = ", ".join(f"{camera.serial}={camera.root_controller}" for camera in selected)
    if len(selected) == EXPECTED_CAMERA_COUNT and not distinct_roots:
        message += "; shared controller requires full-rate dual-camera HIL"
    return Check(
        "camera_root_controller_topology",
        len(selected) == EXPECTED_CAMERA_COUNT,
        message,
    )


def _files_identical(expected: Path, installed: Path) -> bool:
    try:
        return expected.read_bytes() == installed.read_bytes()
    except OSError:
        return False


def _systemctl_succeeds(arguments: Sequence[str]) -> tuple[bool, str]:
    completed = subprocess.run(
        ["systemctl", *arguments],
        check=False,
        capture_output=True,
        text=True,
    )
    message = (completed.stdout or completed.stderr).strip()
    return completed.returncode == 0, message


def _translated_device_path(path: Path, dev_root: Path) -> Path:
    if dev_root == Path("/dev"):
        return path
    try:
        return dev_root / path.relative_to("/dev")
    except ValueError:
        return path


@dataclasses.dataclass(frozen=True)
class DoctorInputs:
    """Collected station state plus injectable host inspection boundaries."""

    root: Path
    config_path: Path
    config: StationConfig | None
    config_error: str
    cameras: Sequence[UsbCamera]
    alsa_cards: Sequence[AlsaCard]
    serial_links: Sequence[SerialLink]
    etc_root: Path
    sys_root: Path
    dev_root: Path
    group_check: Callable[[str], bool]
    service_check: Callable[[Sequence[str]], tuple[bool, str]]
    access: Callable[[Path, int], bool]


def build_checks(inputs: DoctorInputs) -> list[Check]:
    """Evaluate configuration, host setup, and stable device selections."""
    root = inputs.root
    config_path = inputs.config_path
    config = inputs.config
    cameras = inputs.cameras
    alsa_cards = inputs.alsa_cards
    serial_links = inputs.serial_links
    etc_root = inputs.etc_root
    sys_root = inputs.sys_root
    dev_root = inputs.dev_root
    checks = [
        Check(
            "station_config",
            config is not None,
            str(config_path) if config is not None else inputs.config_error,
        ),
        Check(
            "plugdev_membership",
            inputs.group_check("plugdev"),
            f"user={pwd.getpwuid(os.getuid()).pw_name}",
        ),
    ]
    installed_files = (
        (
            root / "config/udev/99-swing-capture-daheng.rules",
            etc_root / "udev/rules.d/99-swing-capture-daheng.rules",
            "daheng_udev_rule",
        ),
        (
            root / "config/udev/60-swing-capture-rp2040.rules",
            etc_root / "udev/rules.d/60-swing-capture-rp2040.rules",
            "rp2040_udev_rule",
        ),
        (
            root / "config/systemd/swing-capture-usbfs-memory.service",
            etc_root / "systemd/system/swing-capture-usbfs-memory.service",
            "usbfs_service_file",
        ),
    )
    for expected, installed, name in installed_files:
        checks.append(Check(name, _files_identical(expected, installed), str(installed)))

    usbfs_value = _read_text(sys_root / "module/usbcore/parameters/usbfs_memory_mb")
    checks.append(
        Check("usbfs_memory_mb", usbfs_value == "2000", f"value={usbfs_value or 'missing'}")
    )
    enabled, enabled_message = inputs.service_check(
        ("is-enabled", "swing-capture-usbfs-memory.service")
    )
    active, active_message = inputs.service_check(
        ("is-active", "swing-capture-usbfs-memory.service")
    )
    checks.extend(
        (
            Check("usbfs_service_enabled", enabled, enabled_message or "no status"),
            Check("usbfs_service_active", active, active_message or "no status"),
            Check(
                "two_daheng_cameras",
                len(cameras) == EXPECTED_CAMERA_COUNT,
                f"found={len(cameras)}",
            ),
        )
    )
    if config is None:
        return checks

    checks.append(
        Check(
            "camera_roles_verified",
            config.camera_roles_verified,
            "physical down-the-line and face-on assignments must be confirmed",
        )
    )
    camera_by_serial = {camera.serial: camera for camera in cameras}
    configured_cameras = (
        ("down_the_line", config.down_the_line_camera_serial),
        ("face_on", config.face_on_camera_serial),
    )
    selected: list[UsbCamera] = []
    for role, serial in configured_cameras:
        camera = camera_by_serial.get(serial)
        present = camera is not None
        checks.append(Check(f"camera_{role}_present", present, f"serial={serial}"))
        if camera is None:
            continue
        selected.append(camera)
        checks.append(
            Check(
                f"camera_{role}_superspeed",
                camera.speed_mbps >= SUPER_SPEED_MBPS,
                f"serial={serial} speed_mbps={camera.speed_mbps}",
            )
        )
        checks.append(
            Check(
                f"camera_{role}_access",
                camera.read_write,
                f"serial={serial} node={camera.device_node}",
            )
        )
    checks.append(_camera_root_topology_check(selected))

    audio_match = AUDIO_DEVICE_PATTERN.fullmatch(config.audio_alsa_device)
    if audio_match is None:
        message = "validated audio identifier did not match"
        raise AssertionError(message)
    card_id = audio_match.group("card")
    device_number = int(audio_match.group("device"))
    card = next((candidate for candidate in alsa_cards if candidate.card_id == card_id), None)
    checks.append(
        Check(
            "audio_card_present",
            card is not None,
            f"device={config.audio_alsa_device}",
        )
    )
    if card is not None:
        control = dev_root / "snd" / f"controlC{card.index}"
        capture = dev_root / "snd" / f"pcmC{card.index}D{device_number}c"
        checks.append(
            Check(
                "audio_device_access",
                inputs.access(control, os.R_OK | os.W_OK)
                and inputs.access(capture, os.R_OK | os.W_OK),
                f"control={control} capture={capture}",
            )
        )

    configured_feather_path = _translated_device_path(config.feather_serial_path, dev_root)
    feather = next(
        (
            link
            for link in serial_links
            if link.path == configured_feather_path
            or link.path.name == config.feather_serial_path.name
        ),
        None,
    )
    checks.append(
        Check(
            "feather_serial_present",
            feather is not None,
            str(config.feather_serial_path),
        )
    )
    if feather is not None:
        checks.append(Check("feather_serial_access", feather.read_write, str(feather.target)))
    return checks


def _station_json(config: StationConfig | None) -> dict[str, object] | None:
    if config is None:
        return None
    return {
        "camera_roles_verified": config.camera_roles_verified,
        "cameras": {
            "down_the_line": config.down_the_line_camera_serial,
            "face_on": config.face_on_camera_serial,
        },
        "audio_alsa_device": config.audio_alsa_device,
        "audio_channel_count": config.audio_channel_count,
        "audio_selected_channel": config.audio_selected_channel,
        "feather_serial_path": str(config.feather_serial_path),
    }


def build_report(
    inputs: DoctorInputs,
    checks: Sequence[Check],
) -> dict[str, object]:
    """Build the station-doctor JSON report."""
    return {
        "schema_version": 1,
        "passed": all(check.passed for check in checks),
        "config_path": str(inputs.config_path),
        "station": _station_json(inputs.config),
        "checks": [check.as_json() for check in checks],
        "inventory": {
            "cameras": [camera.as_json() for camera in inputs.cameras],
            "alsa_cards": [card.as_json() for card in inputs.alsa_cards],
            "serial_links": [link.as_json() for link in inputs.serial_links],
        },
    }


def atomic_write_json(destination: Path, value: Mapping[str, object]) -> None:
    """Atomically publish a machine-readable doctor report."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f"{destination.name}.tmp.{os.getpid()}")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(destination)


def print_report(report: Mapping[str, object]) -> None:
    """Print a concise human-readable inventory and readiness summary."""
    inventory = cast("dict[str, object]", report["inventory"])
    cameras = cast("list[dict[str, object]]", inventory["cameras"])
    alsa_cards = cast("list[dict[str, object]]", inventory["alsa_cards"])
    serial_links = cast("list[dict[str, object]]", inventory["serial_links"])
    print("Cameras:")
    for camera in cameras:
        print(
            " ".join(
                (
                    f"  {camera['serial']} {camera['speed_mbps']} Mb/s",
                    f"path={camera['sysfs_name']} root={camera['root_controller']}",
                    f"access={'read/write' if camera['read_write'] else 'insufficient'}",
                )
            )
        )
    print("ALSA cards:")
    for card in alsa_cards:
        print(f"  {card['card_id']} index={card['index']} {card['name']}")
    print("Stable serial links:")
    for link in serial_links:
        print(
            " ".join(
                (
                    f"  {link['path']} -> {link['target']}",
                    f"access={'read/write' if link['read_write'] else 'insufficient'}",
                )
            )
        )
    print("Checks:")
    checks = cast("list[dict[str, object]]", report["checks"])
    for check in checks:
        state = "PASS" if check["passed"] else "FAIL"
        print(f"  [{state}] {check['name']}: {check['message']}")
    print(f"Station doctor {'PASS' if report['passed'] else 'FAIL'}")


@dataclasses.dataclass(frozen=True)
class Arguments:
    """Typed command-line selections."""

    config: Path | None
    json_output: Path | None


def _parse_arguments(arguments: Sequence[str]) -> Arguments:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, help="machine-local station config path")
    parser.add_argument(
        "--json", dest="json_output", type=Path, help="write a machine-readable report"
    )
    parsed = parser.parse_args(arguments)
    return Arguments(
        config=cast("Path | None", parsed.config),
        json_output=cast("Path | None", parsed.json_output),
    )


def main(arguments: Sequence[str] | None = None) -> int:
    """Run the read-only station doctor and optionally retain its report."""
    options = _parse_arguments(sys.argv[1:] if arguments is None else arguments)
    root = repository_root(os.environ, Path.cwd())
    config_path = resolve_config_path(options.config, os.environ, root)
    config: StationConfig | None = None
    config_error = ""
    try:
        config = parse_station_config(config_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        config_error = f"{config_path}: {error}"

    cameras = find_daheng_cameras()
    alsa_cards = parse_alsa_cards(_read_text(Path("/proc/asound/cards")))
    serial_links = find_serial_links()
    inputs = DoctorInputs(
        root=root,
        config_path=config_path,
        config=config,
        config_error=config_error,
        cameras=cameras,
        alsa_cards=alsa_cards,
        serial_links=serial_links,
        etc_root=Path("/etc"),
        sys_root=Path("/sys"),
        dev_root=Path("/dev"),
        group_check=current_user_in_group,
        service_check=_systemctl_succeeds,
        access=os.access,
    )
    checks = build_checks(inputs)
    report = build_report(inputs, checks)
    print_report(report)
    if options.json_output is not None:
        output = (
            options.json_output if options.json_output.is_absolute() else root / options.json_output
        )
        atomic_write_json(output, report)
        print(f"JSON: {output}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
