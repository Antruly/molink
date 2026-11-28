#include "crash_dump.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#else
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

#include "version.h"

namespace molink {
namespace {

constexpr std::size_t kPathMax = 4096;
constexpr int kMaxFrames = 64;

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

wchar_t g_dump_dir[kPathMax] = {};
char g_dump_dir_utf8[kPathMax] = {};
/// Double buffered so set_crash_diag_file() can update the path while the
/// filter reads it: the writer fills the inactive buffer and then flips the
/// index, so the filter always sees a complete path (or an empty one).
wchar_t g_diag_paths[2][kPathMax] = {};
volatile LONG g_diag_active = 0;
char g_started[64] = {};
volatile LONG g_in_filter = 0;

void append_utf8_path(char*& cursor, char* end, const char* text) {
  while (*text != '\0' && cursor < end) *cursor++ = *text++;
}

void append_ascii_int(char*& cursor, char* end, unsigned long long value) {
  char digits[24] = {};
  int count = 0;
  do {
    digits[count++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0);
  while (count > 0 && cursor < end) *cursor++ = digits[--count];
}

void append_ascii_hex(char*& cursor, char* end, unsigned long long value) {
  static const char kDigits[] = "0123456789abcdef";
  char digits[2 * sizeof(unsigned long long)] = {};
  int count = 0;
  do {
    digits[count++] = kDigits[value & 0xf];
    value >>= 4;
  } while (value != 0);
  if (cursor < end) *cursor++ = '0';
  if (cursor < end) *cursor++ = 'x';
  while (count > 0 && cursor < end) *cursor++ = digits[--count];
}

/// Copy an ASCII fragment into a wide path at @p cursor.
void append_wide_ascii(wchar_t*& cursor, const wchar_t* end, const char* text) {
  while (*text != '\0' && cursor < end) {
    *cursor++ = static_cast<wchar_t>(static_cast<unsigned char>(*text++));
  }
}

void append_wide_int(wchar_t*& cursor, const wchar_t* end,
                     unsigned long long value) {
  char digits[24] = {};
  int count = 0;
  do {
    digits[count++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0);
  while (count > 0 && cursor < end) {
    *cursor++ = static_cast<wchar_t>(digits[--count]);
  }
}

std::size_t utf8_to_wide(const std::string& text, wchar_t* out,
                         std::size_t capacity) {
  if (capacity == 0) return 0;
  out[0] = L'\0';
  if (text.empty()) return 0;
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (needed <= 0) return 0;
  if (static_cast<std::size_t>(needed) > capacity) return 0;
  const int written =
      MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out,
                          static_cast<int>(capacity));
  return written > 0 ? static_cast<std::size_t>(written - 1) : 0;
}

void write_handle(HANDLE file, const char* text, std::size_t length) {
  DWORD written = 0;
  std::size_t offset = 0;
  while (offset < length) {
    const DWORD chunk =
        static_cast<DWORD>(length - offset > 0x7fffffff
                               ? 0x7fffffff
                               : length - offset);
    if (!WriteFile(file, text + offset, chunk, &written, nullptr)) return;
    if (written == 0) return;
    offset += written;
  }
}

/// Appends one line to the pre-resolved diagnostic file, if any. This runs in
/// the unhandled-exception filter, so it uses a plain append handle instead of
/// diag(), whose mutex could be held by the crashing thread.
void append_diag_line(const char* line, std::size_t length) {
  const wchar_t* path = g_diag_paths[g_diag_active];
  if (path[0] == L'\0') return;
  HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  write_handle(file, line, length);
  CloseHandle(file);
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) {
  if (InterlockedCompareExchange(&g_in_filter, 1, 0) != 0) {
    return EXCEPTION_EXECUTE_HANDLER;
  }

  DWORD code = 0;
  std::uintptr_t address = 0;
  if (info != nullptr && info->ExceptionRecord != nullptr) {
    code = info->ExceptionRecord->ExceptionCode;
    address = reinterpret_cast<std::uintptr_t>(
        info->ExceptionRecord->ExceptionAddress);
  }
  const unsigned long long pid = GetCurrentProcessId();
  const long long timestamp = static_cast<long long>(std::time(nullptr));

  // core-<pid>-<ts>.dmp / .txt under the prepared directory.
  wchar_t dump_path[kPathMax] = {};
  wchar_t text_path[kPathMax] = {};
  wchar_t* dump_cursor = dump_path;
  wchar_t* text_cursor = text_path;
  const wchar_t* dump_end = dump_path + kPathMax - 1;
  const wchar_t* text_end = text_path + kPathMax - 1;
  // (append_* take a const end pointer; only the cursor advances.)
  if (g_dump_dir[0] != L'\0') {
    for (const wchar_t* p = g_dump_dir; *p != L'\0' && dump_cursor < dump_end;
         ++p) {
      *dump_cursor++ = *p;
    }
    for (const wchar_t* p = g_dump_dir; *p != L'\0' && text_cursor < text_end;
         ++p) {
      *text_cursor++ = *p;
    }
    if (dump_cursor < dump_end) *dump_cursor++ = L'\\';
    if (text_cursor < text_end) *text_cursor++ = L'\\';
    append_wide_ascii(dump_cursor, dump_end, "core-");
    append_wide_int(dump_cursor, dump_end, pid);
    if (dump_cursor < dump_end) *dump_cursor++ = L'-';
    append_wide_int(dump_cursor, dump_end,
                    static_cast<unsigned long long>(timestamp));
    append_wide_ascii(dump_cursor, dump_end, ".dmp");
    append_wide_ascii(text_cursor, text_end, "core-");
    append_wide_int(text_cursor, text_end, pid);
    if (text_cursor < text_end) *text_cursor++ = L'-';
    append_wide_int(text_cursor, text_end,
                    static_cast<unsigned long long>(timestamp));
    append_wide_ascii(text_cursor, text_end, ".txt");
  }
  *dump_cursor = L'\0';
  *text_cursor = L'\0';

  DWORD dump_error = 0;
  if (dump_path[0] != L'\0') {
    HANDLE file = CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION exception_info{};
      exception_info.ThreadId = GetCurrentThreadId();
      exception_info.ExceptionPointers = info;
      exception_info.ClientPointers = FALSE;
      const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
          MiniDumpNormal | MiniDumpWithHandleData);
      if (!MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                             type,
                             info != nullptr ? &exception_info : nullptr,
                             nullptr, nullptr)) {
        dump_error = GetLastError();
      }
      CloseHandle(file);
    } else {
      dump_error = GetLastError();
    }
  }

