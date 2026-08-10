#include "capture/application/session_catalog.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

namespace {

using swing_capture::application::DiscoverPublishedSessions;
using swing_capture::application::FindPublishedSession;

std::filesystem::path TestRoot() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *temporary = std::getenv("TEST_TMPDIR");
  assert(temporary != nullptr);
  return std::filesystem::path(temporary) / "catalog";
}

void WriteSession(const std::filesystem::path &root, const std::string &session_id,
                  const std::string &created_at) {
  const std::filesystem::path directory = root / session_id;
  std::filesystem::create_directories(directory);
  std::ofstream(directory / "down_the_line.webm", std::ios::binary) << "down";
  std::ofstream(directory / "face_on.webm", std::ios::binary) << "face";
  std::ofstream(directory / "manifest.json")
      << "{\"schema_version\":1,\"session_id\":\"" << session_id << "\",\"created_at_utc\":\""
      << created_at
      << "\",\"views\":["
         "{\"role\":\"down_the_line\",\"media\":{\"path\":\"down_the_line.webm\","
         "\"mime_type\":\"video/webm\",\"encoded_bytes\":4}},"
         "{\"role\":\"face_on\",\"media\":{\"path\":\"face_on.webm\","
         "\"mime_type\":\"video/webm\",\"encoded_bytes\":4}}]}";
}

void DiscoversCompleteSessionsAcrossProcessRestarts() {
  const std::filesystem::path root = TestRoot();
  std::filesystem::create_directories(root);
  WriteSession(root, "session-old", "2026-08-09T10:00:00Z");
  WriteSession(root, "session-new", "2026-08-09T11:00:00Z");

  const auto sessions = DiscoverPublishedSessions(root);
  assert(sessions.size() == 2);
  assert(sessions.at(0).session_id == "session-new");
  assert(sessions.at(1).session_id == "session-old");
  const auto found = FindPublishedSession(root, "session-old");
  assert(found.has_value());
  assert(found->media_paths.at(0).filename() == "down_the_line.webm");
  assert(found->media_paths.at(1).filename() == "face_on.webm");
}

void IgnoresPartialMalformedAndLinkedSessions() {
  const std::filesystem::path root = TestRoot();
  const std::filesystem::path pending = root / ".session.pending-1";
  std::filesystem::create_directories(pending);
  std::ofstream(pending / "manifest.json") << "{}";
  const std::filesystem::path malformed = root / "malformed";
  std::filesystem::create_directories(malformed);
  std::ofstream(malformed / "manifest.json") << "not-json";
  const std::filesystem::path outside = root / "outside";
  WriteSession(root, "outside", "2026-08-09T12:00:00Z");
  WriteSession(root, "invalid-time", "2026-99-09T12:00:00Z");
  std::filesystem::create_directory_symlink(outside, root / "linked-session");

  const auto sessions = DiscoverPublishedSessions(root);
  assert(sessions.size() == 3);
  assert(!FindPublishedSession(root, "../outside").has_value());
  assert(!FindPublishedSession(root, "linked-session").has_value());
}

void CachedCatalogRefreshesOnlyWhenPublishedStateChanges() {
  const std::filesystem::path root = TestRoot() / "cached";
  swing_capture::application::PublishedSessionCatalog catalog(root);
  assert(catalog.Sessions().empty());
  WriteSession(root, "session-cached", "2026-08-09T13:00:00Z");
  assert(catalog.Sessions().empty());
  catalog.Refresh();
  assert(catalog.Sessions().size() == 1);
  assert(catalog.Find("session-cached").has_value());
}

}  // namespace

int main() {
  DiscoversCompleteSessionsAcrossProcessRestarts();
  IgnoresPartialMalformedAndLinkedSessions();
  CachedCatalogRefreshesOnlyWhenPublishedStateChanges();
  return 0;
}
