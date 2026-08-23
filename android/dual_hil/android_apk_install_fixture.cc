#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>

int main(int argument_count, char **arguments) {
  bool skip = false;
  const auto argument_span =
      std::span(arguments, static_cast<std::size_t>(argument_count)).subspan(1);
  for (const char *argument : argument_span) {
    skip = skip || std::string_view(argument) == "--skip-exact-match" ||
           std::string_view(argument) == "--require-exact-match";
  }
  constexpr std::string_view kSha =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  std::cout << "{\"passed\":true,\"installed\":" << (skip ? "false" : "true") << ",\"reason\":\""
            << (skip ? "exact_match" : "forced") << "\",\"local_sha256\":\"" << kSha
            << "\",\"installed_before_sha256\":\"" << kSha << "\",\"installed_after_sha256\":\""
            << kSha << "\",\"elapsed_ms\":7}\n";
}
