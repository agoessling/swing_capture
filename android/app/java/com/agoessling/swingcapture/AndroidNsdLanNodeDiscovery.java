package com.agoessling.swingcapture;

import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.util.Log;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;

/** Android NSD/mDNS advertisement and discovery adapter. TXT metadata is an untrusted hint. */
final class AndroidNsdLanNodeDiscovery implements LanNodeDiscovery {
  static final String SERVICE_TYPE = "_swing-capture._tcp.";
  private static final String TAG = "SwingCaptureNsd";
  private static final long OBSERVATION_LIFETIME_MILLIS = 30_000;

  private final NsdManager manager;
  private final Executor executor;
  private final LanNodeDiscoveryRegistry registry;
  private final AtomicBoolean started = new AtomicBoolean();
  private volatile Advertisement advertisement;
  private volatile NsdManager.RegistrationListener registrationListener;

  AndroidNsdLanNodeDiscovery(
      NsdManager manager, Executor executor, LanNodeDiscoveryRegistry registry) {
    this.manager = Objects.requireNonNull(manager, "manager");
    this.executor = Objects.requireNonNull(executor, "executor");
    this.registry = Objects.requireNonNull(registry, "registry");
  }

  @Override
  public synchronized void start(Advertisement initialAdvertisement) {
    advertisement = Objects.requireNonNull(initialAdvertisement, "advertisement");
    if (!started.compareAndSet(false, true)) {
      updateAdvertisement(initialAdvertisement);
      return;
    }
    register(initialAdvertisement);
    manager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener);
  }

  @Override
  public synchronized void updateAdvertisement(Advertisement nextAdvertisement) {
    advertisement = Objects.requireNonNull(nextAdvertisement, "advertisement");
    if (!started.get()) {
      return;
    }
    unregister();
    register(nextAdvertisement);
  }

  @Override
  public List<Observation> snapshot(long nowEpochMillis) {
    return registry.snapshot(nowEpochMillis);
  }

  @Override
  public synchronized void close() {
    if (!started.compareAndSet(true, false)) {
      return;
    }
    unregister();
    try {
      manager.stopServiceDiscovery(discoveryListener);
    } catch (IllegalArgumentException | IllegalStateException alreadyStopped) {
      Log.w(TAG, "NSD discovery was already stopped", alreadyStopped);
    }
  }

  private void register(Advertisement value) {
    NsdServiceInfo service = new NsdServiceInfo();
    service.setServiceName("Swing Capture " + value.nodeId().substring(0, Math.min(8, value.nodeId().length())));
    service.setServiceType(SERVICE_TYPE);
    service.setPort(value.port());
    service.setAttribute("schema", "1");
    service.setAttribute("node_id", value.nodeId());
    service.setAttribute("role", value.role());
    service.setAttribute("label", value.label());
    NsdManager.RegistrationListener listener = new RegistrationCallbacks();
    registrationListener = listener;
    manager.registerService(service, NsdManager.PROTOCOL_DNS_SD, executor, listener);
  }

  private void unregister() {
    NsdManager.RegistrationListener listener = registrationListener;
    registrationListener = null;
    if (listener == null) {
      return;
    }
    try {
      manager.unregisterService(listener);
    } catch (IllegalArgumentException | IllegalStateException alreadyStopped) {
      Log.w(TAG, "NSD advertisement was already stopped", alreadyStopped);
    }
  }

  private final NsdManager.DiscoveryListener discoveryListener =
      new NsdManager.DiscoveryListener() {
        @Override
        public void onDiscoveryStarted(String serviceType) {}

        @Override
        public void onServiceFound(NsdServiceInfo service) {
          if (!SERVICE_TYPE.equals(service.getServiceType())) {
            return;
          }
          manager.resolveService(
              service,
              executor,
              new NsdManager.ResolveListener() {
                @Override
                public void onResolveFailed(NsdServiceInfo unresolved, int errorCode) {
                  Log.w(TAG, "NSD resolve failed: " + errorCode);
                }

                @Override
                public void onServiceResolved(NsdServiceInfo resolved) {
                  acceptResolved(resolved);
                }
              });
        }

        @Override
        public void onServiceLost(NsdServiceInfo service) {
          registry.lost(service.getServiceName());
        }

        @Override
        public void onDiscoveryStopped(String serviceType) {}

        @Override
        public void onStartDiscoveryFailed(String serviceType, int errorCode) {
          Log.w(TAG, "NSD discovery start failed: " + errorCode);
        }

        @Override
        public void onStopDiscoveryFailed(String serviceType, int errorCode) {
          Log.w(TAG, "NSD discovery stop failed: " + errorCode);
        }
      };

  private void acceptResolved(NsdServiceInfo service) {
    try {
      Map<String, byte[]> attributes = service.getAttributes();
      if (!"1".equals(attribute(attributes, "schema"))) {
        return;
      }
      List<InetAddress> addresses = service.getHostAddresses();
      if (addresses.isEmpty() || service.getPort() < 1) {
        return;
      }
      InetAddress host = LanDiscoveryAddress.select(addresses);
      String origin = LanDiscoveryAddress.httpOrigin(host, service.getPort());
      long observedAt = System.currentTimeMillis();
      registry.observed(
          new Observation(
              service.getServiceName(),
              attribute(attributes, "node_id"),
              attribute(attributes, "role"),
              attribute(attributes, "label"),
              origin,
              observedAt,
              Math.addExact(observedAt, OBSERVATION_LIFETIME_MILLIS)));
    } catch (RuntimeException malformedAdvertisement) {
      Log.w(TAG, "Ignoring malformed NSD advertisement", malformedAdvertisement);
    }
  }

  private static String attribute(Map<String, byte[]> attributes, String name) {
    byte[] value = attributes.get(name);
    return value == null ? "" : new String(value, StandardCharsets.UTF_8);
  }

  private static final class RegistrationCallbacks implements NsdManager.RegistrationListener {
    @Override
    public void onServiceRegistered(NsdServiceInfo serviceInfo) {}

    @Override
    public void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode) {
      Log.w(TAG, "NSD registration failed: " + errorCode);
    }

    @Override
    public void onServiceUnregistered(NsdServiceInfo serviceInfo) {}

    @Override
    public void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode) {
      Log.w(TAG, "NSD unregistration failed: " + errorCode);
    }
  }
}
