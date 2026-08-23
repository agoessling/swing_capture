#include "android/dual_hil/configuration_recovery_journal.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using swing_capture::android::dual_hil::BuildConfigurationBackupRetirementPlan;
using swing_capture::android::dual_hil::BuildConfigurationRestorePlan;
using swing_capture::android::dual_hil::ConfigurationRecoveryJournal;
using swing_capture::android::dual_hil::ConfigurationRecoveryState;
using swing_capture::android::dual_hil::ConfigurationSnapshotReference;
using swing_capture::android::dual_hil::ParseConfigurationRecoveryJournal;
using swing_capture::android::dual_hil::ReadConfigurationRecoveryJournal;
using swing_capture::android::dual_hil::SerializeConfigurationRecoveryJournal;
using swing_capture::android::dual_hil::WriteConfigurationRecoveryJournalAtomically;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error("configuration recovery journal assertion failed: " + message);
  }
}

template <typename Function>
void ExpectFailure(Function function, const std::string &message) {
  try {
    function();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error("expected configuration recovery failure: " + message);
}

ConfigurationRecoveryJournal Journal() {
  const std::string transaction = "dual_hil_20260822_001";
  return {
      .transaction_id = transaction,
      .created_at_epoch_ms = 1'787'424'000'000ULL,
      .package_name = "com.agoessling.swingcapture",
      .state = ConfigurationRecoveryState::kPrepared,
      .snapshots =
          {
              {
                  .role = "down_the_line",
                  .serial = "1A011JEG501717",
                  .original_existed = true,
                  .backup_relative_path =
                      "files/hil_recovery/" + transaction + "/down_the_line.xml",
                  .sha256 = std::string(64, 'a'),
                  .byte_count = 1'024,
              },
              {
                  .role = "face_on",
                  .serial = "22181FDF6005QH",
                  .original_existed = false,
                  .backup_relative_path = {},
                  .sha256 = {},
                  .byte_count = 0,
              },
          },
  };
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("swing_capture_recovery_journal_" + std::to_string(getpid()) + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(path_);
    std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
  }
  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  const std::filesystem::path &path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void RoundTripsWithoutSecrets() {
  const ConfigurationRecoveryJournal journal = Journal();
  const nlohmann::json encoded = SerializeConfigurationRecoveryJournal(journal);
  Check(ParseConfigurationRecoveryJournal(encoded) == journal, "journal round trip");
  Check(encoded.at("contains_secrets") == false, "explicit secret-free contract");
  const std::string rendered = encoded.dump();
  Check(!rendered.contains("control_token") && !rendered.contains("<map>"),
        "journal cannot contain preference contents");
}

void RejectsUnsafeOrIncompleteJournals() {
  ConfigurationRecoveryJournal duplicate = Journal();
  duplicate.snapshots[1].serial = duplicate.snapshots[0].serial;
  ExpectFailure([&] { SerializeConfigurationRecoveryJournal(duplicate); }, "duplicate serial");

  ConfigurationRecoveryJournal escaped = Journal();
  escaped.snapshots[0].backup_relative_path = "files/hil_recovery/../node_configuration.xml";
  ExpectFailure([&] { SerializeConfigurationRecoveryJournal(escaped); }, "escaped backup path");

  nlohmann::json injected = SerializeConfigurationRecoveryJournal(Journal());
  injected["preference_contents"] = "secret";
  ExpectFailure([&] { ParseConfigurationRecoveryJournal(injected); }, "unknown secret field");

  nlohmann::json secret_flag = SerializeConfigurationRecoveryJournal(Journal());
  secret_flag["contains_secrets"] = true;
  ExpectFailure([&] { ParseConfigurationRecoveryJournal(secret_flag); }, "secret-bearing journal");
}

void PlansIdempotentRestoreBeforeRetirement() {
  const ConfigurationRecoveryJournal journal = Journal();
  const auto commands = BuildConfigurationRestorePlan(journal);
  Check(commands.size() == 14U, "two force stops plus present/absent recovery commands");
  Check(commands[0].purpose == "force_stop_package" && commands[1].purpose == "force_stop_package",
        "both packages stop before either preference is changed");
  Check(commands[2].purpose == "verify_private_backup" &&
            commands[2].expected_sha256 == std::string(64, 'a'),
        "private backup hash verified before restore");
  Check(commands[9].purpose == "verify_restored_configuration" &&
            commands[9].expected_sha256 == std::string(64, 'a'),
        "published configuration hash verified");
  Check(commands[10].accepted_exit_codes == std::vector<int>{1},
        "staging absence is an explicit expected exit");
  Check(commands.back().purpose == "verify_original_absent" &&
            commands.back().accepted_exit_codes == std::vector<int>{1},
        "original absence is restored and verified");
  for (const auto &command : commands) {
    for (const std::string &argument : command.adb_arguments) {
      Check(!argument.contains("control_token") && !argument.contains("<map>"),
            "restore argv contains no preference contents");
    }
  }
  ExpectFailure([&] { BuildConfigurationBackupRetirementPlan(journal); },
                "prepared backups cannot retire");

  ConfigurationRecoveryJournal restored = journal;
  restored.state = ConfigurationRecoveryState::kRestored;
  const auto retirement = BuildConfigurationBackupRetirementPlan(restored);
  Check(retirement.size() == 1U && retirement[0].purpose == "remove_private_backup",
        "present backup retires idempotently after restored journal publish");
  ExpectFailure([&] { BuildConfigurationRestorePlan(restored); },
                "restored journal cannot replay mutation plan");
}

void AtomicallyReplacesProtectedJournal() {
  TemporaryDirectory directory;
  const std::filesystem::path path = directory.path() / "pending.json";
  const std::filesystem::path stale_temporary = directory.path() / "pending.json.tmp";
  {
    std::ofstream stale(stale_temporary);
    stale << "truncated";
  }
  const ConfigurationRecoveryJournal prepared = Journal();
  WriteConfigurationRecoveryJournalAtomically(path, prepared);
  Check(ReadConfigurationRecoveryJournal(path) == prepared, "prepared journal persisted");
  Check(!std::filesystem::exists(stale_temporary), "atomic temp consumed by publication");

  struct stat metadata{};
  Check(stat(path.c_str(), &metadata) == 0, "journal metadata readable");
  Check((metadata.st_mode & (S_IRWXG | S_IRWXO)) == 0, "journal is owner-only");

  ConfigurationRecoveryJournal restored = prepared;
  restored.state = ConfigurationRecoveryState::kRestored;
  WriteConfigurationRecoveryJournalAtomically(path, restored);
  Check(ReadConfigurationRecoveryJournal(path) == restored, "restored state atomically replaces");

  std::filesystem::permissions(path, std::filesystem::perms::group_read,
                               std::filesystem::perm_options::add);
  ExpectFailure([&] { ReadConfigurationRecoveryJournal(path); }, "permissive journal rejected");
}

}  // namespace

int main() {
  RoundTripsWithoutSecrets();
  RejectsUnsafeOrIncompleteJournals();
  PlansIdempotentRestoreBeforeRetirement();
  AtomicallyReplacesProtectedJournal();
  return 0;
}
