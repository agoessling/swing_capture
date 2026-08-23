#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_CONFIGURATION_RECOVERY_JOURNAL_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_CONFIGURATION_RECOVERY_JOURNAL_H_

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace swing_capture::android::dual_hil {

enum class ConfigurationRecoveryState { kPrepared, kRestored };

struct ConfigurationSnapshotReference {
  std::string role;
  std::string serial;
  bool original_existed = false;
  std::string backup_relative_path;
  std::string sha256;
  std::uint64_t byte_count = 0;

  bool operator==(const ConfigurationSnapshotReference &) const = default;
};

struct ConfigurationRecoveryJournal {
  std::string transaction_id;
  std::uint64_t created_at_epoch_ms = 0;
  std::string package_name;
  ConfigurationRecoveryState state = ConfigurationRecoveryState::kPrepared;
  std::vector<ConfigurationSnapshotReference> snapshots;

  bool operator==(const ConfigurationRecoveryJournal &) const = default;
};

struct ConfigurationRecoveryCommand {
  std::string role;
  std::string purpose;
  std::vector<std::string> adb_arguments;
  std::vector<int> accepted_exit_codes = {0};
  std::optional<std::string> expected_sha256;

  bool operator==(const ConfigurationRecoveryCommand &) const = default;
};

nlohmann::json SerializeConfigurationRecoveryJournal(const ConfigurationRecoveryJournal &journal);
ConfigurationRecoveryJournal ParseConfigurationRecoveryJournal(const nlohmann::json &value);

std::vector<ConfigurationRecoveryCommand> BuildConfigurationRestorePlan(
    const ConfigurationRecoveryJournal &journal);
std::vector<ConfigurationRecoveryCommand> BuildConfigurationBackupRetirementPlan(
    const ConfigurationRecoveryJournal &journal);

void WriteConfigurationRecoveryJournalAtomically(const std::filesystem::path &path,
                                                 const ConfigurationRecoveryJournal &journal);
ConfigurationRecoveryJournal ReadConfigurationRecoveryJournal(const std::filesystem::path &path);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_CONFIGURATION_RECOVERY_JOURNAL_H_
