# ##BAZEL_DEVTOOLS_MANAGED_BEGIN:aspects##
load(
    "@bazel_devtools//checks:python.bzl",
    "basedpyright_aspect",
    "ruff_format_aspect",
    "ruff_lint_aspect",
)

load(
    "@bazel_devtools//checks:cpp.bzl",
    "clang_format_aspect",
    "lint_clang_tidy_aspect",
)

ruff = ruff_lint_aspect(
    binary = Label("@bazel_devtools//tools:ruff"),
    configs = [
        Label("//:.ruff.toml"),
        Label("//:.bazel_devtools/ruff.toml"),
    ],
)

basedpyright = basedpyright_aspect(
    binary = Label("@bazel_devtools//tools:basedpyright"),
    config = Label("//:basedpyright.json"),
    configs = [Label("//:.bazel_devtools/basedpyright.json")],
)

ruff_format = ruff_format_aspect(
    binary = Label("@bazel_devtools//tools:ruff"),
    configs = [
        Label("//:.ruff.toml"),
        Label("//:.bazel_devtools/ruff.toml"),
    ],
)

clang_tidy = lint_clang_tidy_aspect(
    binary = Label("//tools/bazel_devtools:clang_tidy"),
    global_config = [Label("//:.clang-tidy")],
    lint_target_headers = True,
)

clang_format = clang_format_aspect(
    binary = Label("//tools/bazel_devtools:clang_format"),
    config = Label("//:.clang-format"),
)
# ##BAZEL_DEVTOOLS_MANAGED_END:aspects##
