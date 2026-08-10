#ifndef SWING_CAPTURE_CAPTURE_APPLICATION_SESSION_CATALOG_H_
#define SWING_CAPTURE_CAPTURE_APPLICATION_SESSION_CATALOG_H_

#include <array>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::application {

struct PublishedSessionRecord {
  std::string session_id;
  std::string created_at_utc;
  std::filesystem::path manifest_path;
  std::array<std::filesystem::path, 2> media_paths;
};

// Discovers only complete, schema-compatible sessions published by the atomic
// session writer. Symlinks, pending directories, malformed manifests, and
// mismatched media metadata are ignored rather than exposed over HTTP.
[[nodiscard]] std::vector<PublishedSessionRecord> DiscoverPublishedSessions(
    const std::filesystem::path &root);

[[nodiscard]] std::optional<PublishedSessionRecord> FindPublishedSession(
    const std::filesystem::path &root, std::string_view session_id);

// Thread-safe process catalog. It scans once at startup and is refreshed by
// the application immediately after an atomic session publication, avoiding
// manifest parsing and file stats on the UI's one-second polling path.
class PublishedSessionCatalog final {
 public:
  explicit PublishedSessionCatalog(std::filesystem::path root);
  ~PublishedSessionCatalog() = default;

  PublishedSessionCatalog(const PublishedSessionCatalog &) = delete;
  PublishedSessionCatalog &operator=(const PublishedSessionCatalog &) = delete;
  PublishedSessionCatalog(PublishedSessionCatalog &&) = delete;
  PublishedSessionCatalog &operator=(PublishedSessionCatalog &&) = delete;

  void Refresh();
  [[nodiscard]] std::vector<PublishedSessionRecord> Sessions() const;
  [[nodiscard]] std::optional<PublishedSessionRecord> Find(std::string_view session_id) const;

 private:
  std::filesystem::path root_;
  mutable std::mutex mutex_;
  std::vector<PublishedSessionRecord> sessions_;
};

}  // namespace swing_capture::application

#endif  // SWING_CAPTURE_CAPTURE_APPLICATION_SESSION_CATALOG_H_
