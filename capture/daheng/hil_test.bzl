"""Bazel-native hardware-in-the-loop test definitions."""

load("@rules_cc//cc:cc_test.bzl", "cc_test")

def daheng_dual_camera_hil_test(
        name,
        duration_seconds,
        minimum_fps_ratio,
        size,
        timeout):
    """Defines an explicit, local-only test that owns both Daheng cameras."""
    cc_test(
        name = name,
        srcs = ["camera_probe.cc"],
        args = [
            "--duration-seconds",
            str(duration_seconds),
            "--minimum-fps-ratio",
            str(minimum_fps_ratio),
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
            "2",
        ],
        size = size,
        tags = [
            "exclusive",
            "hil",
            "local",
            "manual",
            "no-sandbox",
            "requires-daheng-camera",
            "requires-daheng-sdk",
        ],
        timeout = timeout,
        deps = [
            "//capture/core:device_clock_mapper",
            "//capture/core:pooled_raw_frame_ring",
            "//capture/hil:hil_metrics",
            "//capture/image:bayer_rg8",
            "//capture/image:image_quality",
            ":daheng_camera",
        ],
    )
