# Camera setup web slice

This package is a static React and TypeScript UI for station camera setup. The
production bundle uses the versioned `/api/v1` HTTP contract in `src/api.ts`.
The fixture bundle injects `FakeStationApi` and uses deterministic preview
artwork, so UI work does not require a camera or the Galaxy SDK.

Build and test with Bazel:

```bash
bazel build //web:static_app //web:fixture_demo
bazel test //web:component_test
```

`bazel-bin/web/static_app/` contains the deployable production asset tree.
`bazel-bin/web/fixture_demo/` contains a directly viewable fixture asset tree.
`//capture/service:preview_server` embeds the production tree in its Bazel
runfiles and serves it together with the `/api/v1` station endpoints.

The component test covers status rendering, image refresh URLs, apply/revert
behavior, disconnected controls, the HTTP paths and request shape, schema
rejection, and an axe semantic-accessibility scan. Browser screenshots are not
yet a Bazel target: the repository does not currently pin a Playwright browser
toolchain, and using a host-installed browser would make the test
non-hermetic.
