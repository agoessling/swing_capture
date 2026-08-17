package com.agoessling.swingcapture;

public final class StaticWebAssetRouteTest {
  private StaticWebAssetRouteTest() {}

  public static void main(String[] arguments) {
    rootAndIndexResolveToDocument();
    assetsReceiveCorrectMetadata();
    extensionlessRoutesUseSpaFallback();
    apiAndUnsafePathsAreRejected();
  }

  private static void rootAndIndexResolveToDocument() {
    assertRoute("/", "index.html", "text/html; charset=utf-8", "no-store");
    assertRoute("/index.html", "index.html", "text/html; charset=utf-8", "no-store");
    assertRoute("//", "index.html", "text/html; charset=utf-8", "no-store");
  }

  private static void assetsReceiveCorrectMetadata() {
    assertRoute("/app.js", "app.js", "text/javascript; charset=utf-8", "no-cache");
    assertRoute("/app.css", "app.css", "text/css; charset=utf-8", "no-cache");
    assertRoute(
        "/assets/logo.svg", "assets/logo.svg", "image/svg+xml", "public, max-age=86400");
    assertRoute(
        "/favicon.ico", "favicon.ico", "image/x-icon", "public, max-age=86400");
    assertRoute(
        "/assets/font.woff2", "assets/font.woff2", "font/woff2", "public, max-age=86400");
    assertRoute(
        "/assets/data.bin",
        "assets/data.bin",
        "application/octet-stream",
        "public, max-age=86400");
  }

  private static void extensionlessRoutesUseSpaFallback() {
    assertRoute("/review", "index.html", "text/html; charset=utf-8", "no-store");
    assertRoute("/sessions/latest", "index.html", "text/html; charset=utf-8", "no-store");
    assertRoute("/review%2Flatest", "index.html", "text/html; charset=utf-8", "no-store");
  }

  private static void apiAndUnsafePathsAreRejected() {
    assertRejected("/api");
    assertRejected("/api/v1/unknown");
    assertRejected("/../index.html");
    assertRejected("/%2e%2e/index.html");
    assertRejected("/assets%2f..%2findex.html");
    assertRejected("/assets\\index.html");
    assertRejected("/app.js%00.html");
    assertRejected("/%zz");
    assertRejected("/%c0%afindex.html");
    assertRejected("https://other.example/index.html");
  }

  private static void assertRoute(
      String path, String expectedAsset, String expectedContentType, String expectedCacheControl) {
    StaticWebAssetRoute.Result result = StaticWebAssetRoute.resolve(path);
    if (result == null) {
      throw new AssertionError("Expected a route for " + path);
    }
    assertEquals(expectedAsset, result.assetPath(), "asset path");
    assertEquals(expectedContentType, result.contentType(), "content type");
    assertEquals(expectedCacheControl, result.cacheControl(), "cache control");
  }

  private static void assertRejected(String path) {
    if (StaticWebAssetRoute.resolve(path) != null) {
      throw new AssertionError("Expected route rejection for " + path);
    }
  }

  private static void assertEquals(String expected, String actual, String label) {
    if (!expected.equals(actual)) {
      throw new AssertionError(label + ": expected " + expected + ", got " + actual);
    }
  }
}
