#include <cstddef>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

int main(int argument_count, char **arguments) {
  if (argument_count != 2) {
    return 64;
  }
  const std::span<char *> argument_span(arguments, static_cast<std::size_t>(argument_count));
  const std::string_view mode(argument_span[1]);
  if (mode == "echo") {
    const std::string input(std::istreambuf_iterator<char>(std::cin), {});
    std::cout << "stdout:" << input;
    std::cerr << ":stderr";
    return 0;
  }
  if (mode == "fail") {
    std::cerr << "expected failure";
    return 7;
  }
  return 65;
}
