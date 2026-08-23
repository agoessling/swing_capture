package com.agoessling.swingcapture;

import java.nio.file.Files;
import java.nio.file.Path;
import javax.xml.parsers.DocumentBuilderFactory;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

/** Locks the ordinary-app reboot policy to the packaged Android manifest. */
public final class AndroidManifestPolicyTest {
  private static final String ANDROID_NAMESPACE = "http://schemas.android.com/apk/res/android";

  private AndroidManifestPolicyTest() {}

  public static void main(String[] arguments) throws Exception {
    Path manifest = runfile("android/app/AndroidManifest.xml");
    DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
    factory.setNamespaceAware(true);
    Element root = factory.newDocumentBuilder().parse(manifest.toFile()).getDocumentElement();
    NodeList sdkDeclarations = root.getElementsByTagName("uses-sdk");
    check(sdkDeclarations.getLength() == 1, "exactly one API floor");
    Element sdk = (Element) sdkDeclarations.item(0);
    check(
        sdk.getAttributeNS(ANDROID_NAMESPACE, "minSdkVersion").equals("34"),
        "manifest API floor matches runtime policy");
    boolean requiredOpenGlEs31 = false;
    NodeList features = root.getElementsByTagName("uses-feature");
    for (int index = 0; index < features.getLength(); ++index) {
      Element feature = (Element) features.item(index);
      if (feature.getAttributeNS(ANDROID_NAMESPACE, "glEsVersion").equals("0x00030001")
          && feature.getAttributeNS(ANDROID_NAMESPACE, "required").equals("true")) {
        requiredOpenGlEs31 = true;
      }
    }
    check(requiredOpenGlEs31, "manifest GLES floor matches runtime policy");
    boolean bootPermission = false;
    NodeList permissions = root.getElementsByTagName("uses-permission");
    for (int index = 0; index < permissions.getLength(); ++index) {
      Element permission = (Element) permissions.item(index);
      if (permission
          .getAttributeNS(ANDROID_NAMESPACE, "name")
          .equals("android.permission.RECEIVE_BOOT_COMPLETED")) {
        bootPermission = true;
      }
    }
    check(bootPermission, "boot-marker receiver permission");
    NodeList applications = root.getElementsByTagName("application");
    check(applications.getLength() == 1, "exactly one application declaration");
    Element application = (Element) applications.item(0);
    check(
        application.getAttributeNS(ANDROID_NAMESPACE, "directBootAware").equals("false"),
        "application explicitly remains outside direct boot");

    NodeList services = application.getElementsByTagName("service");
    check(services.getLength() == 1, "exactly one capture service");
    Element service = (Element) services.item(0);
    check(
        service.getAttributeNS(ANDROID_NAMESPACE, "name").equals(".CaptureForegroundService"),
        "capture service identity");
    check(
        service.getAttributeNS(ANDROID_NAMESPACE, "directBootAware").equals("false"),
        "capture service cannot run before first unlock");
    check(
        service.getAttributeNS(ANDROID_NAMESPACE, "foregroundServiceType")
            .equals("camera|microphone"),
        "while-in-use foreground service types remain explicit");
    NodeList receivers = application.getElementsByTagName("receiver");
    check(receivers.getLength() == 1, "exactly one boot-marker receiver");
    Element receiver = (Element) receivers.item(0);
    check(
        receiver.getAttributeNS(ANDROID_NAMESPACE, "name").equals(".StationBootReceiver"),
        "boot-marker receiver identity");
    check(
        receiver.getAttributeNS(ANDROID_NAMESPACE, "directBootAware").equals("true"),
        "boot marker can observe the locked-boot boundary");
    check(
        receiver.getAttributeNS(ANDROID_NAMESPACE, "exported").equals("false"),
        "boot marker is not an exported app endpoint");
    java.util.Set<String> actions = new java.util.HashSet<>();
    NodeList actionElements = receiver.getElementsByTagName("action");
    for (int index = 0; index < actionElements.getLength(); ++index) {
      actions.add(
          ((Element) actionElements.item(index)).getAttributeNS(ANDROID_NAMESPACE, "name"));
    }
    check(
        actions.equals(
            java.util.Set.of(
                UnattendedRecoveryPolicy.ACTION_LOCKED_BOOT_COMPLETED,
                UnattendedRecoveryPolicy.ACTION_BOOT_COMPLETED,
                UnattendedRecoveryPolicy.ACTION_USER_UNLOCKED)),
        "receiver observes only the explicit boot lifecycle actions");
  }

  private static Path runfile(String relativePath) {
    String directory = System.getenv("TEST_SRCDIR");
    String workspace = System.getenv("TEST_WORKSPACE");
    if (directory == null || workspace == null) {
      throw new IllegalStateException("Bazel runfiles environment is unavailable");
    }
    Path path = Path.of(directory, workspace, relativePath);
    if (!Files.isRegularFile(path)) {
      throw new IllegalStateException("Manifest runfile is unavailable");
    }
    return path;
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
