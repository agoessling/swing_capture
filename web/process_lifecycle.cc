#include "web/process_lifecycle.h"

#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cerrno>

namespace swing_capture::web {

bool ArmParentDeathSignal(pid_t expected_parent) {
  if (expected_parent <= 1) {
    errno = EINVAL;
    return false;
  }
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
    return false;
  }
  if (getppid() != expected_parent) {
    errno = ESRCH;
    return false;
  }
  return true;
}

}  // namespace swing_capture::web
