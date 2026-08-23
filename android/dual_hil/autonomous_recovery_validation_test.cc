#include "android/dual_hil/autonomous_recovery_validation.h"

#include <cassert>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string Status(std::string autonomous_state, bool peer_available, int backlog,
                   std::string backlog_outcome, std::string phase = "monitoring",
                   std::string mode = "leader") {
  std::ostringstream status;
  status << "{\"state\":\"armed\",\"armed\":true,\"pose\":{"
         << "\"phase\":\"" << phase << "\",\"mode\":\"" << mode << "\",\"autonomous_pair\":{"
         << "\"state\":\"" << autonomous_state << "\","
         << "\"peer_available\":" << (peer_available ? "true" : "false") << ','
         << "\"recovered_from_checkpoint\":true,"
         << "\"active_shared_session_id\":null,"
         << "\"last_completed_shared_session_id\":\"swing-1\","
         << "\"last_outcome\":\"local_only\","
         << "\"replication_backlog_size\":" << backlog << ','
         << "\"last_backlog_shared_session_id\":\"swing-1\","
         << "\"last_backlog_outcome\":\"" << backlog_outcome << "\"}}}";
  return status.str();
}

template <typename Function>
void ExpectFailure(Function function) {
  try {
    function();
    assert(false);
  } catch (const std::runtime_error &) {
  }
}

}  // namespace

int main() {
  using swing_capture::android::dual_hil::AutonomousLocalOnlyRecoveryInspection;
  using swing_capture::android::dual_hil::AutonomousRecoveredStationInspection;
  using swing_capture::android::dual_hil::ValidateAutonomousLocalOnlyRecovery;
  using swing_capture::android::dual_hil::ValidateAutonomousPeerUnavailable;
  using swing_capture::android::dual_hil::ValidateAutonomousRecoveredStation;

  const std::string local_only = Status("degraded_monitoring", false, 1, "pending_IOException");
  ValidateAutonomousLocalOnlyRecovery(AutonomousLocalOnlyRecoveryInspection{
      .leader_status_json = local_only, .expected_shared_session_id = "swing-1"});
  ValidateAutonomousPeerUnavailable(local_only);

  const std::string leader = Status("monitoring", true, 0, "replicated");
  const std::string shadow = Status("stopped", false, 0, "none", "monitoring", "shadow");
  ValidateAutonomousRecoveredStation(
      AutonomousRecoveredStationInspection{.leader_status_json = leader,
                                           .shadow_status_json = shadow,
                                           .expected_shared_session_id = "swing-1"});

  ExpectFailure([&] {
    ValidateAutonomousLocalOnlyRecovery(
        {.leader_status_json = leader, .expected_shared_session_id = "swing-1"});
  });
  ExpectFailure([&] {
    ValidateAutonomousRecoveredStation({.leader_status_json = local_only,
                                        .shadow_status_json = shadow,
                                        .expected_shared_session_id = "swing-1"});
  });
  ExpectFailure([&] {
    ValidateAutonomousRecoveredStation({.leader_status_json = leader,
                                        .shadow_status_json = shadow,
                                        .expected_shared_session_id = "other-swing"});
  });
  ExpectFailure([&] { ValidateAutonomousPeerUnavailable(leader); });
  ExpectFailure([&] { ValidateAutonomousPeerUnavailable("not json"); });
  return 0;
}
