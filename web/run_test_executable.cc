#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

int main(int argument_count, char **arguments) {
  if (argument_count < 2) {
    std::cerr << "Expected a Bazel test-executable runfile argument\n";
    return 2;
  }
  const std::filesystem::path executable = std::filesystem::absolute(arguments[1]);
  if (!std::filesystem::is_regular_file(executable) || access(executable.c_str(), X_OK) != 0) {
    std::cerr << "Test runfile is not executable: " << executable << '\n';
    return 2;
  }

  std::vector<char *> child_arguments(arguments + 1, arguments + argument_count);
  child_arguments.push_back(nullptr);
  execv(executable.c_str(), child_arguments.data());
  std::cerr << "Unable to execute " << executable << ": " << std::strerror(errno) << '\n';
  return 1;
}
