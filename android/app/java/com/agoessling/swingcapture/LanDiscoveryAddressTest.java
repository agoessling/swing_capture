package com.agoessling.swingcapture;

import java.net.Inet6Address;
import java.net.InetAddress;
import java.util.List;

/** IPv4 preference, deterministic fallback, rejection, and URI encoding coverage. */
public final class LanDiscoveryAddressTest {
  private LanDiscoveryAddressTest() {}

  public static void main(String[] arguments) throws Exception {
    InetAddress ipv4 = InetAddress.getByName("10.168.168.241");
    InetAddress globalIpv6 = InetAddress.getByName("2001:db8::2");
    InetAddress linkLocalIpv6 = InetAddress.getByName("fe80::2");
    check(
        LanDiscoveryAddress.select(List.of(linkLocalIpv6, globalIpv6, ipv4)).equals(ipv4),
        "IPv4 preferred over every IPv6 address");
    check(
        LanDiscoveryAddress.select(List.of(linkLocalIpv6, globalIpv6)).equals(globalIpv6),
        "global IPv6 preferred over link-local IPv6");
    check(
        LanDiscoveryAddress.httpOrigin(ipv4, 8088).equals("http://10.168.168.241:8088"),
        "IPv4 origin");
    String globalIpv6Origin = LanDiscoveryAddress.httpOrigin(globalIpv6, 8088);
    check(
        globalIpv6Origin.startsWith("http://[") && globalIpv6Origin.endsWith("]:8088"),
        "IPv6 brackets");

    InetAddress scoped = Inet6Address.getByAddress(null, linkLocalIpv6.getAddress(), 7);
    String scopedOrigin = LanDiscoveryAddress.httpOrigin(scoped, 8088);
    check(scopedOrigin.contains("%25"), "IPv6 scope identifier is URI encoded: " + scopedOrigin);
    expectFailure(
        () -> LanDiscoveryAddress.select(List.of(InetAddress.getLoopbackAddress())),
        "loopback discovery");
  }

  private static void expectFailure(Runnable operation, String label) {
    try {
      operation.run();
      throw new AssertionError(label + " did not fail");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
