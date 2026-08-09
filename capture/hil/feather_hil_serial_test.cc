#include "capture/hil/feather_hil_serial.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "capture/hil/feather_hil_protocol.h"

namespace {

using swing_capture::hil::BuildFeatherQueryCommand;
using swing_capture::hil::FeatherHilSerial;
using swing_capture::hil::FeatherResponseKind;

class PseudoTerminal final {
 public:
  PseudoTerminal() {
    master_ = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(master_ >= 0);
    assert(grantpt(master_) == 0);
    assert(unlockpt(master_) == 0);
    std::array<char, 256> path = {};
    assert(ptsname_r(master_, path.data(), path.size()) == 0);
    slave_path_ = path.data();
  }

  ~PseudoTerminal() { close(master_); }

  PseudoTerminal(const PseudoTerminal &) = delete;
  PseudoTerminal &operator=(const PseudoTerminal &) = delete;
  PseudoTerminal(PseudoTerminal &&) = delete;
  PseudoTerminal &operator=(PseudoTerminal &&) = delete;

  [[nodiscard]] int master() const { return master_; }
  [[nodiscard]] const std::filesystem::path &slave_path() const { return slave_path_; }

 private:
  int master_ = -1;
  std::filesystem::path slave_path_;
};

void TestRoundTrip() {
  PseudoTerminal terminal;
  FeatherHilSerial serial(terminal.slave_path());
  std::jthread device([&terminal] {
    std::array<char, 128> command = {};
    const ssize_t command_size = read(terminal.master(), command.data(), command.size());
    assert(command_size > 0);
    assert(std::string(command.data(), static_cast<std::size_t>(command_size)) ==
           "SC-HIL/1 17 QUERY\n");
    const std::string first = "SC-HIL/1 17 OK QUERY firmware=fixture-1 ";
    const std::string second = "protocol=1 capabilities=query,led,tone\r\n";
    assert(write(terminal.master(), first.data(), first.size()) ==
           static_cast<ssize_t>(first.size()));
    assert(write(terminal.master(), second.data(), second.size()) ==
           static_cast<ssize_t>(second.size()));
  });

  serial.Write(BuildFeatherQueryCommand(17), std::chrono::milliseconds(100));
  const auto response = serial.Read(std::chrono::milliseconds(100));
  assert(response.kind == FeatherResponseKind::kOk);
  assert(response.request_id == 17);
  assert(response.fields.at("capabilities") == "query,led,tone");
}

}  // namespace

int main() {
  TestRoundTrip();
  return 0;
}
