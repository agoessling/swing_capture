package com.agoessling.swingcapture;

import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.util.Comparator;
import java.util.List;

/** Deterministic NSD address selection and URI formatting independent of Android callbacks. */
final class LanDiscoveryAddress {
  private LanDiscoveryAddress() {}

  static InetAddress select(List<InetAddress> addresses) {
    return addresses.stream()
        .filter(LanDiscoveryAddress::isUsable)
        .min(
            Comparator.comparingInt(LanDiscoveryAddress::preference)
                .thenComparing(LanDiscoveryAddress::addressKey))
        .orElseThrow(() -> new IllegalArgumentException("NSD service has no usable host address"));
  }

  static String httpOrigin(InetAddress address, int port) {
    if (!isUsable(address) || port < 1 || port > 65_535) {
      throw new IllegalArgumentException("NSD service address or port is invalid");
    }
    String host = address.getHostAddress();
    if (address instanceof Inet6Address) {
      // RFC 6874 requires the zone delimiter in an IPv6 URI literal to be encoded as "%25".
      return "http://[" + host.replace("%", "%25") + "]:" + port;
    }
    return "http://" + host + ":" + port;
  }

  private static boolean isUsable(InetAddress address) {
    return address != null
        && (address instanceof Inet4Address || address instanceof Inet6Address)
        && !address.isAnyLocalAddress()
        && !address.isLoopbackAddress()
        && !address.isMulticastAddress();
  }

  private static int preference(InetAddress address) {
    if (address instanceof Inet4Address) {
      return 0;
    }
    return address.isLinkLocalAddress() ? 2 : 1;
  }

  private static String addressKey(InetAddress address) {
    StringBuilder key = new StringBuilder();
    for (byte value : address.getAddress()) {
      key.append(String.format("%02x", Byte.toUnsignedInt(value)));
    }
    return key.append(':').append(address.getHostAddress()).toString();
  }
}
