#include <csignal>
#include <cstdlib>

#include "android/dual_hil/dual_android_hil_runner.h"

int main(int argument_count, char **arguments) {
  if (argument_count < 4 ||
      setenv("SWING_CAPTURE_ANDROID_APK_INSTALLER", arguments[argument_count - 1], 1) != 0) {
    return 2;
  }
  std::signal(SIGPIPE, SIG_IGN);
  return swing_capture::android::dual_hil::RunDualAndroidHil(argument_count - 1, arguments);
}
