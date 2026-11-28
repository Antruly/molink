// Fatal-signal crash report. The handler lives in crash_dump.cpp and is
// installed by main() at startup; here a forked child installs it against a
// temporary config directory, raises SIGSEGV, and the parent checks that the
// report is non-empty, names the signal and carries the raw frame addresses.
//
// Windows has no fork(); the test prints the reason and exits 77, which CTest
// records as skipped.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "crash_dump.h"

#if !defined(_WIN32)
#include <csignal>
#include <chrono>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

#if !defined(_WIN32)

#if defined(__GNUC__)
#define MOLINK_NOINLINE __attribute__((noinline))
#else
#define MOLINK_NOINLINE
#endif

// A few real frames below the signal handler, so the report has more than the
// handler and the libc trampoline to show.
MOLINK_NOINLINE void crash_level_2() { std::raise(SIGSEGV); }
MOLINK_NOINLINE void crash_level_1() { crash_level_2(); }
MOLINK_NOINLINE void crash_entry() { crash_level_1(); }

fs::path make_temp_dir() {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = fs::temp_directory_path() /
                       ("molink-crash-dump-" + std::to_string(stamp) + "-" +
                        std::to_string(static_cast<long long>(::getpid())));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

int count_frame_addresses(const std::string& text) {
  int count = 0;
  std::size_t line_start = 0;
  while (line_start < text.size()) {
    const std::size_t line_end = text.find('\n', line_start);
    const std::string line =
        text.substr(line_start, line_end == std::string::npos
                                    ? std::string::npos
                                    : line_end - line_start);
    if (line.rfind("frame", 0) == 0 && line.find("=0x") != std::string::npos) {
      ++count;
    }
    if (line_end == std::string::npos) break;
    line_start = line_end + 1;
  }
  return count;
}

#endif  // !_WIN32

}  // namespace

int main() {
#if defined(_WIN32)
  std::printf(
      "SKIP: crash_dump_test forks a child and raises SIGSEGV; Windows has "
      "no fork() and the unhandled-exception path is covered by "
      "MiniDumpWriteDump at runtime instead\n");
  return 77;
#else
  const fs::path config_dir = make_temp_dir();
  const fs::path dump_dir = config_dir / "crashdumps";

  const pid_t pid = ::fork();
  if (pid < 0) {
    std::fprintf(stderr, "FAIL: fork() failed\n");
    fs::remove_all(config_dir);
    return 1;
  }
  if (pid == 0) {
    // Child: install against the temporary directory, then fault. The handler
    // writes the report and _exit(128 + SIGSEGV); nothing returns here.
    molink::install_crash_handlers(config_dir.string(), std::string());
    crash_entry();
    ::_exit(0);
  }

  int status = 0;
  if (::waitpid(pid, &status, 0) != pid) {
    std::fprintf(stderr, "FAIL: waitpid() failed\n");
    fs::remove_all(config_dir);
    return 1;
  }
  expect(WIFEXITED(status), "the handler terminates with _exit()");
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGSEGV,
         "the exit code is 128 + SIGSEGV");

  fs::path report;
  std::error_code ec;
  if (fs::is_directory(dump_dir, ec)) {
    for (const fs::directory_entry& entry :
         fs::directory_iterator(dump_dir, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("crash-", 0) == 0 && entry.path().extension() == ".txt") {
        report = entry.path();
        break;
      }
    }
  }
  expect(!report.empty(), "a crash-*.txt was written under crashdumps/");

  if (!report.empty()) {
    const std::string text = read_text(report);
    expect(!text.empty(), "the crash report is non-empty");
    expect(text.find("SIGSEGV") != std::string::npos,
           "the report names SIGSEGV");
    const int frames = count_frame_addresses(text);
    expect(frames >= 3,
           "the report carries at least three raw frame addresses");
    if (frames < 3) {
      std::fprintf(stderr, "  found %d frame address line(s)\n", frames);
    }
    expect(text.find("version=") != std::string::npos,
           "the report carries the version");
    expect(text.find("started=") != std::string::npos,
           "the report carries the process start time");
    std::printf("crash report: %s (%zu bytes, %d frames)\n",
                report.string().c_str(), text.size(), frames);
  }

  fs::remove_all(config_dir, ec);
  if (failures != 0) {
    std::fprintf(stderr, "%d/%d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("crash_dump: %d checks passed\n", checks);
  return 0;
#endif
}
