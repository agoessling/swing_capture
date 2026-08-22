#include "web/process_lifecycle.h"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <cerrno>

namespace {

void Close(int descriptor) { assert(close(descriptor) == 0); }

void WriteExactly(int descriptor, const void *data, std::size_t size) {
  const auto *bytes = static_cast<const char *>(data);
  std::size_t written = 0;
  while (written < size) {
    const ssize_t result = write(descriptor, bytes + written, size - written);
    assert(result > 0);
    written += static_cast<std::size_t>(result);
  }
}

void ReadExactly(int descriptor, void *data, std::size_t size) {
  auto *bytes = static_cast<char *>(data);
  std::size_t consumed = 0;
  while (consumed < size) {
    const ssize_t result = read(descriptor, bytes + consumed, size - consumed);
    assert(result > 0);
    consumed += static_cast<std::size_t>(result);
  }
}

}  // namespace

int main() {
  std::array<int, 2> ready{};
  std::array<int, 2> worker_pid_pipe{};
  std::array<int, 2> lifetime{};
  assert(pipe(ready.data()) == 0);
  assert(pipe(worker_pid_pipe.data()) == 0);
  assert(pipe(lifetime.data()) == 0);

  const pid_t supervisor = fork();
  assert(supervisor >= 0);
  if (supervisor == 0) {
    Close(worker_pid_pipe[0]);
    Close(lifetime[0]);
    const pid_t worker = fork();
    if (worker < 0) {
      _exit(2);
    }
    if (worker == 0) {
      Close(ready[0]);
      Close(worker_pid_pipe[1]);
      if (!swing_capture::web::ArmParentDeathSignal(getppid())) {
        _exit(3);
      }
      const char marker = 'R';
      WriteExactly(ready[1], &marker, sizeof(marker));
      Close(ready[1]);
      while (true) {
        pause();
      }
    }

    Close(ready[1]);
    Close(lifetime[1]);
    char marker = '\0';
    ReadExactly(ready[0], &marker, sizeof(marker));
    Close(ready[0]);
    if (marker != 'R') {
      _exit(4);
    }
    WriteExactly(worker_pid_pipe[1], &worker, sizeof(worker));
    Close(worker_pid_pipe[1]);
    while (true) {
      pause();
    }
  }

  Close(ready[0]);
  Close(ready[1]);
  Close(worker_pid_pipe[1]);
  Close(lifetime[1]);
  pid_t worker = 0;
  ReadExactly(worker_pid_pipe[0], &worker, sizeof(worker));
  Close(worker_pid_pipe[0]);
  assert(worker > 1);

  assert(kill(supervisor, SIGKILL) == 0);
  int supervisor_status = 0;
  assert(waitpid(supervisor, &supervisor_status, 0) == supervisor);
  assert(WIFSIGNALED(supervisor_status));
  assert(WTERMSIG(supervisor_status) == SIGKILL);

  pollfd descriptor{.fd = lifetime[0], .events = POLLIN | POLLHUP, .revents = 0};
  int poll_result = 0;
  do {
    poll_result = poll(&descriptor, 1, 2'000);
  } while (poll_result < 0 && errno == EINTR);
  assert(poll_result == 1);
  char byte = '\0';
  assert(read(lifetime[0], &byte, sizeof(byte)) == 0);
  Close(lifetime[0]);
  return 0;
}
