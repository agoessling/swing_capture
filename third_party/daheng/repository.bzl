"""Repository definition for the binary Daheng Galaxy Linux SDK."""

_GALAXY_SDK_SHA256 = "b1ea630f2ca3bcb4406434691e69ce509f4edf9f259260f263bec3ba66d45769"
_GALAXY_SDK_STRIP_PREFIX = "Galaxy_Linux-x86_Gige-U3_32bits-64bits_2.6.2606.9251"
_GALAXY_SDK_URLS = [
    "https://en.daheng-imaging.com/uploadfile/2026/0703/20260703023200473.zip",
]


def _daheng_galaxy_sdk_repository_impl(repository_ctx):
    repository_ctx.download_and_extract(
        url = repository_ctx.attr.urls,
        sha256 = repository_ctx.attr.sha256,
        stripPrefix = repository_ctx.attr.strip_prefix,
    )

    installer = repository_ctx.path("Galaxy_camera.run")
    if not installer.exists:
        fail("Daheng archive did not contain Galaxy_camera.run")

    python = repository_ctx.which("python3")
    if python == None:
        fail("python3 is required to unpack Daheng's self-extracting SDK payload")

    result = repository_ctx.execute([
        python,
        repository_ctx.path(repository_ctx.attr._extractor),
        installer,
        repository_ctx.path("."),
    ])
    if result.return_code != 0:
        fail("failed to extract Daheng SDK payload:\n%s\n%s" % (
            result.stdout,
            result.stderr,
        ))

    repository_ctx.delete(installer)
    repository_ctx.symlink(repository_ctx.attr._build_file, "BUILD.bazel")


daheng_galaxy_sdk_repository = repository_rule(
    implementation = _daheng_galaxy_sdk_repository_impl,
    attrs = {
        "sha256": attr.string(default = _GALAXY_SDK_SHA256),
        "strip_prefix": attr.string(default = _GALAXY_SDK_STRIP_PREFIX),
        "urls": attr.string_list(default = _GALAXY_SDK_URLS),
        "_build_file": attr.label(
            allow_single_file = True,
            default = "//third_party/daheng:galaxy_sdk.BUILD.bazel",
        ),
        "_extractor": attr.label(
            allow_single_file = True,
            default = "//third_party/daheng:extract_sdk.py",
        ),
    },
    doc = "Downloads and exposes the minimal Galaxy SDK surface used by capture.",
)