  if (text_path[0] != L'\0') {
    HANDLE file = CreateFileW(text_path, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      char line[512] = {};
      char* cursor = line;
      char* end = line + sizeof(line) - 1;
      append_utf8_path(cursor, end, "molink crash dump\n");
      append_utf8_path(cursor, end, "exception=");
      append_ascii_hex(cursor, end, code);
      append_utf8_path(cursor, end, "\naddr=");
      append_ascii_hex(cursor, end, address);
      append_utf8_path(cursor, end, "\npid=");
      append_ascii_int(cursor, end, pid);
      append_utf8_path(cursor, end, "\nstarted=");
      append_utf8_path(cursor, end, g_started);
      append_utf8_path(cursor, end, "\nversion=" MOLINK_VERSION "\n");
      if (dump_path[0] != L'\0') {
        append_utf8_path(cursor, end, "dump=");
        append_utf8_path(cursor, end, g_dump_dir_utf8);
        append_utf8_path(cursor, end, "\\core-");
        append_ascii_int(cursor, end, pid);
        if (cursor < end) *cursor++ = '-';
        append_ascii_int(cursor, end,
                         static_cast<unsigned long long>(timestamp));
        append_utf8_path(cursor, end, ".dmp\n");
      }
      if (dump_error != 0) {
        append_utf8_path(cursor, end, "dump_error=");
        append_ascii_hex(cursor, end, dump_error);
        append_utf8_path(cursor, end, "\n");
      }
      write_handle(file, line, static_cast<std::size_t>(cursor - line));
      CloseHandle(file);
    }
  }

