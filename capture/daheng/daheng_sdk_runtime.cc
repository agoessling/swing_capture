#include "capture/daheng/daheng_sdk_runtime.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "tools/cpp/runfiles/runfiles.h"

namespace swing_capture::daheng {
namespace {

using bazel::tools::cpp::runfiles::Runfiles;

constexpr std::string_view kGentlProducerRunfile =
    "daheng_galaxy_sdk/Galaxy_camera/lib/x86_64/GxU3VTL.cti";

std::optional<std::filesystem::path> ResolveGentlProducer(std::unique_ptr<Runfiles> runfiles,
                                                          std::string *error) {
  if (runfiles == nullptr) {
    return std::nullopt;
  }

  const std::string producer_runfile(kGentlProducerRunfile);
  const std::string resolved = runfiles->Rlocation(producer_runfile);
  if (resolved.empty()) {
    if (error != nullptr) {
      *error = "the runfiles manifest did not resolve " + producer_runfile;
    }
    return std::nullopt;
  }

  std::filesystem::path producer(resolved);
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(producer, filesystem_error)) {
    if (error != nullptr) {
      *error = "the resolved GenTL producer is not a regular file: " + producer.string();
      if (filesystem_error) {
        *error += " (" + filesystem_error.message() + ")";
      }
    }
    return std::nullopt;
  }
  return producer;
}

}  // namespace

std::optional<std::string> FindBundledGentlDirectory(std::string *error) {
  if (error != nullptr) {
    error->clear();
  }

  std::error_code executable_error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", executable_error);

  std::string create_error;
  if (!executable_error) {
    std::unique_ptr<Runfiles> runfiles(
        Runfiles::Create(executable.string(), BAZEL_CURRENT_REPOSITORY, &create_error));
    if (const auto producer = ResolveGentlProducer(std::move(runfiles), error)) {
      return producer->parent_path().string();
    }
  }

  std::string test_create_error;
  std::unique_ptr<Runfiles> test_runfiles(
      Runfiles::CreateForTest(BAZEL_CURRENT_REPOSITORY, &test_create_error));
  if (const auto producer = ResolveGentlProducer(std::move(test_runfiles), error)) {
    return producer->parent_path().string();
  }

  if (error != nullptr && error->empty()) {
    *error = "could not initialize Bazel runfiles";
    if (executable_error) {
      *error += "; /proc/self/exe: " + executable_error.message();
    }
    if (!create_error.empty()) {
      *error += "; binary lookup: " + create_error;
    }
    if (!test_create_error.empty()) {
      *error += "; test lookup: " + test_create_error;
    }
  }
  return std::nullopt;
}

}  // namespace swing_capture::daheng
