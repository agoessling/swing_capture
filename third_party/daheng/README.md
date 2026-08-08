# Daheng Galaxy SDK Bazel adapter

This package owns the nonstandard details required to consume Daheng Galaxy
Linux SDK `2.6.2606.9251`. The main `MODULE.bazel` only instantiates
`daheng_galaxy_sdk_repository`; vendor URLs, checksums, archive layout, and the
generated external-repository targets stay here.

## Fetch and extraction

`repository.bzl` downloads the vendor archive with a pinned SHA-256. Daheng
ships the SDK payload as a gzip-compressed tar archive appended to
`Galaxy_camera.run`. `extract_sdk.py` finds that payload and streams only an
explicit allowlist of headers, shared libraries, and the USB GenTL producer
into the external repository. It never executes the vendor installer and it
never performs a general tar extraction.

The repository rule uses the host's `python3` and its standard library for this
bootstrap-only extraction step. The downloaded bytes are checksum-pinned, and
the extractor has a hermetic Bazel test with synthetic installer fixtures.

`galaxy_sdk.BUILD.bazel` makes the result look like an ordinary Bazel C++
dependency and explicitly constrains the binary artifacts to Linux/x86-64:

- `@daheng_galaxy_sdk//:gxiapi` provides headers and the two shared libraries.
- `@daheng_galaxy_sdk//:gentl_producers` provides `GxU3VTL.cti` as runtime data.

The capture adapter resolves the CTI through Bazel's C++ runfiles library.
There are no generated headers containing Bazel cache paths and no system SDK
installation is required.

## Updating the SDK

Update the URL, SHA-256, and strip prefix together in `repository.bzl`, then
review `REQUIRED_MEMBERS` in `extract_sdk.py` and the targets in
`galaxy_sdk.BUILD.bazel`. Validate both extraction and runtime packaging with:

```bash
bazel test //third_party/daheng:extract_sdk_test
bazel test //capture/daheng:daheng_sdk_runtime_test
bazel build //capture/daheng:camera_probe
```
