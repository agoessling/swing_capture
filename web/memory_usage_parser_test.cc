#include "web/memory_usage_parser.h"

#include <cassert>
#include <cstdint>
#include <sstream>

int main() {
  std::istringstream status(
      "Name:\tnode\n"
      "VmSize:\t999999 kB\n"
      "VmRSS:\t204800 kB\n"
      "VmSwap:\t32768 kB\n");
  const std::optional<std::uint64_t> memory =
      swing_capture::web::ParseProcStatusMemoryBytes(status);
  assert(memory.has_value());
  assert(*memory == (204800ULL + 32768ULL) * 1024ULL);

  std::istringstream malformed("VmRSS:\t4 MB\n");
  assert(!swing_capture::web::ParseProcStatusMemoryBytes(malformed).has_value());

  std::istringstream empty("Name:\tnode\n");
  assert(swing_capture::web::ParseProcStatusMemoryBytes(empty) == 0);
  return 0;
}
