// Host PTY, the output ring and the session bookkeeping. POSIX-only: the
// Windows build compiles the stub backend and skips the tests until ConPTY
// lands in the next batch.
#include <cstdio>

#if !defined(_WIN32)

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "terminal/pty.h"
#include "terminal/terminal_service.h"

namespace {

using molink::pty_handle;
using molink::pty_kill;
using molink::pty_poll_exit;
using molink::pty_read;
using molink::pty_resize;
using molink::pty_spawn;
using molink::pty_write;
using molink::terminal_ring_buffer;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

bool deadline_passed(std::chrono::steady_clock::time_point deadline) {
  return std::chrono::steady_clock::now() >= deadline;
}

/// Read until `needle` appears or the deadline expires. The PTY is
/// non-blocking; poll() keeps this from spinning.
std::string read_until(pty_handle& handle, const std::string& needle,
                       int timeout_ms) {
  std::string out;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (!deadline_passed(deadline)) {
    struct pollfd watched = {};
    watched.fd = handle.fd;
    watched.events = POLLIN;
    const int ready = ::poll(&watched, 1, 20);
    if (ready > 0) {
      char buffer[4096];
      const int n = pty_read(handle, buffer, sizeof(buffer));
      if (n > 0) {
        out.append(buffer, static_cast<std::size_t>(n));
        if (out.find(needle) != std::string::npos) return out;
        continue;
      }
      if (n == 0) break;
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
    }
  }
  return out;
}

bool wait_exit(pty_handle& handle, int& code, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (!deadline_passed(deadline)) {
    if (pty_poll_exit(handle, code)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pty_poll_exit(handle, code);
}

void test_echo_and_exit_code() {
  std::string error;
  pty_handle handle = pty_spawn("/bin/sh -c 'echo hello'", 80, 24, error);
  expect(handle.fd >= 0, "spawn /bin/sh -c 'echo hello'");
  if (handle.fd < 0) {
    std::fprintf(stderr, "  spawn error: %s\n", error.c_str());
    return;
  }
  const std::string out = read_until(handle, "hello", 3000);
  expect(out.find("hello") != std::string::npos, "read hello from the PTY");
  int code = -1;
  expect(wait_exit(handle, code, 3000), "shell exits after echo");
  expect(code == 0, "exit code is 0");
  pty_kill(handle);
}

void test_bidirectional_cat() {
  std::string error;
  pty_handle handle = pty_spawn("cat", 80, 24, error);
  expect(handle.fd >= 0, "spawn cat");
  if (handle.fd < 0) {
    std::fprintf(stderr, "  spawn error: %s\n", error.c_str());
    return;
  }
  const char input[] = "abc\n";
  const int written =
      pty_write(handle, input, sizeof(input) - 1);
  expect(written == static_cast<int>(sizeof(input) - 1), "write abc\\n");
  const std::string out = read_until(handle, "abc", 3000);
  expect(out.find("abc") != std::string::npos, "read abc back from cat");
  pty_kill(handle);
}

void test_resize() {
  std::string error;
  pty_handle handle =
      pty_spawn("/bin/sh -c 'sleep 0.2; stty size'", 80, 24, error);
  expect(handle.fd >= 0, "spawn shell that reads its window size");
  if (handle.fd < 0) {
    std::fprintf(stderr, "  spawn error: %s\n", error.c_str());
    return;
  }
  expect(pty_resize(handle, 100, 30), "resize PTY to 100x30");

  struct winsize size = {};
  const bool got_size =
      ::ioctl(handle.fd, TIOCGWINSZ, &size) == 0;
  expect(got_size && size.ws_col == 100 && size.ws_row == 30,
         "master reports the new 100x30 size");

  // The child's own view after the SIGWINCH: `stty size` prints rows cols.
  const std::string out = read_until(handle, "30 100", 3000);
  expect(out.find("30 100") != std::string::npos,
         "child sees the new 100x30 size");
  int code = -1;
  expect(wait_exit(handle, code, 3000), "resize test child exits");
  pty_kill(handle);
}

void test_kill_reaps_process() {
  std::string error;
  pty_handle handle = pty_spawn("/bin/sh -c 'sleep 30'", 80, 24, error);
  expect(handle.fd >= 0, "spawn a long-lived child");
  if (handle.fd < 0) {
    std::fprintf(stderr, "  spawn error: %s\n", error.c_str());
    return;
  }
  const int pid = handle.pid;
  expect(pid > 0, "child pid is valid");
  pty_kill(handle);
  errno = 0;
  const int probe = ::kill(pid, 0);
  expect(probe == -1 && errno == ESRCH,
         "kill(pid, 0) reports the child is gone after pty_kill");
  expect(handle.fd < 0, "pty_kill closes the master side");
}

void test_ring_buffer() {
  const std::size_t capacity = 1024u * 1024u;
  const std::size_t total = capacity + 123;
  std::vector<uint8_t> data(total);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(i & 0xff);
  }

  terminal_ring_buffer ring;
  ring.append(data.data(), data.size());
  expect(ring.seq() == total, "seq counts every appended byte");
  expect(ring.size() == capacity, "the ring keeps exactly its capacity");
  expect(ring.oldest_seq() == total - capacity,
         "oldest_seq points at the first retained byte");

  uint64_t start = 0;
  bool truncated = false;
  const std::vector<uint8_t> all = ring.replay(0, start, truncated);
  expect(truncated, "replay from an evicted seq reports truncation");
  expect(start == total - capacity, "replay starts at the oldest byte");
  expect(all.size() == capacity, "replay returns the whole ring");
  if (!all.empty()) {
    expect(all.front() == data[total - capacity],
           "replayed first byte matches the input");
    expect(all.back() == data.back(), "replayed last byte matches the input");
  }

  const std::vector<uint8_t> tail = ring.replay(total - 10, start, truncated);
  expect(!truncated, "replay of retained bytes is not truncated");
  expect(start == total - 10, "tail replay starts at the requested seq");
  expect(tail.size() == 10, "tail replay has the requested length");

  const std::vector<uint8_t> ahead = ring.replay(total + 50, start, truncated);
  expect(ahead.empty(), "replay past the end returns nothing");
  expect(start == total, "replay past the end reports seq as the start");

  // Wrapping a small ring must drop exactly the oldest bytes.
  terminal_ring_buffer small(8);
  const char first[] = "abcdef";
  const char second[] = "ghij";
  small.append(reinterpret_cast<const uint8_t*>(first), 6);
  small.append(reinterpret_cast<const uint8_t*>(second), 4);
  expect(small.seq() == 10, "small ring seq after wrapping");
  expect(small.oldest_seq() == 2, "small ring dropped the first two bytes");
  const std::vector<uint8_t> small_all = small.replay(0, start, truncated);
  expect(truncated && start == 2, "small ring reports the drop");
  expect(std::string(small_all.begin(), small_all.end()) == "cdefghij",
         "small ring keeps the newest bytes in order");
}

}  // namespace

int main() {
  // A child that dies mid-write must not take the test process with it.
  ::signal(SIGPIPE, SIG_IGN);
  test_echo_and_exit_code();
  test_bidirectional_cat();
  test_resize();
  test_kill_reaps_process();
  test_ring_buffer();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

#else  // _WIN32

int main() {
  std::printf("terminal tests are POSIX-only; skipping\n");
  return 0;
}

#endif  // _WIN32
