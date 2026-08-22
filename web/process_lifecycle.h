#pragma once

#include <sys/types.h>

namespace swing_capture::web {

// Arms SIGKILL delivery if expected_parent exits. Returns false when the
// relationship changed during setup or Linux rejected the request.
bool ArmParentDeathSignal(pid_t expected_parent);

}  // namespace swing_capture::web
