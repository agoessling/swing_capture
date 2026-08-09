#include "station/hardware_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace swing_capture::station {
namespace {

std::string ErrorMessage(int error_number) {
  return std::error_code(error_number, std::generic_category()).message();
}

}  // namespace

HardwareLock::HardwareLock(const std::filesystem::path &path) : descriptor_(Open(path)) {
  if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
    const std::string detail = ErrorMessage(errno);
    static_cast<void>(close(descriptor_));
    descriptor_ = -1;
    throw std::runtime_error("another camera job holds " + path.string() + ": " + detail);
  }
}

HardwareLock::~HardwareLock() {
  if (descriptor_ >= 0) {
    static_cast<void>(close(descriptor_));
  }
}

int HardwareLock::Open(const std::filesystem::path &path) {
  if (path.empty() || path.filename().empty()) {
    throw std::invalid_argument("hardware lock path must name a file");
  }
  std::filesystem::create_directories(path.parent_path());
  // open(2) is required to combine creation, O_NOFOLLOW, and a descriptor
  // suitable for flock(2).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int descriptor = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0660);
  if (descriptor < 0) {
    throw std::runtime_error("cannot open hardware lock " + path.string() + ": " +
                             ErrorMessage(errno));
  }
  return descriptor;
}

}  // namespace swing_capture::station
