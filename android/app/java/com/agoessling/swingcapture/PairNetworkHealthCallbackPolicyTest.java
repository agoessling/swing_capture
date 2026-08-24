package com.agoessling.swingcapture;

import java.net.InetAddress;

public final class PairNetworkHealthCallbackPolicyTest {
  private PairNetworkHealthCallbackPolicyTest() {}

  public static void main(String[] arguments) throws Exception {
    InetAddress peer = InetAddress.getByAddress(new byte[] {(byte) 192, (byte) 168, 4, 20});
    check(
        PairNetworkHealthCallbackPolicy.validate("http://192.168.4.20:8088", peer, 8088)
            .toString()
            .equals("http://192.168.4.20:8088"),
        "exact accepted peer and listener port accepted");

    for (String rejected :
        new String[] {
          "http://192.168.4.21:8088",
          "http://192.168.4.20:8089",
          "http://phone.local:8088",
          "https://192.168.4.20:8088",
          "http://192.168.4.20:8088/path"
        }) {
      expectFailure(() -> PairNetworkHealthCallbackPolicy.validate(rejected, peer, 8088));
    }
    InetAddress ipv6 = InetAddress.getByName("2001:db8::1");
    expectFailure(
        () ->
            PairNetworkHealthCallbackPolicy.validate(
                "http://[2001:db8::1]:8088", ipv6, 8088));
  }

  private static void expectFailure(Runnable action) {
    try {
      action.run();
      throw new AssertionError("unsafe callback was accepted");
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