  // One structured line in the diagnostic log. The crash report above is the
  // authoritative record; this line just puts the event on the timeline.
  {
    char line[640] = {};
    char* cursor = line;
    char* end = line + sizeof(line) - 1;
    // Same field name as the POSIX line, so one grep finds both. The value
    // here is the Win32 exception code rather than a signal number.
    append_utf8_path(cursor, end, "app.crash signal=");
    append_ascii_hex(cursor, end, code);
    append_utf8_path(cursor, end, " addr=");
    append_ascii_hex(cursor, end, address);
    append_utf8_path(cursor, end, " dump=");
    if (dump_path[0] != L'\0') {
      append_utf8_path(cursor, end, g_dump_dir_utf8);
      append_utf8_path(cursor, end, "\\core-");
      append_ascii_int(cursor, end, pid);
      if (cursor < end) *cursor++ = '-';
      append_ascii_int(cursor, end,
                       static_cast<unsigned long long>(timestamp));
      append_utf8_path(cursor, end, ".dmp");
    } else {
      append_utf8_path(cursor, end, "(none)");
    }
    append_utf8_path(cursor, end, "\n");
    append_diag_line(line, static_cast<std::size_t>(cursor - line));
  }

  return EXCEPTION_EXECUTE_HANDLER;
}

void install_windows(const std::string& config_dir,
                     const std::string& diag_file) {
  if (config_dir.empty()) return;
  const std::filesystem::path directory =
      std::filesystem::path(config_dir) / "crashdumps";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    std::fprintf(stderr, "molink crash dump: cannot create %s: %s\n",
                 directory.string().c_str(), ec.message().c_str());
    return;
  }
  const std::wstring wide = directory.wstring();
  if (wide.size() >= kPathMax) {
    std::fprintf(stderr, "molink crash dump: directory path is too long\n");
    return;
  }
  for (std::size_t i = 0; i < wide.size(); ++i) g_dump_dir[i] = wide[i];
  g_dump_dir[wide.size()] = L'\0';
  const std::string utf8 = directory.string();
  const std::size_t copied = utf8.copy(g_dump_dir_utf8, kPathMax - 1);
  g_dump_dir_utf8[copied] = '\0';

  // Install runs once, before the worker threads start, so the shared
  // localtime() buffer cannot race. Avoid localtime_s: MSVC and MinGW order
  // its arguments differently (see diag_log.cpp).
  const std::time_t now = std::time(nullptr);
  const std::tm* parts = std::localtime(&now);
  if (parts != nullptr) {
    std::strftime(g_started, sizeof(g_started), "%Y-%m-%dT%H:%M:%S", parts);
  }

  set_crash_diag_file(diag_file);
  SetUnhandledExceptionFilter(on_unhandled_exception);
}

#else  // !_WIN32
// ---------------------------------------------------------------------------
// POSIX
// ---------------------------------------------------------------------------

char g_dump_dir[kPathMax] = {};
/// Double buffered, same reason as the Windows side: the maintenance thread
/// rewrites the inactive buffer and then publishes it with a sig_atomic_t.
char g_diag_paths[2][kPathMax] = {};
volatile sig_atomic_t g_diag_active = 0;
char g_started[64] = {};
volatile sig_atomic_t g_in_handler = 0;

void append_cstr(char*& cursor, char* end, const char* text) {
  while (*text != '\0' && cursor < end) *cursor++ = *text++;
}

