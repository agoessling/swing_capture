#include "android/dual_hil/configuration_recovery_journal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr std::string_view kJournalType = "android_dual_hil_configuration_recovery";
constexpr std::string_view kConfigurationPath = "shared_prefs/node_configuration.xml";
constexpr std::string_view kStagingPath = "shared_prefs/.node_configuration.xml.hil-recovery";
constexpr std::uintmax_t kMaximumJournalBytes = std::uintmax_t{64} * 1024U;

class FileDescriptor {
 public:
  explicit FileDescriptor(int value) : value_(value) {}
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&) = delete;
  FileDescriptor &operator=(FileDescriptor &&) = delete;
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(close(value_));
    }
  }

  [[nodiscard]] int get() const { return value_; }

 private:
  int value_;
};

[[noreturn]] void ThrowSystemError(std::string_view operation) {
  throw std::system_error(errno, std::generic_category(), std::string(operation));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool SafeCharacters(std::string_view value, std::string_view punctuation) {
  return !value.empty() && std::ranges::all_of(value, [&](unsigned char character) {
    return std::isalnum(character) != 0 || punctuation.contains(static_cast<char>(character));
  });
}

std::string StateName(ConfigurationRecoveryState state) {
  switch (state) {
    case ConfigurationRecoveryState::kPrepared:
      return "prepared";
    case ConfigurationRecoveryState::kRestored:
      return "restored";
  }
  throw std::logic_error("unknown configuration recovery state");
}

ConfigurationRecoveryState ParseState(const Json &value) {
  if (!value.is_string()) {
    throw std::invalid_argument("configuration recovery state must be a string");
  }
  const std::string state = value.get<std::string>();
  if (state == "prepared") {
    return ConfigurationRecoveryState::kPrepared;
  }
  if (state == "restored") {
    return ConfigurationRecoveryState::kRestored;
  }
  throw std::invalid_argument("unsupported configuration recovery state");
}

void RequireExactFields(const Json &value, const std::set<std::string> &fields,
                        std::string_view label) {
  if (!value.is_object() || value.size() != fields.size()) {
    throw std::invalid_argument(std::string(label) + " fields do not match schema 1");
  }
  for (const std::string &field : fields) {
    if (!value.contains(field)) {
      throw std::invalid_argument(std::string(label) + " is missing " + field);
    }
  }
}

std::string RequiredString(const Json &value, std::string_view field) {
  const Json &candidate = value.at(std::string(field));
  if (!candidate.is_string()) {
    throw std::invalid_argument(std::string(field) + " must be a string");
  }
  return candidate.get<std::string>();
}

std::uint64_t DecimalString(const Json &value, std::string_view field) {
  const std::string encoded = RequiredString(value, field);
  std::uint64_t result = 0;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char *encoded_end = encoded.data() + encoded.size();
  const auto [end, error] = std::from_chars(encoded.data(), encoded_end, result);
  if (error != std::errc() || end != encoded_end) {
    throw std::invalid_argument(std::string(field) + " must be a decimal uint64 string");
  }
  return result;
}

void ValidateSnapshot(const ConfigurationSnapshotReference &snapshot,
                      std::string_view transaction_id) {
  if (snapshot.role != "down_the_line" && snapshot.role != "face_on") {
    throw std::invalid_argument("configuration recovery role is unsupported");
  }
  if (!SafeCharacters(snapshot.serial, "._:-") || snapshot.serial.front() == '-' ||
      snapshot.serial.size() > 128U) {
    throw std::invalid_argument("configuration recovery serial is unsafe");
  }
  if (!snapshot.original_existed) {
    if (!snapshot.backup_relative_path.empty() || !snapshot.sha256.empty() ||
        snapshot.byte_count != 0) {
      throw std::invalid_argument("absent configuration must not reference a backup");
    }
    return;
  }
  const std::string expected_path =
      "files/hil_recovery/" + std::string(transaction_id) + "/" + snapshot.role + ".xml";
  if (snapshot.backup_relative_path != expected_path) {
    throw std::invalid_argument("configuration backup path is not transaction scoped");
  }
  if (snapshot.sha256.size() != 64U ||
      !std::ranges::all_of(snapshot.sha256, [](unsigned char character) {
        return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
      })) {
    throw std::invalid_argument("configuration backup SHA-256 is invalid");
  }
}

void ValidateJournal(const ConfigurationRecoveryJournal &journal) {
  if (!SafeCharacters(journal.transaction_id, "_-") || journal.transaction_id.size() > 80U) {
    throw std::invalid_argument("configuration recovery transaction ID is unsafe");
  }
  if (journal.created_at_epoch_ms == 0) {
    throw std::invalid_argument("configuration recovery creation time must be positive");
  }
  if (!SafeCharacters(journal.package_name, "._") || journal.package_name.front() == '.' ||
      journal.package_name.contains("..") || journal.package_name.size() > 200U) {
    throw std::invalid_argument("configuration recovery package name is unsafe");
  }
  if (journal.snapshots.size() != 2U) {
    throw std::invalid_argument("configuration recovery journal requires exactly two phones");
  }
  std::set<std::string> roles;
  std::set<std::string> serials;
  for (const ConfigurationSnapshotReference &snapshot : journal.snapshots) {
    ValidateSnapshot(snapshot, journal.transaction_id);
    roles.insert(snapshot.role);
    serials.insert(snapshot.serial);
  }
  if (roles != std::set<std::string>{"down_the_line", "face_on"} || serials.size() != 2U) {
    throw std::invalid_argument("configuration recovery phones must have distinct roles/serials");
  }
}

Json SnapshotJson(const ConfigurationSnapshotReference &snapshot) {
  Json backup = nullptr;
  if (snapshot.original_existed) {
    backup = {
        {"relative_path", snapshot.backup_relative_path},
        {"sha256", snapshot.sha256},
        {"byte_count", std::to_string(snapshot.byte_count)},
    };
  }
  return {
      {"role", snapshot.role},
      {"serial", snapshot.serial},
      {"original_configuration", snapshot.original_existed ? "present" : "absent"},
      {"backup", std::move(backup)},
  };
}

ConfigurationSnapshotReference ParseSnapshot(const Json &value, std::string_view transaction_id) {
  RequireExactFields(value, {"role", "serial", "original_configuration", "backup"},
                     "configuration recovery snapshot");
  ConfigurationSnapshotReference snapshot{
      .role = RequiredString(value, "role"),
      .serial = RequiredString(value, "serial"),
      .original_existed = false,
      .backup_relative_path = {},
      .sha256 = {},
      .byte_count = 0,
  };
  const std::string original = RequiredString(value, "original_configuration");
  if (original == "absent") {
    if (!value.at("backup").is_null()) {
      throw std::invalid_argument("absent configuration backup must be null");
    }
  } else if (original == "present") {
    snapshot.original_existed = true;
    const Json &backup = value.at("backup");
    RequireExactFields(backup, {"relative_path", "sha256", "byte_count"},
                       "configuration recovery backup");
    snapshot.backup_relative_path = RequiredString(backup, "relative_path");
    snapshot.sha256 = RequiredString(backup, "sha256");
    snapshot.byte_count = DecimalString(backup, "byte_count");
  } else {
    throw std::invalid_argument("original configuration state is unsupported");
  }
  ValidateSnapshot(snapshot, transaction_id);
  return snapshot;
}

std::vector<std::string> DeviceArguments(std::string_view serial,
                                         std::initializer_list<std::string> arguments) {
  std::vector<std::string> result = {"-s", std::string(serial)};
  result.insert(result.end(), arguments.begin(), arguments.end());
  return result;
}

ConfigurationRecoveryCommand Command(const ConfigurationSnapshotReference &snapshot,
                                     std::string purpose,
                                     std::initializer_list<std::string> arguments) {
  return {
      .role = snapshot.role,
      .purpose = std::move(purpose),
      .adb_arguments = DeviceArguments(snapshot.serial, arguments),
      .accepted_exit_codes = {0},
      .expected_sha256 = std::nullopt,
  };
}

void WriteAll(int descriptor, std::string_view contents) {
  while (!contents.empty()) {
    const ssize_t written = write(descriptor, contents.data(), contents.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowSystemError("write configuration recovery journal");
    }
    if (written == 0) {
      throw std::runtime_error("configuration recovery journal write made no progress");
    }
    contents.remove_prefix(static_cast<std::size_t>(written));
  }
}

void FsyncDirectory(const std::filesystem::path &directory) {
  const FileDescriptor descriptor(
      open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (descriptor.get() < 0) {
    ThrowSystemError("open configuration recovery journal directory");
  }
  if (fsync(descriptor.get()) != 0) {
    ThrowSystemError("fsync configuration recovery journal directory");
  }
}

void ValidatePrivateDirectory(const std::filesystem::path &directory) {
  struct stat metadata{};
  if (stat(directory.c_str(), &metadata) != 0) {
    ThrowSystemError("inspect configuration recovery journal directory");
  }
  if (!S_ISDIR(metadata.st_mode) || metadata.st_uid != geteuid() ||
      (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    throw std::runtime_error("configuration recovery journal directory is not owner-private");
  }
}

}  // namespace

Json SerializeConfigurationRecoveryJournal(const ConfigurationRecoveryJournal &journal) {
  ValidateJournal(journal);
  Json snapshots = Json::array();
  for (const ConfigurationSnapshotReference &snapshot : journal.snapshots) {
    snapshots.push_back(SnapshotJson(snapshot));
  }
  return {
      {"schema_version", 1},
      {"journal_type", kJournalType},
      {"contains_secrets", false},
      {"transaction_id", journal.transaction_id},
      {"created_at_epoch_ms", std::to_string(journal.created_at_epoch_ms)},
      {"package_name", journal.package_name},
      {"state", StateName(journal.state)},
      {"snapshots", std::move(snapshots)},
  };
}

ConfigurationRecoveryJournal ParseConfigurationRecoveryJournal(const Json &value) {
  RequireExactFields(value,
                     {"schema_version", "journal_type", "contains_secrets", "transaction_id",
                      "created_at_epoch_ms", "package_name", "state", "snapshots"},
                     "configuration recovery journal");
  if (value.at("schema_version") != 1 || value.at("journal_type") != kJournalType ||
      value.at("contains_secrets") != false || !value.at("snapshots").is_array()) {
    throw std::invalid_argument("configuration recovery journal header is invalid");
  }
  ConfigurationRecoveryJournal journal{
      .transaction_id = RequiredString(value, "transaction_id"),
      .created_at_epoch_ms = DecimalString(value, "created_at_epoch_ms"),
      .package_name = RequiredString(value, "package_name"),
      .state = ParseState(value.at("state")),
      .snapshots = {},
  };
  for (const Json &snapshot : value.at("snapshots")) {
    journal.snapshots.push_back(ParseSnapshot(snapshot, journal.transaction_id));
  }
  ValidateJournal(journal);
  return journal;
}

std::vector<ConfigurationRecoveryCommand> BuildConfigurationRestorePlan(
    const ConfigurationRecoveryJournal &journal) {
  ValidateJournal(journal);
  if (journal.state != ConfigurationRecoveryState::kPrepared) {
    throw std::logic_error("only a prepared configuration journal needs restoration");
  }
  std::vector<ConfigurationRecoveryCommand> commands;
  commands.reserve(2U + journal.snapshots.size() * 10U);
  for (const ConfigurationSnapshotReference &snapshot : journal.snapshots) {
    commands.push_back(Command(snapshot, "force_stop_package",
                               {"shell", "am", "force-stop", journal.package_name}));
  }
  for (const ConfigurationSnapshotReference &snapshot : journal.snapshots) {
    if (!snapshot.original_existed) {
      commands.push_back(Command(snapshot, "remove_original_and_staging",
                                 {"shell", "run-as", journal.package_name, "rm", "-f",
                                  std::string(kConfigurationPath), std::string(kStagingPath)}));
      commands.push_back(Command(snapshot, "sync_absent_restore", {"shell", "sync"}));
      ConfigurationRecoveryCommand verify = Command(
          snapshot, "verify_original_absent",
          {"shell", "run-as", journal.package_name, "test", "-e", std::string(kConfigurationPath)});
      verify.accepted_exit_codes = {1};
      commands.push_back(std::move(verify));
      continue;
    }
    ConfigurationRecoveryCommand verify_backup = Command(
        snapshot, "verify_private_backup",
        {"exec-out", "run-as", journal.package_name, "sha256sum", snapshot.backup_relative_path});
    verify_backup.expected_sha256 = snapshot.sha256;
    commands.push_back(std::move(verify_backup));
    commands.push_back(
        Command(snapshot, "remove_restore_staging",
                {"shell", "run-as", journal.package_name, "rm", "-f", std::string(kStagingPath)}));
    commands.push_back(Command(snapshot, "copy_private_backup_to_staging",
                               {"shell", "run-as", journal.package_name, "cp",
                                snapshot.backup_relative_path, std::string(kStagingPath)}));
    commands.push_back(Command(
        snapshot, "protect_restore_staging",
        {"shell", "run-as", journal.package_name, "chmod", "0600", std::string(kStagingPath)}));
    commands.push_back(Command(snapshot, "sync_restore_staging", {"shell", "sync"}));
    commands.push_back(Command(snapshot, "publish_restored_configuration",
                               {"shell", "run-as", journal.package_name, "mv", "-f",
                                std::string(kStagingPath), std::string(kConfigurationPath)}));
    commands.push_back(Command(snapshot, "sync_restored_configuration", {"shell", "sync"}));
    ConfigurationRecoveryCommand verify_configuration = Command(
        snapshot, "verify_restored_configuration",
        {"exec-out", "run-as", journal.package_name, "sha256sum", std::string(kConfigurationPath)});
    verify_configuration.expected_sha256 = snapshot.sha256;
    commands.push_back(std::move(verify_configuration));
    ConfigurationRecoveryCommand verify_staging =
        Command(snapshot, "verify_restore_staging_absent",
                {"shell", "run-as", journal.package_name, "test", "-e", std::string(kStagingPath)});
    verify_staging.accepted_exit_codes = {1};
    commands.push_back(std::move(verify_staging));
  }
  return commands;
}

std::vector<ConfigurationRecoveryCommand> BuildConfigurationBackupRetirementPlan(
    const ConfigurationRecoveryJournal &journal) {
  ValidateJournal(journal);
  if (journal.state != ConfigurationRecoveryState::kRestored) {
    throw std::logic_error("configuration backups cannot retire before restoration is durable");
  }
  std::vector<ConfigurationRecoveryCommand> commands;
  for (const ConfigurationSnapshotReference &snapshot : journal.snapshots) {
    if (!snapshot.original_existed) {
      continue;
    }
    commands.push_back(Command(
        snapshot, "remove_private_backup",
        {"shell", "run-as", journal.package_name, "rm", "-f", snapshot.backup_relative_path}));
  }
  return commands;
}

void WriteConfigurationRecoveryJournalAtomically(const std::filesystem::path &path,
                                                 const ConfigurationRecoveryJournal &journal) {
  if (path.empty() || path.filename().empty() || path.parent_path().empty()) {
    throw std::invalid_argument("configuration recovery journal parent directory is invalid");
  }
  ValidatePrivateDirectory(path.parent_path());
  const std::string contents = SerializeConfigurationRecoveryJournal(journal).dump(2) + "\n";
  const std::filesystem::path temporary = path.string() + ".tmp";
  const FileDescriptor descriptor(open(
      temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
  if (descriptor.get() < 0) {
    ThrowSystemError("open temporary configuration recovery journal");
  }
  if (fchmod(descriptor.get(), S_IRUSR | S_IWUSR) != 0) {
    ThrowSystemError("protect temporary configuration recovery journal");
  }
  WriteAll(descriptor.get(), contents);
  if (fsync(descriptor.get()) != 0) {
    ThrowSystemError("fsync temporary configuration recovery journal");
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, path, rename_error);
  if (rename_error) {
    throw std::system_error(rename_error, "publish configuration recovery journal");
  }
  FsyncDirectory(path.parent_path());
}

ConfigurationRecoveryJournal ReadConfigurationRecoveryJournal(const std::filesystem::path &path) {
  if (path.empty() || path.filename().empty() || path.parent_path().empty()) {
    throw std::invalid_argument("configuration recovery journal path is invalid");
  }
  ValidatePrivateDirectory(path.parent_path());
  const FileDescriptor descriptor(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (descriptor.get() < 0) {
    ThrowSystemError("open configuration recovery journal");
  }
  struct stat metadata{};
  if (fstat(descriptor.get(), &metadata) != 0) {
    ThrowSystemError("inspect configuration recovery journal");
  }
  if (!S_ISREG(metadata.st_mode) || (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
      metadata.st_size <= 0 ||
      std::cmp_greater(static_cast<std::uintmax_t>(metadata.st_size), kMaximumJournalBytes)) {
    throw std::runtime_error("configuration recovery journal file is unsafe");
  }
  std::string contents(static_cast<std::size_t>(metadata.st_size), '\0');
  std::size_t offset = 0;
  while (offset < contents.size()) {
    char *destination = &contents.at(offset);
    const ssize_t count = read(descriptor.get(), destination, contents.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowSystemError("read configuration recovery journal");
    }
    if (count == 0) {
      throw std::runtime_error("configuration recovery journal was truncated while reading");
    }
    offset += static_cast<std::size_t>(count);
  }
  return ParseConfigurationRecoveryJournal(Json::parse(contents));
}

}  // namespace swing_capture::android::dual_hil
