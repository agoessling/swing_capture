#ifndef SWING_CAPTURE_WEB_MEMORY_USAGE_PARSER_H_
#define SWING_CAPTURE_WEB_MEMORY_USAGE_PARSER_H_

#include <cstdint>
#include <iosfwd>
#include <optional>

namespace swing_capture::web {

// Parses Linux /proc/<pid>/status and returns VmRSS + VmSwap in bytes.
std::optional<std::uint64_t> ParseProcStatusMemoryBytes(std::istream &input);

}  // namespace swing_capture::web

#endif  // SWING_CAPTURE_WEB_MEMORY_USAGE_PARSER_H_
