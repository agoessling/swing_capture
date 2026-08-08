"""Bazel-native RP2040 firmware and flashing rules."""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")


def _rp2040_transition_impl(_settings, attr):
    return {
        "//command_line_option:platforms": "@pico-sdk//bazel/platform:rp2040",
        "@pico-sdk//bazel/config:PICO_CONFIG_PLATFORM_HEADER": str(attr.board_config),
        "@pico-sdk//bazel/config:PICO_STDIO_UART": False,
        "@pico-sdk//bazel/config:PICO_STDIO_USB": True,
    }


_rp2040_transition = transition(
    implementation = _rp2040_transition_impl,
    inputs = [],
    outputs = [
        "//command_line_option:platforms",
        "@pico-sdk//bazel/config:PICO_CONFIG_PLATFORM_HEADER",
        "@pico-sdk//bazel/config:PICO_STDIO_UART",
        "@pico-sdk//bazel/config:PICO_STDIO_USB",
    ],
)


def _rp2040_uf2_impl(ctx):
    elf = ctx.file.elf
    uf2 = ctx.actions.declare_file(ctx.label.name + ".uf2")
    ctx.actions.run(
        arguments = [
            "uf2",
            "convert",
            "--quiet",
            "-t",
            "elf",
            elf.path,
            uf2.path,
        ],
        executable = ctx.executable._picotool,
        inputs = [elf],
        mnemonic = "PicoUf2",
        outputs = [uf2],
        progress_message = "Converting %{label} to UF2",
        tools = [ctx.executable._picotool],
    )
    return [DefaultInfo(files = depset([uf2]))]


_rp2040_uf2 = rule(
    implementation = _rp2040_uf2_impl,
    attrs = {
        "board_config": attr.label(mandatory = True),
        "elf": attr.label(
            allow_single_file = True,
            cfg = _rp2040_transition,
            mandatory = True,
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
        "_picotool": attr.label(
            cfg = "exec",
            default = "@picotool//:picotool",
            executable = True,
        ),
    },
)


def _rp2040_flash_impl(ctx):
    firmware = ctx.file.firmware
    picotool = ctx.executable._picotool
    launcher = ctx.actions.declare_file(ctx.label.name + ".sh")

    workspace = ctx.workspace_name
    firmware_path = "${runfiles_dir}/%s/%s" % (workspace, firmware.short_path)
    picotool_path = "${runfiles_dir}/%s/%s" % (workspace, picotool.short_path)

    ctx.actions.write(
        output = launcher,
        is_executable = True,
        content = """#!/usr/bin/env bash
set -euo pipefail

runfiles_dir="${RUNFILES_DIR:-$0.runfiles}"
firmware="%s"
picotool="%s"

if [[ ! -f "$firmware" ]]; then
  echo "Firmware runfile is missing: $firmware" >&2
  exit 2
fi
if [[ ! -x "$picotool" ]]; then
  echo "picotool runfile is missing: $picotool" >&2
  exit 2
fi

echo "Flashing $firmware"
if ! "$picotool" load -f -v -x "$firmware" "$@"; then
  cat >&2 <<'EOF'

Unable to enter the RP2040 bootloader automatically.
Hold BOOT, tap RESET, release BOOT, and run this Bazel target again.
If picotool reports a permission error, install the repository's RP2040 udev
rule with: sudo ./tools/setup_prop_maker_host.sh
EOF
  exit 1
fi
""" % (firmware_path, picotool_path),
    )

    runfiles = ctx.runfiles(files = [firmware, picotool])
    runfiles = runfiles.merge(ctx.attr._picotool[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = launcher, runfiles = runfiles)]


rp2040_flash = rule(
    implementation = _rp2040_flash_impl,
    executable = True,
    attrs = {
        "firmware": attr.label(
            allow_single_file = [".uf2"],
            mandatory = True,
        ),
        "_picotool": attr.label(
            cfg = "exec",
            default = "@picotool//:picotool",
            executable = True,
        ),
    },
)


def rp2040_firmware(
        name,
        board_config,
        srcs,
        deps = [],
        tags = [],
        visibility = None,
        **kwargs):
    """Builds an RP2040 ELF with rules_cc and exposes a UF2 artifact."""
    firmware_tags = tags + ["no-host-sanitizer"]
    elf_name = name + "_elf"
    cc_binary(
        name = elf_name,
        srcs = srcs,
        deps = deps,
        tags = firmware_tags,
        target_compatible_with = ["@pico-sdk//bazel/constraint:rp2040"],
        visibility = ["//visibility:private"],
        **kwargs
    )
    uf2_kwargs = {}
    if visibility != None:
        uf2_kwargs["visibility"] = visibility
    _rp2040_uf2(
        name = name,
        board_config = board_config,
        elf = ":" + elf_name,
        tags = firmware_tags,
        **uf2_kwargs
    )
