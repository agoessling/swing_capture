"""Tests for machine-local station parsing and read-only inventory."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools import station_doctor

VALID_CONFIG = """
schema_version = 1
camera.roles_verified = true
camera.down_the_line.serial = DOWN123
camera.face_on.serial = FACE456
audio.alsa_device = hw:CARD=microphone,DEV=0
audio.channel_count = 1
audio.selected_channel = 0
feather.serial_path = /dev/serial/by-id/usb-Raspberry_Pi_Pico_123-if00
"""
SUPER_SPEED_MBPS = 5000


class StationDoctorTest(unittest.TestCase):
    """Exercise deterministic station configuration and inventory behavior."""

    def test_parse_valid_station_config(self) -> None:
        """Parse stable identifiers and explicit role verification."""
        config = station_doctor.parse_station_config(VALID_CONFIG)
        self.assertTrue(config.camera_roles_verified)
        self.assertEqual("DOWN123", config.down_the_line_camera_serial)
        self.assertEqual("FACE456", config.face_on_camera_serial)
        self.assertEqual("hw:CARD=microphone,DEV=0", config.audio_alsa_device)
        self.assertEqual(1, config.audio_channel_count)
        self.assertEqual(0, config.audio_selected_channel)

    def test_reject_malformed_station_config(self) -> None:
        """Reject missing, duplicate, volatile, and ambiguous selections."""
        cases = (
            "schema_version = 1\n",
            VALID_CONFIG + "unknown = value\n",
            VALID_CONFIG + "schema_version = 1\n",
            VALID_CONFIG.replace("FACE456", "DOWN123"),
            VALID_CONFIG.replace("CARD=microphone", "CARD=1"),
            VALID_CONFIG.replace("audio.channel_count = 1", "audio.channel_count = 0"),
            VALID_CONFIG.replace("audio.selected_channel = 0", "audio.selected_channel = 1"),
            VALID_CONFIG.replace("audio.channel_count = 1", "audio.channel_count = stereo"),
            VALID_CONFIG.replace(
                "/dev/serial/by-id/usb-Raspberry_Pi_Pico_123-if00", "/dev/ttyACM0"
            ),
        )
        for value in cases:
            with self.subTest(value=value), self.assertRaises(ValueError):
                station_doctor.parse_station_config(value)

    def test_parse_alsa_cards(self) -> None:
        """Preserve a stable card ID separately from its volatile index."""
        cards = station_doctor.parse_alsa_cards(
            """ 0 [PCH            ]: HDA-Intel - HDA Intel PCH
 1 [microphone     ]: USB-Audio - TONOR G11 USB microphone
"""
        )
        self.assertEqual(
            [(0, "PCH"), (1, "microphone")],
            [(card.index, card.card_id) for card in cards],
        )

    def test_inventory_cameras_from_sysfs(self) -> None:
        """Find serials, SuperSpeed links, access, and PCI root controllers."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sysfs = root / "sys/bus/usb/devices"
            dev = root / "dev"
            sysfs.mkdir(parents=True)
            for index, serial in enumerate(("DOWN123", "FACE456"), start=1):
                target = (
                    root
                    / "sys/devices"
                    / f"pci0000:0{index}"
                    / f"0000:0{index}:00.0"
                    / f"usb{index}"
                    / f"{index}-1"
                )
                target.mkdir(parents=True)
                (target / "idVendor").write_text("2ba2\n", encoding="utf-8")
                (target / "serial").write_text(f"{serial}\n", encoding="utf-8")
                (target / "product").write_text("MER2-160-227U3C\n", encoding="utf-8")
                (target / "busnum").write_text(f"{index}\n", encoding="utf-8")
                (target / "devnum").write_text(f"{index + 2}\n", encoding="utf-8")
                (target / "speed").write_text("5000\n", encoding="utf-8")
                (target / "devpath").write_text("1\n", encoding="utf-8")
                (sysfs / f"{index}-1").symlink_to(target, target_is_directory=True)
                node = dev / "bus/usb" / f"{index:03d}" / f"{index + 2:03d}"
                node.parent.mkdir(parents=True, exist_ok=True)
                node.touch()

            cameras = station_doctor.find_daheng_cameras(sysfs, dev)
            self.assertEqual(["DOWN123", "FACE456"], [camera.serial for camera in cameras])
            self.assertTrue(all(camera.speed_mbps == SUPER_SPEED_MBPS for camera in cameras))
            self.assertTrue(all(camera.read_write for camera in cameras))
            self.assertEqual(2, len({camera.root_controller for camera in cameras}))

    def test_hardware_checks_gate_roles_but_report_shared_root(self) -> None:
        """Gate unverified roles and require HIL evidence for shared USB capacity."""
        config = station_doctor.parse_station_config(
            VALID_CONFIG.replace("camera.roles_verified = true", "camera.roles_verified = false")
        )
        cameras = [
            station_doctor.UsbCamera(
                serial=serial,
                product="MER2-160-227U3C",
                sysfs_name=f"4-{index}",
                usb_path=str(index),
                root_controller="/same/controller",
                speed_mbps=5000,
                device_node=Path(f"/dev/camera{index}"),
                read_write=True,
            )
            for index, serial in enumerate(("DOWN123", "FACE456"), start=1)
        ]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            checks = station_doctor.build_checks(
                station_doctor.DoctorInputs(
                    root=root,
                    config_path=root / ".station.local.conf",
                    config=config,
                    config_error="",
                    cameras=cameras,
                    alsa_cards=[station_doctor.AlsaCard(1, "microphone", "USB microphone")],
                    serial_links=[],
                    etc_root=root / "etc",
                    sys_root=root / "sys",
                    dev_root=root / "dev",
                    group_check=lambda _group: True,
                    service_check=lambda _arguments: (True, "ok"),
                    access=lambda _path, _mode: True,
                )
            )
        by_name = {check.name: check for check in checks}
        self.assertFalse(by_name["camera_roles_verified"].passed)
        topology = by_name["camera_root_controller_topology"]
        self.assertTrue(topology.passed)
        self.assertIn("shared controller requires full-rate dual-camera HIL", topology.message)


if __name__ == "__main__":
    unittest.main()