void append_int(char*& cursor, char* end, unsigned long long value) {
  char digits[24] = {};
  int count = 0;
  do {
    digits[count++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0);
  while (count > 0 && cursor < end) *cursor++ = digits[--count];
}

void append_signed_int(char*& cursor, char* end, long long value) {
  if (value < 0) {
    if (cursor < end) *cursor++ = '-';
    // Avoid overflow on LLONG_MIN.
    append_int(cursor, end,
               static_cast<unsigned long long>(-(value + 1)) + 1ull);
  } else {
    append_int(cursor, end, static_cast<unsigned long long>(value));
  }
}

void append_hex(char*& cursor, char* end, std::uintptr_t value) {
  static const char kDigits[] = "0123456789abcdef";
  char digits[2 * sizeof(std::uintptr_t)] = {};
  int count = 0;
  do {
    digits[count++] = kDigits[value & 0xf];
    value >>= 4;
  } while (value != 0);
  if (cursor < end) *cursor++ = '0';
  if (cursor < end) *cursor++ = 'x';
  while (count > 0 && cursor < end) *cursor++ = digits[--count];
}

const char* signal_name(int signal_number) {
  switch (signal_number) {
    case SIGSEGV:
      return "SIGSEGV";
    case SIGABRT:
      return "SIGABRT";
    case SIGBUS:
      return "SIGBUS";
    case SIGILL:
      return "SIGILL";
    case SIGFPE:
      return "SIGFPE";
    default:
      return "SIGUNKNOWN";
  }
}

void write_all(int fd, const char* data, std::size_t length) {
  std::size_t written = 0;
  while (written < length) {
    const ssize_t n = ::write(fd, data + written, length - written);
    if (n <= 0) return;
    written += static_cast<std::size_t>(n);
  }
}

/// Appends one line to the pre-resolved daily diagnostic file. diag() cannot
/// run in a signal handler (mutex + stdio); this is a plain O_APPEND write.
void append_diag_line(const char* line, std::size_t length) {
  const char* path = g_diag_paths[g_diag_active];
  if (path[0] == '\0') return;
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return;
  write_all(fd, line, length);
  ::close(fd);
}

void on_fatal_signal(int signal_number, siginfo_t* info, void*) {
  if (g_in_handler) _exit(128 + signal_number);
  g_in_handler = 1;

  // A second fault while writing the report must not re-enter (and corrupt)
  // it. Blocking everything also keeps a ctrl-alt-c-style secondary signal
  // from interrupting the file writes.
  sigset_t all_signals;
  sigfillset(&all_signals);
  sigprocmask(SIG_BLOCK, &all_signals, nullptr);

  const unsigned long long pid = static_cast<unsigned long long>(::getpid());
  const long long timestamp = static_cast<long long>(::time(nullptr));
  const std::uintptr_t fault_address =
      info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0;

  char path[kPathMax + 64] = {};
  bool have_path = false;
  if (g_dump_dir[0] != '\0') {
    char* cursor = path;
    char* end = path + sizeof(path) - 1;
    append_cstr(cursor, end, g_dump_dir);
    if (cursor < end) *cursor++ = '/';
    append_cstr(cursor, end, "crash-");
    append_int(cursor, end, pid);
    if (cursor < end) *cursor++ = '-';
    append_signed_int(cursor, end, timestamp);
    append_cstr(cursor, end, ".txt");
    *cursor = '\0';
    have_path = true;
  }

  int fd = -1;
  if (have_path) {
    fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  }

  int frame_count = 0;
  if (fd >= 0) {
    char line[256] = {};
    char* cursor = line;
    char* end = line + sizeof(line) - 1;

    append_cstr(cursor, end, "molink crash dump\n");
    append_cstr(cursor, end, "signal=");
    append_signed_int(cursor, end, signal_number);
    if (cursor < end) *cursor++ = ' ';
    append_cstr(cursor, end, signal_name(signal_number));
    if (cursor < end) *cursor++ = '\n';
    append_cstr(cursor, end, "addr=");
    append_hex(cursor, end, fault_address);
    if (cursor < end) *cursor++ = '\n';
    append_cstr(cursor, end, "pid=");
    append_int(cursor, end, pid);
    if (cursor < end) *cursor++ = '\n';
    append_cstr(cursor, end, "started=");
    append_cstr(cursor, end, g_started);
    if (cursor < end) *cursor++ = '\n';
    append_cstr(cursor, end, "version=" MOLINK_VERSION "\n");
    write_all(fd, line, static_cast<std::size_t>(cursor - line));

    void* addresses[kMaxFrames] = {};
    frame_count = backtrace(addresses, kMaxFrames);
    if (frame_count < 0) frame_count = 0;

    cursor = line;
    end = line + sizeof(line) - 1;
    append_cstr(cursor, end, "frames=");
    append_signed_int(cursor, end, frame_count);
    if (cursor < end) *cursor++ = '\n';
    write_all(fd, line, static_cast<std::size_t>(cursor - line));

    // Raw addresses first: these are what a symbol-less build (or an address
    // captured before a mapping went away) can still be resolved from.
    for (int i = 0; i < frame_count; ++i) {
      cursor = line;
      end = line + sizeof(line) - 1;
      append_cstr(cursor, end, "frame");
      append_signed_int(cursor, end, i);
      if (cursor < end) *cursor++ = '=';
      append_hex(cursor, end,
                 reinterpret_cast<std::uintptr_t>(addresses[i]));
      if (cursor < end) *cursor++ = '\n';
      write_all(fd, line, static_cast<std::size_t>(cursor - line));
    }
    write_all(fd, "symbols:\n", 9);
    backtrace_symbols_fd(addresses, frame_count, fd);
    ::close(fd);
  }

  // The heartbeat: app.crash must not depend on diag() being usable here.
  {
    char line[kPathMax + 256] = {};
    char* cursor = line;
    char* end = line + sizeof(line) - 1;
    append_cstr(cursor, end, "app.crash signal=");
    append_signed_int(cursor, end, signal_number);
    append_cstr(cursor, end, " addr=");
    append_hex(cursor, end, fault_address);
    append_cstr(cursor, end, " dump=");
    if (have_path) {
      append_cstr(cursor, end, path);
    } else {
      append_cstr(cursor, end, "(none)");
    }
    if (cursor < end) *cursor++ = '\n';
    append_diag_line(line, static_cast<std::size_t>(cursor - line));
  }

  _exit(128 + signal_number);
}

void install_posix(const std::string& config_dir,
                   const std::string& diag_file) {
  if (!config_dir.empty()) {
    const std::filesystem::path directory =
        std::filesystem::path(config_dir) / "crashdumps";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
      std::fprintf(stderr, "molink crash dump: cannot create %s: %s\n",
                   directory.string().c_str(), ec.message().c_str());
    } else {
      const std::string text = directory.string();
      const std::size_t copied = text.copy(g_dump_dir, kPathMax - 1);
      g_dump_dir[copied] = '\0';
    }
  }

  const std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
  if (localtime_r(&now, &tm_buf) != nullptr) {
    std::strftime(g_started, sizeof(g_started), "%Y-%m-%dT%H:%M:%S%z",
                  &tm_buf);
  }

  set_crash_diag_file(diag_file);

  // A stack overflow (a realistic SIGSEGV source) leaves no room to build the
  // report on the faulting stack; run the handler on a dedicated one.
  static char alternate_stack[64 * 1024] = {};
  stack_t stack{};
  stack.ss_sp = alternate_stack;
  stack.ss_size = sizeof(alternate_stack);
  stack.ss_flags = 0;
  if (sigaltstack(&stack, nullptr) != 0) {
    std::fprintf(stderr, "molink crash dump: sigaltstack failed\n");
  }

  struct sigaction action {};
  action.sa_sigaction = on_fatal_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  const int signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
  for (const int signal_number : signals) {
    if (sigaction(signal_number, &action, nullptr) != 0) {
      std::fprintf(stderr, "molink crash dump: cannot handle signal %d\n",
                   signal_number);
    }
  }
}
#endif  // _WIN32

}  // namespace

void install_crash_handlers(const std::string& config_dir,
                            const std::string& diag_file) {
#if defined(_WIN32)
  install_windows(config_dir, diag_file);
#else
  install_posix(config_dir, diag_file);
#endif
}

void set_crash_diag_file(const std::string& path) {
#if defined(_WIN32)
  const LONG next = g_diag_active == 0 ? 1 : 0;
  utf8_to_wide(path, g_diag_paths[next], kPathMax);
  g_diag_active = next;
#else
  const sig_atomic_t next = g_diag_active == 0 ? 1 : 0;
  const std::size_t copied = path.copy(g_diag_paths[next], kPathMax - 1);
  g_diag_paths[next][copied] = '\0';
  g_diag_active = next;
#endif
}

}  // namespace molink
