#include "web/memory_usage_parser.h"

#include <limits>
#include <sstream>
#include <string>

namespace swing_capture::web {
namespace {

constexpr std::uint64_t kBytesPerKibibyte = 1024;

}  // namespace

std::optional<std::uint64_t> ParseProcStatusMemoryBytes(std::istream &input) {
  std::uint64_t total_kibibytes = 0;
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::string label;
    if (!(fields >> label) || (label != "VmRSS:" && label != "VmSwap:")) {
      continue;
    }
    std::uint64_t value = 0;
    std::string unit;
    if (!(fields >> value >> unit) || unit != "kB" ||
        value > std::numeric_limits<std::uint64_t>::max() - total_kibibytes) {
      return std::nullopt;
    }
    total_kibibytes += value;
  }
  if (total_kibibytes > std::numeric_limits<std::uint64_t>::max() / kBytesPerKibibyte) {
    return std::nullopt;
  }
  return total_kibibytes * kBytesPerKibibyte;
}

}  // namespace swing_capture::web
